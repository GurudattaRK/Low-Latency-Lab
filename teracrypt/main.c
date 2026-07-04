#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>
#include <assert.h>
#include <pthread.h>

#include "crypto_core.h"

#include <liburing.h>

/* ---- constants ---- */
#define CACHE_LINE_SIZE 64
#define DIRECT_IO_ALIGN 4096
#define PAGE_SIZE 4096

#define EXIT_SUCCESS           0
#define EXIT_INVALID_ARGS      1
#define EXIT_FILE_NOT_FOUND    2
#define EXIT_MEMORY_ALLOC_FAIL 3
#define EXIT_WORKER_CRASH      4
#define EXIT_CORE_COUNT_FAIL   5
#define EXIT_IO_ERROR          6
#define EXIT_KEY_FILE_ERROR    7

#define CHUNK_SIZE                 (8ULL * 1024 * 1024)
#define MAX_INFLIGHT_READS         16
#define MAX_INFLIGHT_WRITES        16
#define URING_QUEUE_DEPTH          64
#define CRASH_CHECK_EVERY_N_ITERS  200000

#define HUGEPAGE_SIZE (2ULL * 1024 * 1024)

/* ---- PGO worker termination ---- */
#if defined(__clang__) && defined(PGO_INSTRUMENTED)
static void worker_sigterm_handler(int sig) {
    (void)sig;
    _exit(0);
}
#define WORKER_TERM_SIGNAL SIGTERM
#else
#define WORKER_TERM_SIGNAL SIGKILL
#endif

/* io_uring user_data: (op << 32) | slot_idx */
typedef enum { OP_READ = 0, OP_WRITE = 1, OP_TAIL_READ = 2, OP_TAIL_WRITE = 3 } OpType;

__attribute__((always_inline)) static inline uint64_t pack_ud(OpType op, uint32_t slot) {
    return ((uint64_t)op << 32) | (uint64_t)slot;
}
__attribute__((always_inline)) static inline OpType unpack_op(uint64_t ud)     { return (OpType)(ud >> 32); }
__attribute__((always_inline)) static inline uint32_t unpack_slot(uint64_t ud) { return (uint32_t)(ud & 0xFFFFFFFFu); }

__attribute__((always_inline)) static inline uint32_t next_pow2(uint32_t v) {
    if (v < 2) return 2;
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* ---- perf timer (inlined) ---- */
__attribute__((always_inline)) static inline void start_measuring(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

__attribute__((always_inline)) static inline uint64_t stop_measuring(struct timespec *start)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (uint64_t)(end.tv_sec - start->tv_sec) * 1000000000ULL
         + (uint64_t)(end.tv_nsec - start->tv_nsec);
}

__attribute__((always_inline)) static inline void print_measured_results(uint64_t elapsed_ns)
{
    printf("Elapsed: %.3f ms (%.6f s)\n", elapsed_ns / 1e6, elapsed_ns / 1e9);
}

/* ---- platform functions (inlined) ---- */
__attribute__((always_inline)) static inline int get_core_count(void) {
    int max_cores = 1024;
    int* core_ids = calloc(max_cores, sizeof(int));
    if (!core_ids) return 1;

    int unique_count = 0;
    int consecutive_failures = 0;

    for (int cpu = 0; cpu < max_cores; cpu++) {
        char path[256];
        snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);

        FILE* fp = fopen(path, "r");
        if (!fp) {
            consecutive_failures++;
            if (consecutive_failures > 32) break;
            continue;
        }

        consecutive_failures = 0;
        int core_id;
        if (fscanf(fp, "%d", &core_id) == 1) {
            int found = 0;
            for (int i = 0; i < unique_count; i++) {
                if (core_ids[i] == core_id) { found = 1; break; }
            }
            if (!found) core_ids[unique_count++] = core_id;
        }
        fclose(fp);
    }

    free(core_ids);
    return (unique_count > 0) ? unique_count : 1;
}

