#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdalign.h>

#if defined(__APPLE__) && defined(__aarch64__)
    /* Apple Silicon uses 128-byte cache lines; most other CPUs use 64 bytes. */
    #define MATRIX_ALIGN 128
#else
    #define MATRIX_ALIGN 64
#endif

/* Make hot helper functions inline as aggressively as the compiler allows. */
#if defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE __attribute__((always_inline)) static inline
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline static
#else
    #define FORCE_INLINE static inline
#endif

/* Allocate memory aligned to MATRIX_ALIGN bytes on any platform. */
static void *aligned_alloc_wrapper(size_t align, size_t size)
{
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
#else
    (void)align;
    return malloc(size);
#endif
}

static void aligned_free_wrapper(void *p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

/* Use the AVX2 transpose on x86 with AVX2; otherwise use the scalar one. */
#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) && defined(__AVX2__)
    #define HAVE_AVX2 1
    #include "avx2.c"
    #define transpose_matrix transpose_matrix_avx2
#else
    #define transpose_matrix transpose_matrix_scalar
#endif

/* Pick the best timing backend for this platform. */
#if USE_PICOPERF
    #include "picoperf.h"
    typedef int perf_timer_t;
    #define START_MEASURING(t)   do { (void)(t); start_measuring(); } while(0)
    #define STOP_MEASURING(t)    do { (void)(t); print_measured_results(stop_measuring()); } while(0)
#else
    #include "perf_timer.h"
    #define START_MEASURING(t)   start_measuring(&(t))
    #define STOP_MEASURING(t)    print_measured_results(stop_measuring(&(t)))
#endif

#define BATCH_SIZE (1 << 20)  /* Read and write in 1 MiB chunks. */

#if defined(__clang__)
    #define rotate_left(x,n)  __builtin_rotateleft32((x),(n))
    #define rotate_right(x,n) __builtin_rotateright32((x),(n))
#elif defined(__GNUC__) && __GNUC__ >= 15
    #define rotate_left(x,n)  __builtin_stdc_rotate_left((x),(n))
    #define rotate_right(x,n) __builtin_stdc_rotate_right((x),(n))
#else
static inline uint32_t rotate_left(uint32_t x, unsigned n)
{
    n &= 31;
    return (x << n) | (x >> ((32 - n) & 31));
}

static inline uint32_t rotate_right(uint32_t x, unsigned n)
{
    n &= 31;
    return (x >> n) | (x << ((32 - n) & 31));
}
#endif


FORCE_INLINE void transpose_matrix_scalar(uint32_t * __restrict__ m)
{
    uint32_t t;

    /* Stage 1: swap the upper and lower 16 bits of each row. */
    uint32_t mask = 0x0000FFFF;
    for (int j = 0; j < 16; j++) 
    {
        t = ((m[j] >> 16) ^ m[j + 16]) & mask;
        m[j + 16] = m[j + 16] ^ t;
        m[j] = m[j] ^ (t << 16);
    }

    /* Stage 2: swap the upper and lower 8 bits of each row. */
    mask = 0x00FF00FF;
    for (int j = 0; j < 32; j += 16)
    {
        for (int i = j; i < j + 8; i++) 
        {
            t = ((m[i] >> 8) ^ m[i + 8]) & mask;
            m[i + 8] = m[i + 8] ^ t;
            m[i] = m[i] ^ (t << 8);
        }
    }
    /* Stage 3: swap the upper and lower 4 bits of each row. */
    mask = 0x0F0F0F0F;
    for (int j = 0; j < 32; j += 8)
    {
        for (int i = j; i < j + 4; i++) 
        {
            t = ((m[i] >> 4) ^ m[i + 4]) & mask;
            m[i + 4] = m[i + 4] ^ t;
            m[i] = m[i] ^ (t << 4);
        }
    }
    /* Stage 4: swap the upper and lower 2 bits of each row. */
    mask = 0x33333333;
    for (int j = 0; j < 32; j += 4)
    {
        for (int i = j; i < j + 2; i++) 
        {
            t = ((m[i] >> 2) ^ m[i + 2]) & mask;
            m[i + 2] = m[i + 2] ^ t;
            m[i] = m[i] ^ (t << 2);
        }
    }
    /* Stage 5: swap adjacent bits inside each row. */
    mask = 0x55555555;
    for (int j = 0; j < 32; j += 2) 
    {
        t = ((m[j] >> 1) ^ m[j + 1]) & mask;
        m[j + 1] = m[j + 1] ^ t;
        m[j] = m[j] ^ (t << 1);
    }
}

/*
 * Precomputed tables for the rotation and XOR amounts.
 * Index 0 = encryption, index 1 = decryption.
 */
static uint8_t  scalar_rot_amount[2][8][2][32];
static uint32_t scalar_xor_key[2][8][2];

#if HAVE_AVX2
    static __m256i avx2_rot_amount[2][8][2][4];
    static __m256i avx2_xor_vec[2][8][2];
#endif

/*
 * Build the rotation-amount and XOR-key tables from the 128-byte key.
 * The tables depend only on the key, so they are computed once.
 */
static void precompute_key_tables(const uint32_t *key_matrix)
{
    for (int round = 0; round < 8; round++)
    {
        /* Encryption uses key words 0..31 in forward order. */
        int ke_rot0 = (int)key_matrix[4 * round + 0];
        int ke_xor0 = (int)key_matrix[4 * round + 1];
        int ke_rot1 = (int)key_matrix[4 * round + 2];
        int ke_xor1 = (int)key_matrix[4 * round + 3];

        /* Decryption uses the same key words in reverse order. */
        int kd_xor0 = (int)key_matrix[31 - 4 * round];
        int kd_rot0 = (int)key_matrix[30 - 4 * round];
        int kd_xor1 = (int)key_matrix[29 - 4 * round];
        int kd_rot1 = (int)key_matrix[28 - 4 * round];

        scalar_xor_key[0][round][0] = (uint32_t)ke_xor0;
        scalar_xor_key[0][round][1] = (uint32_t)ke_xor1;
        scalar_xor_key[1][round][0] = (uint32_t)kd_xor0;
        scalar_xor_key[1][round][1] = (uint32_t)kd_xor1;

        for (int i = 0; i < 32; i++)
        {
            int amt;

            /* Encryption phase 0: even left, odd right. */
            amt = (ke_rot0 + i + (i & 1)) & 31;
            scalar_rot_amount[0][round][0][i] = (uint8_t)amt;

            /* Encryption phase 1: even right, odd left. */
            amt = (ke_rot1 + i + (i & 1)) & 31;
            scalar_rot_amount[0][round][1][i] = (uint8_t)amt;

            /* Decryption phase 0: even left, odd right. */
            amt = (kd_rot0 + i + (i & 1)) & 31;
            scalar_rot_amount[1][round][0][i] = (uint8_t)amt;

            /* Decryption phase 1: even right, odd left. */
            amt = (kd_rot1 + i + (i & 1)) & 31;
            scalar_rot_amount[1][round][1][i] = (uint8_t)amt;
        }

#if HAVE_AVX2
        for (int ymm = 0; ymm < 4; ymm++)
        {
            int base = ymm * 8;
            avx2_rot_amount[0][round][0][ymm] = _mm256_set_epi32(
                scalar_rot_amount[0][round][0][base + 7],
                scalar_rot_amount[0][round][0][base + 6],
                scalar_rot_amount[0][round][0][base + 5],
                scalar_rot_amount[0][round][0][base + 4],
                scalar_rot_amount[0][round][0][base + 3],
                scalar_rot_amount[0][round][0][base + 2],
                scalar_rot_amount[0][round][0][base + 1],
                scalar_rot_amount[0][round][0][base + 0]);
            avx2_rot_amount[0][round][1][ymm] = _mm256_set_epi32(
                scalar_rot_amount[0][round][1][base + 7],
                scalar_rot_amount[0][round][1][base + 6],
                scalar_rot_amount[0][round][1][base + 5],
                scalar_rot_amount[0][round][1][base + 4],
                scalar_rot_amount[0][round][1][base + 3],
                scalar_rot_amount[0][round][1][base + 2],
                scalar_rot_amount[0][round][1][base + 1],
                scalar_rot_amount[0][round][1][base + 0]);
            avx2_rot_amount[1][round][0][ymm] = _mm256_set_epi32(
                scalar_rot_amount[1][round][0][base + 7],
                scalar_rot_amount[1][round][0][base + 6],
                scalar_rot_amount[1][round][0][base + 5],
                scalar_rot_amount[1][round][0][base + 4],
                scalar_rot_amount[1][round][0][base + 3],
                scalar_rot_amount[1][round][0][base + 2],
                scalar_rot_amount[1][round][0][base + 1],
                scalar_rot_amount[1][round][0][base + 0]);
            avx2_rot_amount[1][round][1][ymm] = _mm256_set_epi32(
                scalar_rot_amount[1][round][1][base + 7],
                scalar_rot_amount[1][round][1][base + 6],
                scalar_rot_amount[1][round][1][base + 5],
                scalar_rot_amount[1][round][1][base + 4],
                scalar_rot_amount[1][round][1][base + 3],
                scalar_rot_amount[1][round][1][base + 2],
                scalar_rot_amount[1][round][1][base + 1],
                scalar_rot_amount[1][round][1][base + 0]);
        }

        /* XOR vector has the key in element 7 (state index 31) only. */
        avx2_xor_vec[0][round][0] = _mm256_set_epi32(ke_xor0, 0, 0, 0, 0, 0, 0, 0);
        avx2_xor_vec[0][round][1] = _mm256_set_epi32(ke_xor1, 0, 0, 0, 0, 0, 0, 0);
        avx2_xor_vec[1][round][0] = _mm256_set_epi32(kd_xor0, 0, 0, 0, 0, 0, 0, 0);
        avx2_xor_vec[1][round][1] = _mm256_set_epi32(kd_xor1, 0, 0, 0, 0, 0, 0, 0);
#endif
    }
}

#if HAVE_AVX2
/*
 * Encrypt one 128-byte block entirely in AVX2 registers.
 * Loads the state, runs 8 rounds, stores the result.
 */
FORCE_INLINE void encrypt_block_avx2(uint32_t *state,
                                       const __m256i amounts[8][2][4],
                                       const __m256i xor_vec[8][2])
{
    __m256i r0 = _mm256_loadu_si256((__m256i *)&state[0]);
    __m256i r1 = _mm256_loadu_si256((__m256i *)&state[8]);
    __m256i r2 = _mm256_loadu_si256((__m256i *)&state[16]);
    __m256i r3 = _mm256_loadu_si256((__m256i *)&state[24]);

    for (int round = 0; round < 8; round++)
    {
        r0 = rotate_avx2_phase0(r0, amounts[round][0][0]);
        r1 = rotate_avx2_phase0(r1, amounts[round][0][1]);
        r2 = rotate_avx2_phase0(r2, amounts[round][0][2]);
        r3 = rotate_avx2_phase0(r3, amounts[round][0][3]);
        r3 = _mm256_xor_si256(r3, xor_vec[round][0]);
        transpose_avx2_regs(&r0, &r1, &r2, &r3);

        r0 = rotate_avx2_phase1(r0, amounts[round][1][0]);
        r1 = rotate_avx2_phase1(r1, amounts[round][1][1]);
        r2 = rotate_avx2_phase1(r2, amounts[round][1][2]);
        r3 = rotate_avx2_phase1(r3, amounts[round][1][3]);
        r3 = _mm256_xor_si256(r3, xor_vec[round][1]);
        transpose_avx2_regs(&r0, &r1, &r2, &r3);
    }

    _mm256_storeu_si256((__m256i *)&state[0],  r0);
    _mm256_storeu_si256((__m256i *)&state[8],  r1);
    _mm256_storeu_si256((__m256i *)&state[16], r2);
    _mm256_storeu_si256((__m256i *)&state[24], r3);
}

/*
 * Decrypt one 128-byte block entirely in AVX2 registers.
 */
FORCE_INLINE void decrypt_block_avx2(uint32_t *state,
                                       const __m256i amounts[8][2][4],
                                       const __m256i xor_vec[8][2])
{
    __m256i r0 = _mm256_loadu_si256((__m256i *)&state[0]);
    __m256i r1 = _mm256_loadu_si256((__m256i *)&state[8]);
    __m256i r2 = _mm256_loadu_si256((__m256i *)&state[16]);
    __m256i r3 = _mm256_loadu_si256((__m256i *)&state[24]);

    for (int round = 0; round < 8; round++)
    {
        transpose_avx2_regs(&r0, &r1, &r2, &r3);
        r3 = _mm256_xor_si256(r3, xor_vec[round][0]);
        r0 = rotate_avx2_phase0(r0, amounts[round][0][0]);
        r1 = rotate_avx2_phase0(r1, amounts[round][0][1]);
        r2 = rotate_avx2_phase0(r2, amounts[round][0][2]);
        r3 = rotate_avx2_phase0(r3, amounts[round][0][3]);

        transpose_avx2_regs(&r0, &r1, &r2, &r3);
        r3 = _mm256_xor_si256(r3, xor_vec[round][1]);
        r0 = rotate_avx2_phase1(r0, amounts[round][1][0]);
        r1 = rotate_avx2_phase1(r1, amounts[round][1][1]);
        r2 = rotate_avx2_phase1(r2, amounts[round][1][2]);
        r3 = rotate_avx2_phase1(r3, amounts[round][1][3]);
    }

    _mm256_storeu_si256((__m256i *)&state[0],  r0);
    _mm256_storeu_si256((__m256i *)&state[8],  r1);
    _mm256_storeu_si256((__m256i *)&state[16], r2);
    _mm256_storeu_si256((__m256i *)&state[24], r3);
}

/*
 * Encrypt two 128-byte blocks in parallel to expose more instruction-level
 * parallelism.  The two states are independent, so the CPU can issue the
 * transposes and rotations of the second block while the first is in flight.
 */
FORCE_INLINE void encrypt_two_blocks_avx2(uint32_t *a, uint32_t *b,
                                          const __m256i amounts[8][2][4],
                                          const __m256i xor_vec[8][2])
{
    __m256i a0 = _mm256_loadu_si256((__m256i *)&a[0]);
    __m256i a1 = _mm256_loadu_si256((__m256i *)&a[8]);
    __m256i a2 = _mm256_loadu_si256((__m256i *)&a[16]);
    __m256i a3 = _mm256_loadu_si256((__m256i *)&a[24]);

    __m256i b0 = _mm256_loadu_si256((__m256i *)&b[0]);
    __m256i b1 = _mm256_loadu_si256((__m256i *)&b[8]);
    __m256i b2 = _mm256_loadu_si256((__m256i *)&b[16]);
    __m256i b3 = _mm256_loadu_si256((__m256i *)&b[24]);

    for (int round = 0; round < 8; round++)
    {
        a0 = rotate_avx2_phase0(a0, amounts[round][0][0]);
        a1 = rotate_avx2_phase0(a1, amounts[round][0][1]);
        a2 = rotate_avx2_phase0(a2, amounts[round][0][2]);
        a3 = rotate_avx2_phase0(a3, amounts[round][0][3]);

        b0 = rotate_avx2_phase0(b0, amounts[round][0][0]);
        b1 = rotate_avx2_phase0(b1, amounts[round][0][1]);
        b2 = rotate_avx2_phase0(b2, amounts[round][0][2]);
        b3 = rotate_avx2_phase0(b3, amounts[round][0][3]);

        a3 = _mm256_xor_si256(a3, xor_vec[round][0]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][0]);

        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);

        a0 = rotate_avx2_phase1(a0, amounts[round][1][0]);
        a1 = rotate_avx2_phase1(a1, amounts[round][1][1]);
        a2 = rotate_avx2_phase1(a2, amounts[round][1][2]);
        a3 = rotate_avx2_phase1(a3, amounts[round][1][3]);

        b0 = rotate_avx2_phase1(b0, amounts[round][1][0]);
        b1 = rotate_avx2_phase1(b1, amounts[round][1][1]);
        b2 = rotate_avx2_phase1(b2, amounts[round][1][2]);
        b3 = rotate_avx2_phase1(b3, amounts[round][1][3]);

        a3 = _mm256_xor_si256(a3, xor_vec[round][1]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][1]);

        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);
    }

    _mm256_storeu_si256((__m256i *)&a[0],  a0);
    _mm256_storeu_si256((__m256i *)&a[8],  a1);
    _mm256_storeu_si256((__m256i *)&a[16], a2);
    _mm256_storeu_si256((__m256i *)&a[24], a3);

    _mm256_storeu_si256((__m256i *)&b[0],  b0);
    _mm256_storeu_si256((__m256i *)&b[8],  b1);
    _mm256_storeu_si256((__m256i *)&b[16], b2);
    _mm256_storeu_si256((__m256i *)&b[24], b3);
}

