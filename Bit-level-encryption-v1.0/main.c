#include "/home/strange/Documents/GitHub/PicoPerf/picoperf.h"
#include<time.h>
#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>

#include <stdint.h>

#define MATRIX_ALIGN 64  // cache line size on x86

// Aligned storage — define once, reuse forever, zero heap
__attribute__((always_inline))
static inline void transpose_matrix(uint32_t * __restrict__ m) {
    uint32_t t;

    // Pass 1: 16x16
    uint32_t mask = 0x0000FFFF;
    for (int j = 0; j < 16; j++) {
        t = ((m[j] >> 16) ^ m[j + 16]) & mask;
        m[j + 16] ^= t;
        m[j]      ^= (t << 16);
    }

    // Pass 2: 8x8
    mask = 0x00FF00FF;
    for (int j = 0; j < 32; j += 16)
        for (int i = j; i < j + 8; i++) {
            t = ((m[i] >> 8) ^ m[i + 8]) & mask;
            m[i + 8] ^= t;
            m[i]     ^= (t << 8);
        }

    // Pass 3: 4x4
    mask = 0x0F0F0F0F;
    for (int j = 0; j < 32; j += 8)
        for (int i = j; i < j + 4; i++) {
            t = ((m[i] >> 4) ^ m[i + 4]) & mask;
            m[i + 4] ^= t;
            m[i]     ^= (t << 4);
        }

    // Pass 4: 2x2
    mask = 0x33333333;
    for (int j = 0; j < 32; j += 4)
        for (int i = j; i < j + 2; i++) {
            t = ((m[i] >> 2) ^ m[i + 2]) & mask;
            m[i + 2] ^= t;
            m[i]     ^= (t << 2);
        }

    // Pass 5: 1x1
    mask = 0x55555555;
    for (int j = 0; j < 32; j += 2) {
        t = ((m[j] >> 1) ^ m[j + 1]) & mask;
        m[j + 1] ^= t;
        m[j]     ^= (t << 1);
    }
}

#if defined(__clang__)
    #define rotate_left(x,n) __builtin_rotateleft32((x),(n))
    #define rotate_right(x,n) __builtin_rotateright32((x),(n))
#elif defined(__GNUC__) && __GNUC__ >= 15
    #define rotate_left(x,n) __builtin_stdc_rotate_left((x),(n))
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

