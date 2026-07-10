#pragma once
#include <stdint.h>
#include <stdio.h>

/* One little timer, three OS flavours. Same three functions everywhere so the
 * caller never has to care which platform it's on. On x86 Linux we usually use
 * PicoPerf (rdtsc) instead - this is the fallback for everything else. */

// linux (also the fallback on non-x86 boxes where PicoPerf won't build)
#if defined(__linux__)

#include <time.h>

typedef struct timespec perf_timer_t;

static inline void start_measuring(perf_timer_t *t) 
{
    clock_gettime(CLOCK_MONOTONIC_RAW, t);
}

static inline uint64_t stop_measuring(perf_timer_t *t) 
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    return (uint64_t)(end.tv_sec  - t->tv_sec)  * 1000000000ULL + (uint64_t)(end.tv_nsec - t->tv_nsec);
}

static inline void print_measured_results(uint64_t ns) 
{
    printf("elapsed: %llu ns  (%.3f ms)\n",(unsigned long long)ns,(double)ns / 1000000.0);
}

// macOS - has its own nanosecond clock, no need for timespec math
#elif defined(__APPLE__)

#include <time.h>   /* for clock_gettime_nsec_np */

typedef uint64_t perf_timer_t;

static inline void start_measuring(perf_timer_t *t) 
{
    *t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static inline uint64_t stop_measuring(perf_timer_t *t) 
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - *t;
}

static inline void print_measured_results(uint64_t ns) 
{
    printf("elapsed: %llu ns  (%.3f ms)\n",(unsigned long long)ns,(double)ns / 1000000.0);
}


// windows - QPC counts ticks, divide by the frequency to get nanoseconds
#elif defined(_WIN32)

#include <windows.h>

typedef LARGE_INTEGER perf_timer_t;

static inline void start_measuring(perf_timer_t *t) 
{
    QueryPerformanceCounter(t);
}

static inline uint64_t stop_measuring(perf_timer_t *t) 
{
    LARGE_INTEGER end, freq;
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&freq);
    return (uint64_t)(end.QuadPart - t->QuadPart) * 1000000000ULL / (uint64_t)freq.QuadPart;
}

static inline void print_measured_results(uint64_t ns) 
{
    printf("elapsed: %llu ns  (%.3f ms)\n",(unsigned long long)ns,(double)ns / 1000000.0);
}

#else
#error "Unsupported platform. No timer implementation for this target."
#endif