__attribute__((always_inline)) static inline int get_nth_online_cpu(int n)
{
    static int online_cpus[1024];
    static int num_online = 0;
    static int initialized = 0;
    if (!initialized) {
        FILE* fp = fopen("/sys/devices/system/cpu/online", "r");
        if (fp) {
            char buf[256];
            if (fgets(buf, sizeof(buf), fp)) {
                char* p = buf;
                while (*p) {
                    int start = -1, end = -1;
                    if (sscanf(p, "%d-%d", &start, &end) == 2 && start >= 0 && end >= start) {
                        for (int i = start; i <= end && num_online < 1024; i++)
                            online_cpus[num_online++] = i;
                    } else if (sscanf(p, "%d", &start) == 1 && start >= 0 && num_online < 1024) {
                        online_cpus[num_online++] = start;
                    }
                    while (*p && *p != ',') p++;
                    if (*p == ',') p++;
                }
            }
            fclose(fp);
        }
        initialized = 1;
    }
    if (num_online == 0) return n;
    if (n < 0 || n >= num_online) return online_cpus[n % num_online];
    return online_cpus[n];
}

__attribute__((always_inline)) static inline void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

__attribute__((always_inline)) static inline size_t get_available_ram(void) {
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0)
        return (size_t)pages * (size_t)page_size;
    return 2ULL * 1024 * 1024 * 1024;
}

__attribute__((always_inline)) static inline void* allocate_aligned_buffer(size_t size) {
    void* ptr = NULL;
    posix_memalign(&ptr, PAGE_SIZE, size);
    return ptr;
}

/* ---- shared memory allocation ---- */
typedef struct {
    void*  base;
    void*  aligned;
    size_t alloc_size;
} SharedMem;

__attribute__((always_inline)) static inline SharedMem allocate_shared_mem(size_t size, size_t align)
{
    SharedMem mem = {NULL, NULL, 0};
    if (align < 64) align = 64;

    size_t alloc_size = size + align;
    void* base = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return mem;

    uintptr_t addr = (uintptr_t)base;
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);

    mem.base = base;
    mem.aligned = (void*)aligned;
    mem.alloc_size = alloc_size;
    return mem;
}

/* ---- chunk pool (inlined) ---- */
typedef enum {
    SLOT_FREE = 0,
    SLOT_READ_PENDING,
    SLOT_READY,
    SLOT_PROCESSING,
    SLOT_WRITE_READY,
    SLOT_WRITE_PENDING
} SlotState;

typedef struct {
    uint8_t*     buf;
    size_t       file_offset;
    size_t       actual_size;
    size_t       padded_size;
    _Atomic int  state;
    char         _pad[CACHE_LINE_SIZE - (sizeof(uint8_t*) + 3 * sizeof(size_t) + sizeof(_Atomic int))];
} ChunkSlot __attribute__((aligned(CACHE_LINE_SIZE)));

__attribute__((always_inline)) static inline void chunk_pool_init(ChunkSlot* slots, uint8_t* base_buffer,
                                    uint32_t total_slots, size_t chunk_size)
{
    for (uint32_t i = 0; i < total_slots; i++) {
        slots[i].buf         = base_buffer + (size_t)i * chunk_size;
        slots[i].file_offset = 0;
        slots[i].actual_size = 0;
        slots[i].padded_size = 0;
        atomic_store_explicit(&slots[i].state, SLOT_FREE, memory_order_relaxed);
    }
}

/* ---- MPMC queue (inlined) ---- */
typedef struct {
    _Atomic uint32_t seq;
    uint32_t         value;
} mpmc_cell_t;

typedef struct {
    mpmc_cell_t      *cells;
    uint32_t          mask;
    char              _pad0[CACHE_LINE_SIZE - sizeof(void*) - sizeof(uint32_t)];
    _Atomic uint32_t  enqueue_pos;
    char              _pad1[CACHE_LINE_SIZE - sizeof(_Atomic uint32_t)];
    _Atomic uint32_t  dequeue_pos;
    char              _pad2[CACHE_LINE_SIZE - sizeof(_Atomic uint32_t)];
} mpmc_queue_t;

__attribute__((always_inline)) static inline size_t mpmc_queue_cells_bytes(uint32_t capacity)
{
    return (size_t)capacity * sizeof(mpmc_cell_t);
}

