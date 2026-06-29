#ifndef BARRIER_H
#define BARRIER_H

#include <pthread.h>

typedef struct {
    pthread_mutex_t  mtx;
    pthread_cond_t   cond;
    int              count;
    int              threshold;
    unsigned int     generation;
} portable_barrier_t;

static inline void portable_barrier_init(portable_barrier_t *b, int threshold, int process_shared)
{
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    if (process_shared)
        pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&b->mtx, &ma);
    pthread_mutexattr_destroy(&ma);

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    if (process_shared)
        pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&b->cond, &ca);
    pthread_condattr_destroy(&ca);

    b->count      = 0;
    b->threshold  = threshold;
    b->generation = 0;
}

static inline void portable_barrier_wait(portable_barrier_t *b)
{
    pthread_mutex_lock(&b->mtx);
    unsigned int gen = b->generation;
    b->count++;
    if (b->count == b->threshold) {
        b->count = 0;
        b->generation++;
        pthread_cond_broadcast(&b->cond);
    } else {
        while (b->generation == gen)
            pthread_cond_wait(&b->cond, &b->mtx);
    }
    pthread_mutex_unlock(&b->mtx);
}

static inline void portable_barrier_destroy(portable_barrier_t *b)
{
    pthread_mutex_destroy(&b->mtx);
    pthread_cond_destroy(&b->cond);
}

#endif
