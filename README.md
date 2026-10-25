# Low-Latency-Lab

A performance engineering lab where I take my earlier projects and re-engineer them
with modern HPC techniques — the same toolkit used in low-latency trading and
high-throughput systems: `io_uring` async I/O, lock-free data structures, AVX2 SIMD,
`O_DIRECT` zero-copy disk I/O, CPU pinning, and PGO compilation.

Each project starts from a working baseline, gets profiled with `perf stat` / `perf
record`, then systematically optimized at the instruction and cache-line level until
the hardware is the bottleneck, not the code.

## Projects

### [TeraCrypt v4.0](teracrypt/ARCHITECTURE.md) — Multi-process async encryption pipeline

Rebuilt from [TeraCrypt v2.1](https://github.com/GurudattaRK/TeraCrypt-v2.1) (x86-64
assembly + Python ctypes) into a pure-C multi-process pipeline:

- **`io_uring`** for async I/O with batched submissions — reads, writes, and
  crypto overlap (fewer syscalls, not zero)
- **Lock-free Vyukov MPMC queues** in shared memory for cross-process chunk dispatch
- **AVX2 SIMD** for parallel block encryption (two 128-byte blocks at a time)
- **`O_DIRECT`** for zero-copy disk I/O that bypasses the page cache
- **CPU pinning** via `sched_setaffinity` — parent and workers spread across
  the process's allowed CPU set (works inside taskset/cgroup cpusets)
- **PGO** with Clang (instrument → train → merge → rebuild)

**1.44 GB/s** on a 6-core AMD Ryzen 4600H with a PCIe Gen-3 NVMe SSD
(~0.33 GB/s encrypting a 256 MiB file in tmpfs on the same machine).

### [Bit-Level Encryption v1.0](Bit-level-encryption-v1.0/ARCHITECTURE.md) — AVX2-optimized single-threaded cipher

The single-threaded reference implementation that TeraCrypt's crypto core is derived
from. 128-byte blocks (32×32 bit matrix), 8 rounds of per-lane rotation + XOR +
bit-matrix transpose.

- **AVX2 intrinsics** for in-register transpose and variable per-lane rotation
- Two-block parallelism for more instruction-level parallelism
- x86-64 Linux (uses AVX2 intrinsics; no ARM/RISC-V fallback currently)
- PGO-compiled with Clang

**0.4 GB/s** — down from 5 minutes per GB in the
[original implementation](https://github.com/GurudattaRK/Bit-level-encryption-v1.0).

## Build

Each project has its own Makefile. See the `ARCHITECTURE.md` in each folder for
details.

```
cd teracrypt && make
cd Bit-level-encryption-v1.0 && make
```

Both support `make pgo` for profile-guided optimization.