/*
 * Decrypt two 128-byte blocks in parallel.
 */
FORCE_INLINE void decrypt_two_blocks_avx2(uint32_t *a, uint32_t *b,
                                          const __m256i amounts[8][2][4],
                                          const __m256i xor_vec[8][2])
{
    __m256i a0 = _mm256_loadu_si256((__m256i *)&a[0]);
    __m256i a1 = _mm256_loadu_si256((__m256i *)&a[8]);
    __m256i a2 = _mm256_loadu_si256((__m256i *)&a[16]);
    __m256i a3 = _mm256_loadu_si256((__m256i *)&a[24]);

    __m256i b0 = _mm256_loadu_si256((__m256i *)&b[0]);
    __m256i b1 = _mm256_loadu_si256((__m256i *)&b[8]);
    __m256i b2 = _mm256_loadu_si256((__m256i *)&b[16]);
    __m256i b3 = _mm256_loadu_si256((__m256i *)&b[24]);

    for (int round = 0; round < 8; round++)
    {
        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);

        a3 = _mm256_xor_si256(a3, xor_vec[round][0]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][0]);

        a0 = rotate_avx2_phase0(a0, amounts[round][0][0]);
        a1 = rotate_avx2_phase0(a1, amounts[round][0][1]);
        a2 = rotate_avx2_phase0(a2, amounts[round][0][2]);
        a3 = rotate_avx2_phase0(a3, amounts[round][0][3]);

        b0 = rotate_avx2_phase0(b0, amounts[round][0][0]);
        b1 = rotate_avx2_phase0(b1, amounts[round][0][1]);
        b2 = rotate_avx2_phase0(b2, amounts[round][0][2]);
        b3 = rotate_avx2_phase0(b3, amounts[round][0][3]);

        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);

        a3 = _mm256_xor_si256(a3, xor_vec[round][1]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][1]);

        a0 = rotate_avx2_phase1(a0, amounts[round][1][0]);
        a1 = rotate_avx2_phase1(a1, amounts[round][1][1]);
        a2 = rotate_avx2_phase1(a2, amounts[round][1][2]);
        a3 = rotate_avx2_phase1(a3, amounts[round][1][3]);

        b0 = rotate_avx2_phase1(b0, amounts[round][1][0]);
        b1 = rotate_avx2_phase1(b1, amounts[round][1][1]);
        b2 = rotate_avx2_phase1(b2, amounts[round][1][2]);
        b3 = rotate_avx2_phase1(b3, amounts[round][1][3]);
    }

    _mm256_storeu_si256((__m256i *)&a[0],  a0);
    _mm256_storeu_si256((__m256i *)&a[8],  a1);
    _mm256_storeu_si256((__m256i *)&a[16], a2);
    _mm256_storeu_si256((__m256i *)&a[24], a3);

    _mm256_storeu_si256((__m256i *)&b[0],  b0);
    _mm256_storeu_si256((__m256i *)&b[8],  b1);
    _mm256_storeu_si256((__m256i *)&b[16], b2);
    _mm256_storeu_si256((__m256i *)&b[24], b3);
}
#endif


