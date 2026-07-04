# Bit-level Encryption v1.0 — Architecture

Single-threaded, cross-platform file encryption using bit-level transposition and per-lane rotation. AVX2-accelerated on x86, scalar fallback everywhere else.

## Overview

The cipher treats every 128-byte block as a 32x32 bit matrix of `uint32_t` values. Each round rotates every lane by a key-derived amount, XORs a key word into lane 31, then transposes the matrix. 8 rounds total. The same code handles encryption and decryption — just with the key schedule reversed and the round operations in reverse order.

```
┌─────────────────────────────────────────────────────────────┐
│                        main()                                │
│                                                              │
│   parse args (encrypt|decrypt, input, key)                   │
│        │                                                     │
│        ▼                                                     │
│   read 128-byte key → key_matrix[32]                         │
│        │                                                     │
│        ▼                                                     │
│   precompute_key_tables(key_matrix)                          │
│   (builds scalar + AVX2 rotation/XOR tables)                 │
│        │                                                     │
│        ▼                                                     │
│   allocate aligned 1 MiB batch buffer                        │
│        │                                                     │
│        ▼                                                     │
│   ┌─── loop: fread 1 MiB batch ───────────────────────┐     │
│   │                                                    │     │
│   │   pad to 128-byte boundary                        │     │
│   │        │                                           │     │
│   │        ▼                                           │     │
│   │   for each pair of 128-byte blocks:               │     │
│   │        │                                           │     │
│   │        ▼                                           │     │
│   │   encrypt_two_blocks_avx2() / decrypt_two_blocks  │     │
│   │   (or scalar fallback for odd block)              │     │
│   │        │                                           │     │
│   │        ▼                                           │     │
│   │   fwrite batch (padded size on encrypt,           │     │
│   │           original size on decrypt)               │     │
│   └────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────┘
```

## Data Flow: End to End

```
INPUT FILE
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│ 1. Read key file (128 bytes = 32 x uint32_t)             │
│    key_matrix[0..31]                                     │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 2. Precompute key tables                                 │
│                                                          │
│    For each of 8 rounds:                                 │
│      Encryption: key words 0..31 forward                 │
│      Decryption: key words 31..0 reversed                │
│                                                          │
│    scalar_rot_amount[2][8][2][32]  -- per-lane rot amt   │
│    scalar_xor_key[2][8][2]        -- XOR for lane 31     │
│    avx2_rot_amount[2][8][2][4]    -- packed __m256i      │
│    avx2_rot_comp[2][8][2][4]      -- 32 - amt (left sh)  │
│    avx2_xor_vec[2][8][2]          -- XOR vector           │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ 3. Process file in 1 MiB batches                         │
│                                                          │
│    fread(batch_buf, 1, 1MiB, reader)                     │
│    pad to 128-byte boundary (zero-fill)                  │
│                                                          │
│    num_blocks = padded_size / 128                        │
│                                                          │
│    AVX2 path (if available):                             │
│      b = 0                                               │
│      while b + 1 < num_blocks:                           │
│        encrypt_two_blocks_avx2(block[b], block[b+1])     │
│        b += 2                                            │
│      if b < num_blocks:                                  │
│        encrypt_block_avx2(block[b])                      │
│                                                          │
│    Scalar path (no AVX2):                                │
│      for each block:                                     │
│        8 rounds of: rotate → XOR → transpose             │
│                                                          │
│    fwrite(batch_buf, padded_or_orig, writer)             │
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
              OUTPUT FILE
```

## Cipher Round Structure

```
ENCRYPT (per round):
┌──────────────────────────────────────────────┐
│  Phase 0:                                    │
│    for i in 0..31:                            │
│      state[i] = rotate_right(state[i], amt0) │
│    state[31] ^= xor_key0                     │
│    transpose(state)                           │
│                                               │
│  Phase 1:                                    │
│    for i in 0..31:                            │
│      state[i] = rotate_right(state[i], amt1) │
│    state[31] ^= xor_key1                     │
│    transpose(state)                           │
└──────────────────────────────────────────────┘

DECRYPT (per round -- reverse of encrypt):
┌──────────────────────────────────────────────┐
│  Phase 0:                                    │
│    transpose(state)                           │
│    state[31] ^= xor_key0                     │
│    for i in 0..31:                            │
│      state[i] = rotate_right(state[i], amt0) │
│                                               │
│  Phase 1:                                    │
│    transpose(state)                           │
│    state[31] ^= xor_key1                     │
│    for i in 0..31:                            │
│      state[i] = rotate_right(state[i], amt1) │
└──────────────────────────────────────────────┘
```

