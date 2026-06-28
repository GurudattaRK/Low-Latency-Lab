#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>

#include "platform.h"
#include "crypto_core.h"
#include "aio_wrapper.h"

#define USE_PICOPERF 0
#include "perf_timer.h"

#define EXIT_SUCCESS           0
#define EXIT_INVALID_ARGS      1
#define EXIT_FILE_NOT_FOUND    2
#define EXIT_MEMORY_ALLOC_FAIL 3
#define EXIT_WORKER_CRASH      4
#define EXIT_CORE_COUNT_FAIL   5
#define EXIT_KEY_FILE_ERROR    7

#define PARALLEL_THRESHOLD (128ULL * 1024 * 1024)
#define BUFFER_SIZE (512ULL * 1024 * 1024)  // 512 MiB per buffer
#define NUM_BUFFERS 3  // Three-buffer pipeline

typedef struct {
    uint8_t* buffer;
    size_t region_size;
    int worker_id;
    int core_id;
    OperationMode mode;
    const struct KeyTables* key_tables;
    
    #if defined(__linux__)
    sem_t wake_sem;
    sem_t done_sem;
    #elif defined(__APPLE__)
    sem_t* wake_sem;
    sem_t* done_sem;
    #endif
    
    volatile int shutdown;
} WorkerTask;

void* worker_thread(void* arg) {
    WorkerTask* task = (WorkerTask*)arg;
    
    pin_to_core(task->core_id);
    
    while (1) {
        #if defined(__linux__)
        sem_wait(&task->wake_sem);
        #elif defined(__APPLE__)
        sem_wait(task->wake_sem);
        #endif
        
        if (task->shutdown) {
            break;
        }
        
        if (task->region_size > 0) {
            process_region(task->buffer, task->region_size, task->mode, task->key_tables);
        }
        
        #if defined(__linux__)
        sem_post(&task->done_sem);
        #elif defined(__APPLE__)
        sem_post(task->done_sem);
        #endif
    }
    
    return NULL;
}

static void dispatch_work(WorkerTask* tasks, int num_workers, uint8_t* buffer, 
                         size_t chunk_size, OperationMode mode, const struct KeyTables* key_tables) {
    if (chunk_size == 0) return;
    
    size_t base_size = chunk_size / (num_workers + 1);
    size_t remainder = chunk_size % (num_workers + 1);
    size_t offset = 0;
    
    // Assign work to workers
    for (int i = 0; i < num_workers; i++) {
        size_t sz = base_size + (i < (int)remainder ? 1 : 0);
        tasks[i].buffer = buffer + offset;
        tasks[i].region_size = sz;
        tasks[i].mode = mode;
        tasks[i].key_tables = key_tables;
        
        #if defined(__linux__)
        sem_post(&tasks[i].wake_sem);
        #elif defined(__APPLE__)
        sem_post(tasks[i].wake_sem);
        #endif
        
        offset += sz;
    }
    
    // Main thread processes its chunk
    if (offset < chunk_size) {
        process_region(buffer + offset, chunk_size - offset, mode, key_tables);
    }
    
    // Wait for all workers
    for (int i = 0; i < num_workers; i++) {
        #if defined(__linux__)
        sem_wait(&tasks[i].done_sem);
        #elif defined(__APPLE__)
        sem_wait(tasks[i].done_sem);
        #endif
    }
}