int main()
{
    /*
     * Hardcoded to '2' (decrypt) so the benchmark runs without interaction.
     * The original interactive prompts are kept below as comments for reference.
     */
    char ch = '2';
    /* printf("what do you want to do?\n1.Encrypt a file\n2.Decrypt a file\nenter your option's number:"); */
    /* scanf("%s", &ch); */

    _Alignas(MATRIX_ALIGN) uint32_t key_matrix[32];

    uint64_t bytes=0;

    if (ch == '1') /* ENCRYPTION path */
    {
        char *file, *lockedfile, *keyfile;

        file       = "../../big.data\0";
        keyfile    = "../../test.key\0";
        lockedfile = "../../big.data.locked\0";

        FILE *reader    = fopen(file,      "rb");
        FILE *keyreader = fopen(keyfile,   "rb");

        if (reader == NULL) 
        {
            printf("\nError in opening/finding the file %s", file);
            return 1;
        }
        if (keyreader == NULL) 
        {
            printf("\nError in opening/finding the file %s", keyfile);
            return 1;
        }

        fseek(keyreader, 0, SEEK_END);
        bytes = ftell(keyreader);
        fseek(keyreader, 0, SEEK_SET);
        if (bytes < 128) 
        {
            printf("\n%s is too small. For security reasons, the key file must be at least 128 bytes.", keyfile);
            return 1;
        }

        printf("ENCRYPTION STARTED...\n");

        FILE *writer = fopen(lockedfile, "wb");

        fread(key_matrix, 128, 1, keyreader);
        fclose(keyreader);

        precompute_key_tables(key_matrix);

        uint8_t *batch_buf = NULL;
        batch_buf = aligned_alloc_wrapper(MATRIX_ALIGN, BATCH_SIZE);
        if (batch_buf == NULL)
        {
            printf("\nFailed to allocate I/O buffer");
            fclose(reader);
            fclose(writer);
            return 1;
        }

        perf_timer_t timer;
        START_MEASURING(timer);

        size_t bytes_read;
        while ((bytes_read = fread(batch_buf, 1, BATCH_SIZE, reader)) > 0)
        {
            size_t num_blocks = bytes_read / 128;
            size_t b = 0;

#if HAVE_AVX2
            for (; b + 1 < num_blocks; b += 2)
            {
                uint32_t *a = (uint32_t *)(batch_buf + b * 128);
                uint32_t *bb = (uint32_t *)(batch_buf + (b + 1) * 128);
                encrypt_two_blocks_avx2(a, bb, avx2_rot_amount[0], avx2_xor_vec[0]);
            }
#endif

            for (; b < num_blocks; b++)
            {
                uint32_t *state = (uint32_t *)(batch_buf + b * 128);

#if HAVE_AVX2
                encrypt_block_avx2(state, avx2_rot_amount[0], avx2_xor_vec[0]);
#else
                for (int round = 0; round < 8; round++)
                {
                    for (int index = 0; index < 32; index += 2)
                    {
                        state[index]   = rotate_left (state[index],   scalar_rot_amount[0][round][0][index]);
                        state[index+1] = rotate_right(state[index+1], scalar_rot_amount[0][round][0][index + 1]);
                    }
                    state[31] ^= scalar_xor_key[0][round][0];
                    transpose_matrix(state);

                    for (int index = 0; index < 32; index += 2)
                    {
                        state[index]   = rotate_right(state[index],   scalar_rot_amount[0][round][1][index]);
                        state[index+1] = rotate_left (state[index+1], scalar_rot_amount[0][round][1][index + 1]);
                    }
                    state[31] ^= scalar_xor_key[0][round][1];
                    transpose_matrix(state);
                }
#endif
            }

            fwrite(batch_buf, 1, bytes_read, writer);
        }

        STOP_MEASURING(timer);

        aligned_free_wrapper(batch_buf);
        fclose(reader);
        fclose(writer);
        return 0;
    }
    else if (ch == '2') /* DECRYPTION path */
    {
        char *lockedfile, *unlockedfile, *keyfile;

        lockedfile   = "../../big.data.locked\0";
        keyfile      = "../../test.key\0";
        unlockedfile = "../../unlocked_test.data\0";

        FILE *reader    = fopen(lockedfile, "rb");
        FILE *keyreader = fopen(keyfile,    "rb");

        if (reader == NULL) 
        {
            printf("\nError in opening/finding the file %s", lockedfile);
            return 1;
        }
        if (keyreader == NULL) 
        {
            printf("\nError in opening/finding the file %s", keyfile);
            return 1;
        }

        fseek(keyreader, 0, SEEK_END);
        bytes = ftell(keyreader);
        fseek(keyreader, 0, SEEK_SET);
        if (bytes < 128) 
        {
            printf("\n%s is too small. For security reasons, the key file must be at least 128 bytes.", keyfile);
            return 1;
        }

        printf("DECRYPTION STARTED....\n");

        FILE *writer = fopen(unlockedfile, "wb");

        fread(key_matrix, 128, 1, keyreader);
        fclose(keyreader);

        precompute_key_tables(key_matrix);

        uint8_t *batch_buf = NULL;
        batch_buf = aligned_alloc_wrapper(MATRIX_ALIGN, BATCH_SIZE);
        if (batch_buf == NULL)
        {
            printf("\nFailed to allocate I/O buffer");
            fclose(reader);
            fclose(writer);
            return 1;
        }

        perf_timer_t timer;
        START_MEASURING(timer);

        size_t bytes_read;
        while ((bytes_read = fread(batch_buf, 1, BATCH_SIZE, reader)) > 0)
        {
            size_t num_blocks = bytes_read / 128;
            size_t b = 0;

#if HAVE_AVX2
            for (; b + 1 < num_blocks; b += 2)
            {
                uint32_t *a = (uint32_t *)(batch_buf + b * 128);
                uint32_t *bb = (uint32_t *)(batch_buf + (b + 1) * 128);
                decrypt_two_blocks_avx2(a, bb, avx2_rot_amount[1], avx2_xor_vec[1]);
            }
#endif

            for (; b < num_blocks; b++)
            {
                uint32_t *state = (uint32_t *)(batch_buf + b * 128);

#if HAVE_AVX2
                decrypt_block_avx2(state, avx2_rot_amount[1], avx2_xor_vec[1]);
#else
                for (int round = 0; round < 8; round++)
                {
                    transpose_matrix(state);
                    state[31] ^= scalar_xor_key[1][round][0];
                    for (int index = 0; index < 32; index += 2)
                    {
                        state[index]   = rotate_left (state[index],   scalar_rot_amount[1][round][0][index]);
                        state[index+1] = rotate_right(state[index+1], scalar_rot_amount[1][round][0][index + 1]);
                    }

                    transpose_matrix(state);
                    state[31] ^= scalar_xor_key[1][round][1];
                    for (int index = 0; index < 32; index += 2)
                    {
                        state[index]   = rotate_right(state[index],   scalar_rot_amount[1][round][1][index]);
                        state[index+1] = rotate_left (state[index+1], scalar_rot_amount[1][round][1][index + 1]);
                    }
                }
#endif
            }

            fwrite(batch_buf, 1, bytes_read, writer);
        }

        STOP_MEASURING(timer);

        aligned_free_wrapper(batch_buf);
        fclose(reader);
        fclose(writer);
        return 0;
    }
    else
    {
        printf("invalid input");
        return 1;
    }
}