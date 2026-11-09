/* zero_rt thread: C11 <threads.h> and minimal POSIX pthread support.
 *
 * Implements thread creation (clone), synchronization (futex-based mutex,
 * condvar, call_once), and thread termination.
 */
#include <stdint.h>
#include <stddef.h>
#include "rt_os.h"

typedef unsigned long thrd_t;

typedef union {
    char __size[40];
    long __align;
} mtx_t;

typedef union {
    char __size[48];
    long __align;
} cnd_t;

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

typedef struct ThreadControlBlock {
    int tid;              /* Cleared by kernel on exit via CLONE_CHILD_CLEARTID */
    int detached;         /* 1 if detached */
    int retval;           /* int return value */
    void *retval_ptr;     /* pointer return value for pthread */
    void *stack_base;
    uint64_t stack_size;
    int is_pthread;
    void *(*pthread_fn)(void *);
    int (*thrd_fn)(void *);
    void *arg;
} ThreadControlBlock;

static int thread_entry(void *arg) {
    ThreadControlBlock *tcb = (ThreadControlBlock *)arg;
    if (tcb->is_pthread) {
        tcb->retval_ptr = tcb->pthread_fn(tcb->arg);
        tcb->retval = (int)(intptr_t)tcb->retval_ptr;
    } else {
        tcb->retval = tcb->thrd_fn(tcb->arg);
        tcb->retval_ptr = (void *)(intptr_t)tcb->retval;
    }
    return tcb->retval;
}

#define THREAD_DEFAULT_STACK (1024 * 1024UL) /* 1 MiB */

static int create_thread_internal(thrd_t *res_thr, int is_pthread, void *fn, void *arg) {
    size_t stack_size = THREAD_DEFAULT_STACK;
    void *stack = zeroc_rt_mmap(0, stack_size, RT_PROT_READ | RT_PROT_WRITE,
                                RT_MAP_PRIVATE | RT_MAP_ANONYMOUS, -1, 0);
    if (stack == RT_MAP_FAILED || !stack) {
        return -1;
    }
    uintptr_t top = (uintptr_t)stack + stack_size;
    top -= sizeof(ThreadControlBlock);
    top &= ~15UL;
    ThreadControlBlock *tcb = (ThreadControlBlock *)top;
    tcb->tid = 0;
    tcb->detached = 0;
    tcb->retval = 0;
    tcb->retval_ptr = 0;
    tcb->stack_base = stack;
    tcb->stack_size = stack_size;
    tcb->is_pthread = is_pthread;
    if (is_pthread) {
        tcb->pthread_fn = (void *(*)(void *))fn;
        tcb->thrd_fn = 0;
    } else {
        tcb->thrd_fn = (int (*)(void *))fn;
        tcb->pthread_fn = 0;
    }
    tcb->arg = arg;

    unsigned long flags = RT_CLONE_VM | RT_CLONE_FS | RT_CLONE_FILES | RT_CLONE_SIGHAND |
                          RT_CLONE_THREAD | RT_CLONE_SYSVSEM | RT_CLONE_CHILD_CLEARTID |
                          RT_CLONE_PARENT_SETTID;

    long tid = zeroc_rt_clone(thread_entry, (void *)top, flags, tcb, &tcb->tid, 0, &tcb->tid);
    if (tid <= 0) {
        zeroc_rt_munmap(stack, stack_size);
        return -1;
    }
    if (res_thr) *res_thr = (thrd_t)(uintptr_t)tcb;
    return 0;
}

int thrd_create(thrd_t *thr, int (*func)(void *), void *arg) {
    int ret = create_thread_internal(thr, 0, (void *)func, arg);
    return ret == 0 ? thrd_success : thrd_error;
}

int pthread_create(thrd_t *thread, void *attr, void *(*start_routine)(void *), void *arg) {
    (void)attr;
    int ret = create_thread_internal(thread, 1, (void *)start_routine, arg);
    return ret == 0 ? 0 : 12 /* ENOMEM */;
}

