# TeraCrypt v4.0 — Architecture

Multi-process async encryption pipeline using io_uring, lock-free queues, and AVX2 SIMD.

## Overview

TeraCrypt encrypts/decrypts files in 8 MiB chunks. A parent process manages I/O via io_uring while N worker processes (one per remaining physical core) handle the crypto. Workers communicate with the parent through lock-free MPMC queues in shared memory.

```
                          ┌──────────────────────────────────────────────┐
                          │              PARENT (core 0)                 │
                          │                                              │
                          │   ┌─────────┐    ┌──────────┐   ┌─────────┐  │
                          │   │ io_uring│───▶│ CQE loop │──▶│free_list│  │
                          │   │ SQ ring │    │ (reap    │   │ (recycle│  │
                          │   │ (submit │    │  complet-│   │  slots) │  │
                          │   │  reads/ │    │  ions)   │   └────┬────┘  │
                          │   │  writes)│    └────┬─────┘        │       │
                          │   └────┬────┘         │              │       │
                          │        │              │              │       │
                          │   submit reads   push to         pop free    │
                          │   (SQEs)         read_queue       slot for   │
                          │                                     new read │
                          └────────┬─────────────┬────────────────┬──────┘
                                   │             │                │
                              ┌────▼────┐   ┌────▼─────┐     ┌────▼─────┐
                              │  read   │   │  write   │     │  chunk   │
                              │  queue  │   │  queue   │     │  pool    │
                              │ (MPMC)  │   │ (MPMC)   │     │ (shared  │
                              └────┬────┘   └────▲─────┘     │  mmap)   │
                                   │             │           └──────────┘
                          ┌────────┴─────────────┴───────────────┐
                          │           WORKER PROCESSES           │
                          │     (pinned to cores 1..N-1)         │
                          │                                      │
                          │   pop from read_queue                │
                          │        │                             │
                          │        ▼                             │
                          │   process_region()                   │
                          │   (AVX2 encrypt/decrypt)             │
                          │        │                             │
                          │        ▼                             │
                          │   push to write_queue                │
                          └──────────────────────────────────────┘
```

## Data Flow: End to End

```
INPUT FILE
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ 1. PARENT: submit read SQEs to io_uring                  │
│    - Pop a free slot from free_list                      │
│    - Set slot state → SLOT_READ_PENDING                  │
│    - io_uring_prep_read(fd_in, slot->buf, 8MiB, offset)  │
│    - Up to MAX_INFLIGHT_READS (16) reads in flight       │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 2. KERNEL: io_uring completes reads                      │
│    - CQE arrives with user_data = (OP_READ << 32 | slot) │
│    - Parent reaps CQEs in a loop (io_uring_peek_cqe)     │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 3. PARENT: push completed reads to read_queue            │
│    - Set slot state → SLOT_READY                         │
│    - posix_fadvise(DONTNEED) if buffered I/O             │
│    - mpmc_queue_push(read_queue, slot_idx)               │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 4. WORKER: pop from read_queue, encrypt, push to write_q │
│    - mpmc_queue_pop(read_queue, &slot_idx)               │
│    - Set slot state → SLOT_PROCESSING                    │
│    - process_region(buf, padded_size, mode, key_tables)  │
│      └─ AVX2: two 128-byte blocks in parallel            │
│      └─ Scalar fallback for odd block                    │
│    - Set slot state → SLOT_WRITE_READY                   │
│    - mpmc_queue_push(write_queue, slot_idx)              │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 5. PARENT: drain write_queue, submit write SQEs          │
│    - mpmc_queue_pop(write_queue, &slot_idx)              │
│    - Set slot state → SLOT_WRITE_PENDING                 │
│    - io_uring_prep_write(fd_out, slot->buf, size, offset)│
│    - Up to MAX_INFLIGHT_WRITES (16) writes in flight     │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 6. KERNEL: io_uring completes writes                     │
│    - CQE arrives with user_data = (OP_WRITE << 32 | slot)│
│    - Parent reaps CQE                                    │
│    - Set slot state → SLOT_FREE                          │
│    - Return slot to free_list                            │
│    - completed_writes++                                  │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
              OUTPUT FILE
```

