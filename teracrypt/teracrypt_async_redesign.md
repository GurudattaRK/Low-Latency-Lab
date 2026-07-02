# TeraCrypt — async pipeline redesign (checkpoint)

Status: **design finalized, implementation started.** This doc is a resumable
checkpoint — everything below reflects the current agreed design after two
rounds of review, all open questions resolved. If you are a fresh session
picking this up: read this whole file before touching code, it supersedes
any assumptions in the original `main.c`/`crypto_core.c` about how workers
are scheduled. See "Implementation status" below for exactly what's built,
tested, and where.

## Implementation status

| Piece | Status | File |
|---|---|---|
| Lock-free MPMC queue (used for both read and write queues) | **Done, tested** | `mpmc_queue.h` |
| Queue stress test (6 producer + 6 consumer threads, 1.8M ops, plain run + ThreadSanitizer) | **Done, passing** | `test_mpmc_queue.c` |
| `ChunkSlot` state machine, buffer pool allocation | Not started | — |
| Parent reactor loop (io_uring + SQPOLL, CQE dispatch, tail-chunk handling, crash poll) | Not started | — |
| Worker loop | Not started | — |
| `main.c` rewire, `barrier.h` removal, `Makefile`/`bench.sh` updates | Not started | — |

`mpmc_queue.h` and `test_mpmc_queue.c` were validated with: a plain stress
run (6 producers × 300k items + 6 consumers against a deliberately small
1024-slot ring, to force full/empty races), repeated 5x clean; and a
ThreadSanitizer build (smaller workload, same logic), clean with no races
reported. Confidence is high that the queue algorithm itself is correct;
what's unverified is its behavior under real `fork()`-shared `mmap` memory
(the test uses pthreads over a heap allocation, which exercises the same
atomics but not the exact memory type) — worth a quick smoke test once it's
wired into `main.c`, but not expected to behave differently.

## Goal

Current program (`main.c` as it exists in the repo) forks N workers and
synchronizes them with `portable_barrier_t` (pthread mutex+cond,
process-shared) once per 512MiB buffer: parent reads a buffer via io_uring,
splits it evenly across workers+parent, blocks on a barrier until all are
done, writes the buffer, repeats. Workers are idle between barrier
rendezvous points and the barrier itself has wake/contention overhead.

New design: parent does **only** async I/O (via io_uring) and buffer
bookkeeping — no crypto. Workers do **only** crypto. Coordination between
them is two lock-free ring buffers instead of a barrier. Workers never sit
in cross-process rendezvous; they just spin on a queue.

## Two kinds of queue — do not conflate them

- **A. io_uring's own SQ/CQ rings.** Touched *only* by the parent. Workers
  never call any `io_uring_*` function. io_uring's submission queue is not
  safe for concurrent multi-process submission without SQPOLL/locking, so
  this is a hard boundary, not a style choice.
- **B. Two custom lock-free rings in shared memory** (plain atomics, no
  io_uring involvement):
  - **Read queue (SPMC):** parent pushes slot indices for chunks whose read
    just completed; workers pop to claim a chunk to process.
  - **Write queue (MPSC):** workers push slot indices for chunks they just
    finished encrypting/decrypting; parent pops and issues the real
    `io_uring_prep_write` for each.

```
input file --read--> PARENT (io_uring reactor, SQPOLL, pinned core 0)
                         |  pushes completed reads
                         v
                  read queue (SPMC ring)
                         |  workers pop to claim
                         v
              WORKERS (N = cores-1, one pinned core each)
                         |  push finished chunk
                         v
                  write queue (MPSC ring)
                         |  parent pops, issues io_uring write
                         v
                     output file
```

Tail/remainder chunk (see below) bypasses the queues entirely and is
processed inline by the parent.

## Constants / tunables

