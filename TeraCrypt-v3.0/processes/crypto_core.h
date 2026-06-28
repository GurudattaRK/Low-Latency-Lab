#ifndef CRYPTO_CORE_H
#define CRYPTO_CORE_H

#include <stdint.h>
#include <stddef.h>

#define BLOCK_SIZE 128
#define BLOCK_WORDS 32
#define NUM_ROUNDS 8

#if defined(__AVX2__)
#define HAVE_AVX2 1
#include <immintrin.h>
#else
#define HAVE_AVX2 0
#endif

typedef enum {
    MODE_ENCRYPT = 0,
    MODE_DECRYPT = 1
} OperationMode;

struct KeyTables {
    uint8_t scalar_rot_amount[2][8][2][32];
    uint32_t scalar_xor_key[2][8][2];
    
    #if HAVE_AVX2
    __m256i avx2_rot_amount[2][8][2][4];
    __m256i avx2_xor_vec[2][8][2];
    #endif
};

void precompute_key_tables(const uint32_t key_matrix[32], struct KeyTables* tables);

void process_region(uint8_t* data, size_t size, OperationMode mode, 
                   const struct KeyTables* tables);

#endif
