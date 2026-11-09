#include <threads.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_THREADS 4
#define ITERS_PER_THREAD 10000

static mtx_t g_mtx;
static cnd_t g_cnd;
static long g_counter = 0;
static once_flag g_once = ONCE_FLAG_INIT;
static int g_init_count = 0;

static void init_routine(void) {
    g_init_count++;
}

static int worker_mtx(void *arg) {
    long id = (long)(intptr_t)arg;
    call_once(&g_once, init_routine);

    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        mtx_lock(&g_mtx);
        g_counter++;
        mtx_unlock(&g_mtx);
    }
    return (int)(id * 10);
}

static int g_ready = 0;
static int consumer_worker(void *arg) {
    (void)arg;
    mtx_lock(&g_mtx);
    while (g_ready == 0) {
        cnd_wait(&g_cnd, &g_mtx);
    }
    mtx_unlock(&g_mtx);
    return 42;
}

static int worker_heap(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        void *p = malloc(64);
        if (!p) return 1;
        free(p);
    }
    return 0;
}

int main(void) {
    printf("Starting thread_test...\n");

    mtx_init(&g_mtx, mtx_plain);
    cnd_init(&g_cnd);

    /* Test 1: Mutex + Threads + call_once */
    thrd_t threads[NUM_THREADS];
    for (long i = 0; i < NUM_THREADS; i++) {
        if (thrd_create(&threads[i], worker_mtx, (void *)(intptr_t)(i + 1)) != thrd_success) {
            printf("FAIL: thrd_create worker %ld failed\n", i);
            return 1;
        }
    }

    for (long i = 0; i < NUM_THREADS; i++) {
        int res = 0;
        if (thrd_join(threads[i], &res) != thrd_success) {
            printf("FAIL: thrd_join %ld failed\n", i);
            return 2;
        }
        if (res != (int)((i + 1) * 10)) {
            printf("FAIL: thread %ld return val %d != %ld\n", i, res, (i + 1) * 10);
            return 3;
        }
    }

    if (g_counter != (long)NUM_THREADS * ITERS_PER_THREAD) {
        printf("FAIL: g_counter is %ld, expected %ld\n", g_counter, (long)NUM_THREADS * ITERS_PER_THREAD);
        return 4;
    }
    if (g_init_count != 1) {
        printf("FAIL: g_init_count is %d, expected 1\n", g_init_count);
        return 5;
    }
    printf("PASS: mutex + threads + call_once (counter=%ld)\n", g_counter);

    /* Test 2: Condition Variable */
    thrd_t consumer;
    if (thrd_create(&consumer, consumer_worker, NULL) != thrd_success) {
        printf("FAIL: thrd_create consumer failed\n");
        return 6;
    }

    thrd_yield();

    mtx_lock(&g_mtx);
    g_ready = 1;
    cnd_signal(&g_cnd);
    mtx_unlock(&g_mtx);

    int c_res = 0;
    if (thrd_join(consumer, &c_res) != thrd_success || c_res != 42) {
        printf("FAIL: consumer join failed or bad return: %d\n", c_res);
        return 7;
    }
    printf("PASS: condition variable\n");

    /* Test 3: Concurrent heap operations */
    for (long i = 0; i < NUM_THREADS; i++) {
        if (thrd_create(&threads[i], worker_heap, NULL) != thrd_success) {
            printf("FAIL: thrd_create heap worker %ld failed\n", i);
            return 8;
        }
    }
    for (long i = 0; i < NUM_THREADS; i++) {
        int res = 0;
        int rc = thrd_join(threads[i], &res);
        if (rc != thrd_success || res != 0) {
            printf("FAIL: heap worker %ld join failed rc=%d res=%d\n", i, rc, res);
            return 9;
        }
    }
    printf("PASS: concurrent heap allocation\n");

    printf("ALL THREAD TESTS PASSED!\n");
    return 0;
}