## Tail Chunk Handling

If the file size isn't a multiple of 8 MiB, the ragged tail is handled inline by the parent (no worker involved):

```
┌─────────────────────────────────────────────┐
│ PARENT: io_uring_prep_read(tail_buf, ...)   │
│         user_data = OP_TAIL_READ            │
└──────────────────┬──────────────────────────┘
                   │ CQE
                   ▼
┌─────────────────────────────────────────────┐
│ PARENT: process_region(tail_buf, padded)    │
│         (yes, parent does crypto here --    │
│          it's just one small chunk)         │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│ PARENT: io_uring_prep_write(tail_buf, ...)  │
│         user_data = OP_TAIL_WRITE           │
└──────────────────┬──────────────────────────┘
                   │ CQE
                   ▼
              tail_write_done = 1
```

## Small File Path (< 8 MiB)

No pipeline at all. No fork, no io_uring, no workers. Just:

```
pread → process_region → pwrite → done
```

## Shared Memory Layout

All inter-process state lives in anonymous shared memory (MAP_SHARED | MAP_ANONYMOUS):

```
┌─────────────────────────────────────────────────────────────┐
│ SHARED MEMORY (all MAP_ANONYMOUS, MAP_SHARED)               │
│                                                             │
│  ┌─────────────────┐  ┌──────────────────────────────────┐  │
│  │ KeyTables       │  │ ChunkSlot[total_slots]           │  │
│  │ (precomputed    │  │  - each cache-line aligned       │  │
│  │  from 128-byte  │  │  - buf ptr, offset, sizes,       │  │
│  │  key file)      │  │    atomic state, padding         │  │
│  └─────────────────┘  └──────────────────────────────────┘  │
│                                                             │
│  ┌─────────────────┐  ┌──────────────────────────────────┐  │
│  │ Chunk buffer    │  │ Tail buffer (if has_tail)        │  │
│  │ pool            │  │  - single 8 MiB slot             │  │
│  │ total_slots *   │  │  - MADV_HUGEPAGE                 │  │
│  │ 8 MiB           │  └──────────────────────────────────┘  │
│  │ MADV_HUGEPAGE   │                                        │
│  └─────────────────┘                                        │
│                                                             │
│  ┌─────────────────┐  ┌─────────────────┐                   │
│  │ read_queue      │  │ write_queue     │                   │
│  │ (mpmc_queue_t   │  │ (mpmc_queue_t   │                   │
│  │  + cells)       │  │  + cells)       │                   │
│  └─────────────────┘  └─────────────────┘                   │
└─────────────────────────────────────────────────────────────┘

total_slots = num_workers + MAX_INFLIGHT_READS + MAX_INFLIGHT_WRITES
            = (cores-1) + 16 + 16
```

## Slot State Machine

```
SLOT_FREE ──────▶ SLOT_READ_PENDING ──▶ SLOT_READY ──▶ SLOT_PROCESSING
    ▲                    (parent          (parent        (worker
    │                     submits         pushes to       pops and
    │                     read SQE)       read_queue)     processes)
    │                                                       │
    │                                                       ▼
    │                                              SLOT_WRITE_READY
    │                                                    │
    │                                              (worker pushes
    │                                               to write_queue)
    │                                                    │
    │                                                    ▼
    │                                            SLOT_WRITE_PENDING
    │                                                    │
    │                                              (parent submits
    │                                               write SQE)
    │                                                    │
    │                                                    ▼
    └──────────────────────────────────── CQE (write done)
```

## MPMC Queue (Vyukov Bounded Queue)

Lock-free, bounded, multi-producer multi-consumer queue. Each cell has a sequence number that acts as the synchronization primitive — no locks, no CAS on a central counter.

