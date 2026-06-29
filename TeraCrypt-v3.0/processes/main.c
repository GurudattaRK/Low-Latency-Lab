#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
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

// Worker process - ONLY does computation, NO I/O
void worker_process(WorkerTask* task) {
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
}

static void dispatch_work(WorkerTask* tasks, int num_workers, uint8_t* buffer, 
                         size_t chunk_size, OperationMode mode, const struct KeyTables* key_tables) {
    if (chunk_size == 0) return;
    
    // Split on 128-byte block boundaries so no worker ever starts mid-block
    // and there are no unencrypted gaps between worker regions. Only complete
    // blocks are processed; any trailing < 128 bytes are passed through
    // unchanged (handled identically on encrypt and decrypt).
    size_t total_blocks = chunk_size / BLOCK_SIZE;
    size_t base_blocks  = total_blocks / (num_workers + 1);
    size_t extra_blocks = total_blocks % (num_workers + 1);
    size_t offset = 0;
    
    // Assign work to workers (block-aligned regions)
    for (int i = 0; i < num_workers; i++) {
        size_t blocks = base_blocks + ((size_t)i < extra_blocks ? 1 : 0);
        size_t sz = blocks * BLOCK_SIZE;
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
    
    // Parent processes the remaining complete blocks (the trailing
    // chunk_size % 128 tail bytes are intentionally left untouched).
    size_t parent_blocks = total_blocks - base_blocks * num_workers - extra_blocks;
    if (parent_blocks > 0) {
        process_region(buffer + offset, parent_blocks * BLOCK_SIZE, mode, key_tables);
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
    printf("TeraCrypt v3.0 - Process Pool Variant (3-Buffer Async I/O - CORRECTED)\n");
    printf("========================================================================\n\n");
    
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
    
    // Precompute key tables in SHARED MEMORY (so children can access)
    printf("Precomputing key tables in shared memory...\n");
    struct KeyTables* key_tables = (struct KeyTables*)mmap(NULL, sizeof(struct KeyTables),
                                                            PROT_READ | PROT_WRITE,
                                                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (key_tables == MAP_FAILED) {
        fprintf(stderr, "[ERROR] Failed to allocate shared memory for key tables\n");
        return EXIT_MEMORY_ALLOC_FAIL;
    }
    
    precompute_key_tables(key_matrix, key_tables);
    printf("Key tables precomputed\n\n");
    
    // Open files (PARENT ONLY)
    printf("Opening files...\n");
    int fd_in = open(input_file, O_RDONLY);
    if (fd_in < 0) {
        fprintf(stderr, "[ERROR] Cannot open input file: %s\n", input_file);
        munmap(key_tables, sizeof(struct KeyTables));
        return EXIT_FILE_NOT_FOUND;
    }
    
    struct stat st;
    if (fstat(fd_in, &st) != 0) {
        fprintf(stderr, "[ERROR] Cannot stat input file\n");
        close(fd_in);
        munmap(key_tables, sizeof(struct KeyTables));
        return EXIT_FILE_NOT_FOUND;
    }
    
    size_t file_size = st.st_size;
    printf("Input file size: %zu bytes (%.2f MB)\n\n", file_size, file_size / (1024.0 * 1024));
    
    int fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        fprintf(stderr, "[ERROR] Cannot create output file: %s\n", output_file);
        close(fd_in);
        munmap(key_tables, sizeof(struct KeyTables));
        return EXIT_FILE_NOT_FOUND;
    }
    
    // Allocate three SHARED buffers with huge page hint
    printf("Allocating %d SHARED buffers (%zu MB each)...\n", NUM_BUFFERS, buffer_size / (1024 * 1024));
    uint8_t* buffers[NUM_BUFFERS];
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = (uint8_t*)mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (buffers[i] == MAP_FAILED) {
            fprintf(stderr, "[ERROR] Failed to allocate shared buffer %d\n", i);
            for (int j = 0; j < i; j++) {
                munmap(buffers[j], buffer_size);
            }
            close(fd_in);
            close(fd_out);
            munmap(key_tables, sizeof(struct KeyTables));
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
        printf("File size below threshold (%llu MB), using single-core mode\n\n", 
               PARALLEL_THRESHOLD / (1024 * 1024));
        
        // Single-core processing
        struct timespec timer;
        start_measuring(&timer);
        
        size_t offset = 0;
        while (offset < file_size) {
            size_t to_read = (file_size - offset < buffer_size) ? (file_size - offset) : buffer_size;
            
            ssize_t nread = pread(fd_in, buffers[0], to_read, offset);
            if (nread <= 0) break;
            
            // Pad the final partial block with zeros to a full 128-byte block,
            // then encrypt everything. The padded bytes are persisted, so the
            // output is rounded up to a multiple of 128.
            size_t padded_size = (((size_t)nread + 127) / 128) * 128;
            if (padded_size > (size_t)nread) {
                memset(buffers[0] + nread, 0, padded_size - nread);
            }
            
            process_region(buffers[0], padded_size, mode, key_tables);
            
            ssize_t nwritten = pwrite(fd_out, buffers[0], padded_size, offset);
            if (nwritten != (ssize_t)padded_size) {
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
        munmap(key_tables, sizeof(struct KeyTables));
        
        printf("Done!\n");
        return EXIT_SUCCESS;
    }
    
    // Parallel mode with worker processes
    int num_workers = num_cores - 1;
    printf("Using parallel mode: %d workers + 1 parent = %d total cores\n", num_workers, num_cores);
    
    // Allocate worker tasks in SHARED MEMORY
    size_t tasks_size = num_workers * sizeof(WorkerTask);
    WorkerTask* tasks = (WorkerTask*)mmap(NULL, tasks_size, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (tasks == MAP_FAILED) {
        fprintf(stderr, "[ERROR] Failed to allocate shared memory for worker tasks\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            munmap(buffers[i], buffer_size);
        }
        close(fd_in);
        close(fd_out);
        munmap(key_tables, sizeof(struct KeyTables));
        return EXIT_MEMORY_ALLOC_FAIL;
    }
    
    memset(tasks, 0, tasks_size);
    
    // Initialize semaphores in shared memory
    for (int i = 0; i < num_workers; i++) {
        tasks[i].worker_id = i;
        tasks[i].core_id = i + 1;
        tasks[i].shutdown = 0;
        tasks[i].key_tables = key_tables;  // Shared key tables
        
        #if defined(__linux__)
        sem_init(&tasks[i].wake_sem, 1, 0);  // 1 = process-shared
        sem_init(&tasks[i].done_sem, 1, 0);
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
            munmap(tasks, tasks_size);
            for (int j = 0; j < NUM_BUFFERS; j++) {
                munmap(buffers[j], buffer_size);
            }
            close(fd_in);
            close(fd_out);
            munmap(key_tables, sizeof(struct KeyTables));
            return EXIT_WORKER_CRASH;
        }
        #endif
    }
    
    // Fork worker processes
    printf("\nForking worker processes...\n");
    pid_t* pids = (pid_t*)malloc(num_workers * sizeof(pid_t));
    
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[ERROR] Failed to fork worker process %d\n", i);
            free(pids);
            munmap(tasks, tasks_size);
            for (int j = 0; j < NUM_BUFFERS; j++) {
                munmap(buffers[j], buffer_size);
            }
            close(fd_in);
            close(fd_out);
            munmap(key_tables, sizeof(struct KeyTables));
            return EXIT_WORKER_CRASH;
        } else if (pid == 0) {
            // CHILD PROCESS - Close file descriptors immediately!
            close(fd_in);
            close(fd_out);
            
            // Child only does computation
            worker_process(&tasks[i]);
            exit(0);
        } else {
            // Parent - store PID
            pids[i] = pid;
            printf("  Worker %d forked (PID %d, pinned to core %d)\n", i, pid, i + 1);
        }
    }
    
    // Pin parent to core 0
    pin_to_core(0);
    
    printf("\nStarting three-buffer async I/O pipeline (PARENT ONLY)...\n\n");
    
    struct timespec timer;
    start_measuring(&timer);
    
    // Three-buffer pipeline state (PARENT ONLY)
    size_t file_read_offset = 0;
    size_t file_write_offset = 0;
    int buf_index = 0;
    size_t chunk_sizes[NUM_BUFFERS] = {0};
    size_t chunk_actual[NUM_BUFFERS] = {0};  // Actual bytes read (before padding)
    AioHandle pending_read, pending_write;
    int have_pending_read = 0, have_pending_write = 0;
    
    // Prime the pipeline: synchronous read of first chunk
    size_t first_chunk = (file_size < buffer_size) ? file_size : buffer_size;
    ssize_t nread = pread(fd_in, buffers[0], first_chunk, 0);
    if (nread > 0) {
        file_read_offset = nread;
        chunk_actual[0] = (size_t)nread;
        
        // Pad to 128-byte boundary
        size_t padded = ((nread + 127) / 128) * 128;
        if (padded > (size_t)nread) {
            memset(buffers[0] + nread, 0, padded - nread);
        }
        chunk_sizes[0] = padded;
    }
    
    while (file_write_offset < file_size) {
        // Step 1 & 2: Wait for this buffer's read to finish, then pad it.
        if (have_pending_read) {
            aio_wait(&pending_read);
            have_pending_read = 0;
            size_t actual = chunk_actual[buf_index];
            size_t padded = ((actual + 127) / 128) * 128;
            if (padded > actual) {
                memset(buffers[buf_index] + actual, 0, padded - actual);
            }
            chunk_sizes[buf_index] = padded;
        }
        
        // Step 3: Fire next read BEFORE compute so it overlaps the cipher.
        int next_buf = (buf_index + 1) % NUM_BUFFERS;
        if (file_read_offset < file_size) {
            size_t to_read = (file_size - file_read_offset < buffer_size) ? 
                            (file_size - file_read_offset) : buffer_size;
            aio_read_chunk(&pending_read, fd_in, buffers[next_buf], to_read, file_read_offset);
            have_pending_read = 1;
            chunk_actual[next_buf] = to_read;
            file_read_offset += to_read;
        } else {
            chunk_sizes[next_buf] = 0;
            chunk_actual[next_buf] = 0;
        }
        
        // Step 4: Compute current buffer (overlaps read of next, write of prev).
        // Process the full padded size so the final partial block is encrypted.
        if (chunk_sizes[buf_index] > 0) {
            dispatch_work(tasks, num_workers, buffers[buf_index], chunk_sizes[buf_index], mode, key_tables);
        }
        
        // Step 5: Wait for the previous write to finish.
        if (have_pending_write) {
            aio_wait(&pending_write);
            have_pending_write = 0;
        }
        
        // Step 6: Fire write of this buffer. Write the full padded size so the
        // zero-padded final block is persisted; the output file is rounded up
        // to a multiple of 128 bytes.
        if (chunk_sizes[buf_index] > 0) {
            size_t bytes_to_write = chunk_sizes[buf_index];
            aio_write_chunk(&pending_write, fd_out, buffers[buf_index], bytes_to_write, file_write_offset);
            have_pending_write = 1;
            file_write_offset += bytes_to_write;
        }
        
        buf_index = next_buf;
    }
    
    // Wait for final write
    if (have_pending_write) {
        aio_wait(&pending_write);
    }
    
    uint64_t elapsed_ns = stop_measuring(&timer);
    
    // Shutdown workers
    printf("\nShutting down workers...\n");
    for (int i = 0; i < num_workers; i++) {
        tasks[i].shutdown = 1;
        #if defined(__linux__)
        sem_post(&tasks[i].wake_sem);
        #elif defined(__APPLE__)
        sem_post(tasks[i].wake_sem);
        #endif
    }
    
    // Wait for all worker processes
    for (int i = 0; i < num_workers; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        printf("  Worker %d (PID %d) exited with code %d\n", i, pids[i], WEXITSTATUS(status));
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
    
    free(pids);
    munmap(tasks, tasks_size);
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        munmap(buffers[i], buffer_size);
    }
    
    close(fd_in);
    close(fd_out);
    munmap(key_tables, sizeof(struct KeyTables));
    
    printf("\nProcessing complete!\n");
    print_measured_results(elapsed_ns);
    
    double elapsed_s = elapsed_ns / 1e9;
    double throughput = (file_size / (1024.0 * 1024 * 1024)) / elapsed_s;
    printf("Throughput: %.2f GB/s\n", throughput);
    printf("Speedup: %.2fx vs single-core\n\n", throughput / 0.25);
    
    printf("Done!\n");
    return EXIT_SUCCESS;
}
