#ifndef _ZERO_CC_THREADS_H
#define _ZERO_CC_THREADS_H

typedef unsigned long thrd_t;
typedef union {
    char __size[40];
    long __align;
} mtx_t;

enum {
    thrd_success  = 0,
    thrd_error    = 1,
    thrd_busy     = 2,
    thrd_nomem    = 3,
    thrd_timedout = 4
};

enum {
    mtx_plain     = 0,
    mtx_recursive = 1,
    mtx_timed     = 2
};

int pthread_create(thrd_t *thread, void *attr, void *start_routine, void *arg);
int pthread_join(thrd_t thread, void **retval);
int pthread_mutex_init(mtx_t *mutex, void *attr);
int pthread_mutex_lock(mtx_t *mutex);
int pthread_mutex_unlock(mtx_t *mutex);
int pthread_mutex_destroy(mtx_t *mutex);

#define thrd_create(t, fn, arg) (pthread_create((t), (void *)0, (void *)(fn), (void *)(arg)) == 0 ? thrd_success : thrd_error)
#define thrd_join(t, res)       (pthread_join((t), (void **)(res)) == 0 ? thrd_success : thrd_error)
#define mtx_init(m, type)       (pthread_mutex_init((m), (void *)0) == 0 ? thrd_success : thrd_error)
#define mtx_lock(m)             (pthread_mutex_lock(m) == 0 ? thrd_success : thrd_error)
#define mtx_unlock(m)           (pthread_mutex_unlock(m) == 0 ? thrd_success : thrd_error)
#define mtx_destroy(m)          pthread_mutex_destroy(m)

#endif /* _ZERO_CC_THREADS_H */
