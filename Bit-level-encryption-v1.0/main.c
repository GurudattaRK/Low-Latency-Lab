#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>

/* Loop unroll hints */
#if defined(__GNUC__) && !defined(__clang__)
    #define UNROLL_4  _Pragma("GCC unroll 4")
    #define UNROLL_8  _Pragma("GCC unroll 8")
    #define UNROLL_16 _Pragma("GCC unroll 16")
    #define UNROLL_32 _Pragma("GCC unroll 32")
#elif defined(__clang__)
    #define UNROLL_4  _Pragma("unroll 4")
    #define UNROLL_8  _Pragma("unroll 8")
    #define UNROLL_16 _Pragma("unroll 16")
    #define UNROLL_32 _Pragma("unroll 32")
#else
    #define UNROLL_4
    #define UNROLL_8
    #define UNROLL_16
    #define UNROLL_32
#endif

#if defined(__APPLE__) && defined(__aarch64__)
    /* Apple Silicon: 128-byte cache lines */
    #define MATRIX_ALIGN 128
#else
    #define MATRIX_ALIGN 64
#endif

/* Hot helpers: inline as aggressively as possible */
#if defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE __attribute__((always_inline)) static inline
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline static
#else
    #define FORCE_INLINE static inline
#endif

/* Aligned allocation wrapper */
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

/* Use AVX2 transpose on x86, scalar elsewhere */
#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) && defined(__AVX2__)
    #define HAVE_AVX2 1
    #include "avx2.c"
    #define transpose_matrix transpose_matrix_avx2
#else
    #define transpose_matrix transpose_matrix_scalar
#endif

/* Timing backend */
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

#define BATCH_SIZE (1 << 20)

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

    /* Stage 1 */
    uint32_t mask = 0x0000FFFF;
    for (int j = 0; j < 16; j++) 
    {
        t = ((m[j] >> 16) ^ m[j + 16]) & mask;
        m[j + 16] = m[j + 16] ^ t;
        m[j] = m[j] ^ (t << 16);
    }

    /* Stage 2 */
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
    /* Stage 3 */
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
    /* Stage 4 */
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
    /* Stage 5 */
    mask = 0x55555555;
    for (int j = 0; j < 32; j += 2) 
    {
        t = ((m[j] >> 1) ^ m[j + 1]) & mask;
        m[j + 1] = m[j + 1] ^ t;
        m[j] = m[j] ^ (t << 1);
    }
}

/* Precomputed tables. Index 0 = encrypt, 1 = decrypt. */
static uint8_t  scalar_rot_amount[2][8][2][32];
static uint32_t scalar_xor_key[2][8][2];

#if HAVE_AVX2
    static __m256i avx2_rot_amount[2][8][2][4];
    static __m256i avx2_rot_comp[2][8][2][4];
    static __m256i avx2_xor_vec[2][8][2];
#endif