```
push:                              pop:
  pos = enqueue_pos (relaxed)        pos = dequeue_pos (relaxed)
  loop:                              loop:
    cell = &cells[pos & mask]          cell = &cells[pos & mask]
    seq = cell->seq (acquire)          seq = cell->seq (acquire)
    if seq == pos:                     if seq == pos+1:
      CAS enqueue_pos → pos+1            CAS dequeue_pos → pos+1
      break                            break
    elif seq < pos:                   elif seq < pos:
      return 0 (full)                   return 0 (empty)
    else:                             else:
      reload enqueue_pos                reload dequeue_pos
  cell->value = value               *out = cell->value
  cell->seq = pos+1 (release)       cell->seq = pos+mask+1 (release)
```

## Crypto Core

Each 128-byte block is treated as a 32x32 bit matrix of uint32_t values.

```
Per round (8 rounds total):

  ENCRYPT:                          DECRYPT:
    rotate_right each lane            transpose
    XOR key into lane 31              XOR key into lane 31
    transpose                         rotate_right each lane
    rotate_right each lane            transpose
    XOR key into lane 31              XOR key into lane 31
    transpose                         rotate_right each lane

AVX2 path (two blocks in parallel):
  - 4 x __m256i registers per block (8 uint32 per register = 32 total)
  - Variable per-lane rotation via _mm256_srlv/sllv_epi32
  - In-register transpose (no memory access during rounds)
  - Two independent blocks → more ILP for the OOO engine

Scalar fallback:
  - One block at a time
  - In-memory transpose
  - Used for the odd block when block count is odd
```

## Key Table Precomputation

The 128-byte key (32 uint32_t values) is expanded into precomputed tables at startup:

```
KeyTables {
  scalar_rot_amount[2][8][2][32]   // per-lane rotation amounts
  scalar_xor_key[2][8][2]          // XOR keys for lane 31
  avx2_rot_amount[2][8][2][4]      // packed into __m256i vectors
  avx2_rot_comp[2][8][2][4]        // 32 - rot_amount (for left shift)
  avx2_xor_vec[2][8][2]            // XOR vector (key in element 7)
}

Index meaning: [encrypt/decrypt][round][phase][lane]
```

## I/O Strategy

```
┌─────────────────────────────────────────────────────┐
│ O_DIRECT (preferred)                                │
│   reads:  bypass page cache, aligned to 4096        │
│   writes: bypass page cache, aligned to 4096        │
│   → no kernel copy, no cache pollution              │
│   → posix_fadvise(DONTNEED) skipped (nothing to     │
│     evict)                                          │
├─────────────────────────────────────────────────────┤
│ Buffered (fallback)                                 │
│   → O_DIRECT not supported (tmpfs, network FS)      │
│   → posix_fadvise(DONTNEED) after each read/write   │
│     to evict pages from cache                       │
└─────────────────────────────────────────────────────┘
```

## Core Pinning

```
Core 0:          Parent (I/O + queue management)
Core 1..N-1:     Workers (crypto only)

Parent pins to core 0 before the main loop.
Workers pin to their assigned core before entering the spin loop.
CPU affinity via sched_setaffinity.
```

## Build Pipeline

```
make         →  clang -Ofast -flto -march=native -mavx2
                ↓
                teracrypt binary

make pgo     →  1. Instrumented build (-fprofile-generate)
                2. Training runs (16M + 192M files, encrypt + decrypt)
                3. llvm-profdata merge
                4. Optimized rebuild (-fprofile-use)
                ↓
                teracrypt (PGO-optimized)
```

## Files

```
teracrypt/
├── main.c          Parent + worker logic, io_uring, queues, shared memory
├── crypto_core.h   AVX2 + scalar crypto, key tables, process_region
├── Makefile        Build + PGO targets
├── bench.sh        Full benchmark suite (perf stat + record + correctness)
├── cold_bench.sh   Single-run cold-cache benchmark
└── ARCHITECTURE.md This file
```
