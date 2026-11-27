/* zero_rt AddressSanitizer (ASan) runtime (Sprint 256).
 *
 * Provides shadow memory management, stack redzone poisoning/unpoisoning,
 * access checking, and diagnostic crash reporting for -fsanitize=address.
 */
#ifndef ZERO_RT_ASAN_H
#define ZERO_RT_ASAN_H

#include <stdint.h>
#include <stddef.h>

#define ASAN_SHADOW_SCALE 3

#define ASAN_VALID                 0x00
#define ASAN_STACK_LEFT_RZ         0xF1
#define ASAN_STACK_MID_RZ          0xF2
#define ASAN_STACK_RIGHT_RZ        0xF3
#define ASAN_STACK_USE_AFTER_RET   0xF5
#define ASAN_STACK_USE_AFTER_SCOPE 0xF8
#define ASAN_GLOBAL_RZ             0xFA
#define ASAN_HEAP_USE_AFTER_FREE   0xFD

extern uintptr_t __asan_shadow_offset;
extern int __asan_is_initialized;

void __asan_init(void);
void __asan_poison_memory_region(void const volatile *addr, size_t size, uint8_t val);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
void __asan_poison_stack_var(void const volatile *addr, size_t size, size_t rz);
void __asan_unpoison_stack_frame(void const volatile *frame_bottom, size_t frame_size);
void __asan_check_memory_access(void const volatile *addr, size_t size, int is_write);
void __asan_report_error(void *addr, size_t size, int is_write, int poison_val);

#endif /* ZERO_RT_ASAN_H */