/* Build rotation/XOR tables from the 128-byte key */
static void precompute_key_tables(const uint32_t *key_matrix)
{
    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        /* Encryption: key words forward */
        int ke_rot0 = (int)key_matrix[4 * round + 0];
        int ke_xor0 = (int)key_matrix[4 * round + 1];
        int ke_rot1 = (int)key_matrix[4 * round + 2];
        int ke_xor1 = (int)key_matrix[4 * round + 3];

        /* Decryption: same key words reversed */
        int kd_xor0 = (int)key_matrix[31 - 4 * round];
        int kd_rot0 = (int)key_matrix[30 - 4 * round];
        int kd_xor1 = (int)key_matrix[29 - 4 * round];
        int kd_rot1 = (int)key_matrix[28 - 4 * round];

        scalar_xor_key[0][round][0] = (uint32_t)ke_xor0;
        scalar_xor_key[0][round][1] = (uint32_t)ke_xor1;
        scalar_xor_key[1][round][0] = (uint32_t)kd_xor0;
        scalar_xor_key[1][round][1] = (uint32_t)kd_xor1;

        UNROLL_32
        for (int i = 0; i < 32; i++)
        {
            int amt;

            /* Even lanes: right-rotate */
            amt = (ke_rot0 + i + (i & 1)) & 31;
            if (!(i & 1))
                amt = (32 - amt) & 31;
            scalar_rot_amount[0][round][0][i] = (uint8_t)amt;

            /* Odd lanes: right-rotate */
            amt = (ke_rot1 + i + (i & 1)) & 31;
            if (i & 1)
                amt = (32 - amt) & 31;
            scalar_rot_amount[0][round][1][i] = (uint8_t)amt;

            /* Even lanes: right-rotate */
            amt = (kd_rot0 + i + (i & 1)) & 31;
            if (!(i & 1))
                amt = (32 - amt) & 31;
            scalar_rot_amount[1][round][0][i] = (uint8_t)amt;

            /* Odd lanes: right-rotate */
            amt = (kd_rot1 + i + (i & 1)) & 31;
            if (i & 1)
                amt = (32 - amt) & 31;
            scalar_rot_amount[1][round][1][i] = (uint8_t)amt;
        }

#if HAVE_AVX2
        UNROLL_4
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

        UNROLL_4
        for (int ymm = 0; ymm < 4; ymm++)
        {
            __m256i thirty_two = _mm256_set1_epi32(32);
            avx2_rot_comp[0][round][0][ymm] = _mm256_sub_epi32(thirty_two, avx2_rot_amount[0][round][0][ymm]);
            avx2_rot_comp[0][round][1][ymm] = _mm256_sub_epi32(thirty_two, avx2_rot_amount[0][round][1][ymm]);
            avx2_rot_comp[1][round][0][ymm] = _mm256_sub_epi32(thirty_two, avx2_rot_amount[1][round][0][ymm]);
            avx2_rot_comp[1][round][1][ymm] = _mm256_sub_epi32(thirty_two, avx2_rot_amount[1][round][1][ymm]);
        }

        /* XOR vector: key in element 7 (state[31]) only */
        avx2_xor_vec[0][round][0] = _mm256_set_epi32(ke_xor0, 0, 0, 0, 0, 0, 0, 0);
        avx2_xor_vec[0][round][1] = _mm256_set_epi32(ke_xor1, 0, 0, 0, 0, 0, 0, 0);
        avx2_xor_vec[1][round][0] = _mm256_set_epi32(kd_xor0, 0, 0, 0, 0, 0, 0, 0);
        avx2_xor_vec[1][round][1] = _mm256_set_epi32(kd_xor1, 0, 0, 0, 0, 0, 0, 0);
#endif
    }
}

#if HAVE_AVX2
/* Encrypt one 128-byte block in AVX2 registers */
FORCE_INLINE void encrypt_block_avx2(uint32_t *state,
                                       const __m256i amounts[8][2][4],
                                       const __m256i comp[8][2][4],
                                       const __m256i xor_vec[8][2])
{
    __m256i r0 = _mm256_load_si256((__m256i *)&state[0]);
    __m256i r1 = _mm256_load_si256((__m256i *)&state[8]);
    __m256i r2 = _mm256_load_si256((__m256i *)&state[16]);
    __m256i r3 = _mm256_load_si256((__m256i *)&state[24]);

    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        r0 = rotate_all_right(r0, amounts[round][0][0], comp[round][0][0]);
        r1 = rotate_all_right(r1, amounts[round][0][1], comp[round][0][1]);
        r2 = rotate_all_right(r2, amounts[round][0][2], comp[round][0][2]);
        r3 = rotate_all_right(r3, amounts[round][0][3], comp[round][0][3]);
        r3 = _mm256_xor_si256(r3, xor_vec[round][0]);
        transpose_avx2_regs(&r0, &r1, &r2, &r3);

        r0 = rotate_all_right(r0, amounts[round][1][0], comp[round][1][0]);
        r1 = rotate_all_right(r1, amounts[round][1][1], comp[round][1][1]);
        r2 = rotate_all_right(r2, amounts[round][1][2], comp[round][1][2]);
        r3 = rotate_all_right(r3, amounts[round][1][3], comp[round][1][3]);
        r3 = _mm256_xor_si256(r3, xor_vec[round][1]);
        transpose_avx2_regs(&r0, &r1, &r2, &r3);
    }

    _mm256_store_si256((__m256i *)&state[0],  r0);
    _mm256_store_si256((__m256i *)&state[8],  r1);
    _mm256_store_si256((__m256i *)&state[16], r2);
    _mm256_store_si256((__m256i *)&state[24], r3);
}

