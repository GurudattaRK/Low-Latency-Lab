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

FORCE_INLINE void transpose_matrix_avx2(uint32_t * __restrict__ m)
{
    /*
     * Load the 32x32-bit matrix into four 256-bit AVX2 registers.
     * Each register holds 8 uint32_t values.
     *
     * r0 = rows 0..7
     * r1 = rows 8..15
     * r2 = rows 16..23
     * r3 = rows 24..31
     */
    __m256i r0 = _mm256_loadu_si256((__m256i*)&m[0]);
    __m256i r1 = _mm256_loadu_si256((__m256i*)&m[8]);
    __m256i r2 = _mm256_loadu_si256((__m256i*)&m[16]);
    __m256i r3 = _mm256_loadu_si256((__m256i*)&m[24]);

    __m256i mask, t;

    /*
     * Stage 1: swap the upper and lower 16 bits of each row.
     * This is the same butterfly operation the scalar code does,
     * just applied to four registers at once.
     */
    mask = _mm256_set1_epi32(0x0000FFFF);

    /* Compute the difference between the top 16 bits of r0 and r2. */
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r0, 16), r2), mask);
    r2 = _mm256_xor_si256(r2, t);
    r0 = _mm256_xor_si256(r0, _mm256_slli_epi32(t, 16));

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r1, 16), r3), mask);
    r3 = _mm256_xor_si256(r3, t);
    r1 = _mm256_xor_si256(r1, _mm256_slli_epi32(t, 16));

    /*
     * Stage 2: swap the upper and lower 8 bits of each element.
     * Rows that are 8 apart sit in neighbouring registers (r0/r1, r2/r3),
     * so we work within each pair.
     */
    mask = _mm256_set1_epi32(0x00FF00FF);

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r0, 8), r1), mask);
    r1 = _mm256_xor_si256(r1, t);
    r0 = _mm256_xor_si256(r0, _mm256_slli_epi32(t, 8));

    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r2, 8), r3), mask);
    r3 = _mm256_xor_si256(r3, t);
    r2 = _mm256_xor_si256(r2, _mm256_slli_epi32(t, 8));

    /*
     * Stage 3: swap the upper and lower 4 bits of each element.
     * Rows that are 4 apart live in the same register but in different
     * 128-bit lanes. We swap the lanes, apply the butterfly, then swap back.
     */
    mask = _mm256_set1_epi32(0x0F0F0F0F);

    /* 0x4E swaps the two 128-bit lanes of a 256-bit register. */
    __m256i r0p = _mm256_permute4x64_epi64(r0, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r0, 4), r0p), mask);
    /* Update the permuted copy and then fold it back into the original. */
    r0p = _mm256_xor_si256(r0p, t);
    r0  = _mm256_xor_si256(r0,  _mm256_slli_epi32(t, 4));
    /* Restore the original lane order. */
    r0  = _mm256_blend_epi32(r0, _mm256_permute4x64_epi64(r0p, 0x4E), 0xF0);

    __m256i r1p = _mm256_permute4x64_epi64(r1, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r1, 4), r1p), mask);
    r1p = _mm256_xor_si256(r1p, t);
    r1  = _mm256_xor_si256(r1,  _mm256_slli_epi32(t, 4));
    r1  = _mm256_blend_epi32(r1, _mm256_permute4x64_epi64(r1p, 0x4E), 0xF0);

    __m256i r2p = _mm256_permute4x64_epi64(r2, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r2, 4), r2p), mask);
    r2p = _mm256_xor_si256(r2p, t);
    r2  = _mm256_xor_si256(r2,  _mm256_slli_epi32(t, 4));
    r2  = _mm256_blend_epi32(r2, _mm256_permute4x64_epi64(r2p, 0x4E), 0xF0);

    __m256i r3p = _mm256_permute4x64_epi64(r3, 0x4E);
    t  = _mm256_and_si256(_mm256_xor_si256(_mm256_srli_epi32(r3, 4), r3p), mask);
    r3p = _mm256_xor_si256(r3p, t);
    r3  = _mm256_xor_si256(r3,  _mm256_slli_epi32(t, 4));
    r3  = _mm256_blend_epi32(r3, _mm256_permute4x64_epi64(r3p, 0x4E), 0xF0);

    /*
     * Stage 4: swap the upper and lower 2 bits of each element.
     * Rows that are 2 apart are 64 bits apart inside the same register.
     * We shuffle the 32-bit words to bring them next to each other,
     * apply the butterfly, and blend the result back.
     */
    mask = _mm256_set1_epi32(0x33333333);

    /*
     * 0x4E shuffles each 4-word group as [2,3,0,1].
     * 0x44 broadcasts the lower two words to the upper two.
     * 0xCC blends the updated odd rows back into the original register.
     */
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

    STAGE4_REG(r0);
    STAGE4_REG(r1);
    STAGE4_REG(r2);
    STAGE4_REG(r3);

    /*
     * Stage 5: swap the two adjacent bits inside each element.
     * Adjacent rows are paired inside each register.  We shuffle them
     * together, apply the butterfly, and blend back with 0xAA.
     */
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

    STAGE5_REG(r0);
    STAGE5_REG(r1);
    STAGE5_REG(r2);
    STAGE5_REG(r3);

    /*
     * Store the result back.  Unaligned stores are safe whether the input
     * is heap-allocated or on the stack, so they keep tests and callers
     * from crashing on alignment differences.
     */
    _mm256_storeu_si256((__m256i*)&m[0],  r0);
    _mm256_storeu_si256((__m256i*)&m[8],  r1);
    _mm256_storeu_si256((__m256i*)&m[16], r2);
    _mm256_storeu_si256((__m256i*)&m[24], r3);

    #undef STAGE4_REG
    #undef STAGE5_REG
}