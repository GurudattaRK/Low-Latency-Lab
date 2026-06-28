#include "crypto_core.h"
#include <string.h>

#if HAVE_AVX2
#include "avx2.c"
#endif

#if defined(__GNUC__) && !defined(__clang__)
    #define UNROLL_8  _Pragma("GCC unroll 8")
    #define UNROLL_16 _Pragma("GCC unroll 16")
    #define UNROLL_32 _Pragma("GCC unroll 32")
#elif defined(__clang__)
    #define UNROLL_8  _Pragma("unroll 8")
    #define UNROLL_16 _Pragma("unroll 16")
    #define UNROLL_32 _Pragma("unroll 32")
#else
    #define UNROLL_8
    #define UNROLL_16
    #define UNROLL_32
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE __attribute__((always_inline)) static inline
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline static
#else
    #define FORCE_INLINE static inline
#endif

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
    uint32_t mask = 0x0000FFFF;
    for (int j = 0; j < 16; j++) 
    {
        t = ((m[j] >> 16) ^ m[j + 16]) & mask;
        m[j + 16] = m[j + 16] ^ t;
        m[j] = m[j] ^ (t << 16);
    }

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
    
    mask = 0x55555555;
    for (int j = 0; j < 32; j += 2) 
    {
        t = ((m[j] >> 1) ^ m[j + 1]) & mask;
        m[j + 1] = m[j + 1] ^ t;
        m[j] = m[j] ^ (t << 1);
    }
}

void precompute_key_tables(const uint32_t key_matrix[32], struct KeyTables* tables)
{
    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        int ke_rot0 = (int)key_matrix[4 * round + 0];
        int ke_xor0 = (int)key_matrix[4 * round + 1];
        int ke_rot1 = (int)key_matrix[4 * round + 2];
        int ke_xor1 = (int)key_matrix[4 * round + 3];

        int kd_xor0 = (int)key_matrix[31 - 4 * round];
        int kd_rot0 = (int)key_matrix[30 - 4 * round];
        int kd_xor1 = (int)key_matrix[29 - 4 * round];
        int kd_rot1 = (int)key_matrix[28 - 4 * round];

        tables->scalar_xor_key[0][round][0] = (uint32_t)ke_xor0;
        tables->scalar_xor_key[0][round][1] = (uint32_t)ke_xor1;
        tables->scalar_xor_key[1][round][0] = (uint32_t)kd_xor0;
        tables->scalar_xor_key[1][round][1] = (uint32_t)kd_xor1;

        UNROLL_32
        for (int i = 0; i < 32; i++)
        {
            int amt;
            amt = (ke_rot0 + i + (i & 1)) & 31;
            tables->scalar_rot_amount[0][round][0][i] = (uint8_t)amt;

            amt = (ke_rot1 + i + (i & 1)) & 31;
            tables->scalar_rot_amount[0][round][1][i] = (uint8_t)amt;

            amt = (kd_rot0 + i + (i & 1)) & 31;
            tables->scalar_rot_amount[1][round][0][i] = (uint8_t)amt;

            amt = (kd_rot1 + i + (i & 1)) & 31;
            tables->scalar_rot_amount[1][round][1][i] = (uint8_t)amt;
        }

#if HAVE_AVX2
        UNROLL_8
        for (int ymm = 0; ymm < 4; ymm++)
        {
            int base = ymm * 8;
            tables->avx2_rot_amount[0][round][0][ymm] = _mm256_set_epi32(
                tables->scalar_rot_amount[0][round][0][base + 7],
                tables->scalar_rot_amount[0][round][0][base + 6],
                tables->scalar_rot_amount[0][round][0][base + 5],
                tables->scalar_rot_amount[0][round][0][base + 4],
                tables->scalar_rot_amount[0][round][0][base + 3],
                tables->scalar_rot_amount[0][round][0][base + 2],
                tables->scalar_rot_amount[0][round][0][base + 1],
                tables->scalar_rot_amount[0][round][0][base + 0]);
            tables->avx2_rot_amount[0][round][1][ymm] = _mm256_set_epi32(
                tables->scalar_rot_amount[0][round][1][base + 7],
                tables->scalar_rot_amount[0][round][1][base + 6],
                tables->scalar_rot_amount[0][round][1][base + 5],
                tables->scalar_rot_amount[0][round][1][base + 4],
                tables->scalar_rot_amount[0][round][1][base + 3],
                tables->scalar_rot_amount[0][round][1][base + 2],
                tables->scalar_rot_amount[0][round][1][base + 1],
                tables->scalar_rot_amount[0][round][1][base + 0]);
            tables->avx2_rot_amount[1][round][0][ymm] = _mm256_set_epi32(
                tables->scalar_rot_amount[1][round][0][base + 7],
                tables->scalar_rot_amount[1][round][0][base + 6],
                tables->scalar_rot_amount[1][round][0][base + 5],
                tables->scalar_rot_amount[1][round][0][base + 4],
                tables->scalar_rot_amount[1][round][0][base + 3],
                tables->scalar_rot_amount[1][round][0][base + 2],
                tables->scalar_rot_amount[1][round][0][base + 1],
                tables->scalar_rot_amount[1][round][0][base + 0]);
            tables->avx2_rot_amount[1][round][1][ymm] = _mm256_set_epi32(
                tables->scalar_rot_amount[1][round][1][base + 7],
                tables->scalar_rot_amount[1][round][1][base + 6],
                tables->scalar_rot_amount[1][round][1][base + 5],
                tables->scalar_rot_amount[1][round][1][base + 4],
                tables->scalar_rot_amount[1][round][1][base + 3],
                tables->scalar_rot_amount[1][round][1][base + 2],
                tables->scalar_rot_amount[1][round][1][base + 1],
                tables->scalar_rot_amount[1][round][1][base + 0]);
        }

        tables->avx2_xor_vec[0][round][0] = _mm256_set_epi32(ke_xor0, 0, 0, 0, 0, 0, 0, 0);
        tables->avx2_xor_vec[0][round][1] = _mm256_set_epi32(ke_xor1, 0, 0, 0, 0, 0, 0, 0);
        tables->avx2_xor_vec[1][round][0] = _mm256_set_epi32(kd_xor0, 0, 0, 0, 0, 0, 0, 0);
        tables->avx2_xor_vec[1][round][1] = _mm256_set_epi32(kd_xor1, 0, 0, 0, 0, 0, 0, 0);
#endif
    }
}