/* Decrypt one 128-byte block in AVX2 registers */
FORCE_INLINE void decrypt_block_avx2(uint32_t *state,
                                       const __m256i amounts[8][2][4],
                                       const __m256i comp[8][2][4],
                                       const __m256i xor_vec[8][2])
{
    __m256i r0 = _mm256_load_si256((__m256i *)&state[0]);
    __m256i r1 = _mm256_load_si256((__m256i *)&state[8]);
    __m256i r2 = _mm256_load_si256((__m256i *)&state[16]);
    __m256i r3 = _mm256_load_si256((__m256i *)&state[24]);

    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        transpose_avx2_regs(&r0, &r1, &r2, &r3);
        r3 = _mm256_xor_si256(r3, xor_vec[round][0]);
        r0 = rotate_all_right(r0, amounts[round][0][0], comp[round][0][0]);
        r1 = rotate_all_right(r1, amounts[round][0][1], comp[round][0][1]);
        r2 = rotate_all_right(r2, amounts[round][0][2], comp[round][0][2]);
        r3 = rotate_all_right(r3, amounts[round][0][3], comp[round][0][3]);

        transpose_avx2_regs(&r0, &r1, &r2, &r3);
        r3 = _mm256_xor_si256(r3, xor_vec[round][1]);
        r0 = rotate_all_right(r0, amounts[round][1][0], comp[round][1][0]);
        r1 = rotate_all_right(r1, amounts[round][1][1], comp[round][1][1]);
        r2 = rotate_all_right(r2, amounts[round][1][2], comp[round][1][2]);
        r3 = rotate_all_right(r3, amounts[round][1][3], comp[round][1][3]);
    }

    _mm256_store_si256((__m256i *)&state[0],  r0);
    _mm256_store_si256((__m256i *)&state[8],  r1);
    _mm256_store_si256((__m256i *)&state[16], r2);
    _mm256_store_si256((__m256i *)&state[24], r3);
}

/* Two blocks in parallel for more ILP */
FORCE_INLINE void encrypt_two_blocks_avx2(uint32_t *a, uint32_t *b,
                                          const __m256i amounts[8][2][4],
                                          const __m256i comp[8][2][4],
                                          const __m256i xor_vec[8][2])
{
    __m256i a0 = _mm256_load_si256((__m256i *)&a[0]);
    __m256i a1 = _mm256_load_si256((__m256i *)&a[8]);
    __m256i a2 = _mm256_load_si256((__m256i *)&a[16]);
    __m256i a3 = _mm256_load_si256((__m256i *)&a[24]);

    __m256i b0 = _mm256_load_si256((__m256i *)&b[0]);
    __m256i b1 = _mm256_load_si256((__m256i *)&b[8]);
    __m256i b2 = _mm256_load_si256((__m256i *)&b[16]);
    __m256i b3 = _mm256_load_si256((__m256i *)&b[24]);

    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        a0 = rotate_all_right(a0, amounts[round][0][0], comp[round][0][0]);
        a1 = rotate_all_right(a1, amounts[round][0][1], comp[round][0][1]);
        a2 = rotate_all_right(a2, amounts[round][0][2], comp[round][0][2]);
        a3 = rotate_all_right(a3, amounts[round][0][3], comp[round][0][3]);

        b0 = rotate_all_right(b0, amounts[round][0][0], comp[round][0][0]);
        b1 = rotate_all_right(b1, amounts[round][0][1], comp[round][0][1]);
        b2 = rotate_all_right(b2, amounts[round][0][2], comp[round][0][2]);
        b3 = rotate_all_right(b3, amounts[round][0][3], comp[round][0][3]);

        a3 = _mm256_xor_si256(a3, xor_vec[round][0]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][0]);

        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);

        a0 = rotate_all_right(a0, amounts[round][1][0], comp[round][1][0]);
        a1 = rotate_all_right(a1, amounts[round][1][1], comp[round][1][1]);
        a2 = rotate_all_right(a2, amounts[round][1][2], comp[round][1][2]);
        a3 = rotate_all_right(a3, amounts[round][1][3], comp[round][1][3]);

        b0 = rotate_all_right(b0, amounts[round][1][0], comp[round][1][0]);
        b1 = rotate_all_right(b1, amounts[round][1][1], comp[round][1][1]);
        b2 = rotate_all_right(b2, amounts[round][1][2], comp[round][1][2]);
        b3 = rotate_all_right(b3, amounts[round][1][3], comp[round][1][3]);

        a3 = _mm256_xor_si256(a3, xor_vec[round][1]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][1]);

        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);
    }

    _mm256_store_si256((__m256i *)&a[0],  a0);
    _mm256_store_si256((__m256i *)&a[8],  a1);
    _mm256_store_si256((__m256i *)&a[16], a2);
    _mm256_store_si256((__m256i *)&a[24], a3);

    _mm256_store_si256((__m256i *)&b[0],  b0);
    _mm256_store_si256((__m256i *)&b[8],  b1);
    _mm256_store_si256((__m256i *)&b[16], b2);
    _mm256_store_si256((__m256i *)&b[24], b3);
}

