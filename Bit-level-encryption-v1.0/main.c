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

    //for 16x sub matrix16
    uint32_t mask = 0x0000FFFF;
    for (int j = 0; j < 16; j++) 
    {
        t = ((m[j] >> 16) ^ m[j + 16]) & mask;
        m[j + 16] = m[j + 16] ^ t;
        m[j] = m[j] ^ (t << 16);
    }

    //for 8x8 sub matrix
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
    //for 4x4 sub matrix
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
    //for 2x2 sub matrix
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
    //for 1x1 sub matrix
    mask = 0x55555555;
    for (int j = 0; j < 32; j += 2) 
    {
        t = ((m[j] >> 1) ^ m[j + 1]) & mask;
        m[j + 1] = m[j + 1] ^ t;
        m[j] = m[j] ^ (t << 1);
    }
}


int main()
{
    char ch = '2';   /* profiling: always decrypt */
    /* printf("what do you want to do?\n1.Encrypt a file\n2.Decrypt a file\nenter your option's number:"); */
    /* scanf("%s", &ch); */

    _Alignas(MATRIX_ALIGN) uint32_t key_matrix[32];

    uint64_t bytes=0;

    /* Timer variable is declared right before each measured region. */

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
            printf("\n%s is too small. For security reasons keyfile's size should be at least 128 bytes.", keyfile);
            return 1;
        }

        printf("ENCRYPTION STARTED...\n");

        FILE *writer = fopen(lockedfile, "wb");

        fread(key_matrix, 128, 1, keyreader);
        fclose(keyreader);

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
            for (size_t b = 0; b < num_blocks; b++)
            {
                uint32_t *state_matrix = (uint32_t *)(batch_buf + b * 128);
                int sub_key = 0;

                for (int round = 0; round < 8; round++)
                {
                    for (int index = 0; index < 32; index += 2)
                    {
                        state_matrix[index]   = rotate_left (state_matrix[index],   (key_matrix[sub_key] + index)     & 31);
                        state_matrix[index+1] = rotate_right(state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                    }
                    sub_key++;

                    state_matrix[31] ^= key_matrix[sub_key];
                    sub_key++;

                    transpose_matrix(state_matrix);

                    for (int index = 0; index < 32; index += 2)
                    {
                        state_matrix[index]   = rotate_right(state_matrix[index],   (key_matrix[sub_key] + index)     & 31);
                        state_matrix[index+1] = rotate_left (state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                    }
                    sub_key++;

                    state_matrix[31] ^= key_matrix[sub_key];
                    sub_key++;

                    transpose_matrix(state_matrix);
                }
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
            printf("\n%s is too small. For security reasons keyfile's should be at least 128 bytes.", keyfile);
            return 1;
        }

        printf("DECRYPTION STARTED....\n");

        FILE *writer = fopen(unlockedfile, "wb");

        fread(key_matrix, 128, 1, keyreader);
        fclose(keyreader);

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
            for (size_t b = 0; b < num_blocks; b++)
            {
                uint32_t *state_matrix = (uint32_t *)(batch_buf + b * 128);
                int sub_key = 31;

                for (int round = 0; round < 8; round++)
                {
                    transpose_matrix(state_matrix);

                    state_matrix[31] ^= key_matrix[sub_key];
                    sub_key--;

                    for (int index = 0; index < 32; index += 2)
                    {
                        state_matrix[index]   = rotate_left (state_matrix[index],   (key_matrix[sub_key] + index)     & 31);
                        state_matrix[index+1] = rotate_right(state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                    }

                    transpose_matrix(state_matrix);

                    sub_key--;

                    state_matrix[31] ^= key_matrix[sub_key];
                    sub_key--;

                    for (int index = 0; index < 32; index += 2)
                    {
                        state_matrix[index]   = rotate_right(state_matrix[index],   (key_matrix[sub_key] + index)     & 31);
                        state_matrix[index+1] = rotate_left (state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                    }

                    sub_key--;
                }
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