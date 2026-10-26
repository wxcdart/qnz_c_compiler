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
 */
#include <stdint.h>
#include <stdlib.h>
#include "rt_os.h"

#define RT_ALIGN        16
#define RT_FREE_BIT     (1ULL << 63)
#define RT_HDR_SIZE     (2 * sizeof(uint64_t))
#define RT_MIN_PAYLOAD  16
#define RT_PAGE         4096ULL
#define RT_INIT_ARENA   (256 * 1024ULL)
#define RT_GROWTH       (1024 * 1024ULL)

typedef struct {
    uint64_t size;       /* payload size | RT_FREE_BIT */
    uint64_t prev_size;  /* previous block's payload size */
} RtHdr;

static uint8_t  *g_lo = 0;   /* arena start */
static uint64_t  g_hi = 0;   /* arena end (one past) */
static int       g_ready = 0;

static int rt_free_flag(const RtHdr *h) { return (h->size & RT_FREE_BIT) != 0; }
static uint64_t rt_size(const RtHdr *h) { return h->size & ~RT_FREE_BIT; }

static int rt_arena_init(void) {
    uint64_t base = zeroc_rt_brk(0);
    if (base == 0 || base > (1ULL << 40)) return -1;
    uint64_t start = (base + RT_PAGE - 1) & ~(RT_PAGE - 1);
    uint64_t end = start + RT_INIT_ARENA;
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

static void *rt_alloc(uint64_t need) {
    if (!g_ready && rt_arena_init() != 0) return 0;
    for (;;) {
        for (uint8_t *p = g_lo; ; ) {
            RtHdr *h = (RtHdr *)p;
            uint64_t sz = rt_size(h);
            uint8_t *end = (uint8_t *)(h + 1) + sz;
            if (rt_free_flag(h) && sz >= need) {
                if (sz >= need + RT_HDR_SIZE + RT_MIN_PAYLOAD) {
                    /* Split off the requested prefix, keep the rest free. */
                    RtHdr *rest = (RtHdr *)((uint8_t *)(h + 1) + need);
                    uint64_t rest_sz = sz - need - RT_HDR_SIZE;
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
        uint64_t new_hi = g_hi + RT_GROWTH;
        if (new_hi > (1ULL << 40) || zeroc_rt_brk(new_hi) != new_hi) return 0;
        sent->size += RT_GROWTH;
        g_hi = new_hi;
    }
}

void *malloc(size_t n) {
    uint64_t need = (n + (RT_ALIGN - 1)) & ~(uint64_t)(RT_ALIGN - 1);
    if (need < RT_MIN_PAYLOAD) need = RT_MIN_PAYLOAD;
    return rt_alloc(need);
}

void *calloc(size_t nmemb, size_t size) {
    /* Overflow-safe: reject when nmemb * size would wrap. */
    if (size != 0 && nmemb > (1ULL << 63) / size) return 0;
    void *p = malloc(nmemb * size);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        size_t i;
        for (i = 0; i < nmemb * size; i++) b[i] = 0;
    }
    return p;
}

void free(void *ptr) {
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

void *realloc(void *ptr, size_t n) {
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return 0; }
    uint64_t need = (n + (RT_ALIGN - 1)) & ~(uint64_t)(RT_ALIGN - 1);
    if (need < RT_MIN_PAYLOAD) need = RT_MIN_PAYLOAD;
    RtHdr *h = (RtHdr *)ptr - 1;
    if (rt_size(h) >= need) {
        /* Shrink: split off the tail if worthwhile. */
        uint64_t sz = rt_size(h);
        if (sz >= need + RT_HDR_SIZE + RT_MIN_PAYLOAD) {
            RtHdr *rest = (RtHdr *)((uint8_t *)(h + 1) + need);
            uint64_t rest_sz = sz - need - RT_HDR_SIZE;
            h->size = need;
            rest->size = rest_sz | RT_FREE_BIT;
            rest->prev_size = need;
            if ((uint8_t *)(rest + 1) + rest_sz < (uint8_t *)g_hi) {
                RtHdr *nxt = (RtHdr *)((uint8_t *)(rest + 1) + rest_sz);
                nxt->prev_size = rest_sz;
            }
        }
        return ptr;
    }
    void *np = malloc(n);
    if (!np) return 0;
    size_t copy = n < rt_size(h) ? n : (size_t)rt_size(h);
    uint8_t *d = (uint8_t *)np;
    uint8_t *s = (uint8_t *)ptr;
    size_t i;
    for (i = 0; i < copy; i++) d[i] = s[i];
    free(ptr);
    return np;
}
