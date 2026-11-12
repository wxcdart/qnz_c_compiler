/* zero_rt heap: brk-backed first-fit allocator.
 *
 * Layout (arena start is 16-byte aligned, so every header is aligned):
 *
 *   [ hdr: size|FREE, prev_size (16 B) ][ payload (16 B aligned) ][ next ... ]
 *
 * - size: payload size in bits 0-62, free flag in bit 63.
 * - prev_size: the previous block's payload size (enables backward
 *   coalescing without a free list).
 * - The last block is always a free "sentinel" that reaches the end of
 *   the arena; growing the arena extends it.
 *
 * Guarantees: 16-byte payload alignment, first-fit allocation, full
 * forward + backward coalescing on free. No threads, no mprotect, no
 * mmap - Phase 0 scope.
 *
 * -fsanitize=leak (Sprint 169): the header bookkeeping IS the leak
 * detector - at exit, every block whose header still lacks the FREE bit
 * was still allocated. No per-malloc instrumentation (LSan-grade
 * reachability is ASan-class and out of scope, OKF §4.3).
 */
#include <stdint.h>
#include <stdlib.h>
#include "rt_os.h"
#include "san_fmt.h"

#define RT_ALIGN        16
#define RT_FREE_BIT     ((uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1))
#define RT_HDR_SIZE     16
#define RT_MIN_PAYLOAD  16
#define RT_PAGE         4096UL
#define RT_INIT_ARENA   (256 * 1024UL)
#define RT_GROWTH       (1024 * 1024UL)

typedef struct {
    uintptr_t size;       /* payload size | RT_FREE_BIT */
    uintptr_t prev_size;  /* previous block's payload size */
#if __SIZEOF_POINTER__ == 4 || (defined(__riscv) && __riscv_xlen == 32)
    uintptr_t _pad[2];
#endif
} RtHdr;

static uint8_t   *g_lo = 0;   /* arena start */
static uintptr_t  g_hi = 0;   /* arena end (one past) */
static int        g_ready = 0;
static int        g_heap_lock = 0;

static void heap_lock(void) {
    while (1) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_heap_lock, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        zeroc_rt_futex_wait(&g_heap_lock, 1, 0);
    }
}

static void heap_unlock(void) {
    __atomic_store_n(&g_heap_lock, 0, __ATOMIC_RELEASE);
    zeroc_rt_futex_wake(&g_heap_lock, 1);
}

static int rt_free_flag(const RtHdr *h) { return (h->size & RT_FREE_BIT) != 0; }
static uintptr_t rt_size(const RtHdr *h) { return h->size & ~RT_FREE_BIT; }

static int rt_arena_init(void) {
    uintptr_t base = zeroc_rt_brk(0);
    if (base == 0 || ((intptr_t)base < 0 && (intptr_t)base >= -4095)) return -1;
    uintptr_t start = (base + RT_PAGE - 1) & ~(RT_PAGE - 1);
    uintptr_t end = start + RT_INIT_ARENA;
    if (zeroc_rt_brk(end) != end) return -1;
    g_lo = (uint8_t *)start;
    g_hi = end;
    RtHdr *h = (RtHdr *)g_lo;
    h->size = (end - start - RT_HDR_SIZE) | RT_FREE_BIT;
    h->prev_size = 0;
    g_ready = 1;
    return 0;
}

/* The sentinel is the block whose payload ends exactly at g_hi; it is
 * always free. Extending it absorbs freshly brk'ed space. */
static RtHdr *rt_sentinel(void) {
    for (uint8_t *p = g_lo; ; ) {
        RtHdr *h = (RtHdr *)p;
        uint8_t *end = (uint8_t *)(h + 1) + rt_size(h);
        if (end >= (uint8_t *)g_hi) return h;
        p = end; /* the next block's header starts right after the payload */
    }
}