## AVX2 Two-Block Parallelism

Two independent 128-byte blocks are processed simultaneously to expose more instruction-level parallelism. The CPU can issue rotations and transposes for the second block while the first is still in flight.

```
Block A (4 x __m256i)        Block B (4 x __m256i)
a0 = state[0..7]             b0 = state[0..7]
a1 = state[8..15]            b1 = state[8..15]
a2 = state[16..23]           b2 = state[16..23]
a3 = state[24..31]           b3 = state[24..31]
      │                            │
      ▼                            ▼
  rotate_all_right            rotate_all_right
  (per-lane via srlv/sllv)    (same amounts -- shared key tables)
      │                            │
      ▼                            ▼
  XOR key into a3             XOR key into b3
      │                            │
      ▼                            ▼
  transpose_avx2_regs          transpose_avx2_regs
  (in-register, no memory)    (in-register, no memory)
      │                            │
      ▼                            ▼
  rotate phase 1              rotate phase 1
      │                            │
      ▼                            ▼
  XOR + transpose             XOR + transpose
      │                            │
      ▼                            ▼
  store a0..a3                 store b0..b3
```

## 32x32 Bit Matrix Transpose

The transpose is a classic bit-plane swap, done in 5 stages:

```
Stage 1: swap 16-bit halves      mask = 0x0000FFFF
Stage 2: swap 8-bit halves       mask = 0x00FF00FF
Stage 3: swap 4-bit halves       mask = 0x0F0F0F0F
Stage 4: swap 2-bit halves       mask = 0x33333333
Stage 5: swap 1-bit halves       mask = 0x55555555

Scalar version: operates on uint32_t[32] in memory
AVX2 version:   operates on 4 x __m256i registers in-place
                 (uses permute4x64 + blend to handle the
                  128-bit lane crossing in stages 3-5)
```

## Key Schedule

```
128-byte key = 32 x uint32_t (key_matrix[0..31])

Encryption (round r):
  rot0 = key_matrix[4*r + 0]    xor0 = key_matrix[4*r + 1]
  rot1 = key_matrix[4*r + 2]    xor1 = key_matrix[4*r + 3]

Decryption (round r):
  xor0 = key_matrix[31 - 4*r]   rot0 = key_matrix[30 - 4*r]
  xor1 = key_matrix[29 - 4*r]   rot1 = key_matrix[28 - 4*r]

Per-lane rotation amount (lane i, phase p):
  amt = (rot_p + i + (i & 1)) & 31
  if (i & 1) == phase_p:   // even lanes in phase 0, odd in phase 1
    amt = (32 - amt) & 31  // convert to right-rotate
```

## Platform Support

```
┌────────────┬──────────────────┬──────────────────────┐
│ Platform   │ Transpose        │ Timer                │
├────────────┼──────────────────┼──────────────────────┤
│ x86+AVX2   │ AVX2 in-register │ PicoPerf (RDTSC)     │
│ x86 noAVX2 │ Scalar           │ PicoPerf (RDTSC)     │
│ aarch64    │ Scalar           │ clock_gettime        │
│ riscv64    │ Scalar           │ clock_gettime        │
│ macOS      │ Scalar (ARM) or  │ clock_gettime_nsec   │
│            │ AVX2 (Intel)     │                      │
│ Windows    │ AVX2 or scalar   │ QueryPerformance     │
└────────────┴──────────────────┴──────────────────────┘
```

## Build

```
make          optimized build (-Ofast -flto -march=native)
make pgo      PGO build (instrument → train → merge → rebuild)
make profile  debug build with symbols (for perf annotate)
make info     show detected platform, arch, compiler, flags
make clean    remove all build artifacts
```

## Output File Convention

```
encrypt:  input.txt  →  input.txt.locked
decrypt:  input.txt.locked  →  input.txt.locked.unlocked

Encrypted file is always a multiple of 128 bytes (zero-padded).
On decrypt, only the original (pre-padding) bytes are written.
```

## Files

```
Bit-level-encryption-v1.0/
├── main.c          Cipher implementation, key schedule, I/O loop
├── avx2.c          AVX2 transpose + rotate_all_right (included by main.c)
├── perf_timer.h    Cross-platform timing (Linux/macOS/Windows)
├── Makefile        Cross-platform build + PGO
└── ARCHITECTURE.md This file
```

## Relationship to TeraCrypt v4.0

This is the reference implementation. TeraCrypt v4.0 uses the same cipher core but wraps it in a multi-process async pipeline (io_uring + lock-free queues + O_DIRECT). The crypto in `crypto_core.h` is derived directly from this code. Both produce identical output for the same key and input.
