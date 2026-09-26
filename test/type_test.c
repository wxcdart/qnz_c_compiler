#include <stdio.h>
#include <assert.h>
#include "zero_cc/arena.h"
#include "zero_cc/semantic/type.h"

int main(void) {
    Arena *perm = arena_create(1024 * 1024);
    assert(perm != NULL);

    /* 1. Primitive types */
    TypeInfo *t_void = type_void(perm);
    TypeInfo *t_bool = type_bool(perm);
    TypeInfo *t_char = type_char(perm);
    TypeInfo *t_short = type_short(perm);
    TypeInfo *t_int = type_int(perm);
    TypeInfo *t_long = type_long(perm);
    TypeInfo *t_float = type_float(perm);
    TypeInfo *t_double = type_double(perm);

    assert(type_is_void(t_void));
    assert(!type_is_integer(t_void));
    assert(!type_is_arithmetic(t_void));

    assert(type_is_bool(t_bool));
    assert(type_is_integer(t_bool));
    assert(type_is_arithmetic(t_bool));

    assert(type_is_integer(t_char));
    assert(type_is_integer(t_short));
    assert(type_is_integer(t_int));
    assert(type_is_integer(t_long));

    assert(type_is_floating(t_float));
    assert(type_is_floating(t_double));
    assert(!type_is_integer(t_float));
    assert(type_is_arithmetic(t_float));
    assert(type_is_arithmetic(t_double));

    /* Sizes and alignments */
    assert(type_size(t_char) == 1);
    assert(type_size(t_short) == 2);
    assert(type_size(t_int) == 4);
    assert(type_size(t_long) == 8);
    assert(type_size(t_float) == 4);
    assert(type_size(t_double) == 8);

    assert(type_align(t_char) == 1);
    assert(type_align(t_short) == 2);
    assert(type_align(t_int) == 4);
    assert(type_align(t_long) == 8);
    assert(type_align(t_float) == 4);
    assert(type_align(t_double) == 8);

    /* Unsigned types */
    TypeInfo *t_uchar = type_uchar(perm);
    TypeInfo *t_ushort = type_ushort(perm);
    TypeInfo *t_uint = type_uint(perm);
    TypeInfo *t_ulong = type_ulong(perm);
    assert(type_is_integer(t_uchar) && type_is_unsigned(t_uchar) && type_size(t_uchar) == 1);
    assert(type_is_integer(t_ushort) && type_is_unsigned(t_ushort) && type_size(t_ushort) == 2);
    assert(type_is_integer(t_uint) && type_is_unsigned(t_uint) && type_size(t_uint) == 4);
    assert(type_is_integer(t_ulong) && type_is_unsigned(t_ulong) && type_size(t_ulong) == 8);
    assert(!type_is_unsigned(t_int));
    assert(!type_equal(t_int, t_uint));
    assert(!type_equal(t_long, t_ulong));

    /* 2. Pointers */
    TypeInfo *t_ptr_int = type_pointer_to(perm, t_int);
    TypeInfo *t_ptr_int2 = type_pointer_to(perm, t_int);
    TypeInfo *t_ptr_char = type_pointer_to(perm, t_char);

    assert(type_is_pointer(t_ptr_int));
    assert(!type_is_pointer(t_int));
    assert(type_equal(t_ptr_int, t_ptr_int2));
    assert(!type_equal(t_ptr_int, t_ptr_char));
    assert(type_size(t_ptr_int) == 8);
    assert(type_align(t_ptr_int) == 8);

    /* Pointer to pointer */
    TypeInfo *t_pptr_int = type_pointer_to(perm, t_ptr_int);
    assert(type_is_pointer(t_pptr_int));
    assert(type_equal(t_pptr_int->details.pointer.base, t_ptr_int));

    /* 3. Arrays */
    TypeInfo *t_arr10_int = type_array_of(perm, t_int, 10);
    TypeInfo *t_arr10_int2 = type_array_of(perm, t_int, 10);
    TypeInfo *t_arr5_int = type_array_of(perm, t_int, 5);

    assert(type_is_array(t_arr10_int));
    assert(!type_is_array(t_ptr_int));
    assert(type_equal(t_arr10_int, t_arr10_int2));
    assert(!type_equal(t_arr10_int, t_arr5_int));
    assert(type_size(t_arr10_int) == 40);
    assert(type_align(t_arr10_int) == 4);

    /* 4. Functions */
    TypeInfo *params[2] = { t_int, t_ptr_char };
    TypeInfo *fn_type = type_function(perm, t_int, params, 2, false);
    TypeInfo *fn_type2 = type_function(perm, t_int, params, 2, false);

    assert(type_is_function(fn_type));
    assert(!type_is_pointer(fn_type));
    assert(type_equal(fn_type, fn_type2));
    assert(fn_type->details.function.param_cnt == 2);
    assert(type_equal(fn_type->details.function.ret, t_int));

    TypeInfo *params_var[2] = { t_int, t_ptr_char };
    TypeInfo *fn_var = type_function(perm, t_int, params_var, 2, true);
    assert(!type_equal(fn_type, fn_var));

    arena_destroy(perm);
    printf("Type system unit tests passed successfully!\n");
    return 0;
}