static void *rt_alloc(uintptr_t need) {
    if (!g_ready && rt_arena_init() != 0) return 0;
    for (;;) {
        for (uint8_t *p = g_lo; ; ) {
            RtHdr *h = (RtHdr *)p;
            uintptr_t sz = rt_size(h);
            uint8_t *end = (uint8_t *)(h + 1) + sz;
            if (rt_free_flag(h) && sz >= need) {
                if (sz >= need + RT_HDR_SIZE + RT_MIN_PAYLOAD) {
                    /* Split off the requested prefix, keep the rest free. */
                    RtHdr *rest = (RtHdr *)((uint8_t *)(h + 1) + need);
                    uintptr_t rest_sz = sz - need - RT_HDR_SIZE;
                    h->size = need;
                    rest->size = rest_sz | RT_FREE_BIT;
                    rest->prev_size = need;
                    if (end < (uint8_t *)g_hi) {
                        RtHdr *nxt = (RtHdr *)end;
                        nxt->prev_size = rest_sz;
                    }
                } else {
                    h->size = sz; /* use the whole block */
                }
                return (void *)(h + 1);
            }
            if (end >= (uint8_t *)g_hi) break; /* end of arena */
            p = end; /* next block's header starts right after the payload */
        }
        /* No fit: grow the arena (the sentinel is always free). */
        RtHdr *sent = rt_sentinel();
        uintptr_t new_hi = g_hi + RT_GROWTH;
        if (((intptr_t)new_hi < 0 && (intptr_t)new_hi >= -4095) || zeroc_rt_brk(new_hi) != new_hi) return 0;
        sent->size += RT_GROWTH;
        g_hi = new_hi;
    }
}

void *malloc(size_t n) {
    uintptr_t need = (n + (RT_ALIGN - 1)) & ~(uintptr_t)(RT_ALIGN - 1);
    if (need < RT_MIN_PAYLOAD) need = RT_MIN_PAYLOAD;
    heap_lock();
    void *p = rt_alloc(need);
    heap_unlock();
    return p;
}

void *calloc(size_t nmemb, size_t size) {
    /* Overflow-safe: reject when nmemb * size would wrap. */
    if (size != 0 && nmemb > (size_t)-1 / size) return 0;
    void *p = malloc(nmemb * size);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        size_t i;
        for (i = 0; i < nmemb * size; i++) b[i] = 0;
    }
    return p;
}

static void rt_free_unlocked(void *ptr) {
    if (!ptr) return;
    if (!g_ready) return; /* heap was never initialized - nothing to free */
    if ((uint8_t *)ptr < g_lo || (uint8_t *)ptr >= (uint8_t *)g_hi) {
        abort(); /* not from this heap */
    }
    RtHdr *h = (RtHdr *)ptr - 1;
    if (rt_free_flag(h)) abort(); /* double free */
    h->size |= RT_FREE_BIT;

    /* Forward coalesce: merge the next block if it is free. */
    uint8_t *next = (uint8_t *)(h + 1) + rt_size(h);
    if (next < (uint8_t *)g_hi) {
        RtHdr *nh = (RtHdr *)next;
        if (rt_free_flag(nh)) {
            h->size += RT_HDR_SIZE + rt_size(nh);
        }
    }
    /* Backward coalesce: merge the previous block if it is free. */
    if (h->prev_size != 0) {
        RtHdr *ph = (RtHdr *)((uint8_t *)h - h->prev_size);
        if (ph >= (RtHdr *)g_lo && rt_free_flag(ph)) {
            ph->size += RT_HDR_SIZE + rt_size(h);
            ph->prev_size = h->prev_size;
        }
    }
}

void free(void *ptr) {
    heap_lock();
    rt_free_unlocked(ptr);
    heap_unlock();
}

