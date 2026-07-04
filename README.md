# Low-Latency-Lab

Personal playground for low-latency systems programming, SIMD, and performance engineering on x86-64 Linux.

## Projects

### [teracrypt/](teracrypt/ARCHITECTURE.md)
Multi-process async encryption pipeline. io_uring for I/O, lock-free Vyukov MPMC queues for cross-process chunk dispatch, AVX2 SIMD for block encryption, O_DIRECT for zero-copy disk I/O. 5 worker processes pinned to physical cores, PGO-compiled. ~1.33 GB/s on a 6-core Ryzen 4600H.

### [Bit-level-encryption-v1.0/](Bit-level-encryption-v1.0/ARCHITECTURE.md)
Single-threaded reference cipher. 32x32 bit matrix transpose + per-lane rotation, 8 rounds. AVX2 on x86, scalar fallback for aarch64/riscv64. Cross-platform (Linux, macOS, Windows). This is the crypto core that TeraCrypt v4.0 builds on.

## Build

Each project has its own Makefile. See the ARCHITECTURE.md in each folder for details.

```
cd teracrypt && make
cd Bit-level-encryption-v1.0 && make
```

Both support `make pgo` for profile-guided optimization.