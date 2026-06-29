#ifndef AIO_WRAPPER_H
#define AIO_WRAPPER_H

#include <stddef.h>
#include <stdint.h>

#if defined(__linux__) || defined(__APPLE__)
#include <aio.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

typedef struct {
    #if defined(__linux__) || defined(__APPLE__)
    struct aiocb cb;
    #elif defined(_WIN32)
    OVERLAPPED ov;
    HANDLE hEvent;
    #endif
} AioHandle;

// Initialize an async read/write operation.
// The AioHandle is filled in-place; it MUST remain at a stable address until
// aio_wait() returns, because POSIX AIO tracks the aiocb by pointer.
#if defined(_WIN32)
void aio_read_chunk(AioHandle* h, HANDLE fh, void* buf, size_t size, uint64_t offset);
void aio_write_chunk(AioHandle* h, HANDLE fh, void* buf, size_t size, uint64_t offset);
#else
void aio_read_chunk(AioHandle* h, int fd, void* buf, size_t size, off_t offset);
void aio_write_chunk(AioHandle* h, int fd, void* buf, size_t size, off_t offset);
#endif

// Wait for an async operation to complete
void aio_wait(AioHandle* h);

#endif // AIO_WRAPPER_H
