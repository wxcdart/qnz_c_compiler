#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define NUM_THREADS 4
#define ITERS_PER_THREAD 10000

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cnd = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static long g_counter = 0;
static int g_init_count = 0;
static int g_ready = 0;

static void init_routine(void) {
    g_init_count++;
}

static void *worker_mtx(void *arg) {
    long id = (long)(intptr_t)arg;
    pthread_once(&g_once, init_routine);

    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        pthread_mutex_lock(&g_mtx);
        g_counter++;
        pthread_mutex_unlock(&g_mtx);
    }
    return (void *)(intptr_t)(id * 10);
}

static void *consumer_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mtx);
    while (!g_ready) {
        pthread_cond_wait(&g_cnd, &g_mtx);
    }
    pthread_mutex_unlock(&g_mtx);
    return (void *)(intptr_t)42;
}

int main(void) {
    printf("Starting pthread_test...\n");

    pthread_t self = pthread_self();
    if (!pthread_equal(self, pthread_self())) {
        printf("FAIL: pthread_equal self\n");
        return 1;
    }

    pthread_t threads[NUM_THREADS];
    for (long i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker_mtx, (void *)(intptr_t)(i + 1)) != 0) {
            printf("FAIL: pthread_create worker %ld failed\n", i);
            return 2;
        }
    }

    for (long i = 0; i < NUM_THREADS; i++) {
        void *res = NULL;
        if (pthread_join(threads[i], &res) != 0) {
            printf("FAIL: pthread_join %ld failed\n", i);
            return 3;
        }
        if ((long)(intptr_t)res != (i + 1) * 10) {
            printf("FAIL: thread %ld return val %ld != %ld\n", i, (long)(intptr_t)res, (i + 1) * 10);
            return 4;
        }
    }

    if (g_counter != (long)NUM_THREADS * ITERS_PER_THREAD) {
        printf("FAIL: g_counter is %ld, expected %ld\n", g_counter, (long)NUM_THREADS * ITERS_PER_THREAD);
        return 5;
    }
    if (g_init_count != 1) {
        printf("FAIL: g_init_count is %d, expected 1\n", g_init_count);
        return 6;
    }
    printf("PASS: pthread mutex + threads + once (counter=%ld)\n", g_counter);

    pthread_t consumer;
    if (pthread_create(&consumer, NULL, consumer_worker, NULL) != 0) {
        printf("FAIL: pthread_create consumer failed\n");
        return 7;
    }

    pthread_mutex_lock(&g_mtx);
    g_ready = 1;
    pthread_cond_signal(&g_cnd);
    pthread_mutex_unlock(&g_mtx);

    void *c_res = NULL;
    if (pthread_join(consumer, &c_res) != 0 || (long)(intptr_t)c_res != 42) {
        printf("FAIL: consumer join failed\n");
        return 8;
    }
    printf("PASS: pthread condition variable\n");

    printf("ALL PTHREAD TESTS PASSED!\n");
    return 0;
}