```c
#define CHUNK_SIZE        (8ULL * 1024 * 1024)
// Alternatives, uncomment one instead if you want to sweep this in bench.sh:
// #define CHUNK_SIZE      (16ULL * 1024 * 1024)
// #define CHUNK_SIZE      (32ULL * 1024 * 1024)

#define MAX_INFLIGHT_READS  16 // tune via bench.sh, start here, sweep 8..32
#define MAX_INFLIGHT_WRITES 16
#define URING_QUEUE_DEPTH   64 // must comfortably exceed MAX_INFLIGHT_READS+WRITES
#define CACHE_LINE_SIZE     64
#define CRASH_CHECK_EVERY_N_ITERS 200000 // reactor-loop iterations between waitpid polls; tune empirically
```

### Chunk size — decided: 8MiB, not 128MiB

128MiB was the original proposal but breaks down badly on files that
aren't huge relative to core count — which includes `bench.sh`'s own
default 1G test file:

- At `CHUNK_SIZE = 128MiB`, a 1GB file is only **8 full chunks total**. On
  a 16-core box (15 workers) most workers get 0-1 chunks — the parallelism
  the whole redesign is for barely engages.
- At `CHUNK_SIZE = 8MiB`, the same 1GB file is **128 full chunks** — a
  healthy ~8 chunks/worker even at 15 workers, and still reasonable up to
  ~60+ workers.
- For genuinely large (multi-GB/TB) files, any reasonable chunk size gives
  plenty of chunks — no downside there either way.
- 8MiB reads/writes are far above the size where NVMe/SATA devices reach
  full throughput at reasonable queue depth (that threshold is usually in
  the 128KB-1MB range), so there's no I/O-efficiency argument for going
  bigger.

**8MiB is the new default.** Left as a `#define` so it's easy to sweep
alongside `MAX_INFLIGHT_READS/WRITES` during benchmarking.

### Buffer pool sizing — decided: Option B (decoupled from worker count)

Old `NUM_BUFFERS=3` was 1:1 with the old triple-buffer *stages* (read
next / process current / write previous). That meaning doesn't carry over
— buffer count and worker count are now decoupled by the queues, so
there's no structural reason the pool has to scale as `N * 3`. That
multiplier is dropped entirely:

```c
total_slots = N + MAX_INFLIGHT_READS + MAX_INFLIGHT_WRITES
```

One slot actively owned per worker, plus enough slots to cover read-ahead
and write-behind depth. Combined with the smaller 8MiB chunk size, memory
stays modest at any core count — e.g. `N=63` gives `63+16+16=95` slots ×
8MiB ≈ 760MiB, vs. the old `N*3` @ 128MiB proposal's ~24GiB at the same
core count.

Also cap total buffer memory against available RAM (reuse the logic in the
existing but currently-unused `compute_buffer_size()` in `platform.c`)
rather than letting it grow unbounded on high-core-count boxes.

## Data structures

```c
typedef struct {
    uint8_t*      buf;
    size_t        file_offset;
    size_t        actual_size;    // real bytes read (may be short on last full chunk)
    size_t        padded_size;    // rounded up to DIRECT_IO_ALIGN (4096)
    _Atomic int   state;          // FREE / READ_PENDING / READY / PROCESSING / WRITE_READY / WRITE_PENDING
} ChunkSlot __attribute__((aligned(CACHE_LINE_SIZE)));

// The read/write queues themselves: mpmc_queue_t from mpmc_queue.h (DONE,
// see Implementation status). Bounded MPMC ring, Vyukov-style per-cell
// sequence numbers, cache-line-padded head/tail. One implementation
// serves both the SPMC read queue and the MPSC write queue.
mpmc_queue_t read_queue, write_queue;   // both live in shared mmap, capacity = next_pow2(total_slots)
```

`mpmc_queue.h` is a self-contained header (no other project dependencies)
— `mpmc_queue_init/push/pop`, never blocks, returns 0 on full/empty so
callers do their own spin/retry. Its cell backing array and the
`mpmc_queue_t` struct itself both need to be allocated in the shared
`mmap(MAP_SHARED)` region before `fork()`, same as everything else shared
across the worker processes.

