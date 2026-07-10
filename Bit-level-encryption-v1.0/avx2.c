#include <immintrin.h>
#include <stdint.h>

#ifndef FORCE_INLINE
    #if defined(__GNUC__) || defined(__clang__)
        #define FORCE_INLINE __attribute__((always_inline)) static inline
    #elif defined(_MSC_VER)
        #define FORCE_INLINE __forceinline static
    #else
        #define FORCE_INLINE static inline
    #endif
#endif

/* The whole 32x32 transpose done in-register across 4 ymm regs, no memory.
 * r0 holds rows 0..7, r1 rows 8..15, r2 16..23, r3 24..31 (8 uint32 each).
 * Stages 1-2 are easy (they stay inside a 32-bit lane). Stages 3-5 have to
 * hop across the 128-bit lane boundary, which is why the permute/blend dance
 * shows up - AVX2 can't shift across that boundary directly. */
FORCE_INLINE void transpose_avx2_regs(__m256i *r0, __m256i *r1, __m256i *r2, __m256i *r3)
{
    __m256i mask, t;

    /* stage 1: 16-bit swap */
    mask = _mm256_set1_epi32(0x0000FFFF);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r0, 16), *r2), mask);
    *r2 = _mm256_xor_si256(*r2, t);
    *r0 = _mm256_xor_si256(*r0, _mm256_slli_epi32(t, 16));

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r1, 16), *r3), mask);
    *r3 = _mm256_xor_si256(*r3, t);
    *r1 = _mm256_xor_si256(*r1, _mm256_slli_epi32(t, 16));

    /* stage 2: 8-bit */
    mask = _mm256_set1_epi32(0x00FF00FF);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r0, 8), *r1), mask);
    *r1 = _mm256_xor_si256(*r1, t);
    *r0 = _mm256_xor_si256(*r0, _mm256_slli_epi32(t, 8));

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(*r2, 8), *r3), mask);
    *r3 = _mm256_xor_si256(*r3, t);
    *r2 = _mm256_xor_si256(*r2, _mm256_slli_epi32(t, 8));

    /* stage 3: 4-bit - from here on we need permute4x64 to cross lanes */
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

    /* stage 4: 2-bit */
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

    /* stage 5: last one, single bits */
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

/* Convenience wrapper: pull the matrix in from memory, transpose, write back.
 * The scalar path uses this; the block functions keep everything in regs. */
FORCE_INLINE void transpose_matrix_avx2(uint32_t * __restrict__ m)
{
    __m256i r0 = _mm256_load_si256((__m256i*)&m[0]);
    __m256i r1 = _mm256_load_si256((__m256i*)&m[8]);
    __m256i r2 = _mm256_load_si256((__m256i*)&m[16]);
    __m256i r3 = _mm256_load_si256((__m256i*)&m[24]);

    transpose_avx2_regs(&r0, &r1, &r2, &r3);

    _mm256_store_si256((__m256i*)&m[0],  r0);
    _mm256_store_si256((__m256i*)&m[8],  r1);
    _mm256_store_si256((__m256i*)&m[16], r2);
    _mm256_store_si256((__m256i*)&m[24], r3);
}

/* Rotate each of the 8 lanes right by its own amount. AVX2 has no rotate
 * instruction, so it's the usual (x >> n) | (x << (32-n)). 'comp' is the
 * precomputed (32 - amount) so we don't recompute it every call. */
FORCE_INLINE __m256i rotate_all_right(__m256i x, __m256i amount, __m256i comp)
{
    return _mm256_or_si256(_mm256_srlv_epi32(x, amount), _mm256_sllv_epi32(x, comp));
}