void *realloc(void *ptr, size_t n) {
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return 0; }
    uintptr_t need = (n + (RT_ALIGN - 1)) & ~(uintptr_t)(RT_ALIGN - 1);
    if (need < RT_MIN_PAYLOAD) need = RT_MIN_PAYLOAD;
    heap_lock();
    RtHdr *h = (RtHdr *)ptr - 1;
    if (rt_size(h) >= need) {
        /* Shrink: split off the tail if worthwhile. */
        uintptr_t sz = rt_size(h);
        if (sz >= need + RT_HDR_SIZE + RT_MIN_PAYLOAD) {
            RtHdr *rest = (RtHdr *)((uint8_t *)(h + 1) + need);
            uintptr_t rest_sz = sz - need - RT_HDR_SIZE;
            h->size = need;
            rest->size = rest_sz | RT_FREE_BIT;
            rest->prev_size = need;
            if ((uint8_t *)(rest + 1) + rest_sz < (uint8_t *)g_hi) {
                RtHdr *nxt = (RtHdr *)((uint8_t *)(rest + 1) + rest_sz);
                nxt->prev_size = rest_sz;
            }
        }
        heap_unlock();
        return ptr;
    }
    void *np = rt_alloc(need);
    if (!np) {
        heap_unlock();
        return 0;
    }
    size_t copy = n < rt_size(h) ? n : (size_t)rt_size(h);
    uint8_t *d = (uint8_t *)np;
    uint8_t *s = (uint8_t *)ptr;
    size_t i;
    for (i = 0; i < copy; i++) d[i] = s[i];
    rt_free_unlocked(ptr);
    heap_unlock();
    return np;
}

/* ------------------------------------------------------------------ */
/* -fsanitize=leak (Sprint 169): atexit "still allocated" report.      */
/* Freestanding-only - the report is only compiled into / reachable    */
/* when this heap is the program's allocator (zero_rt link).          */
/* ------------------------------------------------------------------ */

#define RT_LEAK_MAX_IGNORE 64
static int  g_leak_on = 0;
static int  g_leak_registered = 0;
static void *g_leak_ignore[RT_LEAK_MAX_IGNORE];
static int   g_leak_ignore_count = 0;

void zeroc_rt_leak_ignore(void *ptr) {
    int i;
    if (!ptr || g_leak_ignore_count >= RT_LEAK_MAX_IGNORE) return;
    for (i = 0; i < g_leak_ignore_count; i++)
        if (g_leak_ignore[i] == ptr) return;
    g_leak_ignore[g_leak_ignore_count++] = ptr;
}

static int rt_leak_ignored(const void *ptr) {
    int i;
    for (i = 0; i < g_leak_ignore_count; i++)
        if (g_leak_ignore[i] == (const void *)ptr) return 1;
    return 0;
}

/* One arena walk: count (or print) the live, non-ignored blocks. */
static void rt_leak_walk(int count_only, size_t *total, size_t *blocks) {
    uint8_t *p;
    if (!g_ready) return;
    for (p = g_lo; ; ) {
        RtHdr *h = (RtHdr *)p;
        uint8_t *end = (uint8_t *)(h + 1) + rt_size(h);
        if (!rt_free_flag(h) && !rt_leak_ignored(h + 1)) {
            if (count_only) {
                *total += rt_size(h);
                (*blocks)++;
            } else {
                char line[96];
                char *q = san_fmt_str(line, "zerocc: leak:   at 0x");
                q = san_fmt_hex(q, (uintptr_t)(h + 1));
                q = san_fmt_str(q, " (");
                q = san_fmt_uint(q, (unsigned)rt_size(h));
                q = san_fmt_str(q, " bytes)\n");
                zeroc_rt_write(2, line, (uint64_t)(q - line));
            }
        }
        if (end >= (uint8_t *)g_hi) break;
        p = end;
    }
}

static void rt_leak_report_atexit(void) {
    size_t total = 0, blocks = 0;
    char buf[96];
    char *e;
    if (!g_leak_on) return;
    rt_leak_walk(1, &total, &blocks);
    if (blocks == 0) return;
    e = san_fmt_str(buf, "zerocc: leak: ");
    e = san_fmt_uint(e, (unsigned)blocks);
    e = san_fmt_str(e, " block(s), ");
    e = san_fmt_uint(e, (unsigned)total);
    e = san_fmt_str(e, " byte(s) still allocated at exit\n");
    zeroc_rt_write(2, buf, (uint64_t)(e - buf));
    rt_leak_walk(0, &total, &blocks);
    /* LSan's exit code for "leaks detected". exit() applies it only when
     * the program itself would exit 0 (a real failure status is kept). */
    zeroc_rt_set_exit_override(23);
}

void zeroc_rt_leak_report_set(int on) {
    g_leak_on = on ? 1 : 0;
    if (g_leak_on && !g_leak_registered) {
        atexit(rt_leak_report_atexit);
        g_leak_registered = 1;
    }
}
