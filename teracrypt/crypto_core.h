#ifndef CRYPTO_CORE_H
#define CRYPTO_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <immintrin.h>
#include <string.h>

#define BLOCK_SIZE 128
#define BLOCK_WORDS 32
#define NUM_ROUNDS 8

#define UNROLL_8  _Pragma("unroll 8")
#define UNROLL_16 _Pragma("unroll 16")
#define UNROLL_32 _Pragma("unroll 32")

#define FORCE_INLINE __attribute__((always_inline)) static inline

#define rotate_right(x,n) __builtin_rotateright32((x),(n))

typedef enum {
    MODE_ENCRYPT = 0,
    MODE_DECRYPT = 1
} OperationMode;

struct KeyTables {
    uint8_t scalar_rot_amount[2][8][2][32];
    uint32_t scalar_xor_key[2][8][2];
    __m256i avx2_rot_amount[2][8][2][4];
    __m256i avx2_rot_comp[2][8][2][4];
    __m256i avx2_xor_vec[2][8][2];
};

/* AVX2 primitives */

FORCE_INLINE void transpose_avx2_regs(__m256i *r0, __m256i *r1, __m256i *r2, __m256i *r3)
{
    __m256i mask, t;

    mask = _mm256_set1_epi32(0x0000FFFF);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r0, 16), *r2), mask);
    *r2 = _mm256_xor_si256(*r2, t);
    *r0 = _mm256_xor_si256(*r0, _mm256_slli_epi32(t, 16));

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r1, 16), *r3), mask);
    *r3 = _mm256_xor_si256(*r3, t);
    *r1 = _mm256_xor_si256(*r1, _mm256_slli_epi32(t, 16));

    mask = _mm256_set1_epi32(0x00FF00FF);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r0, 8), *r1), mask);
    *r1 = _mm256_xor_si256(*r1, t);
    *r0 = _mm256_xor_si256(*r0, _mm256_slli_epi32(t, 8));

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r2, 8), *r3), mask);
    *r3 = _mm256_xor_si256(*r3, t);
    *r2 = _mm256_xor_si256(*r2, _mm256_slli_epi32(t, 8));

    mask = _mm256_set1_epi32(0x0F0F0F0F);

    __m256i r0p = _mm256_permute4x64_epi64(*r0, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r0, 4), r0p), mask);
    r0p = _mm256_xor_si256(r0p, t);
    *r0 = _mm256_xor_si256(*r0,  _mm256_slli_epi32(t, 4));
    *r0 = _mm256_blend_epi32(*r0, _mm256_permute4x64_epi64(r0p, 0x4E), 0xF0);

    __m256i r1p = _mm256_permute4x64_epi64(*r1, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r1, 4), r1p), mask);
    r1p = _mm256_xor_si256(r1p, t);
    *r1 = _mm256_xor_si256(*r1,  _mm256_slli_epi32(t, 4));
    *r1 = _mm256_blend_epi32(*r1, _mm256_permute4x64_epi64(r1p, 0x4E), 0xF0);

    __m256i r2p = _mm256_permute4x64_epi64(*r2, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r2, 4), r2p), mask);
    r2p = _mm256_xor_si256(r2p, t);
    *r2 = _mm256_xor_si256(*r2,  _mm256_slli_epi32(t, 4));
    *r2 = _mm256_blend_epi32(*r2, _mm256_permute4x64_epi64(r2p, 0x4E), 0xF0);

    __m256i r3p = _mm256_permute4x64_epi64(*r3, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r3, 4), r3p), mask);
    r3p = _mm256_xor_si256(r3p, t);
    *r3 = _mm256_xor_si256(*r3,  _mm256_slli_epi32(t, 4));
    *r3 = _mm256_blend_epi32(*r3, _mm256_permute4x64_epi64(r3p, 0x4E), 0xF0);

    mask = _mm256_set1_epi32(0x33333333);

    #define STAGE4_REG(reg)                                                  \
    do {                                                                     \
        __m256i sh = _mm256_shuffle_epi32((reg), 0x4E);                     \
        t   = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32((reg),2), \
                                                 sh), mask);                 \
        __m256i tb = _mm256_shuffle_epi32(t, 0x44);                          \
        sh  = _mm256_xor_si256(sh, tb);                                      \
        (reg) = _mm256_xor_si256((reg), _mm256_slli_epi32(tb, 2));           \
        sh  = _mm256_shuffle_epi32(sh, 0x44);                                \
        (reg) = _mm256_blend_epi32((reg), sh, 0xCC);                        \
    } while(0)

    STAGE4_REG(*r0);
    STAGE4_REG(*r1);
    STAGE4_REG(*r2);
    STAGE4_REG(*r3);

    mask = _mm256_set1_epi32(0x55555555);

    #define STAGE5_REG(reg)                                                  \
    do {                                                                     \
        __m256i sh = _mm256_shuffle_epi32((reg), 0xB1);                     \
        t   = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32((reg),1), \
                                                 sh), mask);                 \
        __m256i tb = _mm256_shuffle_epi32(t, 0xA0);                          \
        sh  = _mm256_xor_si256(sh, tb);                                      \
        (reg) = _mm256_xor_si256((reg), _mm256_slli_epi32(tb, 1));           \
        sh  = _mm256_shuffle_epi32(sh, 0xA0);                                \
        (reg) = _mm256_blend_epi32((reg), sh, 0xAA);                        \
    } while(0)

    STAGE5_REG(*r0);
    STAGE5_REG(*r1);
    STAGE5_REG(*r2);
    STAGE5_REG(*r3);

    #undef STAGE4_REG
    #undef STAGE5_REG
}