int main()
{
    char ch;
    printf("what do you want to do?\n1.Encrypt a file\n2.Decrypt a file\nenter your option's number:");
    scanf("%s",&ch);

    short int i=0,j=0,k=0,l=0,f=0,s=0,q=0,x=0,b,e,round=0,inp_data[1024],key_array[1024],data[32][32];
    // unsigned int row_data_bit[32],col_data_bit[32],key_bit_array[32],state_matrix[32];
    uint32_t state_matrix[32] __attribute__((aligned(MATRIX_ALIGN)));
    uint32_t key_matrix[32] __attribute__((aligned(MATRIX_ALIGN)));
    
    unsigned char buffer[1],inp_key_array[1024];
    unsigned char inputlist[128],binaryarray[1024];
    short int t=0,h,v,binstr[8];
    unsigned long long int bytes=0,bytecount;
    
    if(ch=='1') //ENCRYPTION
    {

        char *file,*lockedfile,*keyfile;
        // char file[100],lockedfile[100],keyfile[100];
        // printf("\nEnter the file's name which u wanna encrypt:\n");
        // scanf("%s",file);
        // printf("\nEnter keyfile's name:\n");
        // scanf("%s",keyfile);
        // printf("\nwhat do u want to call the encrypted file:\n");
        // scanf("%s",lockedfile);
        
        file = "/home/strange/Documents/GitHub/test.data\0";
        keyfile = "/home/strange/Documents/GitHub/test.key\0";
        lockedfile = "/home/strange/Documents/GitHub/test.locked\0";

        FILE *reader = fopen(file,"rb");
        FILE *keyreader = fopen(keyfile,"rb");
        if(reader==NULL){
            printf("\nError in opening/finding the file %s",file);
            return 0;
        }
        if(keyreader==NULL){
            printf("\nError in opening/finding the file %s",keyfile);
            return 0;
        }
        fseek(keyreader,0,SEEK_END);
        bytes=ftell(keyreader);
        fseek(keyreader,0,SEEK_SET);
        if(bytes<128){
            printf("\n%s is too small.For security reasons keyfile's size should be at least 128 bytes.",keyfile);
            return 0;
        }
        clock_t begin,end;
        begin = clock();
        double time_spent;
        printf("ENCRYPTION STARTED...\n");

        FILE *writer = fopen(lockedfile,"wb");


        t=0;
        fread(key_matrix,128,1,keyreader);
        fclose(keyreader);

        fseek(reader,0,SEEK_END);
        bytecount=ftell(reader);
        fseek(reader,0,SEEK_SET);
        
        h=bytecount%128;
        if(h==0)
        bytecount=bytecount;
        else
        {
            v=128-h;
            bytecount=bytecount+v;
        }
        
        start_measuring();
        
        while(fread(state_matrix,128,1,reader) == 1)
        {
            /*encryption stuff starts here*/
            t=0;
            
            int sub_key=0;
            /*core starts here */
            for(int round =0; round<8; round++)
            {
                for(int index=0; index<32; index= index +2)
                {
                    state_matrix[index] = rotate_left(state_matrix[index],(key_matrix[sub_key] + index) & 31);
                    
                    state_matrix[index+1] = rotate_right(state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                }

                sub_key++;
                
                state_matrix[31]=(state_matrix[31])^(key_matrix[sub_key]);
                
                sub_key++;

                transpose_matrix(state_matrix);

                for(int index=0; index<32; index= index +2)
                {
                    state_matrix[index] = rotate_right(state_matrix[index], (key_matrix[sub_key] + index ) & 31);
                    
                    state_matrix[index+1] = rotate_left(state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                }
                
                sub_key++;

                state_matrix[31]=(state_matrix[31])^(key_matrix[sub_key]);
                
                sub_key++;

                transpose_matrix(state_matrix);
            }
            //core ends here
            fwrite(state_matrix,128,1,writer);
            /*encryption stuff ends here*/
        }
        print_measured_results(stop_measuring());
        
        
        fclose(reader);
        fclose(writer);
        end = clock();
        time_spent = (double)(end - begin) / CLOCKS_PER_SEC;    
        // printf("\n\nEncryption of %llu bytes completed in %f seconds ",bytecount,time_spent);
        // printf("\n\nPress any key to exit\n");
        return 0;

    }
    else if (ch=='2') //DECRYPTION
    {

        char *lockedfile,*unlockedfile,*keyfile;
        // char lockedfile[100],unlockedfile[100],keyfile[100];
        // printf("\nEnter the file's name which u wanna decrypt:\n");
        // scanf("%s",lockedfile);
        // printf("\nEnter keyfile's name:\n");
        // scanf("%s",keyfile);
        // printf("\nwhat do u wanna name the decrypted file:\n");
        // scanf("%s",unlockedfile);
        // 
        lockedfile = "/home/strange/Documents/GitHub/test.locked\0";
        keyfile = "/home/strange/Documents/GitHub/test.key\0";
        unlockedfile = "/home/strange/Documents/GitHub/unlocked_test.data\0";

        FILE *reader = fopen(lockedfile,"rb");
        FILE *keyreader = fopen(keyfile,"rb");

        if(reader==NULL){
            printf("\nError in opening/finding the file %s",lockedfile);
            return 0;
        }
        if(keyreader==NULL){
            printf("\nError in opening/finding the file %s",keyfile);
            return 0;
        }
        fseek(keyreader,0,SEEK_END);
        bytes=ftell(keyreader);
        fseek(keyreader,0,SEEK_SET);
        if(bytes<128){
            printf("\n%s is too small.For security reasons keyfile's should be at least 128 bytes.",keyfile);
            return 0;
        }
        clock_t begin,end;
        begin = clock();
        double time_spent;
        printf("DECRYPTION STARTED....\n");

        FILE *writer = fopen(unlockedfile,"wb");

        fread(key_matrix,128,1,keyreader);
        fclose(keyreader);

        start_measuring();
        
        while(fread(state_matrix,128,1,reader) == 1)
        {
            /*decryption stuff starts here*/
            

            int sub_key=31;
            /*core starts here */
            for(int round = 0; round < 8 ; round++)
            {
                transpose_matrix(state_matrix);

                state_matrix[31]=(state_matrix[31])^(key_matrix[sub_key]);
                
                sub_key--;

                for(int index=0; index<32; index= index +2)
                {
                    state_matrix[index] = rotate_left(state_matrix[index], (key_matrix[sub_key] + index ) & 31);
                    
                    state_matrix[index+1] = rotate_right(state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                }

                transpose_matrix(state_matrix);

                sub_key--;
                
                state_matrix[31]=(state_matrix[31])^(key_matrix[sub_key]);
                
                sub_key--;
                
                for(int index=0; index<32; index= index +2)
                {
                    state_matrix[index] = rotate_right(state_matrix[index],(key_matrix[sub_key] + index) & 31);
                    
                    state_matrix[index+1] = rotate_left(state_matrix[index+1], (key_matrix[sub_key] + index + 1) & 31);
                }
                
                sub_key--;
            }
            //core ends here
            fwrite(state_matrix,128,1,writer);
            /*decryption stuff ends here*/
        }
        print_measured_results(stop_measuring());
        
        fclose(reader);
        fclose(writer);
        end = clock();
        time_spent = (double)(end - begin) / CLOCKS_PER_SEC;    
        // printf("\n\nDecryption of %llu bytes completed in %f seconds ",bytecount,time_spent);
        // printf("\n\nPress any key to exit\n");
        return 0;
    }
    else
    {
        printf("invalid input");
        return 0;
    }
    
}