#if HAVE_AVX2
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

    UNROLL_8
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

    UNROLL_8
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

FORCE_INLINE void encrypt_block_scalar(uint32_t *state,
                                        const uint8_t rot_amt[8][2][32],
                                        const uint32_t xor_key[8][2])
{
    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_left(state[i], rot_amt[round][0][i]);
            state[i+1] = rotate_right(state[i+1], rot_amt[round][0][i+1]);
        }
        state[31] ^= xor_key[round][0];
        transpose_matrix_scalar(state);

        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_right(state[i], rot_amt[round][1][i]);
            state[i+1] = rotate_left(state[i+1], rot_amt[round][1][i+1]);
        }
        state[31] ^= xor_key[round][1];
        transpose_matrix_scalar(state);
    }
}

FORCE_INLINE void decrypt_block_scalar(uint32_t *state,
                                        const uint8_t rot_amt[8][2][32],
                                        const uint32_t xor_key[8][2])
{
    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        transpose_matrix_scalar(state);
        state[31] ^= xor_key[round][0];
        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_left(state[i], rot_amt[round][0][i]);
            state[i+1] = rotate_right(state[i+1], rot_amt[round][0][i+1]);
        }

        transpose_matrix_scalar(state);
        state[31] ^= xor_key[round][1];
        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_right(state[i], rot_amt[round][1][i]);
            state[i+1] = rotate_left(state[i+1], rot_amt[round][1][i+1]);
        }
    }
}

void process_region(uint8_t* data, size_t size, OperationMode mode, 
                   const struct KeyTables* tables)
{
    size_t num_blocks = size / BLOCK_SIZE;
    uint32_t* blocks = (uint32_t*)data;

#if HAVE_AVX2
    size_t i = 0;
    for (; i + 1 < num_blocks; i += 2) {
        if (mode == MODE_ENCRYPT) {
            encrypt_two_blocks_avx2(&blocks[i * 32], &blocks[(i+1) * 32],
                                   tables->avx2_rot_amount[0], tables->avx2_xor_vec[0]);
        } else {
            decrypt_two_blocks_avx2(&blocks[i * 32], &blocks[(i+1) * 32],
                                   tables->avx2_rot_amount[1], tables->avx2_xor_vec[1]);
        }
    }
    
    if (i < num_blocks) {
        if (mode == MODE_ENCRYPT) {
            encrypt_block_scalar(&blocks[i * 32], 
                               tables->scalar_rot_amount[0], tables->scalar_xor_key[0]);
        } else {
            decrypt_block_scalar(&blocks[i * 32],
                               tables->scalar_rot_amount[1], tables->scalar_xor_key[1]);
        }
    }
#else
    for (size_t i = 0; i < num_blocks; i++) {
        if (mode == MODE_ENCRYPT) {
            encrypt_block_scalar(&blocks[i * 32],
                               tables->scalar_rot_amount[0], tables->scalar_xor_key[0]);
        } else {
            decrypt_block_scalar(&blocks[i * 32],
                               tables->scalar_rot_amount[1], tables->scalar_xor_key[1]);
        }
    }
#endif
}