int thrd_join(thrd_t thr, int *res) {
    ThreadControlBlock *tcb = (ThreadControlBlock *)(uintptr_t)thr;
    if (!tcb || tcb->detached) return thrd_error;
    while (__atomic_load_n(&tcb->tid, __ATOMIC_ACQUIRE) != 0) {
        zeroc_rt_futex_wait(&tcb->tid, tcb->tid, 0);
    }
    if (res) *res = tcb->retval;
    zeroc_rt_munmap(tcb->stack_base, tcb->stack_size);
    return thrd_success;
}

int pthread_join(thrd_t thread, void **retval) {
    ThreadControlBlock *tcb = (ThreadControlBlock *)(uintptr_t)thread;
    if (!tcb || tcb->detached) return 22 /* EINVAL */;
    while (__atomic_load_n(&tcb->tid, __ATOMIC_ACQUIRE) != 0) {
        zeroc_rt_futex_wait(&tcb->tid, tcb->tid, 0);
    }
    if (retval) *retval = tcb->retval_ptr;
    zeroc_rt_munmap(tcb->stack_base, tcb->stack_size);
    return 0;
}

int thrd_detach(thrd_t thr) {
    ThreadControlBlock *tcb = (ThreadControlBlock *)(uintptr_t)thr;
    if (!tcb) return thrd_error;
    tcb->detached = 1;
    return thrd_success;
}

int pthread_detach(thrd_t thread) {
    ThreadControlBlock *tcb = (ThreadControlBlock *)(uintptr_t)thread;
    if (!tcb) return 22 /* EINVAL */;
    tcb->detached = 1;
    return 0;
}

thrd_t thrd_current(void) {
    return (thrd_t)zeroc_rt_gettid();
}

thrd_t pthread_self(void) {
    return (thrd_t)zeroc_rt_gettid();
}

int thrd_equal(thrd_t a, thrd_t b) {
    return a == b;
}

int pthread_equal(thrd_t t1, thrd_t t2) {
    return t1 == t2;
}

void thrd_yield(void) {
    zeroc_rt_sched_yield();
}

int sched_yield(void) {
    return zeroc_rt_sched_yield();
}

void thrd_exit(int res) {
    zeroc_rt_exit(res);
}

void pthread_exit(void *retval) {
    zeroc_rt_exit((int)(intptr_t)retval);
}

int thrd_sleep(const void *duration, void *remaining) {
    return zeroc_rt_nanosleep(duration, remaining);
}

/* Mutex implementation */
typedef struct {
    int val;    /* 0 = unlocked, 1 = locked, 2 = contended */
    int type;   /* mtx_plain, mtx_recursive, mtx_timed */
    int owner;
    int count;
} RtMutex;

int mtx_init(mtx_t *mutex, int type) {
    RtMutex *m = (RtMutex *)mutex;
    m->val = 0;
    m->type = type;
    m->owner = 0;
    m->count = 0;
    return thrd_success;
}