All of this lives in the existing shared `mmap(MAP_SHARED)` region (same
pattern as `SharedMem` in current `main.c`). Plain C11/GCC atomics on
`MAP_SHARED` memory work correctly across `fork()`'d processes on Linux —
no futex or process-shared pthread primitives needed for the queues
themselves (barrier.h's process-shared mutex/cond is being *removed*, not
reused).

## `user_data` encoding for io_uring completions

64-bit `user_data`, packed as `(op_type << 32) | slot_index`:

```c
enum { OP_READ = 0, OP_WRITE = 1, OP_PARENT_TAIL_READ = 2, OP_PARENT_TAIL_WRITE = 3 };
uint64_t user_data = ((uint64_t)op_type << 32) | (uint64_t)slot_index;
```

Parent's CQE-reap loop decodes this and dispatches accordingly. Reap loop
must drain *all* currently-available CQEs each pass (`io_uring_peek_cqe` in
a loop, not an assumed fixed count) — deep pipelining means many reads and
writes can complete out of submission order and in batches.

## SQPOLL

```c
struct io_uring_params params = {0};
params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
params.sq_thread_cpu = PARENT_CORE_ID;   // same core parent's reactor loop is pinned to
params.sq_thread_idle = 100000;          // ms-ish scale per kernel version; generous, avoid sleep/wakeup churn
io_uring_queue_init_params(URING_QUEUE_DEPTH, &ctx->ring, &params);
```

Effect: submitting a read/write becomes a pure shared-memory write (no
syscall) as long as the poll thread stays awake, which it will for the
whole run except possibly during the one-time tail-chunk pause. Reaping
CQEs is already syscall-free regardless of SQPOLL (`io_uring_peek_cqe`
reads the CQ ring directly). Requires the process to run with sufficient
privilege for SQPOLL (unprivileged since Linux 5.11, but `bench.sh` already
runs as root for perf counters, so not a new constraint here).

Pinning: parent's userspace reactor loop AND the SQPOLL kernel thread share
core 0. This is intentional — everything that is not chunk crypto lives on
that one core, freeing a full extra core for workers vs. giving SQPOLL its
own dedicated core.

## Parent reactor loop (pseudocode)