__attribute__((always_inline)) static inline void mpmc_queue_init(mpmc_queue_t *q, mpmc_cell_t *cells, uint32_t capacity)
{
    assert(capacity >= 2 && (capacity & (capacity - 1)) == 0);
    q->cells = cells;
    q->mask  = capacity - 1;
    for (uint32_t i = 0; i < capacity; i++)
        atomic_store_explicit(&cells[i].seq, i, memory_order_relaxed);
    atomic_store_explicit(&q->enqueue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&q->dequeue_pos, 0, memory_order_relaxed);
}

__attribute__((always_inline)) static inline int mpmc_queue_push(mpmc_queue_t *q, uint32_t value)
{
    mpmc_cell_t *cell;
    uint32_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->cells[pos & q->mask];
        uint32_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1,
                                                        memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return 0;
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }

    cell->value = value;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
    return 1;
}

__attribute__((always_inline)) static inline int mpmc_queue_pop(mpmc_queue_t *q, uint32_t *out)
{
    mpmc_cell_t *cell;
    uint32_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->cells[pos & q->mask];
        uint32_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)(pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1,
                                                        memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return 0;
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }

    *out = cell->value;
    atomic_store_explicit(&cell->seq, pos + q->mask + 1, memory_order_release);
    return 1;
}

/* Workers do crypto only. Parent kills them after all writes complete. */

typedef struct {
    ChunkSlot*               slots;
    mpmc_queue_t*            read_queue;
    mpmc_queue_t*            write_queue;
    const struct KeyTables*  key_tables;
    OperationMode            mode;
    int                      core_id;
} WorkerCtx;

__attribute__((always_inline)) static inline void worker_process(WorkerCtx* w)
{
    pin_to_core(w->core_id);

#if defined(__clang__) && defined(PGO_INSTRUMENTED)
    signal(SIGTERM, worker_sigterm_handler);
#endif

    for (;;) {
        uint32_t slot_idx;
        if (mpmc_queue_pop(w->read_queue, &slot_idx)) {
            ChunkSlot* s = &w->slots[slot_idx];
            atomic_store_explicit(&s->state, SLOT_PROCESSING, memory_order_relaxed);
            process_region(s->buf, s->padded_size, w->mode, w->key_tables);
            atomic_store_explicit(&s->state, SLOT_WRITE_READY, memory_order_relaxed);
            while (!mpmc_queue_push(w->write_queue, slot_idx)) {}
        } else {
            __builtin_ia32_pause();
        }
    }
}

