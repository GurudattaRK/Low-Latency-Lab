#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PARALLEL_THRESHOLD (128ULL * 1024 * 1024)
#define BUFFER_MIN  (128ULL * 1024 * 1024)
#define BUFFER_MAX  (1ULL * 1024 * 1024 * 1024)
#define RAM_CAP_FRACTION 0.80
#define PAGE_SIZE 4096
#define BLOCK_SIZE 128

int get_core_count(void) {
    #if defined(__linux__)
    int max_cores = 256;
    int* core_ids = calloc(max_cores, sizeof(int));
    if (!core_ids) return 1;
    
    int unique_count = 0;
    
    for (int cpu = 0; cpu < max_cores; cpu++) {
        char path[256];
        snprintf(path, sizeof(path), 
                "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        
        FILE* fp = fopen(path, "r");
        if (!fp) break;
        
        int core_id;
        if (fscanf(fp, "%d", &core_id) == 1) {
            int found = 0;
            for (int i = 0; i < unique_count; i++) {
                if (core_ids[i] == core_id) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                core_ids[unique_count++] = core_id;
            }
        }
        fclose(fp);
    }
    
    free(core_ids);
    return (unique_count > 0) ? unique_count : 1;
    
    #elif defined(__APPLE__)
    int count;
    size_t len = sizeof(count);
    if (sysctlbyname("hw.physicalcpu", &count, &len, NULL, 0) == 0) {
        return (count > 0) ? count : 1;
    }
    return 1;
    
    #elif defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* info = 
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(len);
    
    if (!GetLogicalProcessorInformation(info, &len)) {
        free(info);
        return 1;
    }
    
    int count = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* ptr = info;
    DWORD offset = 0;
    
    while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= len) {
        if (ptr->Relationship == RelationProcessorCore) {
            count++;
        }
        offset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ptr++;
    }
    
    free(info);
    return (count > 0) ? count : 1;
    
    #else
    int count = sysconf(_SC_NPROCESSORS_ONLN);
    return (count > 0) ? count : 1;
    #endif
}

void pin_to_core(int core_id) {
    #if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        fprintf(stderr, "[Warning] Failed to pin to core %d: %s\n", 
                core_id, strerror(result));
    }
    
    #elif defined(__APPLE__)
    thread_affinity_policy_data_t policy = { core_id };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    
    kern_return_t result = thread_policy_set(
        mach_thread,
        THREAD_AFFINITY_POLICY,
        (thread_policy_t)&policy,
        THREAD_AFFINITY_POLICY_COUNT
    );
    
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "[Warning] Failed to pin to core %d (macOS hint only)\n", core_id);
    }
    
    #elif defined(_WIN32)
    DWORD_PTR mask = 1ULL << core_id;
    DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), mask);
    
    if (result == 0) {
        fprintf(stderr, "[Warning] Failed to pin to core %d: %lu\n", 
                core_id, GetLastError());
    }
    
    #else
    (void)core_id;
    #endif
}

size_t get_available_ram(void) {
    #if defined(__linux__) || defined(__APPLE__)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    
    if (pages > 0 && page_size > 0) {
        return (size_t)pages * (size_t)page_size;
    }
    
    return 2ULL * 1024 * 1024 * 1024;
    
    #elif defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    
    if (GlobalMemoryStatusEx(&status)) {
        return (size_t)status.ullAvailPhys;
    }
    
    return 2ULL * 1024 * 1024 * 1024;
    
    #else
    return 2ULL * 1024 * 1024 * 1024;
    #endif
}

size_t compute_buffer_size(void) {
    size_t free_ram = get_available_ram();
    
    size_t candidate = (size_t)((double)free_ram * RAM_CAP_FRACTION) / 3;
    
    if (candidate < BUFFER_MIN) {
        candidate = BUFFER_MIN;
    }
    if (candidate > BUFFER_MAX) {
        candidate = BUFFER_MAX;
    }
    
    candidate = (candidate / BLOCK_SIZE) * BLOCK_SIZE;
    
    return candidate;
}

void* allocate_aligned_buffer(size_t size) {
    #if defined(__linux__) || defined(__APPLE__)
    void* ptr = NULL;
    int result = posix_memalign(&ptr, PAGE_SIZE, size);
    
    if (result != 0) {
        fprintf(stderr, "[ERROR] posix_memalign failed: %s\n", strerror(result));
        return NULL;
    }
    
    return ptr;
    
    #elif defined(_WIN32)
    void* ptr = _aligned_malloc(size, PAGE_SIZE);
    
    if (!ptr) {
        fprintf(stderr, "[ERROR] _aligned_malloc failed\n");
    }
    
    return ptr;
    
    #else
    return malloc(size);
    #endif
}

void free_aligned_buffer(void* ptr) {
    if (!ptr) return;
    
    #if defined(__linux__) || defined(__APPLE__)
    free(ptr);
    
    #elif defined(_WIN32)
    _aligned_free(ptr);
    
    #else
    free(ptr);
    #endif
}
