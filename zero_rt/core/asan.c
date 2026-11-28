/* zero_rt AddressSanitizer (ASan) runtime implementation (Sprint 256).
 *
 * Provides shadow memory management, stack redzone poisoning/unpoisoning,
 * access checking, and diagnostic crash reporting for -fsanitize=address.
 */
#include "asan.h"
#include <stdint.h>
#include <stddef.h>
#include "san_fmt.h"
#include "rt_os.h"

#ifndef RT_MAP_NORESERVE
#define RT_MAP_NORESERVE 0x4000
#endif

uintptr_t __asan_shadow_offset = 0;
int __asan_is_initialized = 0;

void __asan_init(void) {
    if (__asan_is_initialized) return;

#if defined(__LP64__) || defined(_LP64) || defined(__x86_64__) || defined(__aarch64__) || (defined(__riscv_xlen) && (__riscv_xlen == 64))
    size_t shadow_sz = (size_t)16 * 1024 * 1024 * 1024 * 1024ULL; /* 16 TB virtual address space */
    void *shadow_mem = zeroc_rt_mmap((void *)0, shadow_sz, RT_PROT_READ | RT_PROT_WRITE,
                                     RT_MAP_PRIVATE | RT_MAP_ANONYMOUS | RT_MAP_NORESERVE, -1, 0);
    if (shadow_mem == (void *)-1 || shadow_mem == NULL) {
        shadow_sz = (size_t)2 * 1024 * 1024 * 1024 * 1024ULL; /* 2 TB fallback */
        shadow_mem = zeroc_rt_mmap((void *)0, shadow_sz, RT_PROT_READ | RT_PROT_WRITE,
                                   RT_MAP_PRIVATE | RT_MAP_ANONYMOUS | RT_MAP_NORESERVE, -1, 0);
    }
    if (shadow_mem == (void *)-1 || shadow_mem == NULL) {
        shadow_sz = 512 * 1024 * 1024;
        shadow_mem = zeroc_rt_mmap((void *)0, shadow_sz, RT_PROT_READ | RT_PROT_WRITE,
                                   RT_MAP_PRIVATE | RT_MAP_ANONYMOUS, -1, 0);
    }
#else
    size_t sz_list[] = { 512 * 1024 * 1024, 256 * 1024 * 1024, 128 * 1024 * 1024 };
    void *shadow_mem = (void *)-1;
    for (size_t k = 0; k < sizeof(sz_list)/sizeof(sz_list[0]); k++) {
        shadow_mem = zeroc_rt_mmap((void *)0, sz_list[k], RT_PROT_READ | RT_PROT_WRITE,
                                   RT_MAP_PRIVATE | RT_MAP_ANONYMOUS, -1, 0);
        if (shadow_mem != (void *)-1 && shadow_mem != NULL) break;
    }
#endif

    if (shadow_mem != (void *)-1 && shadow_mem != NULL) {
        __asan_shadow_offset = (uintptr_t)shadow_mem;
    }
    __asan_is_initialized = 1;
}

void __asan_poison_memory_region(void const volatile *addr, size_t size, uint8_t val) {
    if (!__asan_is_initialized) __asan_init();
    uintptr_t a = (uintptr_t)addr;
    uintptr_t end = a + size;
    for (uintptr_t p = (a & ~7ULL); p < end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        if (p >= a && p + 8 <= end) {
            *s = val;
        } else if (p < a) {
            size_t valid = a - p;
            if (*s == 0 || *s > (uint8_t)valid) {
                *s = (uint8_t)valid;
            }
        } else {
            size_t valid = end - p;
            *s = (uint8_t)valid;
        }
    }
}

void __asan_unpoison_memory_region(void const volatile *addr, size_t size) {
    if (!__asan_is_initialized) return;
    uintptr_t a = (uintptr_t)addr;
    uintptr_t end = a + size;
    for (uintptr_t p = (a & ~7ULL); p < end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        *s = ASAN_VALID;
    }
}

void __asan_poison_stack_var(void const volatile *addr, size_t size, size_t rz) {
    if (!__asan_is_initialized) __asan_init();
    uintptr_t a = (uintptr_t)addr;
    /* Poison left redzone [a - rz, a) with ASAN_STACK_LEFT_RZ */
    uintptr_t left_start = a - rz;
    for (uintptr_t p = (left_start & ~7ULL); p < a; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        *s = ASAN_STACK_LEFT_RZ;
    }
    /* Unpoison valid variable payload [a, a + size) */
    uintptr_t end = a + size;
    for (uintptr_t p = a; p < end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        if (p + 8 <= end) {
            *s = ASAN_VALID;
        } else {
            *s = (uint8_t)(end - p);
        }
    }
    /* Poison right redzone [end, end + rz) with ASAN_STACK_RIGHT_RZ */
    uintptr_t rz_end = end + rz;
    for (uintptr_t p = ((end + 7) & ~7ULL); p < rz_end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        *s = ASAN_STACK_RIGHT_RZ;
    }
}

