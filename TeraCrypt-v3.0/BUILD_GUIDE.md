# TeraCrypt v3.0 - Cross-Platform Build Guide

## Platform Support

✅ **Linux** (Primary development platform)  
✅ **macOS** (Full support)  
✅ **Windows** (Full support with MSVC or MinGW)  

## Prerequisites

### Linux
```bash
# Ubuntu/Debian
sudo apt-get install build-essential clang

# Fedora/RHEL
sudo dnf install gcc clang make

# Arch
sudo pacman -S base-devel clang
```

### macOS
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Or install full Xcode from App Store
```

### Windows

**Option 1: Visual Studio (Recommended)**
- Install Visual Studio 2019 or later
- Select "Desktop development with C++"
- Includes MSVC compiler

**Option 2: MinGW-w64**
```bash
# Using MSYS2
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-clang make
```

## Building

### Linux/macOS

#### Thread Variant
```bash
cd threads
make clean
CC=clang make all

# Or with GCC
CC=gcc make all

# With Profile-Guided Optimization (recommended)
CC=clang make pgo
```

#### Process Variant
```bash
cd processes
make clean
CC=clang make all

# Or with GCC
CC=gcc make all
```

### Windows (MSVC)

#### Using Developer Command Prompt

**Thread Variant:**
```cmd
cd threads
cl /O2 /Ot /GL /arch:AVX2 /D_CRT_SECURE_NO_WARNINGS ^
   main.c platform.c crypto_core.c aio_wrapper.c ^
   /Fe:teracrypt_threads.exe /link /LTCG

# Or use the provided build script
build_windows.bat
```

**Process Variant:**
```cmd
cd processes
cl /O2 /Ot /GL /arch:AVX2 /D_CRT_SECURE_NO_WARNINGS ^
   main.c platform.c crypto_core.c aio_wrapper.c ^
   /Fe:teracrypt_processes.exe /link /LTCG

# Or use the provided build script
build_windows.bat
```

### Windows (MinGW)

```bash
cd threads
gcc -O3 -march=native -flto -funroll-loops -fomit-frame-pointer \
    main.c platform.c crypto_core.c aio_wrapper.c \
    -o teracrypt_threads.exe -lpthread

cd ../processes
gcc -O3 -march=native -flto -funroll-loops -fomit-frame-pointer \
    main.c platform.c crypto_core.c aio_wrapper.c \
    -o teracrypt_processes.exe -lpthread
```

## Usage

### Basic Encryption/Decryption

```bash
# Linux/macOS
./teracrypt_threads encrypt input.dat output.enc key.bin
./teracrypt_threads decrypt output.enc decrypted.dat key.bin

# Windows
teracrypt_threads.exe encrypt input.dat output.enc key.bin
teracrypt_threads.exe decrypt output.enc decrypted.dat key.bin
```

### Key File

The key file must be at least 128 bytes. Generate one with:

```bash
# Linux/macOS
dd if=/dev/urandom of=key.bin bs=128 count=1

# Windows (PowerShell)
$bytes = New-Object byte[] 128
(New-Object Security.Cryptography.RNGCryptoServiceProvider).GetBytes($bytes)
[IO.File]::WriteAllBytes("key.bin", $bytes)
```

## Platform-Specific Notes

### Linux

- **POSIX AIO**: Uses `aio_read`/`aio_write` for async I/O
- **Semaphores**: Anonymous semaphores for threads, process-shared for processes
- **Huge Pages**: Automatically requested via `madvise(MADV_HUGEPAGE)`
- **Core Pinning**: Uses `pthread_setaffinity_np`

**Performance Tips:**
```bash
# Enable transparent huge pages
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled

