#include "aio_wrapper.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#if defined(_WIN32)
void aio_read_chunk(AioHandle* h, HANDLE fh, void* buf, size_t size, uint64_t offset) {
    memset(&h->ov, 0, sizeof(h->ov));
    h->ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    h->ov.OffsetHigh = (DWORD)(offset >> 32);
    h->ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    h->hEvent = h->ov.hEvent;
    
    if (!ReadFile(fh, buf, (DWORD)size, NULL, &h->ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            fprintf(stderr, "ReadFile error: %lu\n", GetLastError());
        }
    }
}

void aio_write_chunk(AioHandle* h, HANDLE fh, void* buf, size_t size, uint64_t offset) {
    memset(&h->ov, 0, sizeof(h->ov));
    h->ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    h->ov.OffsetHigh = (DWORD)(offset >> 32);
    h->ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    h->hEvent = h->ov.hEvent;
    
    if (!WriteFile(fh, buf, (DWORD)size, NULL, &h->ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            fprintf(stderr, "WriteFile error: %lu\n", GetLastError());
        }
    }
}

void aio_wait(AioHandle* h) {
    DWORD bytes;
    WaitForSingleObject(h->hEvent, INFINITE);
    GetOverlappedResult((HANDLE)-1, &h->ov, &bytes, FALSE);
    CloseHandle(h->hEvent);
}

#else  // POSIX (Linux/macOS)

void aio_read_chunk(AioHandle* h, int fd, void* buf, size_t size, off_t offset) {
    memset(&h->cb, 0, sizeof(h->cb));
    h->cb.aio_fildes = fd;
    h->cb.aio_buf = buf;
    h->cb.aio_nbytes = size;
    h->cb.aio_offset = offset;
    
    if (aio_read(&h->cb) == -1) {
        perror("aio_read");
    }
}

void aio_write_chunk(AioHandle* h, int fd, void* buf, size_t size, off_t offset) {
    memset(&h->cb, 0, sizeof(h->cb));
    h->cb.aio_fildes = fd;
    h->cb.aio_buf = buf;
    h->cb.aio_nbytes = size;
    h->cb.aio_offset = offset;
    
    if (aio_write(&h->cb) == -1) {
        perror("aio_write");
    }
}

void aio_wait(AioHandle* h) {
    const struct aiocb* list[] = { &h->cb };
    // Loop because aio_suspend may return early on signal, and aio_error
    // can still report EINPROGRESS until the operation actually completes.
    while (aio_error(&h->cb) == EINPROGRESS) {
        aio_suspend(list, 1, NULL);
    }
    
    int err = aio_error(&h->cb);
    if (err != 0) {
        fprintf(stderr, "AIO error: %d\n", err);
    }
    // Reap the result to free kernel/glibc resources for this request.
    aio_return(&h->cb);
}

#endif