void __asan_unpoison_stack_frame(void const volatile *frame_bottom, size_t frame_size) {
    if (!__asan_is_initialized) return;
    uintptr_t start = (uintptr_t)frame_bottom & ~7ULL;
    uintptr_t end = (uintptr_t)frame_bottom + frame_size;
    for (uintptr_t p = start; p < end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        *s = ASAN_VALID;
    }
}

void __asan_poison_global_var(void const volatile *addr, size_t size, size_t rz) {
    if (!__asan_is_initialized) __asan_init();
    uintptr_t a = (uintptr_t)addr;
    /* Unpoison valid payload [a, a + size) */
    uintptr_t end = a + size;
    for (uintptr_t p = (a & ~7ULL); p < end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        if (p + 8 <= end) {
            *s = ASAN_VALID;
        } else {
            *s = (uint8_t)(end - p);
        }
    }
    /* Poison right redzone [end, end + rz) with ASAN_GLOBAL_RZ */
    uintptr_t rz_end = end + rz;
    for (uintptr_t p = ((end + 7) & ~7ULL); p < rz_end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        *s = ASAN_GLOBAL_RZ;
    }
}

void __asan_register_globals(struct __asan_global const *globals, size_t n) {
    if (!globals || n == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (!globals[i].beg) continue;
        size_t rz = globals[i].size_with_redzone > globals[i].size ?
                    (globals[i].size_with_redzone - globals[i].size) : 32;
        __asan_poison_global_var((void *)globals[i].beg, globals[i].size, rz);
    }
}

void __asan_unregister_globals(struct __asan_global const *globals, size_t n) {
    if (!globals || n == 0 || !__asan_is_initialized) return;
    for (size_t i = 0; i < n; i++) {
        if (!globals[i].beg) continue;
        size_t total = globals[i].size_with_redzone > globals[i].size ?
                       globals[i].size_with_redzone : (globals[i].size + 32);
        __asan_unpoison_memory_region((void *)globals[i].beg, total);
    }
}

void __asan_report_error(void *addr, size_t size, int is_write, int poison_val) {
    const char *desc = "buffer-overflow";
    if (poison_val == (int)(uint8_t)ASAN_STACK_LEFT_RZ) desc = "stack-buffer-underflow";
    else if (poison_val == (int)(uint8_t)ASAN_STACK_MID_RZ) desc = "stack-buffer-overflow";
    else if (poison_val == (int)(uint8_t)ASAN_STACK_RIGHT_RZ) desc = "stack-buffer-overflow";
    else if (poison_val == (int)(uint8_t)ASAN_STACK_USE_AFTER_SCOPE) desc = "stack-use-after-scope";
    else if (poison_val == (int)(uint8_t)ASAN_HEAP_USE_AFTER_FREE) desc = "heap-use-after-free";
    else if (poison_val == (int)(uint8_t)ASAN_GLOBAL_RZ) desc = "global-buffer-overflow";
    else if (poison_val > 0 && poison_val < 8) desc = "stack-buffer-overflow";

    char buf[512];
    char *p = buf;
    p = san_fmt_str(p, "=================================================================\n");
    p = san_fmt_str(p, "==ERROR: AddressSanitizer: ");
    p = san_fmt_str(p, desc);
    p = san_fmt_str(p, " on address 0x");
    p = san_fmt_hex(p, (uintptr_t)addr);
    p = san_fmt_str(p, "\n");
    p = san_fmt_str(p, is_write ? "WRITE of size " : "READ of size ");
    p = san_fmt_uint(p, (unsigned)size);
    p = san_fmt_str(p, " at 0x");
    p = san_fmt_hex(p, (uintptr_t)addr);
    p = san_fmt_str(p, " thread T0\n");
    p = san_fmt_str(p, "=================================================================\n");

    zeroc_rt_write(2, buf, p - buf);
    zeroc_rt_exit(1);
}

void __asan_check_memory_access(void const volatile *addr, size_t size, int is_write) {
    if (!__asan_is_initialized) __asan_init();
    uintptr_t a = (uintptr_t)addr;
    uint8_t *s = (uint8_t *)((a >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
    uint8_t k = *s;
    if (k != 0) {
        int8_t sk = (int8_t)k;
        if (sk < 0 || ((a & 7) + size > (size_t)k)) {
            __asan_report_error((void *)a, size, is_write, (int)k);
        }
    }
}
