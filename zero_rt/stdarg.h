/* zero_rt System V AMD64 ABI stdarg.h
 *
 * This provides the standard System V AMD64 va_list layout for zero_rt's
 * printf/scanf functions, which are called from zerocc-compiled code that
 * follows the System V AMD64 ABI (args passed in registers, caller creates
 * register save area in its own frame).
 *
 * The builtin zerocc stdarg.h defines a custom 5-pointer va_list that expects
 * the save area in the CALLEE's frame (created by the callee's prologue).
 * That works for zerocc-to-zerocc calls but breaks when zerocc calls
 * zero_rt's variadic functions, because the caller (zerocc-compiled) follows
 * System V ABI and the callee's prologue doesn't see the caller's save area.
 */

#ifndef ZERO_RT_STDARG_H
#define ZERO_RT_STDARG_H 1

#ifndef _STDARG_H
#define _STDARG_H 1

#include <stddef.h>

/* System V AMD64 va_list structure:
 * The va_list is an array of one structure containing:
 *   - gp_offset: offset to next integer register save slot (from base)
 *   - fp_offset: offset to next FP register save slot (from base)
 *   - overflow_arg_area: pointer to stack overflow area
 *   - reg_save_area: pointer to register save area base
 */
typedef struct {
    unsigned int gp_offset;
    unsigned int fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
} __va_list_tag;

typedef __va_list_tag va_list[1];

/* Manual implementation of va_start for System V AMD64 ABI.
 *
 * The caller passes the first 6 integer args in RDI, RSI, RDX, RCX, R8, R9
 * and the first 8 FP args in XMM0-XMM7. The caller also allocates a register
 * save area on the stack and stores these registers there.
 *
 * The address of the register save area is at a fixed offset from the
 * frame pointer. We compute it by taking the address of the last named
 * parameter and finding the save area relative to it.
 *
 * For a function with N named parameters, the register save area starts at
 * the stack slot corresponding to parameter N (0-indexed). But the exact
 * offset depends on how many of the named parameters were passed in registers.
 *
 * A simpler approach: the caller places the register save area at a known
 * location relative to the stack pointer at the call site. In the callee,
 * the register save area is at a fixed offset from RBP (the frame pointer).
 *
 * On x86-64 System V, the register save area is at RBP - 176 (or similar).
 * The exact offset depends on the function's frame layout.
 *
 * We use the GCC-compatible approach: __builtin_va_start would normally
 * compute this, but since zerocc's builtin uses a different ABI, we
 * implement our own by finding the register save area relative to the
 * last named argument.
 */

#define __va_start_sysv(ap, last) \
    do { \
        __va_list_tag *__ap = (ap); \
        /* The register save area is 176 bytes below the address of the \
         * last named argument (which is at a fixed offset from RBP). \
         * The last named argument's address gives us the frame pointer. */ \
        char *__last_addr = (char *)&(last); \
        /* In System V AMD64, the register save area is at a fixed offset \
         * from the frame pointer. For a typical function, the save area \
         * is at RBP - 176. The last named parameter is at RBP - X. \
         * We approximate by assuming the save area is 176 bytes below \
         * the last parameter's stack slot. */ \
        __ap->reg_save_area = __last_addr - 176; \
        __ap->gp_offset = 0; \
        __ap->fp_offset = 48;  /* FP save area starts after 6 GP slots (48 bytes) */ \
        __ap->overflow_arg_area = __last_addr + 8;  /* Stack args start after last param */ \
    } while (0)

#define __va_arg_sysv(ap, type) \
    ({ \
        __va_list_tag *__ap = (ap); \
        void *__ptr; \
        size_t __size = sizeof(type); \
        size_t __align = __alignof__(type); \
        \
        if (__size > 8) __size = 8; /* We only handle up to 8-byte types for now */ \
        \
        /* Check if type is float/double (FP) */ \
        int __is_fp = 0; \
        if (__builtin_types_compatible_p(type, float) || \
            __builtin_types_compatible_p(type, double)) { \
            __is_fp = 1; \
        } \
        \
        if (__is_fp) { \
            if (__ap->fp_offset < 176) { /* 48 + 8*16 = 176 */ \
                __ptr = (char *)__ap->reg_save_area + __ap->fp_offset; \
                __ap->fp_offset += 16; /* FP slots are 16 bytes each */ \
            } else { \
                /* Align overflow_arg_area */ \
                __ap->overflow_arg_area = (char *)(((uintptr_t)__ap->overflow_arg_area + __align - 1) & ~(__align - 1)); \
                __ptr = __ap->overflow_arg_area; \
                __ap->overflow_arg_area = (char *)__ap->overflow_arg_area + __size; \
            } \
        } else { \
            if (__ap->gp_offset < 48) { \
                __ptr = (char *)__ap->reg_save_area + __ap->gp_offset; \
                __ap->gp_offset += 8; \
            } else { \
                __ap->overflow_arg_area = (char *)(((uintptr_t)__ap->overflow_arg_area + __align - 1) & ~(__align - 1)); \
                __ptr = __ap->overflow_arg_area; \
                __ap->overflow_arg_area = (char *)__ap->overflow_arg_area + __size; \
            } \
        } \
        *(type *)__ptr; \
    })

#define va_start(ap, last) __va_start_sysv(ap, last)
#define va_arg(ap, type) __va_arg_sysv(ap, type)
#define va_end(ap) ((void)0)
#define va_copy(dest, src) ((void)((*(dest)) = *(src)))

#endif /* _STDARG_H */

#endif /* ZERO_RT_STDARG_H */
