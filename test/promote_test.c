#include <stdio.h>
#include <assert.h>
#include "zero_cc/utils/arena.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/promote.h"
#include "zero_cc/semantic/diagnostics.h"

int main(void) {
    Arena *perm = arena_create(1024 * 1024);
    Arena *tmp = arena_create(1024 * 1024);
    assert(perm != NULL && tmp != NULL);

    DiagBuf diag;
    diag_init(&diag, tmp);

    /* Types setup */
    TypeInfo *t_void = type_void(perm);
    TypeInfo *t_bool = type_bool(perm);
    TypeInfo *t_char = type_char(perm);
    TypeInfo *t_uchar = type_uchar(perm);
    TypeInfo *t_short = type_short(perm);
    TypeInfo *t_ushort = type_ushort(perm);
    TypeInfo *t_int = type_int(perm);
    TypeInfo *t_uint = type_uint(perm);
    TypeInfo *t_long = type_long(perm);
    TypeInfo *t_ulong = type_ulong(perm);
    TypeInfo *t_float = type_float(perm);
    TypeInfo *t_double = type_double(perm);

    /* 1. Integer Promotions: small types promote to signed int */
    assert(type_equal(type_promote(perm, t_bool), t_int));
    assert(type_equal(type_promote(perm, t_char), t_int));
    assert(type_equal(type_promote(perm, t_uchar), t_int));
    assert(type_equal(type_promote(perm, t_short), t_int));
    assert(type_equal(type_promote(perm, t_ushort), t_int));
    assert(type_equal(type_promote(perm, t_int), t_int));
    assert(type_equal(type_promote(perm, t_uint), t_uint));
    assert(type_equal(type_promote(perm, t_long), t_long));
    assert(type_equal(type_promote(perm, t_ulong), t_ulong));
    assert(type_equal(type_promote(perm, t_float), t_float));
    assert(type_equal(type_promote(perm, t_double), t_double));

    /* promote_unary alias */
    assert(type_equal(promote_unary(perm, t_char), t_int));

    /* 2. Common Arithmetic Types (usual arithmetic conversions) */
    /* Floating point rules */
    assert(type_equal(type_common_arithmetic_type(perm, t_double, t_int), t_double));
    assert(type_equal(type_common_arithmetic_type(perm, t_float, t_double), t_double));
    assert(type_equal(type_common_arithmetic_type(perm, t_float, t_int), t_float));
    assert(type_equal(type_common_arithmetic_type(perm, t_float, t_long), t_float));
    assert(type_equal(type_common_arithmetic_type(perm, t_float, t_ulong), t_float));

    /* Integer conversions */
    /* Small integer types promote first */
    assert(type_equal(type_common_arithmetic_type(perm, t_char, t_char), t_int));
    assert(type_equal(type_common_arithmetic_type(perm, t_short, t_uchar), t_int));
    assert(type_equal(type_common_arithmetic_type(perm, t_int, t_short), t_int));

    /* Same signedness */
    assert(type_equal(type_common_arithmetic_type(perm, t_int, t_long), t_long));
    assert(type_equal(type_common_arithmetic_type(perm, t_uint, t_ulong), t_ulong));

    /* Different signedness: unsigned rank >= signed rank -> unsigned type */
    assert(type_equal(type_common_arithmetic_type(perm, t_uint, t_int), t_uint));
    assert(type_equal(type_common_arithmetic_type(perm, t_ulong, t_long), t_ulong));
    assert(type_equal(type_common_arithmetic_type(perm, t_ulong, t_int), t_ulong));

    /* Different signedness: signed rank > unsigned rank and signed can represent all unsigned values */
    /* uint (32-bit) vs long (64-bit) -> long (64-bit) */
    assert(type_equal(type_common_arithmetic_type(perm, t_uint, t_long), t_long));
    assert(type_equal(type_common_arithmetic_type(perm, t_long, t_uint), t_long));

    /* type_common_real alias */
    assert(type_equal(type_common_real(perm, t_int, t_float), t_float));

    /* Non-arithmetic returns NULL */
    TypeInfo *t_ptr_int = type_pointer_to(perm, t_int);
    assert(type_common_arithmetic_type(perm, t_ptr_int, t_int) == NULL);

    /* 3. Pointer Arithmetic & Binary Expression Promotion */
    /* pointer + integer -> pointer */
    TypeInfo *res_add = promote_binary('+', t_ptr_int, t_int, perm, &diag);
    assert(type_equal(res_add, t_ptr_int));
    assert(!diag_has_errors(&diag));

    TypeInfo *res_add_rev = promote_binary('+', t_short, t_ptr_int, perm, &diag);
    assert(type_equal(res_add_rev, t_ptr_int));
    assert(!diag_has_errors(&diag));

    /* pointer - integer -> pointer */
    TypeInfo *res_sub = promote_binary('-', t_ptr_int, t_int, perm, &diag);
    assert(type_equal(res_sub, t_ptr_int));
    assert(!diag_has_errors(&diag));

    /* pointer - pointer -> ptrdiff_t (long) */
    TypeInfo *t_ptr_int2 = type_pointer_to(perm, t_int);
    TypeInfo *res_ptrdiff = promote_binary('-', t_ptr_int, t_ptr_int2, perm, &diag);
    assert(type_equal(res_ptrdiff, t_long));
    assert(!diag_has_errors(&diag));

    /* Relational and equality comparisons return int */
    TypeInfo *res_cmp = promote_binary('<', t_int, t_float, perm, &diag);
    assert(type_equal(res_cmp, t_int));

    TypeInfo *res_eq = promote_binary(PROMOTE_OP_EQ, t_ptr_int, t_ptr_int2, perm, &diag);
    assert(type_equal(res_eq, t_int));

    /* Bitwise shift returns promoted left operand */
    TypeInfo *res_shl = promote_binary(PROMOTE_OP_SHL, t_char, t_long, perm, &diag);
    assert(type_equal(res_shl, t_int));

    /* Arithmetic operations */
    TypeInfo *res_mul = promote_binary('*', t_int, t_double, perm, &diag);
    assert(type_equal(res_mul, t_double));

    TypeInfo *res_div = promote_binary('/', t_uint, t_int, perm, &diag);
    assert(type_equal(res_div, t_uint));

    /* Array and function decay in binary ops */
    TypeInfo *t_arr10_int = type_array_of(perm, t_int, 10);
    TypeInfo *res_arr_add = promote_binary('+', t_arr10_int, t_int, perm, &diag);
    assert(type_equal(res_arr_add, t_ptr_int));

    /* Incompatible pointer subtraction triggers diagnostic */
    TypeInfo *t_ptr_char = type_pointer_to(perm, t_char);
    TypeInfo *res_bad_sub = promote_binary('-', t_ptr_int, t_ptr_char, perm, &diag);
    assert(res_bad_sub == NULL);
    assert(diag_has_errors(&diag));

    /* 4. Type Compatibility (C11 6.2.7) */
    assert(type_is_compatible(t_int, t_int));
    assert(!type_is_compatible(t_int, t_uint));
    assert(!type_is_compatible(t_int, t_long));
    assert(type_is_compatible(t_ptr_int, t_ptr_int2));
    assert(!type_is_compatible(t_ptr_int, t_ptr_char));

    /* Array compatibility: same element type, matching count or count=0 (incomplete) */
    TypeInfo *t_arr_unspec = type_array_of(perm, t_int, 0);
    TypeInfo *t_arr_5 = type_array_of(perm, t_int, 5);
    TypeInfo *t_arr_10 = type_array_of(perm, t_int, 10);
    assert(type_is_compatible(t_arr_10, t_arr_unspec));
    assert(type_is_compatible(t_arr_unspec, t_arr_5));
    assert(!type_is_compatible(t_arr_10, t_arr_5));

    /* Function compatibility */
    TypeInfo *fn_params[2] = { t_int, t_float };
    TypeInfo *fn_params_match[2] = { t_int, t_float };
    TypeInfo *fn_params_diff[2] = { t_int, t_double };
    TypeInfo *fn1 = type_function(perm, t_int, fn_params, 2, false);
    TypeInfo *fn2 = type_function(perm, t_int, fn_params_match, 2, false);
    TypeInfo *fn3 = type_function(perm, t_int, fn_params_diff, 2, false);
    TypeInfo *fn_var = type_function(perm, t_int, fn_params, 2, true);
    assert(type_is_compatible(fn1, fn2));
    assert(!type_is_compatible(fn1, fn3));
    assert(!type_is_compatible(fn1, fn_var));

    /* 5. Explicit Cast (type_can_cast) */
    /* Any type can cast to void */
    assert(type_can_cast(t_int, t_void));
    assert(type_can_cast(t_ptr_int, t_void));

    /* Arithmetic <-> Arithmetic */
    assert(type_can_cast(t_int, t_float));
    assert(type_can_cast(t_double, t_char));

    /* Pointer <-> Integer */
    assert(type_can_cast(t_ptr_int, t_long));
    assert(type_can_cast(t_long, t_ptr_int));

    /* Pointer <-> Pointer */
    assert(type_can_cast(t_ptr_int, t_ptr_char));

    /* Cannot cast float to pointer directly */
    assert(!type_can_cast(t_float, t_ptr_int));
    assert(!type_can_cast(t_ptr_int, t_double));

    /* 6. Assignment Compatibility (type_compatible_assign) */
    /* Arithmetic to arithmetic */
    assert(type_compatible_assign(t_int, t_float));
    assert(type_compatible_assign(t_double, t_int));

    /* _Bool accepts any arithmetic or pointer */
    assert(type_compatible_assign(t_bool, t_int));
    assert(type_compatible_assign(t_bool, t_ptr_int));

    /* Pointer assignment */
    TypeInfo *t_ptr_void = type_pointer_to(perm, t_void);
    /* void* to/from object pointer */
    assert(type_compatible_assign(t_ptr_void, t_ptr_int));
    assert(type_compatible_assign(t_ptr_int, t_ptr_void));

    /* Same pointer types */
    assert(type_compatible_assign(t_ptr_int, t_ptr_int2));

    /* Incompatible pointer types */
    assert(!type_compatible_assign(t_ptr_int, t_ptr_char));

    /* Array decays to pointer for assignment */
    assert(type_compatible_assign(t_ptr_int, t_arr10_int));

    /* Qualifier discard checks */
    TypeInfo *t_const_int = type_new(perm);
    *t_const_int = *t_int;
    t_const_int->modifiers |= TYPE_MOD_CONST;
    TypeInfo *t_ptr_const_int = type_pointer_to(perm, t_const_int);

    /* Assigning int* to const int* is allowed (adding qualifiers) */
    assert(type_compatible_assign(t_ptr_const_int, t_ptr_int));
    /* Assigning const int* to int* is NOT allowed (discards const) */
    assert(!type_compatible_assign(t_ptr_int, t_ptr_const_int));

    arena_destroy(perm);
    arena_destroy(tmp);
    printf("Promotion and conversion unit tests passed successfully!\n");
    return 0;
}
