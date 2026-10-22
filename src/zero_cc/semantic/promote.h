#pragma once

#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/diagnostics.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Binary operators supported by promote_binary.
 * These correspond to C binary operator tokens and ASCII values.
 */
typedef enum PromoteOp {
    PROMOTE_OP_ADD = 1,
    PROMOTE_OP_SUB,
    PROMOTE_OP_MUL,
    PROMOTE_OP_DIV,
    PROMOTE_OP_MOD,
    PROMOTE_OP_BIT_AND,
    PROMOTE_OP_BIT_OR,
    PROMOTE_OP_BIT_XOR,
    PROMOTE_OP_SHL,
    PROMOTE_OP_SHR,
    PROMOTE_OP_EQ,
    PROMOTE_OP_NE,
    PROMOTE_OP_LT,
    PROMOTE_OP_GT,
    PROMOTE_OP_LE,
    PROMOTE_OP_GE,
    PROMOTE_OP_LOG_AND,
    PROMOTE_OP_LOG_OR
} PromoteOp;

/**
 * Perform integer promotions on a type according to C11 6.3.1.1:
 * Types with integer conversion rank less than int (bool, char, signed char,
 * unsigned char, short, unsigned short, and enum) are converted to int
 * (or unsigned int if int cannot represent all values of the type).
 * In our model (where short is 16-bit and int is 32-bit), all values of
 * unsigned short fit in signed int, so bool, char, uchar, short, ushort, enum
 * promote to signed int.
 * Types with rank >= int (int, uint, long, ulong) remain unchanged.
 * Non-integer types remain unchanged.
 */
TypeInfo *type_promote(Arena *perm, const TypeInfo *t);

/**
 * Alias/helper matching promote_unary in SEMANTIC.md roadmap.
 */
TypeInfo *promote_unary(Arena *perm, const TypeInfo *t);

/**
 * Determine the common real/arithmetic type between two arithmetic types
 * according to the usual arithmetic conversions (C11 6.3.1.8).
 * 1. If either operand has type double, the other is converted to double.
 * 2. Otherwise, if either operand is float, the other is converted to float.
 * 3. Otherwise, integer promotions are performed on both operands:
 *    - If both have the same type, that is the common type.
 *    - If both have the same signedness (both signed or both unsigned), the operand
 *      with the type of lesser integer conversion rank is converted to the type of greater rank.
 *    - If unsigned operand has rank >= signed operand, signed operand is converted to unsigned operand's type.
 *    - If signed operand's type can represent all values of the unsigned operand's type
 *      (e.g., unsigned int (32-bit) vs long (64-bit)), the unsigned operand is converted to the signed operand's type.
 *    - Otherwise, both operands are converted to the unsigned type corresponding to the signed operand's type.
 * Returns NULL if either operand is not an arithmetic type.
 */
TypeInfo *type_common_arithmetic_type(Arena *perm, const TypeInfo *t1, const TypeInfo *t2);

/**
 * Alias matching SEMANTIC.md roadmap.
 */
TypeInfo *type_common_real(Arena *perm, const TypeInfo *t1, const TypeInfo *t2);

/**
 * Determine the result type of a binary operator given operand types,
 * handling arithmetic conversions, pointer arithmetic (+ and -),
 * relational comparisons, and bitwise operations.
 * Matching signature from SEMANTIC.md:
 * TypeInfo *promote_binary(uint32_t op, const TypeInfo *l, const TypeInfo *r, Arena *perm, DiagBuf *diag);
 */
TypeInfo *promote_binary(uint32_t op, const TypeInfo *l, const TypeInfo *r, Arena *perm, DiagBuf *diag);

/**
 * Check if two types are compatible according to C11 6.2.7.
 * Ignores top-level type qualifiers (const, volatile, restrict, atomic).
 * Handles:
 * - Identical primitive types
 * - Enums and compatible integer types (or same enum tag)
 * - Pointers: compatible if target types are compatible
 * - Arrays: compatible if element types are compatible and either both have same size
 *   or at least one has unspecified size (count == 0)
 * - Functions: compatible return types, same parameter count, compatible parameter types,
 *   matching variadic flags.
 * - Structs/unions: matching struct/union tag.
 */
bool type_is_compatible(const TypeInfo *t1, const TypeInfo *t2);

/**
 * Check if a source expression of type `src` can be cast to type `dst` (explicit cast).
 * C11 6.5.4:
 * - Any type can be cast to void.
 * - Scalar types (arithmetic and pointer) can be cast to any scalar type.
 * - Array/function types decay to pointers.
 * Returns true if the explicit cast is valid in C11.
 */
bool type_can_cast(const TypeInfo *src, const TypeInfo *dst);

/**
 * Check assignment compatibility according to C11 6.5.16.1.
 * Returns true if an expression of type `src` can be assigned to an lvalue of type `dst`.
 * Allows:
 * - Both are arithmetic types.
 * - Both are compatible struct or union types.
 * - Both are pointers to qualified or unqualified versions of compatible types,
 *   and dst has all qualifiers of src.
 * - One pointer is a pointer to object or incomplete type, and the other is a pointer to
 *   qualified or unqualified void, and dst has all qualifiers of src.
 * - dst is a pointer and src is a null pointer constant (represented by TYPE_NULLPTR or pointer to void/0).
 * - dst is _Bool and src is any pointer or arithmetic type.
 */
bool type_compatible_assign(const TypeInfo *dst, const TypeInfo *src);

#ifdef __cplusplus
}
#endif