int main(int argc, char** argv) {
    printf("TeraCrypt v3.0 - Thread Pool Variant (3-Buffer Async I/O)\n");
    printf("==========================================================\n\n");
    
    if (argc != 5) {
        fprintf(stderr, "Usage: %s {encrypt|decrypt} <input> <output> <keyfile>\n", argv[0]);
        fprintf(stderr, "Example: %s encrypt input.dat output.enc key.bin\n", argv[0]);
        return EXIT_INVALID_ARGS;
    }
    
    OperationMode mode;
    if (strcmp(argv[1], "encrypt") == 0) {
        mode = MODE_ENCRYPT;
        printf("Mode: ENCRYPTION\n");
    } else if (strcmp(argv[1], "decrypt") == 0) {
        mode = MODE_DECRYPT;
        printf("Mode: DECRYPTION\n");
    } else {
        fprintf(stderr, "[ERROR] Invalid mode '%s'. Use 'encrypt' or 'decrypt'.\n", argv[1]);
        return EXIT_INVALID_ARGS;
    }
    
    const char* input_file = argv[2];
    const char* output_file = argv[3];
    const char* key_file = argv[4];
    
    printf("Input:  %s\n", input_file);
    printf("Output: %s\n", output_file);
    printf("Key:    %s\n\n", key_file);
    
    // Get core count
    int num_cores = get_core_count();
    if (num_cores <= 0) {
        fprintf(stderr, "[ERROR] Failed to detect CPU cores\n");
        return EXIT_CORE_COUNT_FAIL;
    }
    printf("Detected %d physical CPU cores\n", num_cores);
    
    // Fixed buffer size: 512 MiB per buffer, 3 buffers = 1.5 GB total
    size_t buffer_size = BUFFER_SIZE;
    printf("Buffer size: %zu MB (per buffer, 3 buffers total = %.2f GB)\n\n", 
           buffer_size / (1024 * 1024), (buffer_size * NUM_BUFFERS) / (1024.0 * 1024 * 1024));
    
    // Read key file
    printf("Reading key file...\n");
    FILE* fkey = fopen(key_file, "rb");
    if (!fkey) {
        fprintf(stderr, "[ERROR] Cannot open key file: %s\n", key_file);
        return EXIT_KEY_FILE_ERROR;
    }
    
    uint32_t key_matrix[32];
    size_t key_read = fread(key_matrix, 1, 128, fkey);
    fclose(fkey);
    
    if (key_read < 128) {
        fprintf(stderr, "[ERROR] Key file must be at least 128 bytes\n");
        return EXIT_KEY_FILE_ERROR;
    }
    printf("Key loaded successfully (%zu bytes)\n\n", key_read);
    
    // Precompute key tables
    printf("Precomputing key tables...\n");
    struct KeyTables* key_tables;
    
    #if defined(_WIN32)
    key_tables = (struct KeyTables*)_aligned_malloc(sizeof(struct KeyTables), 32);
    #else
    if (posix_memalign((void**)&key_tables, 32, sizeof(struct KeyTables)) != 0) {
        fprintf(stderr, "[ERROR] Failed to allocate aligned memory for key tables\n");
        return EXIT_MEMORY_ALLOC_FAIL;
    }
    #endif
    
    if (!key_tables) {
        fprintf(stderr, "[ERROR] Failed to allocate key tables\n");
        return EXIT_MEMORY_ALLOC_FAIL;
    }
    
    precompute_key_tables(key_matrix, key_tables);
    printf("Key tables precomputed\n\n");
    
    // Open files
    printf("Opening files...\n");
    int fd_in = open(input_file, O_RDONLY);
    if (fd_in < 0) {
        fprintf(stderr, "[ERROR] Cannot open input file: %s\n", input_file);
        #if defined(_WIN32)
        _aligned_free(key_tables);
        #else
        free(key_tables);
        #endif
        return EXIT_FILE_NOT_FOUND;
    }
    
    struct stat st;
    if (fstat(fd_in, &st) != 0) {
        fprintf(stderr, "[ERROR] Cannot stat input file\n");
        close(fd_in);
        #if defined(_WIN32)
        _aligned_free(key_tables);
        #else
        free(key_tables);
        #endif
        return EXIT_FILE_NOT_FOUND;
    }
    
    size_t file_size = st.st_size;
    printf("Input file size: %zu bytes (%.2f MB)\n\n", file_size, file_size / (1024.0 * 1024));
    
    int fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        fprintf(stderr, "[ERROR] Cannot create output file: %s\n", output_file);
        close(fd_in);
        #if defined(_WIN32)
        _aligned_free(key_tables);
        #else
        free(key_tables);
        #endif
        return EXIT_FILE_NOT_FOUND;
    }
    
    // Allocate three buffers with huge page hint
    printf("Allocating %d buffers (%zu MB each)...\n", NUM_BUFFERS, buffer_size / (1024 * 1024));
    uint8_t* buffers[NUM_BUFFERS];
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = (uint8_t*)mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buffers[i] == MAP_FAILED) {
            fprintf(stderr, "[ERROR] Failed to allocate buffer %d\n", i);
            for (int j = 0; j < i; j++) {
                munmap(buffers[j], buffer_size);
            }
            close(fd_in);
            close(fd_out);
            #if defined(_WIN32)
            _aligned_free(key_tables);
            #else
            free(key_tables);
            #endif
            return EXIT_MEMORY_ALLOC_FAIL;
        }
        
        // Hint for huge pages
        #if defined(__linux__)
        madvise(buffers[i], buffer_size, MADV_HUGEPAGE);
        #endif
    }
    printf("Buffers allocated successfully\n\n");
    
    // Check if we should use parallel mode
    if (file_size < PARALLEL_THRESHOLD) {
        printf("File size below threshold (%zu MB), using single-core mode\n\n", 
               PARALLEL_THRESHOLD / (1024 * 1024));
        
        // Single-core processing (simplified, reuse buffer[0])
        struct timespec timer;
        start_measuring(&timer);
        
        size_t offset = 0;
        while (offset < file_size) {
            size_t to_read = (file_size - offset < buffer_size) ? (file_size - offset) : buffer_size;
            
            ssize_t nread = pread(fd_in, buffers[0], to_read, offset);
            if (nread <= 0) break;
            
            // Pad to 128-byte boundary
            size_t padded_size = ((nread + 127) / 128) * 128;
            if (padded_size > nread) {
                memset(buffers[0] + nread, 0, padded_size - nread);
            }
            
            process_region(buffers[0], padded_size, mode, key_tables);
            
            ssize_t nwritten = pwrite(fd_out, buffers[0], nread, offset);
            if (nwritten != nread) {
                fprintf(stderr, "[ERROR] Write failed\n");
                break;
            }
            
            offset += nread;
        }
        
        uint64_t elapsed_ns = stop_measuring(&timer);
        print_measured_results(elapsed_ns);
        
        double elapsed_s = elapsed_ns / 1e9;
        double throughput = (file_size / (1024.0 * 1024 * 1024)) / elapsed_s;
        printf("Throughput: %.2f GB/s\n\n", throughput);
        
        for (int i = 0; i < NUM_BUFFERS; i++) {
            munmap(buffers[i], buffer_size);
        }
        close(fd_in);
        close(fd_out);
        #if defined(_WIN32)
        _aligned_free(key_tables);
        #else
        free(key_tables);
        #endif
        
        printf("Done!\n");
        return EXIT_SUCCESS;
    }
    
    // Parallel mode with worker pool
    int num_workers = num_cores - 1;
    printf("Using parallel mode: %d workers + 1 parent = %d total cores\n", num_workers, num_cores);
    
    // Allocate worker tasks
    WorkerTask* tasks = (WorkerTask*)calloc(num_workers, sizeof(WorkerTask));
    if (!tasks) {
        fprintf(stderr, "[ERROR] Failed to allocate worker tasks\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            munmap(buffers[i], buffer_size);
        }
        close(fd_in);
        close(fd_out);
        #if defined(_WIN32)
        _aligned_free(key_tables);
        #else
        free(key_tables);
        #endif
        return EXIT_MEMORY_ALLOC_FAIL;
    }
    
    // Initialize semaphores
    for (int i = 0; i < num_workers; i++) {
        tasks[i].worker_id = i;
        tasks[i].core_id = i + 1;
        tasks[i].shutdown = 0;
        
        #if defined(__linux__)
        sem_init(&tasks[i].wake_sem, 0, 0);
        sem_init(&tasks[i].done_sem, 0, 0);
        #elif defined(__APPLE__)
        char wake_name[64], done_name[64];
        snprintf(wake_name, sizeof(wake_name), "/tc_wake_%d_%d", getpid(), i);
        snprintf(done_name, sizeof(done_name), "/tc_done_%d_%d", getpid(), i);
        
        sem_unlink(wake_name);
        sem_unlink(done_name);
        
        tasks[i].wake_sem = sem_open(wake_name, O_CREAT | O_EXCL, 0600, 0);
        tasks[i].done_sem = sem_open(done_name, O_CREAT | O_EXCL, 0600, 0);
        
        if (tasks[i].wake_sem == SEM_FAILED || tasks[i].done_sem == SEM_FAILED) {
            fprintf(stderr, "[ERROR] Failed to create semaphores for worker %d\n", i);
            free(tasks);
            for (int j = 0; j < NUM_BUFFERS; j++) {
                munmap(buffers[j], buffer_size);
            }
            close(fd_in);
            close(fd_out);
            #if defined(_WIN32)
            _aligned_free(key_tables);
            #else
            free(key_tables);
            #endif
            return EXIT_WORKER_CRASH;
        }
        #endif
    }
    
    // Create worker threads
    pthread_t* threads = (pthread_t*)malloc(num_workers * sizeof(pthread_t));
    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, &tasks[i]) != 0) {
            fprintf(stderr, "[ERROR] Failed to create worker thread %d\n", i);
            free(threads);
            free(tasks);
            for (int j = 0; j < NUM_BUFFERS; j++) {
                munmap(buffers[j], buffer_size);
            }
            close(fd_in);
            close(fd_out);
            #if defined(_WIN32)
            _aligned_free(key_tables);
            #else
            free(key_tables);
            #endif
            return EXIT_WORKER_CRASH;
        }
    }
    
    // Pin main thread to core 0
    pin_to_core(0);
    
    printf("\nStarting three-buffer async I/O pipeline...\n\n");
    
    struct timespec timer;
    start_measuring(&timer);
    
    // Three-buffer pipeline state
    size_t file_read_offset = 0;
    size_t file_write_offset = 0;
    int buf_index = 0;
    size_t chunk_sizes[NUM_BUFFERS] = {0};
    AioHandle pending_read, pending_write;
    int have_pending_read = 0, have_pending_write = 0;
    (void)have_pending_read;  // May be unused in some code paths
    
    // Prime the pipeline: synchronous read of first chunk
    size_t first_chunk = (file_size < buffer_size) ? file_size : buffer_size;
    ssize_t nread = pread(fd_in, buffers[0], first_chunk, 0);
    if (nread > 0) {
        chunk_sizes[0] = nread;
        file_read_offset = nread;
        
        // Pad to 128-byte boundary
        size_t padded = ((nread + 127) / 128) * 128;
        if (padded > (size_t)nread) {
            memset(buffers[0] + nread, 0, padded - nread);
            chunk_sizes[0] = padded;
        }
    }
    
    int iteration = 0;
    while (file_write_offset < file_size) {
        iteration++;
        
        // Process current buffer
        if (chunk_sizes[buf_index] > 0) {
            dispatch_work(tasks, num_workers, buffers[buf_index], chunk_sizes[buf_index], mode, key_tables);
        }
        
        // Wait for pending write to complete (if any)
        if (have_pending_write) {
            aio_wait(&pending_write);
            have_pending_write = 0;
        }
        
        // Start async write of processed buffer
        if (chunk_sizes[buf_index] > 0) {
            // Calculate actual bytes to write (remove padding)
            size_t bytes_to_write = chunk_sizes[buf_index];
            if (file_write_offset + bytes_to_write > file_size) {
                bytes_to_write = file_size - file_write_offset;
            }
            
            pending_write = aio_write_chunk(fd_out, buffers[buf_index], bytes_to_write, file_write_offset);
            have_pending_write = 1;
            file_write_offset += bytes_to_write;
        }
        
        // Move to next buffer
        buf_index = (buf_index + 1) % NUM_BUFFERS;
        
        // Wait for pending read to complete (if any)
        if (have_pending_read) {
            aio_wait(&pending_read);
            have_pending_read = 0;
        }
        
        // Start async read into next buffer (if more data)
        if (file_read_offset < file_size) {
            size_t to_read = (file_size - file_read_offset < buffer_size) ? 
                            (file_size - file_read_offset) : buffer_size;
            
            pending_read = aio_read_chunk(fd_in, buffers[buf_index], to_read, file_read_offset);
            have_pending_read = 1;
            
            // Will need to pad after read completes
            chunk_sizes[buf_index] = to_read;
            file_read_offset += to_read;
        } else {
            chunk_sizes[buf_index] = 0;
        }
    }
    
    // Wait for final write
    if (have_pending_write) {
        aio_wait(&pending_write);
    }
    
    uint64_t elapsed_ns = stop_measuring(&timer);
    
    // Shutdown workers
    for (int i = 0; i < num_workers; i++) {
        tasks[i].shutdown = 1;
        #if defined(__linux__)
        sem_post(&tasks[i].wake_sem);
        #elif defined(__APPLE__)
        sem_post(tasks[i].wake_sem);
        #endif
    }
    
    for (int i = 0; i < num_workers; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Cleanup
    for (int i = 0; i < num_workers; i++) {
        #if defined(__linux__)
        sem_destroy(&tasks[i].wake_sem);
        sem_destroy(&tasks[i].done_sem);
        #elif defined(__APPLE__)
        char wake_name[64], done_name[64];
        snprintf(wake_name, sizeof(wake_name), "/tc_wake_%d_%d", getpid(), i);
        snprintf(done_name, sizeof(done_name), "/tc_done_%d_%d", getpid(), i);
        
        sem_close(tasks[i].wake_sem);
        sem_close(tasks[i].done_sem);
        sem_unlink(wake_name);
        sem_unlink(done_name);
        #endif
    }
    
    free(threads);
    free(tasks);
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        munmap(buffers[i], buffer_size);
    }
    
    close(fd_in);
    close(fd_out);
    
    #if defined(_WIN32)
    _aligned_free(key_tables);
    #else
    free(key_tables);
    #endif
    
    printf("\nProcessing complete!\n");
    print_measured_results(elapsed_ns);
    
    double elapsed_s = elapsed_ns / 1e9;
    double throughput = (file_size / (1024.0 * 1024 * 1024)) / elapsed_s;
    printf("Throughput: %.2f GB/s\n", throughput);
    printf("Speedup: %.2fx vs single-core\n\n", throughput / 0.25);
    
    printf("Done!\n");
    return EXIT_SUCCESS;
}
