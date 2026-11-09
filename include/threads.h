#ifndef _ZERO_CC_THREADS_H
#define _ZERO_CC_THREADS_H

typedef unsigned long thrd_t;

typedef union {
    char __size[40];
    long __align;
} mtx_t;

typedef union {
    char __size[48];
    long __align;
} cnd_t;

typedef int once_flag;
#define ONCE_FLAG_INIT 0

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

typedef int (*thrd_start_t)(void *);

int  thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int  thrd_join(thrd_t thr, int *res);
int  thrd_detach(thrd_t thr);
thrd_t thrd_current(void);
int  thrd_equal(thrd_t a, thrd_t b);
int  thrd_sleep(const void *duration, void *remaining);
void thrd_yield(void);
void thrd_exit(int res);

int  mtx_init(mtx_t *mutex, int type);
int  mtx_lock(mtx_t *mutex);
int  mtx_trylock(mtx_t *mutex);
int  mtx_unlock(mtx_t *mutex);
void mtx_destroy(mtx_t *mutex);

int  cnd_init(cnd_t *cond);
int  cnd_wait(cnd_t *cond, mtx_t *mutex);
int  cnd_timedwait(cnd_t *cond, mtx_t *mutex, const void *ts);
int  cnd_signal(cnd_t *cond);
int  cnd_broadcast(cnd_t *cond);
void cnd_destroy(cnd_t *cond);

void call_once(once_flag *flag, void (*func)(void));

/* Minimal POSIX pthread shims */
int  pthread_create(thrd_t *thread, void *attr, void *(*start_routine)(void *), void *arg);
int  pthread_join(thrd_t thread, void **retval);
int  pthread_detach(thrd_t thread);
thrd_t pthread_self(void);
int  pthread_equal(thrd_t t1, thrd_t t2);
void pthread_exit(void *retval);

int  pthread_mutex_init(mtx_t *mutex, void *attr);
int  pthread_mutex_lock(mtx_t *mutex);
int  pthread_mutex_trylock(mtx_t *mutex);
int  pthread_mutex_unlock(mtx_t *mutex);
int  pthread_mutex_destroy(mtx_t *mutex);

int  pthread_cond_init(cnd_t *cond, void *attr);
int  pthread_cond_wait(cnd_t *cond, mtx_t *mutex);
int  pthread_cond_timedwait(cnd_t *cond, mtx_t *mutex, const void *abstime);
int  pthread_cond_signal(cnd_t *cond);
int  pthread_cond_broadcast(cnd_t *cond);
int  pthread_cond_destroy(cnd_t *cond);

int  pthread_once(int *once_control, void (*init_routine)(void));

#endif /* _ZERO_CC_THREADS_H */