```
num_full_chunks = file_size / CHUNK_SIZE
has_tail        = (file_size % CHUNK_SIZE != 0)   // also true whenever file_size < CHUNK_SIZE
tail_offset     = num_full_chunks * CHUNK_SIZE
tail_write_done = false

init slots[total_slots] = FREE
init read_queue, write_queue (empty)
next_read_offset = 0
inflight_reads = 0, inflight_writes = 0
completed_writes = 0
tail_read_submitted = false
iter_count = 0
crash_check_worker_idx = 0
fatal_error = false

# kick off the tail chunk's read immediately alongside the main pipeline —
# it is small relative to the whole file in the common case, no reason to
# delay it to the end.
submit io_uring read for [tail_offset, file_size) into a dedicated tail
  buffer, tag (OP_PARENT_TAIL_READ, 0)
tail_read_submitted = true

while completed_writes < num_full_chunks || (has_tail && !tail_write_done):

    # drain every available CQE this pass
    while (cqe = io_uring_peek_cqe()):
        (op, slot) = decode(cqe->user_data)
        switch op:
          OP_READ:
            pad tail-of-region to DIRECT_IO_ALIGN if short, mark slot READY
            read_queue.push(slot)          # SPMC push, single producer -> plain release store
            inflight_reads--
          OP_WRITE:
            on error -> fatal, abort pipeline
            mark slot FREE
            completed_writes++
            inflight_writes--
          OP_PARENT_TAIL_READ:
            pad remainder to next multiple of DIRECT_IO_ALIGN with zeros
            process_region(tail_buf, padded_tail_size, mode, key_tables)  # inline, parent's own CPU
            submit io_uring write for tail_buf at tail_offset,
              tag (OP_PARENT_TAIL_WRITE, 0)
          OP_PARENT_TAIL_WRITE:
            on error -> fatal, abort pipeline
            tail_write_done = true
        io_uring_cqe_seen(cqe)

    # drain worker-submitted "ready to write" queue, issue the real writes
    while inflight_writes < MAX_INFLIGHT_WRITES and (slot = write_queue.try_pop()):
        submit io_uring write for slots[slot], tag (OP_WRITE, slot)
        mark slot WRITE_PENDING
        inflight_writes++

    # keep the read pipeline full
    while inflight_reads < MAX_INFLIGHT_READS
          and next_read_offset < num_full_chunks * CHUNK_SIZE
          and a FREE slot exists:
        submit io_uring read of CHUNK_SIZE at next_read_offset into that slot,
          tag (OP_READ, slot)
        mark slot READ_PENDING
        next_read_offset += CHUNK_SIZE
        inflight_reads++

    io_uring_submit()   # cheap/no-op syscall-wise when SQPOLL thread is awake

    # crash safety net: check exactly ONE worker per interval, round-robin,
    # so the syscall rate stays ~1 waitpid per CRASH_CHECK_EVERY_N_ITERS
    # regardless of N. Full coverage sweep takes N * CRASH_CHECK_EVERY_N_ITERS
    # iterations -- this is a hang-prevention safety net, not meant to be
    # instant. Parent-only, as agreed (workers never check anything).
    iter_count++
    if iter_count % CRASH_CHECK_EVERY_N_ITERS == 0:
        pid = pids[crash_check_worker_idx]
        if waitpid(pid, NULL, WNOHANG) == pid:   # exited/died unexpectedly
            fatal_error = true
            break
        crash_check_worker_idx = (crash_check_worker_idx + 1) % num_workers

if fatal_error:
    report which worker died, skip ftruncate, kill remaining workers, exit nonzero
    # a dead worker means whatever chunk it was mid-processing never reaches
    # write_queue -- the file is genuinely incomplete, not just "unfinished
    # bookkeeping" -- so don't pretend success here.
else:
    # every real chunk (and the tail) is written to disk at this point -- no
    # worker can still be touching a shared buffer, since it would not yet
    # have pushed to write_queue.
    ftruncate(fd_out, file_size)   # trims any O_DIRECT tail padding

for each worker pid: kill(pid, WORKER_TERM_SIGNAL)   # see signal choice below
for each worker pid: waitpid(pid, NULL, 0)
cleanup (munmap buffers, close fds)
```

Notes:
- No blocking `io_uring_wait_cqe()` anywhere in the steady state — this is
  a deliberate busy-spin reactor per the "no syscalls, no idling" goal. If
  profiling later shows the peek-loop burning meaningless cycles when
  nothing is happening, revisit; not a concern for a throughput-bound
  batch tool.
- `io_uring_get_sqe()` can return `NULL` if the SQ ring is momentarily full
  (backpressure) — treat as "try again next loop iteration," not an error.
- The crash check is intentionally the *only* syscall-crossing bookkeeping
  in the steady-state loop besides io_uring itself — one `waitpid(WNOHANG)`
  call every `CRASH_CHECK_EVERY_N_ITERS` iterations, never more than one
  worker checked per pass, parent-only (workers do zero crash-related
  work, matching "keep all crash detection on parent core").

## Worker termination signal — SIGKILL normally, SIGTERM only for PGO builds

Both are implemented, gated by the same `PGO_INSTRUMENTED` macro the
original code already uses for its `__llvm_profile_write_file()` call —
non-PGO builds don't compile in any signal-handling code at all:

```c
#if defined(__clang__) && defined(PGO_INSTRUMENTED)
extern int __llvm_profile_write_file(void);
static void worker_sigterm_handler(int sig) {
    (void)sig;
    __llvm_profile_write_file();
    _exit(0);
}
#define WORKER_TERM_SIGNAL SIGTERM
#else
#define WORKER_TERM_SIGNAL SIGKILL
#endif
```

## Worker loop (pseudocode)

