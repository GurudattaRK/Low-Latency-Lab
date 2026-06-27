#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdalign.h>

#if defined(__APPLE__) && defined(__aarch64__)
    #define MATRIX_ALIGN 128    //Apple Silicon has 128-byte cache lines; everything else is 64
    
#else
    #define MATRIX_ALIGN 64
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
    #include "../../PicoPerf/picoperf.h"
    // picoperf exposes start_measuring() / stop_measuring() / print_measured_results()
    // with no arguments, defining a dummy type so the call sites below compile
    // identically regardless of which path is active
    typedef int perf_timer_t;
    #define START_MEASURING(t)   start_measuring()
    #define STOP_MEASURING(t)    print_measured_results(stop_measuring())
#else
    #include "perf_timer.h"
    #define START_MEASURING(t)   start_measuring(&(t))
    #define STOP_MEASURING(t)    print_measured_results(stop_measuring(&(t)))
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


__attribute__((always_inline))
static inline void transpose_matrix(uint32_t * __restrict__ m)
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
    char ch;
    printf("what do you want to do?\n1.Encrypt a file\n2.Decrypt a file\nenter your option's number:");
    scanf("%s", &ch);

    _Alignas(MATRIX_ALIGN) uint32_t state_matrix[32];
    _Alignas(MATRIX_ALIGN) uint32_t key_matrix[32];

    uint64_t bytes=0, bytecount;

    perf_timer_t timer;

    if (ch == '1') // ENCRYPTION
    {
        char *file, *lockedfile, *keyfile;

        file       = "../../test.data\0";
        keyfile    = "../../test.key\0";
        lockedfile = "../../test.locked\0";

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

        fseek(reader, 0, SEEK_END);
        bytecount = ftell(reader);
        fseek(reader, 0, SEEK_SET);

        bytes = bytecount % 128;
        if (bytes != 0)
            bytecount += (128 - bytes);

        START_MEASURING(timer);

        while (fread(state_matrix, 128, 1, reader) == 1)
        {
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

            fwrite(state_matrix, 128, 1, writer);
        }

        STOP_MEASURING(timer);

        fclose(reader);
        fclose(writer);
        return 0;
    }
    else if (ch == '2') // DECRYPTION
    {
        char *lockedfile, *unlockedfile, *keyfile;

        lockedfile   = "../../test.locked\0";
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

        START_MEASURING(timer);

        while (fread(state_matrix, 128, 1, reader) == 1)
        {
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

            fwrite(state_matrix, 128, 1, writer);
        }

        STOP_MEASURING(timer);

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