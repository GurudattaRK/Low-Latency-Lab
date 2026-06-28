#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)
    #include <semaphore.h>
    #include <pthread.h>
    #include <sched.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/sysinfo.h>
#elif defined(__APPLE__)
    #include <semaphore.h>
    #include <pthread.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/sysctl.h>
    #include <mach/thread_policy.h>
    #include <mach/thread_act.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <process.h>
#endif

int get_core_count(void);

void pin_to_core(int core_id);

size_t get_available_ram(void);

size_t compute_buffer_size(void);

void* allocate_aligned_buffer(size_t size);

void free_aligned_buffer(void* ptr);

#endif