```
pin_to_core(core_id)

#if defined(__clang__) && defined(PGO_INSTRUMENTED)
    signal(SIGTERM, worker_sigterm_handler)
#endif
// normal (non-PGO) builds: no handler installed at all, nothing to catch --
// parent's SIGKILL just ends the process, no compiled-in signal path exists.

loop forever:
    slot = read_queue.try_pop()          # busy spin internally if empty, no backoff/yield
    if slot found:
        process_region(slots[slot].buf, slots[slot].padded_size, mode, key_tables)
        mark slot WRITE_READY
        write_queue.push(slot)
    # else: spin again immediately, no sleep/yield/futex
```

No exit condition checked in the hot path at all — `kill(pid,
WORKER_TERM_SIGNAL)` (SIGKILL by default, SIGTERM+profile-flush only when
built with `-DPGO_INSTRUMENTED`) is the only way workers stop. This is
safe specifically *because* the parent only sends it after confirming
every write completed (see reactor loop above) — by that point no worker
can be mid-chunk on live data.

## Known, accepted limitations (explicitly decided, not oversights)

- **Worker crash detection is lightweight, not instant.** Parent polls one
  worker's exit status per `CRASH_CHECK_EVERY_N_ITERS` reactor-loop
  iterations, round-robin, to keep the syscall rate minimal. A dead worker
  is eventually detected (bounded by `N * CRASH_CHECK_EVERY_N_ITERS`
  iterations) rather than instantly, and rather than hanging forever as in
  the earlier no-detection version. Other workers are unaffected either
  way — no corruption, just an incomplete chunk from the dead one.
- **Pure busy-spin everywhere** (workers waiting on the read queue, parent's
  reactor loop). No futex/semaphore/condvar anywhere in the design. Trades
  CPU-when-idle for zero syscall/context-switch overhead — correct choice
  for a short-lived, throughput-bound batch tool that already expects to
  saturate every core for the whole run.
- **`posix_fadvise(..., POSIX_FADV_SEQUENTIAL)` removed** on both fds — no
  longer accurate once reads are pipelined many-deep and writes land out of
  submission order.
- **Tail chunk processed synchronously on the parent's core**, briefly
  pausing the reactor loop (worst case: whole file smaller than one
  `CHUNK_SIZE`, so up to ~128MiB of single-core crypto). Intentional,
  small, one-time exception to "parent does no crypto."

## Still open / needs a decision before implementation continues

1. Exact values for `MAX_INFLIGHT_READS/WRITES`, `URING_QUEUE_DEPTH`, and
   `CRASH_CHECK_EVERY_N_ITERS` — defaults given above are starting points
   to sweep with `bench.sh`, not final.
2. Whether `num_workers` should stay `num_cores - 1` (parent gets a
   dedicated core, now also hosting the SQPOLL kernel thread) or be
   revisited now that parent no longer does any crypto — current
   recommendation is to keep `num_cores - 1` for the first working version
   and benchmark before changing it.

Buffer pool sizing (Option B) and chunk size (8MiB) are now decided — see
their sections above.

## Fixed relative to the original barrier-based code (independent of this
## redesign, but being carried along since we're in here anyway)

- Missing `ftruncate(fd_out, file_size)` at the end — the O_DIRECT tail
  padding could previously leave the output file larger than the real
  data. Now fixed as part of the reactor loop's completion path.

## What has NOT changed

- `crypto_core.c`/`avx2.c` (the actual encrypt/decrypt block transform) —
  untouched. `process_region()` is called exactly the same way, just now
  invoked either by a worker on a full `CHUNK_SIZE` region or by the
  parent inline on the padded tail region.
- `platform.c`/`platform.h` — `pin_to_core`, `get_core_count`,
  `get_nth_online_cpu`, `DIRECT_IO_ALIGN` all still used as-is.
- Key table precomputation (`precompute_key_tables`) — unchanged, still
  computed once in shared memory before forking.
- `barrier.h` is being **removed entirely** — no longer used anywhere.
