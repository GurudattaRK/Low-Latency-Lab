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

// Initialize an async read operation
#if defined(_WIN32)
AioHandle aio_read_chunk(HANDLE fh, void* buf, size_t size, uint64_t offset);
AioHandle aio_write_chunk(HANDLE fh, void* buf, size_t size, uint64_t offset);
#else
AioHandle aio_read_chunk(int fd, void* buf, size_t size, off_t offset);
AioHandle aio_write_chunk(int fd, void* buf, size_t size, off_t offset);
#endif

// Wait for an async operation to complete
void aio_wait(AioHandle* h);

#endif // AIO_WRAPPER_H