int main(int argc, char** argv) {
    printf("TeraCrypt v4.0 - Lock-free async pipeline\n");
    printf("====================================================\n\n");

    if (argc != 5) {
        fprintf(stderr, "Usage: %s {encrypt|decrypt} <input> <output> <keyfile>\n", argv[0]);
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
        fprintf(stderr, "[ERROR] Invalid mode '%s'\n", argv[1]);
        return EXIT_INVALID_ARGS;
    }

    const char* input_file = argv[2];
    const char* output_file = argv[3];
    const char* key_file = argv[4];

    printf("Input:  %s\n", input_file);
    printf("Output: %s\n", output_file);
    printf("Key:    %s\n\n", key_file);

    int num_cores = get_core_count();
    if (num_cores <= 0) {
        fprintf(stderr, "[ERROR] Failed to detect CPU cores\n");
        return EXIT_CORE_COUNT_FAIL;
    }
    printf("Detected %d physical CPU cores\n", num_cores);

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

    printf("Precomputing key tables in shared memory...\n");
    SharedMem key_tables_mem = allocate_shared_mem(sizeof(struct KeyTables), CACHE_LINE_SIZE);
    struct KeyTables* key_tables = (struct KeyTables*)key_tables_mem.aligned;
    if (!key_tables) {
        fprintf(stderr, "[ERROR] Failed to allocate shared key tables\n");
        return EXIT_MEMORY_ALLOC_FAIL;
    }
    precompute_key_tables(key_matrix, key_tables);
    printf("Key tables precomputed\n\n");

    printf("Opening files...\n");
    int fd_in = open(input_file, O_RDONLY | O_DIRECT);
    int using_direct_read = 1;
    if (fd_in < 0) {
        using_direct_read = 0;
        fd_in = open(input_file, O_RDONLY);
    }
    if (fd_in < 0) {
        fprintf(stderr, "[ERROR] Cannot open input file: %s\n", input_file);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        return EXIT_FILE_NOT_FOUND;
    }

    struct stat st;
    if (fstat(fd_in, &st) != 0) {
        fprintf(stderr, "[ERROR] Cannot stat input file\n");
        close(fd_in);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        return EXIT_FILE_NOT_FOUND;
    }
    size_t file_size = (size_t)st.st_size;
    printf("Input file size: %zu bytes (%.2f MB)\n\n", file_size, file_size / (1024.0 * 1024));

    int fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    int using_direct_io = 1;
    if (fd_out < 0) {
        using_direct_io = 0;
        fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd_out < 0) {
        fprintf(stderr, "[ERROR] Cannot create output file: %s\n", output_file);
        close(fd_in);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        return EXIT_FILE_NOT_FOUND;
    }
    if (using_direct_read && using_direct_io) {
        printf("I/O: O_DIRECT (reads + writes bypass page cache)\n\n");
    } else if (using_direct_io) {
        printf("I/O: O_DIRECT writes, buffered reads\n\n");
    } else {
        printf("I/O: buffered (O_DIRECT not supported on this filesystem)\n\n");
    }

    struct timespec timer;
    start_measuring(&timer);

    if (file_size == 0) {
        ftruncate(fd_out, 0);
        close(fd_in);
        close(fd_out);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        printf("Empty input file -- nothing to do.\n");
        return EXIT_SUCCESS;
    }

    uint64_t num_full_chunks = (uint64_t)file_size / CHUNK_SIZE;

    /* Small file: no pipeline, just do it inline */
    if (num_full_chunks == 0) {
        printf("File smaller than one chunk (%llu MiB) -- processing inline, no worker pipeline.\n\n",
               (unsigned long long)(CHUNK_SIZE / (1024 * 1024)));

        size_t padded = ((file_size + (DIRECT_IO_ALIGN - 1)) / DIRECT_IO_ALIGN) * DIRECT_IO_ALIGN;
        uint8_t* buf = (uint8_t*)allocate_aligned_buffer(padded);
        if (!buf) {
            fprintf(stderr, "[ERROR] Failed to allocate inline buffer\n");
            close(fd_in); close(fd_out);
            munmap(key_tables_mem.base, key_tables_mem.alloc_size);
            return EXIT_MEMORY_ALLOC_FAIL;
        }

        ssize_t nread = pread(fd_in, buf, padded, 0);
        if (nread < (ssize_t)file_size) {
            fprintf(stderr, "[ERROR] Read failed (got %zd of %zu bytes)\n", nread, file_size);
            free(buf);
            close(fd_in); close(fd_out);
            munmap(key_tables_mem.base, key_tables_mem.alloc_size);
            return EXIT_IO_ERROR;
        }
        if (padded > file_size) memset(buf + file_size, 0, padded - file_size);

        process_region(buf, padded, mode, key_tables);

        ssize_t nwritten = pwrite(fd_out, buf, padded, 0);
        if (nwritten != (ssize_t)padded) {
            fprintf(stderr, "[ERROR] Write failed (wrote %zd of %zu bytes)\n", nwritten, padded);
            free(buf);
            close(fd_in); close(fd_out);
            munmap(key_tables_mem.base, key_tables_mem.alloc_size);
            return EXIT_IO_ERROR;
        }
        ftruncate(fd_out, file_size);
        free(buf);
        close(fd_in); close(fd_out);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);

        uint64_t elapsed_ns = stop_measuring(&timer);
        print_measured_results(elapsed_ns);
        printf("Done!\n");
        return EXIT_SUCCESS;
    }

    int num_workers = (num_cores > 1) ? (num_cores - 1) : 1;
    uint32_t total_slots = (uint32_t)num_workers + MAX_INFLIGHT_READS + MAX_INFLIGHT_WRITES;
    uint32_t queue_capacity = next_pow2(total_slots);
    int has_tail = (file_size % CHUNK_SIZE != 0);
    size_t tail_offset = (size_t)num_full_chunks * CHUNK_SIZE;
    size_t tail_actual_size = file_size - tail_offset;

    printf("Using async pipeline: %d worker(s) + 1 parent = %d core(s)\n",
           num_workers, num_workers + 1);
    printf("Chunk size: %llu MiB, %llu full chunk(s)%s\n",
           (unsigned long long)(CHUNK_SIZE / (1024 * 1024)),
           (unsigned long long)num_full_chunks,
           has_tail ? " + 1 tail chunk (parent-inline)" : "");

    size_t chunk_pool_bytes = (size_t)total_slots * CHUNK_SIZE;
    printf("Buffer pool: %u slots x %llu MiB = %.2f MiB\n",
           total_slots, (unsigned long long)(CHUNK_SIZE / (1024 * 1024)),
           chunk_pool_bytes / (1024.0 * 1024));
    {
        size_t avail = get_available_ram();
        if (avail > 0 && chunk_pool_bytes > avail / 2) {
            printf("[WARN] Buffer pool is more than half of available RAM (%.2f MiB available) -- "
                   "consider a smaller CHUNK_SIZE or fewer MAX_INFLIGHT_* slots.\n",
                   avail / (1024.0 * 1024));
        }
    }
    printf("\n");

    SharedMem slots_mem     = allocate_shared_mem((size_t)total_slots * sizeof(ChunkSlot), CACHE_LINE_SIZE);
    SharedMem chunk_buf_mem = allocate_shared_mem(chunk_pool_bytes, HUGEPAGE_SIZE);
    SharedMem tail_buf_mem  = has_tail ? allocate_shared_mem(CHUNK_SIZE, HUGEPAGE_SIZE)
                                        : (SharedMem){NULL, NULL, 0};
    SharedMem rq_struct_mem = allocate_shared_mem(sizeof(mpmc_queue_t), CACHE_LINE_SIZE);
    SharedMem rq_cells_mem  = allocate_shared_mem(mpmc_queue_cells_bytes(queue_capacity), CACHE_LINE_SIZE);
    SharedMem wq_struct_mem = allocate_shared_mem(sizeof(mpmc_queue_t), CACHE_LINE_SIZE);
    SharedMem wq_cells_mem  = allocate_shared_mem(mpmc_queue_cells_bytes(queue_capacity), CACHE_LINE_SIZE);

    if (!slots_mem.aligned || !chunk_buf_mem.aligned || (has_tail && !tail_buf_mem.aligned) ||
        !rq_struct_mem.aligned || !rq_cells_mem.aligned || !wq_struct_mem.aligned || !wq_cells_mem.aligned) {
        fprintf(stderr, "[ERROR] Failed to allocate shared pipeline memory\n");
        close(fd_in); close(fd_out);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        return EXIT_MEMORY_ALLOC_FAIL;
    }

    ChunkSlot* slots = (ChunkSlot*)slots_mem.aligned;
    chunk_pool_init(slots, (uint8_t*)chunk_buf_mem.aligned, total_slots, CHUNK_SIZE);
    madvise(chunk_buf_mem.aligned, chunk_pool_bytes, MADV_HUGEPAGE);
    if (has_tail) madvise(tail_buf_mem.aligned, CHUNK_SIZE, MADV_HUGEPAGE);

    mpmc_queue_t* read_queue  = (mpmc_queue_t*)rq_struct_mem.aligned;
    mpmc_queue_t* write_queue = (mpmc_queue_t*)wq_struct_mem.aligned;
    mpmc_queue_init(read_queue,  (mpmc_cell_t*)rq_cells_mem.aligned, queue_capacity);
    mpmc_queue_init(write_queue, (mpmc_cell_t*)wq_cells_mem.aligned, queue_capacity);

    /* Parent-private free list -- no atomics needed */
    uint32_t* free_list = (uint32_t*)malloc((size_t)total_slots * sizeof(uint32_t));
    uint32_t free_count = total_slots;
    for (uint32_t i = 0; i < total_slots; i++) free_list[i] = i;

    pid_t* pids = (pid_t*)malloc((size_t)num_workers * sizeof(pid_t));
    if (!free_list || !pids) {
        fprintf(stderr, "[ERROR] Failed to allocate worker bookkeeping\n");
        close(fd_in); close(fd_out);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        return EXIT_MEMORY_ALLOC_FAIL;
    }

    printf("Forking %d worker process(es)...\n", num_workers);
    int fork_failed = 0;
    for (int i = 0; i < num_workers; i++) {
        WorkerCtx ctx = {
            .slots       = slots,
            .read_queue  = read_queue,
            .write_queue = write_queue,
            .key_tables  = key_tables,
            .mode        = mode,
            .core_id     = get_nth_online_cpu(i + 1),
        };
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[ERROR] Failed to fork worker %d: %s\n", i, strerror(errno));
            for (int j = 0; j < i; j++) { kill(pids[j], WORKER_TERM_SIGNAL); waitpid(pids[j], NULL, 0); }
            fork_failed = 1;
            break;
        }
        if (pid == 0) {
            close(fd_in);
            close(fd_out);
            prctl(PR_SET_PDEATHSIG, WORKER_TERM_SIGNAL);
            if (getppid() == 1) _exit(1);
            worker_process(&ctx);
            _exit(0); /* unreachable -- worker_process() loops until signaled */
        }
        pids[i] = pid;
        printf("  Worker %d forked (PID %d, pinned to core %d)\n", i, pid, ctx.core_id);
    }
    if (fork_failed) {
        close(fd_in); close(fd_out);
        munmap(key_tables_mem.base, key_tables_mem.alloc_size);
        return EXIT_WORKER_CRASH;
    }
    printf("\n");

    pin_to_core(0);

    printf("Initializing io_uring...\n");
    struct io_uring uring;
    {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        params.flags = 0;
        int ret = io_uring_queue_init_params(URING_QUEUE_DEPTH, &uring, &params);
        if (ret < 0) {
            fprintf(stderr, "[ERROR] io_uring init failed: %s\n", strerror(-ret));
            ret = io_uring_queue_init(URING_QUEUE_DEPTH, &uring, 0);
        }
        if (ret < 0) {
            fprintf(stderr, "[ERROR] Failed to initialize io_uring: %s\n", strerror(-ret));
            for (int i = 0; i < num_workers; i++) { kill(pids[i], WORKER_TERM_SIGNAL); waitpid(pids[i], NULL, 0); }
            close(fd_in); close(fd_out);
            munmap(key_tables_mem.base, key_tables_mem.alloc_size);
            return EXIT_MEMORY_ALLOC_FAIL;
        }
    }
    printf("io_uring ready\n\n");

    printf("Starting async pipeline...\n\n");

    uint64_t next_read_offset = 0;
    uint32_t inflight_reads = 0, inflight_writes = 0;
    uint64_t completed_writes = 0;
    int tail_write_done = !has_tail;
    uint64_t iter_count = 0;
    int crash_check_idx = 0;
    int fatal_error = 0;
    size_t tail_padded_size = 0;

    if (has_tail) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&uring);
        if (sqe) {
            io_uring_prep_read(sqe, fd_in, tail_buf_mem.aligned, tail_actual_size, tail_offset);
            io_uring_sqe_set_data(sqe, (void*)(uintptr_t)pack_ud(OP_TAIL_READ, 0));
            io_uring_submit(&uring);
        } else {
            fprintf(stderr, "[ERROR] Failed to get SQE for tail read\n");
            fatal_error = 1;
        }
    }

    while (!fatal_error && (completed_writes < num_full_chunks || !tail_write_done)) {

        struct io_uring_cqe* cqe;
        int got_cqe = 0;
        while (io_uring_peek_cqe(&uring, &cqe) == 0) {
            got_cqe = 1;
            uint64_t ud = (uint64_t)(uintptr_t)cqe->user_data;
            OpType op = unpack_op(ud);
            uint32_t slot_idx = unpack_slot(ud);
            int res = cqe->res;

            switch (op) {
            case OP_READ: {
                if (res < 0) {
                    fprintf(stderr, "[ERROR] chunk read failed: %s\n", strerror(-res));
                    fatal_error = 1;
                } else if ((size_t)res != CHUNK_SIZE) {
                    fprintf(stderr, "[ERROR] short read on full chunk (%d of %llu bytes)\n",
                            res, (unsigned long long)CHUNK_SIZE);
                    fatal_error = 1;
                } else {
                    ChunkSlot* s = &slots[slot_idx];
                    s->actual_size = CHUNK_SIZE;
                    s->padded_size = CHUNK_SIZE;
                    atomic_store_explicit(&s->state, SLOT_READY, memory_order_relaxed);
                    if (!using_direct_read)
                        posix_fadvise(fd_in, (off_t)s->file_offset, (off_t)CHUNK_SIZE, POSIX_FADV_DONTNEED);
                    while (!mpmc_queue_push(read_queue, slot_idx)) { /* capacity >= total_slots */ }
                }
                inflight_reads--;
                break;
            }
            case OP_WRITE: {
                ChunkSlot* s = &slots[slot_idx];
                if (res < 0) {
                    fprintf(stderr, "[ERROR] chunk write failed: %s\n", strerror(-res));
                    fatal_error = 1;
                } else if ((size_t)res != s->padded_size) {
                    fprintf(stderr, "[ERROR] short write (%d of %zu bytes)\n", res, s->padded_size);
                    fatal_error = 1;
                } else {
                    if (!using_direct_io)
                        posix_fadvise(fd_out, (off_t)s->file_offset, (off_t)s->padded_size, POSIX_FADV_DONTNEED);
                    atomic_store_explicit(&s->state, SLOT_FREE, memory_order_relaxed);
                    free_list[free_count++] = slot_idx;
                    completed_writes++;
                }
                inflight_writes--;
                break;
            }
            case OP_TAIL_READ: {
                if (res < 0) {
                    fprintf(stderr, "[ERROR] tail read failed: %s\n", strerror(-res));
                    fatal_error = 1;
                } else {
                    size_t actual = (size_t)res;
                    size_t padded = ((actual + (DIRECT_IO_ALIGN - 1)) / DIRECT_IO_ALIGN) * DIRECT_IO_ALIGN;
                    if (padded > actual) memset((uint8_t*)tail_buf_mem.aligned + actual, 0, padded - actual);
                    if (!using_direct_read)
                        posix_fadvise(fd_in, (off_t)tail_offset, (off_t)actual, POSIX_FADV_DONTNEED);

                    /* Tail processed inline on parent */
                    process_region((uint8_t*)tail_buf_mem.aligned, padded, mode, key_tables);
                    tail_padded_size = padded;

                    struct io_uring_sqe* sqe = io_uring_get_sqe(&uring);
                    if (sqe) {
                        io_uring_prep_write(sqe, fd_out, tail_buf_mem.aligned, padded, tail_offset);
                        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)pack_ud(OP_TAIL_WRITE, 0));
                        io_uring_submit(&uring);
                    } else { fprintf(stderr, "[ERROR] Failed to get SQE for tail write\n"); fatal_error = 1; }
                }
                break;
            }
            case OP_TAIL_WRITE: {
                if (res < 0 || (size_t)res != tail_padded_size) {
                    fprintf(stderr, "[ERROR] tail write failed or short (res=%d, expected %zu)\n",
                            res, tail_padded_size);
                    fatal_error = 1;
                } else {
                    if (!using_direct_io)
                        posix_fadvise(fd_out, (off_t)tail_offset, (off_t)tail_padded_size, POSIX_FADV_DONTNEED);
                    tail_write_done = 1;
                }
                break;
            }
            }
            io_uring_cqe_seen(&uring, cqe);
        }
        if (fatal_error) break;

        /* Drain write queue, issue writes */
        int new_sqes = 0;
        while (inflight_writes < MAX_INFLIGHT_WRITES) {
            uint32_t slot_idx;
            if (!mpmc_queue_pop(write_queue, &slot_idx)) break;
            ChunkSlot* s = &slots[slot_idx];
            atomic_store_explicit(&s->state, SLOT_WRITE_PENDING, memory_order_relaxed);
            struct io_uring_sqe* sqe = io_uring_get_sqe(&uring);
            if (!sqe) {
                while (!mpmc_queue_push(write_queue, slot_idx)) { }
                break;
            }
            io_uring_prep_write(sqe, fd_out, s->buf, s->padded_size, s->file_offset);
            io_uring_sqe_set_data(sqe, (void*)(uintptr_t)pack_ud(OP_WRITE, slot_idx));
            inflight_writes++;
            new_sqes++;
        }

        while (inflight_reads < MAX_INFLIGHT_READS &&
               next_read_offset < num_full_chunks * CHUNK_SIZE &&
               free_count > 0) {
            uint32_t slot_idx = free_list[--free_count];
            ChunkSlot* s = &slots[slot_idx];
            s->file_offset = next_read_offset;
            atomic_store_explicit(&s->state, SLOT_READ_PENDING, memory_order_relaxed);
            struct io_uring_sqe* sqe = io_uring_get_sqe(&uring);
            if (!sqe) { free_list[free_count++] = slot_idx; break; }
            io_uring_prep_read(sqe, fd_in, s->buf, CHUNK_SIZE, next_read_offset);
            io_uring_sqe_set_data(sqe, (void*)(uintptr_t)pack_ud(OP_READ, slot_idx));
            next_read_offset += CHUNK_SIZE;
            inflight_reads++;
            new_sqes++;
        }

        if (new_sqes)
            io_uring_submit(&uring);

        if (!got_cqe && !new_sqes)
            __builtin_ia32_pause();

        /* Periodic worker crash check */
        iter_count++;
        if (iter_count % CRASH_CHECK_EVERY_N_ITERS == 0) {
            pid_t p = pids[crash_check_idx];
            if (p > 0) {
                pid_t r = waitpid(p, NULL, WNOHANG);
                if (r == p) {
                    fprintf(stderr, "[ERROR] worker %d (PID %d) died unexpectedly -- aborting\n",
                            crash_check_idx, p);
                    pids[crash_check_idx] = -1;
                    fatal_error = 1;
                }
            }
            crash_check_idx = (crash_check_idx + 1) % num_workers;
        }
    }

    uint64_t elapsed_ns = stop_measuring(&timer);

    if (!fatal_error) {
        ftruncate(fd_out, (off_t)file_size);
    }

    printf("\nShutting down workers...\n");
    for (int i = 0; i < num_workers; i++) {
        if (pids[i] > 0) kill(pids[i], WORKER_TERM_SIGNAL);
    }
    for (int i = 0; i < num_workers; i++) {
        if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    }

    io_uring_queue_exit(&uring);

    munmap(slots_mem.base, slots_mem.alloc_size);
    munmap(chunk_buf_mem.base, chunk_buf_mem.alloc_size);
    if (has_tail) munmap(tail_buf_mem.base, tail_buf_mem.alloc_size);
    munmap(rq_struct_mem.base, rq_struct_mem.alloc_size);
    munmap(rq_cells_mem.base, rq_cells_mem.alloc_size);
    munmap(wq_struct_mem.base, wq_struct_mem.alloc_size);
    munmap(wq_cells_mem.base, wq_cells_mem.alloc_size);
    munmap(key_tables_mem.base, key_tables_mem.alloc_size);
    free(free_list);
    free(pids);
    close(fd_in);
    close(fd_out);

    if (fatal_error) {
        fprintf(stderr, "\n[ERROR] Aborted -- output file is incomplete/invalid.\n");
        return EXIT_IO_ERROR;
    }

    printf("\nProcessing complete!\n");
    print_measured_results(elapsed_ns);
    double elapsed_s = elapsed_ns / 1e9;
    double throughput = (file_size / (1024.0 * 1024 * 1024)) / elapsed_s;
    printf("Throughput: %.2f GB/s\n", throughput);
    printf("Done!\n");
    return EXIT_SUCCESS;
}