/* Decrypt two blocks in parallel */
FORCE_INLINE void decrypt_two_blocks_avx2(uint32_t *a, uint32_t *b,
                                          const __m256i amounts[8][2][4],
                                          const __m256i comp[8][2][4],
                                          const __m256i xor_vec[8][2])
{
    __m256i a0 = _mm256_load_si256((__m256i *)&a[0]);
    __m256i a1 = _mm256_load_si256((__m256i *)&a[8]);
    __m256i a2 = _mm256_load_si256((__m256i *)&a[16]);
    __m256i a3 = _mm256_load_si256((__m256i *)&a[24]);

    __m256i b0 = _mm256_load_si256((__m256i *)&b[0]);
    __m256i b1 = _mm256_load_si256((__m256i *)&b[8]);
    __m256i b2 = _mm256_load_si256((__m256i *)&b[16]);
    __m256i b3 = _mm256_load_si256((__m256i *)&b[24]);

    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);

        a3 = _mm256_xor_si256(a3, xor_vec[round][0]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][0]);

        a0 = rotate_all_right(a0, amounts[round][0][0], comp[round][0][0]);
        a1 = rotate_all_right(a1, amounts[round][0][1], comp[round][0][1]);
        a2 = rotate_all_right(a2, amounts[round][0][2], comp[round][0][2]);
        a3 = rotate_all_right(a3, amounts[round][0][3], comp[round][0][3]);

        b0 = rotate_all_right(b0, amounts[round][0][0], comp[round][0][0]);
        b1 = rotate_all_right(b1, amounts[round][0][1], comp[round][0][1]);
        b2 = rotate_all_right(b2, amounts[round][0][2], comp[round][0][2]);
        b3 = rotate_all_right(b3, amounts[round][0][3], comp[round][0][3]);

        transpose_avx2_regs(&a0, &a1, &a2, &a3);
        transpose_avx2_regs(&b0, &b1, &b2, &b3);

        a3 = _mm256_xor_si256(a3, xor_vec[round][1]);
        b3 = _mm256_xor_si256(b3, xor_vec[round][1]);

        a0 = rotate_all_right(a0, amounts[round][1][0], comp[round][1][0]);
        a1 = rotate_all_right(a1, amounts[round][1][1], comp[round][1][1]);
        a2 = rotate_all_right(a2, amounts[round][1][2], comp[round][1][2]);
        a3 = rotate_all_right(a3, amounts[round][1][3], comp[round][1][3]);

        b0 = rotate_all_right(b0, amounts[round][1][0], comp[round][1][0]);
        b1 = rotate_all_right(b1, amounts[round][1][1], comp[round][1][1]);
        b2 = rotate_all_right(b2, amounts[round][1][2], comp[round][1][2]);
        b3 = rotate_all_right(b3, amounts[round][1][3], comp[round][1][3]);
    }

    _mm256_store_si256((__m256i *)&a[0],  a0);
    _mm256_store_si256((__m256i *)&a[8],  a1);
    _mm256_store_si256((__m256i *)&a[16], a2);
    _mm256_store_si256((__m256i *)&a[24], a3);

    _mm256_store_si256((__m256i *)&b[0],  b0);
    _mm256_store_si256((__m256i *)&b[8],  b1);
    _mm256_store_si256((__m256i *)&b[16], b2);
    _mm256_store_si256((__m256i *)&b[24], b3);
}
#endif


