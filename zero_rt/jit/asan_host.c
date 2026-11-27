/* Host-side AddressSanitizer (ASan) runtime for zerocc driver --run JIT (Sprint 256).
 *
 * Provides host-compiled definitions of shadow memory and report functions
 * resolved via dlsym(RTLD_DEFAULT) during JIT execution.
 */
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../core/san_fmt.h"
#include "../core/asan.h"

uintptr_t __asan_shadow_offset = 0;
int __asan_is_initialized = 0;

void __asan_init(void) {
    if (__asan_is_initialized) return;

    size_t shadow_sz = (size_t)64 * 1024 * 1024 * 1024ULL;
    #ifndef MAP_NORESERVE
    #define MAP_NORESERVE 0x4000
    #endif
    void *shadow_mem = mmap(NULL, shadow_sz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (shadow_mem == MAP_FAILED || shadow_mem == NULL) {
        shadow_sz = 64 * 1024 * 1024;
        shadow_mem = mmap(NULL, shadow_sz, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }

    if (shadow_mem != MAP_FAILED && shadow_mem != NULL) {
        uintptr_t ref_stack = (uintptr_t)&ref_stack;
        __asan_shadow_offset = (uintptr_t)shadow_mem + (shadow_sz / 2) - (ref_stack >> ASAN_SHADOW_SCALE);
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
    /* Poison left redzone */
    uintptr_t left_start = a - rz;
    for (uintptr_t p = (left_start & ~7ULL); p < a; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        *s = ASAN_STACK_LEFT_RZ;
    }
    /* Unpoison valid variable payload */
    uintptr_t end = a + size;
    for (uintptr_t p = a; p < end; p += 8) {
        uint8_t *s = (uint8_t *)((p >> ASAN_SHADOW_SCALE) + __asan_shadow_offset);
        if (p + 8 <= end) {
            *s = ASAN_VALID;
        } else {
            *s = (uint8_t)(end - p);
        }
    }
    /* Poison right redzone */
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

void __asan_report_error(void *addr, size_t size, int is_write, int poison_val) {
    const char *desc = "buffer-overflow";
    if (poison_val == (int)(uint8_t)ASAN_STACK_LEFT_RZ) desc = "stack-buffer-underflow";
    else if (poison_val == (int)(uint8_t)ASAN_STACK_MID_RZ) desc = "stack-buffer-overflow";
    else if (poison_val == (int)(uint8_t)ASAN_STACK_RIGHT_RZ) desc = "stack-buffer-overflow";
    else if (poison_val == (int)(uint8_t)ASAN_STACK_USE_AFTER_SCOPE) desc = "stack-use-after-scope";
    else if (poison_val == (int)(uint8_t)ASAN_HEAP_USE_AFTER_FREE) desc = "heap-use-after-free";
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

    write(2, buf, p - buf);
    _exit(1);
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
