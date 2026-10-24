/* stdatomic.h - C11 7.17 atomics for ZeroCC.
 *
 * Maps the standard atomic_T types and atomic_* macros onto ZeroCC's
 * __atomic_* builtin family (lowered to locked x86-64 atomics: lock xadd /
 * lock cmpxchg / lock xchg / lock or|and|xor / mfence). Memory orders are
 * accepted; on x86-64 (TSO) every locked RMW is seq_cst-strength, so the
 * order arguments are no-ops except for fences (OP_MFENCE).
 *
 * C11 requires atomic types for every signed/unsigned integer type. zerocc
 * tracks _Atomic as a type qualifier (TYPE_MOD_ATOMIC); the builtin lowering
 * derives the access width from the pointee type, so all widths (1/2/4/8)
 * are correct.
 */
#ifndef _ZERO_CC_STDATOMIC_H
#define _ZERO_CC_STDATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- C11 7.17.1: atomic integer types ---- */
typedef _Atomic bool           atomic_bool;
typedef _Atomic char           atomic_char;
typedef _Atomic signed char    atomic_schar;
typedef _Atomic unsigned char  atomic_uchar;
typedef _Atomic short          atomic_short;
typedef _Atomic unsigned short atomic_ushort;
typedef _Atomic int            atomic_int;
typedef _Atomic unsigned int   atomic_uint;
typedef _Atomic long           atomic_long;
typedef _Atomic unsigned long  atomic_ulong;
typedef _Atomic long long      atomic_llong;
typedef _Atomic unsigned long long atomic_ullong;
typedef _Atomic int8_t         atomic_int_least8_t;
typedef _Atomic uint8_t        atomic_uint_least8_t;
typedef _Atomic int16_t        atomic_int_least16_t;
typedef _Atomic uint16_t       atomic_uint_least16_t;
typedef _Atomic int32_t        atomic_int_least32_t;
typedef _Atomic uint32_t       atomic_uint_least32_t;
typedef _Atomic int64_t        atomic_int_least64_t;
typedef _Atomic uint64_t       atomic_uint_least64_t;
typedef _Atomic int_fast8_t    atomic_int_fast8_t;
typedef _Atomic uint_fast8_t   atomic_uint_fast8_t;
typedef _Atomic int_fast16_t   atomic_int_fast16_t;
typedef _Atomic uint_fast16_t  atomic_uint_fast16_t;
typedef _Atomic int_fast32_t   atomic_int_fast32_t;
typedef _Atomic uint_fast32_t  atomic_uint_fast32_t;
typedef _Atomic int_fast64_t   atomic_int_fast64_t;
typedef _Atomic uint_fast64_t  atomic_uint_fast64_t;
typedef _Atomic intptr_t       atomic_intptr_t;
typedef _Atomic uintptr_t      atomic_uintptr_t;
typedef _Atomic size_t         atomic_size_t;
typedef _Atomic ptrdiff_t      atomic_ptrdiff_t;

/* C11 7.17.6: atomic flags */
typedef _Atomic int atomic_flag;
#define ATOMIC_FLAG_INIT 0

/* ---- C11 7.17.1: initializers ---- */
#define ATOMIC_VAR_INIT(val) (val)
#define ATOMIC_INIT(val)     ATOMIC_VAR_INIT(val) /* C11: obsolescent */

#define atomic_init(obj, desired)          atomic_store(obj, desired)

/* ---- C11 7.17.2: lock-free query ---- */
#define atomic_is_lock_free(obj)           __atomic_is_lock_free(sizeof *(obj), 0)

/* GCC compatibility (2 = lock-free, 1 = available, 0 = unavailable) */
#define __GCC_ATOMIC_BOOL_LOCK_FREE        2
#define __GCC_ATOMIC_CHAR_LOCK_FREE        2
#define __GCC_ATOMIC_CHAR16_T_LOCK_FREE    2
#define __GCC_ATOMIC_CHAR32_T_LOCK_FREE    2
#define __GCC_ATOMIC_WCHAR_T_LOCK_FREE     2
#define __GCC_ATOMIC_SHORT_LOCK_FREE       2
#define __GCC_ATOMIC_INT_LOCK_FREE         2
#define __GCC_ATOMIC_LONG_LOCK_FREE        2
#define __GCC_ATOMIC_LLONG_LOCK_FREE       2
#define __GCC_ATOMIC_POINTER_LOCK_FREE     2
#define __GCC_ATOMIC_TEST_AND_SET_TRUEVAL  1

/* ---- C11 7.17.3: atomic operations ---- */
#define atomic_load(obj)                   __atomic_load_n(obj, __ATOMIC_SEQ_CST)
#define atomic_load_explicit(obj, order)   __atomic_load_n(obj, order)
#define atomic_store(obj, desired)         __atomic_store_n(obj, desired, __ATOMIC_SEQ_CST)
#define atomic_store_explicit(obj, desired, order) __atomic_store_n(obj, desired, order)
#define atomic_exchange(obj, desired)      __atomic_exchange_n(obj, desired, __ATOMIC_SEQ_CST)
#define atomic_exchange_explicit(obj, desired, order) __atomic_exchange_n(obj, desired, order)

#define atomic_compare_exchange_strong(obj, expected, desired) \
    __atomic_compare_exchange_n(obj, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)
#define atomic_compare_exchange_weak(obj, expected, desired) \
    atomic_compare_exchange_strong(obj, expected, desired)
#define atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure) \
    __atomic_compare_exchange_n(obj, expected, desired, 0, success, failure)
#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, failure) \
    atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure)

#define atomic_fetch_add(obj, arg)         __atomic_fetch_add(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(obj, arg, order) __atomic_fetch_add(obj, arg, order)
#define atomic_fetch_sub(obj, arg)         __atomic_fetch_sub(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_fetch_sub_explicit(obj, arg, order) __atomic_fetch_sub(obj, arg, order)
#define atomic_fetch_or(obj, arg)          __atomic_fetch_or(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_fetch_or_explicit(obj, arg, order) __atomic_fetch_or(obj, arg, order)
#define atomic_fetch_and(obj, arg)         __atomic_fetch_and(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_fetch_and_explicit(obj, arg, order) __atomic_fetch_and(obj, arg, order)
#define atomic_fetch_xor(obj, arg)         __atomic_fetch_xor(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_fetch_xor_explicit(obj, arg, order) __atomic_fetch_xor(obj, arg, order)

#define atomic_add_fetch(obj, arg)         __atomic_add_fetch(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_add_fetch_explicit(obj, arg, order) __atomic_add_fetch(obj, arg, order)
#define atomic_sub_fetch(obj, arg)         __atomic_sub_fetch(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_sub_fetch_explicit(obj, arg, order) __atomic_sub_fetch(obj, arg, order)
#define atomic_or_fetch(obj, arg)          __atomic_or_fetch(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_or_fetch_explicit(obj, arg, order) __atomic_or_fetch(obj, arg, order)
#define atomic_and_fetch(obj, arg)         __atomic_and_fetch(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_and_fetch_explicit(obj, arg, order) __atomic_and_fetch(obj, arg, order)
#define atomic_xor_fetch(obj, arg)         __atomic_xor_fetch(obj, arg, __ATOMIC_SEQ_CST)
#define atomic_xor_fetch_explicit(obj, arg, order) __atomic_xor_fetch(obj, arg, order)

/* ---- C11 7.17.6: atomic flags ---- */
#define atomic_flag_test_and_set(obj)      __atomic_test_and_set(obj)
#define atomic_flag_clear(obj)             __atomic_clear(obj)

#endif /* _ZERO_CC_STDATOMIC_H */