int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s {encrypt|decrypt} <input_file> <key_file>\n", argv[0]);
        return 1;
    }

    const char *mode_str = argv[1];
    const char *infile = argv[2];
    const char *keyfile = argv[3];

    _Alignas(MATRIX_ALIGN) uint32_t key_matrix[32];

    uint64_t bytes=0;

    if (strcmp(mode_str, "encrypt") == 0) /* ENCRYPTION path */
    {
        char outfile[512];
        snprintf(outfile, sizeof(outfile), "%s.locked", infile);

        FILE *reader    = fopen(infile,    "rb");
        FILE *keyreader = fopen(keyfile,   "rb");

        if (reader == NULL) 
        {
            printf("\nError in opening/finding the file %s", infile);
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

        FILE *writer = fopen(outfile, "wb");

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

            size_t padded = ((bytes_read + 127) / 128) * 128;
            if (padded > bytes_read)
                memset(batch_buf + bytes_read, 0, padded - bytes_read);

            size_t num_blocks = padded / 128;
            size_t b = 0;

#if HAVE_AVX2
            for (; b + 1 < num_blocks; b += 2)
            {
                uint32_t *a = (uint32_t *)(batch_buf + b * 128);
                uint32_t *bb = (uint32_t *)(batch_buf + (b + 1) * 128);
                encrypt_two_blocks_avx2(a, bb, avx2_rot_amount[0], avx2_rot_comp[0], avx2_xor_vec[0]);
            }
#endif

            for (; b < num_blocks; b++)
            {
                uint32_t *state = (uint32_t *)(batch_buf + b * 128);

#if HAVE_AVX2
                encrypt_block_avx2(state, avx2_rot_amount[0], avx2_rot_comp[0], avx2_xor_vec[0]);
#else
                UNROLL_8
                for (int round = 0; round < 8; round++)
                {
                    UNROLL_32
                    for (int index = 0; index < 32; index++)
                    {
                        state[index] = rotate_right(state[index], scalar_rot_amount[0][round][0][index]);
                    }
                    state[31] ^= scalar_xor_key[0][round][0];
                    transpose_matrix(state);

                    UNROLL_32
                    for (int index = 0; index < 32; index++)
                    {
                        state[index] = rotate_right(state[index], scalar_rot_amount[0][round][1][index]);
                    }
                    state[31] ^= scalar_xor_key[0][round][1];
                    transpose_matrix(state);
                }
#endif
            }

                // Write padded size so encrypted file is always a multiple of 128
                fwrite(batch_buf, 1, padded, writer);
        }

        STOP_MEASURING(timer);

        aligned_free_wrapper(batch_buf);
        fclose(reader);
        fclose(writer);
        return 0;
    }
    else if (strcmp(mode_str, "decrypt") == 0) /* DECRYPTION path */
    {
        char outfile[512];
        snprintf(outfile, sizeof(outfile), "%s.unlocked", infile);

        FILE *reader    = fopen(infile,    "rb");
        FILE *keyreader = fopen(keyfile,   "rb");

        if (reader == NULL) 
        {
            printf("\nError in opening/finding the file %s", infile);
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

        FILE *writer = fopen(outfile, "wb");

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
            size_t bytes_read_orig = bytes_read;

            size_t padded = ((bytes_read + 127) / 128) * 128;
            if (padded > bytes_read)
                memset(batch_buf + bytes_read, 0, padded - bytes_read);

            size_t num_blocks = padded / 128;
            size_t b = 0;

#if HAVE_AVX2
            for (; b + 1 < num_blocks; b += 2)
            {
                uint32_t *a = (uint32_t *)(batch_buf + b * 128);
                uint32_t *bb = (uint32_t *)(batch_buf + (b + 1) * 128);
                decrypt_two_blocks_avx2(a, bb, avx2_rot_amount[1], avx2_rot_comp[1], avx2_xor_vec[1]);
            }
#endif

            for (; b < num_blocks; b++)
            {
                uint32_t *state = (uint32_t *)(batch_buf + b * 128);

#if HAVE_AVX2
                decrypt_block_avx2(state, avx2_rot_amount[1], avx2_rot_comp[1], avx2_xor_vec[1]);
#else
                UNROLL_8
                for (int round = 0; round < 8; round++)
                {
                    transpose_matrix(state);
                    state[31] ^= scalar_xor_key[1][round][0];
                    UNROLL_32
                    for (int index = 0; index < 32; index++)
                    {
                        state[index] = rotate_right(state[index], scalar_rot_amount[1][round][0][index]);
                    }

                    transpose_matrix(state);
                    state[31] ^= scalar_xor_key[1][round][1];
                    UNROLL_32
                    for (int index = 0; index < 32; index++)
                    {
                        state[index] = rotate_right(state[index], scalar_rot_amount[1][round][1][index]);
                    }
                }
#endif
            }

            fwrite(batch_buf, 1, bytes_read_orig, writer);
        }

        STOP_MEASURING(timer);

        aligned_free_wrapper(batch_buf);
        fclose(reader);
        fclose(writer);
        return 0;
    }
    else
    {
        fprintf(stderr, "Invalid mode: %s. Use 'encrypt' or 'decrypt'.\n", mode_str);
        return 1;
    }
}