int mtx_lock(mtx_t *mutex) {
    RtMutex *m = (RtMutex *)mutex;
    int tid = zeroc_rt_gettid();
    if (m->type == mtx_recursive && m->owner == tid) {
        m->count++;
        return thrd_success;
    }
    int expected = 0;
    if (__atomic_compare_exchange_n(&m->val, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        m->owner = tid;
        m->count = 1;
        return thrd_success;
    }
    int c = expected;
    if (c != 2) {
        c = __atomic_exchange_n(&m->val, 2, __ATOMIC_ACQUIRE);
    }
    while (c != 0) {
        zeroc_rt_futex_wait(&m->val, 2, 0);
        c = __atomic_exchange_n(&m->val, 2, __ATOMIC_ACQUIRE);
    }
    m->owner = tid;
    m->count = 1;
    return thrd_success;
}

int mtx_trylock(mtx_t *mutex) {
    RtMutex *m = (RtMutex *)mutex;
    int tid = zeroc_rt_gettid();
    if (m->type == mtx_recursive && m->owner == tid) {
        m->count++;
        return thrd_success;
    }
    int expected = 0;
    if (__atomic_compare_exchange_n(&m->val, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        m->owner = tid;
        m->count = 1;
        return thrd_success;
    }
    return thrd_busy;
}

int mtx_unlock(mtx_t *mutex) {
    RtMutex *m = (RtMutex *)mutex;
    if (m->type == mtx_recursive) {
        if (m->owner != zeroc_rt_gettid()) return thrd_error;
        if (--m->count > 0) return thrd_success;
    }
    m->owner = 0;
    m->count = 0;
    if (__atomic_fetch_sub(&m->val, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->val, 0, __ATOMIC_RELEASE);
        zeroc_rt_futex_wake(&m->val, 1);
    }
    return thrd_success;
}

void mtx_destroy(mtx_t *mutex) {
    (void)mutex;
}

int pthread_mutex_init(mtx_t *mutex, void *attr) {
    (void)attr;
    return mtx_init(mutex, mtx_plain);
}

int pthread_mutex_lock(mtx_t *mutex) {
    return mtx_lock(mutex) == thrd_success ? 0 : 22;
}

int pthread_mutex_trylock(mtx_t *mutex) {
    return mtx_trylock(mutex) == thrd_success ? 0 : 16 /* EBUSY */;
}

int pthread_mutex_unlock(mtx_t *mutex) {
    return mtx_unlock(mutex) == thrd_success ? 0 : 22;
}

int pthread_mutex_destroy(mtx_t *mutex) {
    mtx_destroy(mutex);
    return 0;
}

/* Condition variable implementation */
typedef struct {
    int seq;
} RtCond;

int cnd_init(cnd_t *cond) {
    RtCond *c = (RtCond *)cond;
    c->seq = 0;
    return thrd_success;
}

int cnd_wait(cnd_t *cond, mtx_t *mutex) {
    RtCond *c = (RtCond *)cond;
    int cur = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    mtx_unlock(mutex);
    zeroc_rt_futex_wait(&c->seq, cur, 0);
    mtx_lock(mutex);
    return thrd_success;
}

int cnd_signal(cnd_t *cond) {
    RtCond *c = (RtCond *)cond;
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    zeroc_rt_futex_wake(&c->seq, 1);
    return thrd_success;
}

int cnd_broadcast(cnd_t *cond) {
    RtCond *c = (RtCond *)cond;
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    zeroc_rt_futex_wake(&c->seq, 0x7fffffff);
    return thrd_success;
}

void cnd_destroy(cnd_t *cond) {
    (void)cond;
}

int pthread_cond_init(cnd_t *cond, void *attr) {
    (void)attr;
    return cnd_init(cond);
}

int pthread_cond_wait(cnd_t *cond, mtx_t *mutex) {
    return cnd_wait(cond, mutex);
}

int pthread_cond_signal(cnd_t *cond) {
    return cnd_signal(cond);
}

int pthread_cond_broadcast(cnd_t *cond) {
    return cnd_broadcast(cond);
}

int pthread_cond_destroy(cnd_t *cond) {
    cnd_destroy(cond);
    return 0;
}

/* call_once / pthread_once */
void call_once(int *flag, void (*func)(void)) {
    if (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (__atomic_compare_exchange_n(flag, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        func();
        __atomic_store_n(flag, 2, __ATOMIC_RELEASE);
        zeroc_rt_futex_wake(flag, 0x7fffffff);
    } else {
        while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != 2) {
            zeroc_rt_futex_wait(flag, 1, 0);
        }
    }
}

int pthread_once(int *once_control, void (*init_routine)(void)) {
    call_once(once_control, init_routine);
    return 0;
}