# Increase file descriptor limits
ulimit -n 4096
```

### macOS

- **POSIX AIO**: Uses `aio_read`/`aio_write` for async I/O
- **Semaphores**: Named semaphores (required on macOS)
- **Core Pinning**: Uses `thread_policy_set` (hint, not guaranteed)
- **Memory**: Uses `mmap` with `MAP_ANONYMOUS`

**Known Limitations:**
- Core pinning is a hint, not enforced
- Named semaphores require unique names (uses PID)
- No transparent huge pages support

### Windows

- **Overlapped I/O**: Uses `ReadFile`/`WriteFile` with `OVERLAPPED`
- **Threads**: Uses Windows threads (not pthreads)
- **Processes**: Uses `CreateProcess` (different architecture)
- **Memory**: Uses `VirtualAlloc` with `MEM_LARGE_PAGES` (if available)
- **Core Pinning**: Uses `SetThreadAffinityMask`

**Performance Tips:**
```cmd
# Run as Administrator for large pages
# Enable "Lock pages in memory" privilege in Group Policy

# Set high priority
start /high teracrypt_threads.exe encrypt input.dat output.enc key.bin
```

**Windows-Specific Considerations:**
- Process variant uses different IPC mechanism
- File paths use backslashes: `C:\path\to\file.dat`
- Requires Administrator for huge pages

## Troubleshooting

### Linux

**Error: `aio_read: Function not implemented`**
```bash
# Install libaio
sudo apt-get install libaio-dev

# Or use synchronous I/O fallback
# (automatically handled in code)
```

**Error: `sem_open: Permission denied`**
```bash
# Clean up stale semaphores
rm /dev/shm/sem.tc_*
```

### macOS

**Error: `sem_open failed`**
```bash
# Increase semaphore limits
sudo sysctl kern.sysv.semmni=256
sudo sysctl kern.sysv.semmns=1024
```

**Error: `Cannot allocate memory`**
```bash
# Reduce buffer size in source code
# Edit BUFFER_SIZE in main.c (default 512 MiB)
```

### Windows

**Error: `ERROR_NOT_ENOUGH_MEMORY`**
- Reduce buffer size in source code
- Close other applications
- Run as Administrator

**Error: `The system cannot find the file specified`**
- Use absolute paths: `C:\Users\...\file.dat`
- Check file permissions

## Performance Comparison

| Platform | Thread Variant | Process Variant | Notes |
|----------|----------------|-----------------|-------|
| **Linux** | 0.89 GB/s | 0.89 GB/s | Best performance |
| **macOS** | 0.75-0.85 GB/s | 0.70-0.80 GB/s | Core pinning is hint |
| **Windows** | 0.80-0.90 GB/s | 0.75-0.85 GB/s | With large pages |

*Performance varies based on hardware, RAM speed, and system configuration*

## Verification

Test correctness after building:

```bash
# Generate test data
dd if=/dev/urandom of=test.dat bs=1M count=128

# Generate key
dd if=/dev/urandom of=test.key bs=128 count=1

# Encrypt
./teracrypt_threads encrypt test.dat test.enc test.key

# Decrypt
./teracrypt_threads decrypt test.enc test.dec test.key

# Verify (Linux/macOS)
sha256sum test.dat test.dec

# Verify (Windows PowerShell)
Get-FileHash test.dat, test.dec -Algorithm SHA256
```

Both checksums should match!

## Optimization Levels

### Debug Build
```bash
# Linux/macOS
gcc -g -O0 -Wall -Wextra main.c platform.c crypto_core.c aio_wrapper.c -o teracrypt_debug -lpthread

# Windows
cl /Od /Zi main.c platform.c crypto_core.c aio_wrapper.c /Fe:teracrypt_debug.exe
```

### Release Build (Default)
```bash
# Linux/macOS
clang -Ofast -march=native -flto main.c platform.c crypto_core.c aio_wrapper.c -o teracrypt -lpthread

# Windows
cl /O2 /Ot /GL /arch:AVX2 main.c platform.c crypto_core.c aio_wrapper.c /Fe:teracrypt.exe /link /LTCG
```

### Maximum Performance (PGO)
```bash
# Linux/macOS only
make pgo

# Windows: Use Visual Studio's PGO tools
# 1. Build with /GL /LTCG:PGI
# 2. Run training workload
# 3. Rebuild with /LTCG:PGO
```

## Support

For issues or questions:
- Check the documentation in the `docs/` folder
- Review platform-specific notes above
- Ensure all prerequisites are installed
- Try debug build first to identify issues

## License

See parent repository for license information.