FORCE_INLINE __m256i rotate_all_right(__m256i x, __m256i amount, __m256i comp)
{
    return _mm256_or_si256(_mm256_srlv_epi32(x, amount), _mm256_sllv_epi32(x, comp));
}

/* scalar transpose */

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

/* key table precomputation */

FORCE_INLINE void precompute_key_tables(const uint32_t key_matrix[32], struct KeyTables* tables)
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
            if (!(i & 1))
                amt = (32 - amt) & 31;
            tables->scalar_rot_amount[0][round][0][i] = (uint8_t)amt;

            amt = (ke_rot1 + i + (i & 1)) & 31;
            if (i & 1)
                amt = (32 - amt) & 31;
            tables->scalar_rot_amount[0][round][1][i] = (uint8_t)amt;

            amt = (kd_rot0 + i + (i & 1)) & 31;
            if (!(i & 1))
                amt = (32 - amt) & 31;
            tables->scalar_rot_amount[1][round][0][i] = (uint8_t)amt;

            amt = (kd_rot1 + i + (i & 1)) & 31;
            if (i & 1)
                amt = (32 - amt) & 31;
            tables->scalar_rot_amount[1][round][1][i] = (uint8_t)amt;
        }

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

        UNROLL_8
        for (int ymm = 0; ymm < 4; ymm++)
        {
            __m256i thirty_two = _mm256_set1_epi32(32);
            tables->avx2_rot_comp[0][round][0][ymm] = _mm256_sub_epi32(thirty_two, tables->avx2_rot_amount[0][round][0][ymm]);
            tables->avx2_rot_comp[0][round][1][ymm] = _mm256_sub_epi32(thirty_two, tables->avx2_rot_amount[0][round][1][ymm]);
            tables->avx2_rot_comp[1][round][0][ymm] = _mm256_sub_epi32(thirty_two, tables->avx2_rot_amount[1][round][0][ymm]);
            tables->avx2_rot_comp[1][round][1][ymm] = _mm256_sub_epi32(thirty_two, tables->avx2_rot_amount[1][round][1][ymm]);
        }

        tables->avx2_xor_vec[0][round][0] = _mm256_set_epi32(ke_xor0, 0, 0, 0, 0, 0, 0, 0);
        tables->avx2_xor_vec[0][round][1] = _mm256_set_epi32(ke_xor1, 0, 0, 0, 0, 0, 0, 0);
        tables->avx2_xor_vec[1][round][0] = _mm256_set_epi32(kd_xor0, 0, 0, 0, 0, 0, 0, 0);
        tables->avx2_xor_vec[1][round][1] = _mm256_set_epi32(kd_xor1, 0, 0, 0, 0, 0, 0, 0);
    }
}

/* AVX2 encrypt/decrypt two blocks in parallel for ILP */

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

/* scalar encrypt/decrypt single block */

FORCE_INLINE void encrypt_block_scalar(uint32_t *state,
                                        const uint8_t rot_amt[8][2][32],
                                        const uint32_t xor_key[8][2])
{
    UNROLL_8
    for (int round = 0; round < 8; round++)
    {
        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_right(state[i], rot_amt[round][0][i]);
            state[i+1] = rotate_right(state[i+1], rot_amt[round][0][i+1]);
        }
        state[31] ^= xor_key[round][0];
        transpose_matrix_scalar(state);

        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_right(state[i], rot_amt[round][1][i]);
            state[i+1] = rotate_right(state[i+1], rot_amt[round][1][i+1]);
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
            state[i] = rotate_right(state[i], rot_amt[round][0][i]);
            state[i+1] = rotate_right(state[i+1], rot_amt[round][0][i+1]);
        }

        transpose_matrix_scalar(state);
        state[31] ^= xor_key[round][1];
        UNROLL_16
        for (int i = 0; i < 32; i += 2) {
            state[i] = rotate_right(state[i], rot_amt[round][1][i]);
            state[i+1] = rotate_right(state[i+1], rot_amt[round][1][i+1]);
        }
    }
}

/* process_region: two blocks at a time for max ILP, scalar fallback for odd block */

FORCE_INLINE void process_region(uint8_t* data, size_t size, OperationMode mode,
                   const struct KeyTables* tables)
{
    size_t num_blocks = size / BLOCK_SIZE;
    uint32_t* blocks = (uint32_t*)data;

    if (mode == MODE_ENCRYPT) {
        size_t i = 0;
        for (; i + 1 < num_blocks; i += 2) {
            encrypt_two_blocks_avx2(&blocks[i * 32], &blocks[(i+1) * 32],
                                   tables->avx2_rot_amount[0], tables->avx2_rot_comp[0], tables->avx2_xor_vec[0]);
        }
        if (i < num_blocks) {
            encrypt_block_scalar(&blocks[i * 32],
                               tables->scalar_rot_amount[0], tables->scalar_xor_key[0]);
        }
    } else {
        size_t i = 0;
        for (; i + 1 < num_blocks; i += 2) {
            decrypt_two_blocks_avx2(&blocks[i * 32], &blocks[(i+1) * 32],
                                   tables->avx2_rot_amount[1], tables->avx2_rot_comp[1], tables->avx2_xor_vec[1]);
        }
        if (i < num_blocks) {
            decrypt_block_scalar(&blocks[i * 32],
                               tables->scalar_rot_amount[1], tables->scalar_xor_key[1]);
        }
    }
}

#endif
