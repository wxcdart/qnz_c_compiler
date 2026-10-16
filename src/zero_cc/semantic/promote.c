#include "promote.h"
#include <string.h>

/* Token values matching ZeroCCTokType from types_lexer.h */
#define TOK_PLUS        63
#define TOK_MINUS       64
#define TOK_STAR        65
#define TOK_SLASH       66
#define TOK_PERCENT     67
#define TOK_AMP         68
#define TOK_PIPE        69
#define TOK_CARET       70
#define TOK_EQ          73
#define TOK_NE          74
#define TOK_LT          75
#define TOK_LE          76
#define TOK_GT          77
#define TOK_GE          78
#define TOK_LOGAND      79
#define TOK_LOGOR       80
#define TOK_SHL         81
#define TOK_SHR         82

/* Helper to get integer rank according to C11 6.3.1.1 */
static int get_integer_rank(const TypeInfo *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_KIND_BOOL:
            return 1;
        case TYPE_KIND_CHAR:
            return 2;
        case TYPE_KIND_SHORT:
            return 3;
        case TYPE_KIND_INT:
        case TYPE_KIND_ENUM:
            return 4;
        case TYPE_KIND_LONG:
            return 5;
        default:
            return 0;
    }
}

TypeInfo *type_promote(Arena *perm, const TypeInfo *t) {
    if (!t) return NULL;
    if (!type_is_integer(t)) {
        return (TypeInfo *)t;
    }

    int rank = get_integer_rank(t);
    int int_rank = 4; // rank of int

    if (rank < int_rank) {
        /*
         * In C11 6.3.1.1: If an int can represent all values of the original type,
         * the value is converted to an int; otherwise, it is converted to an unsigned int.
         * Here, short is 16-bit (max 65535) and int is 32-bit (max 2147483647).
         * So all values of unsigned char and unsigned short fit in a signed int.
         * Thus, small integer types always promote to signed int.
         */
        return type_int(perm);
    }

    return (TypeInfo *)t;
}

TypeInfo *promote_unary(Arena *perm, const TypeInfo *t) {
    return type_promote(perm, t);
}

TypeInfo *type_common_arithmetic_type(Arena *perm, const TypeInfo *t1, const TypeInfo *t2) {
    if (!t1 || !t2) return NULL;
    if (!type_is_arithmetic(t1) || !type_is_arithmetic(t2)) return NULL;

    /* 1. If either operand is double, result is double */
    if (t1->kind == TYPE_KIND_DOUBLE || t2->kind == TYPE_KIND_DOUBLE) {
        return type_double(perm);
    }

    /* 2. Otherwise, if either operand is float, result is float */
    if (t1->kind == TYPE_KIND_FLOAT || t2->kind == TYPE_KIND_FLOAT) {
        return type_float(perm);
    }

    /* 3. Both are integers: perform integer promotions */
    TypeInfo *p1 = type_promote(perm, t1);
    TypeInfo *p2 = type_promote(perm, t2);

    /* If both have identical types */
    if (type_equal(p1, p2)) {
        return p1;
    }

    bool u1 = type_is_unsigned(p1);
    bool u2 = type_is_unsigned(p2);
    int r1 = get_integer_rank(p1);
    int r2 = get_integer_rank(p2);

    /* If both operands have the same signedness */
    if (u1 == u2) {
        return (r1 >= r2) ? p1 : p2;
    }

    /* Operands have different signedness */
    TypeInfo *u_type = u1 ? p1 : p2;
    TypeInfo *s_type = u1 ? p2 : p1;
    int u_rank = u1 ? r1 : r2;
    int s_rank = u1 ? r2 : r1;

    /*
     * If the unsigned operand has rank greater than or equal to the signed operand,
     * convert signed to the unsigned type.
     */
    if (u_rank >= s_rank) {
        return u_type;
    }

    /*
     * Unsigned rank < signed rank.
     * Check if signed type can represent all values of unsigned type:
     * In our target, unsigned int is 32-bit (rank 4), signed long is 64-bit (rank 5).
     * Since sizeof(long) (8) > sizeof(uint) (4), signed long CAN represent all values of uint.
     * So convert unsigned operand to signed type.
     */
    if (type_size(s_type) > type_size(u_type)) {
        return s_type;
    }

    /* Otherwise, convert both to unsigned type of signed operand */
    if (s_type->kind == TYPE_KIND_LONG) {
        return type_ulong(perm);
    }
    return type_uint(perm);
}

TypeInfo *type_common_real(Arena *perm, const TypeInfo *t1, const TypeInfo *t2) {
    return type_common_arithmetic_type(perm, t1, t2);
}

bool type_is_compatible(const TypeInfo *t1, const TypeInfo *t2) {
    if (t1 == t2) return true;
    if (!t1 || !t2) return false;

    /* Strip top-level non-signedness modifiers when comparing compatibility */
    uint32_t m1 = t1->modifiers & TYPE_MOD_UNSIGNED;
    uint32_t m2 = t2->modifiers & TYPE_MOD_UNSIGNED;

    /* Special case: enum is compatible with its underlying integer type (int) */
    if (t1->kind == TYPE_KIND_ENUM && t2->kind == TYPE_KIND_INT && m2 == 0) return true;
    if (t2->kind == TYPE_KIND_ENUM && t1->kind == TYPE_KIND_INT && m1 == 0) return true;

    if (t1->kind != t2->kind) return false;
    if (m1 != m2) return false;

    switch (t1->kind) {
        case TYPE_KIND_VOID:
        case TYPE_KIND_BOOL:
        case TYPE_KIND_CHAR:
        case TYPE_KIND_SHORT:
        case TYPE_KIND_INT:
        case TYPE_KIND_LONG:
        case TYPE_KIND_FLOAT:
        case TYPE_KIND_DOUBLE:
            return true;

        case TYPE_KIND_POINTER:
            return type_is_compatible(t1->details.pointer.base, t2->details.pointer.base);

        case TYPE_KIND_ARRAY: {
            if (!type_is_compatible(t1->details.array.base, t2->details.array.base)) {
                return false;
            }
            /* Compatible if both sizes match, or either has size 0 (unspecified/incomplete) */
            if (t1->details.array.count == 0 || t2->details.array.count == 0) {
                return true;
            }
            return t1->details.array.count == t2->details.array.count;
        }

        case TYPE_KIND_FUNCTION: {
            if (!type_is_compatible(t1->details.function.ret, t2->details.function.ret)) {
                return false;
            }
            if ((t1->details.function.is_variadic && t1->details.function.param_cnt == 0) ||
                (t2->details.function.is_variadic && t2->details.function.param_cnt == 0)) {
                return true;
            }
            if (t1->details.function.is_variadic != t2->details.function.is_variadic) {
                return false;
            }
            if (t1->details.function.param_cnt != t2->details.function.param_cnt) {
                return false;
            }
            for (size_t i = 0; i < t1->details.function.param_cnt; ++i) {
                if (!type_is_compatible(t1->details.function.params[i], t2->details.function.params[i])) {
                    return false;
                }
            }
            return true;
        }

        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION:
        case TYPE_KIND_ENUM:
            if (t1->details.record.tag && t2->details.record.tag) {
                return strcmp(t1->details.record.tag, t2->details.record.tag) == 0;
            }
            if (!t1->details.record.tag && !t2->details.record.tag) {
                return t1->details.record.size == t2->details.record.size &&
                       t1->details.record.align == t2->details.record.align;
            }
            return false;

        default:
            return false;
    }
}

bool type_can_cast(const TypeInfo *src, const TypeInfo *dst) {
    if (!src || !dst) return false;

    /* Any type can be cast to void */
    if (type_is_void(dst)) {
        return true;
    }

    /* Decay array or function src to pointer */
    if (type_is_array(src) || type_is_function(src)) {
        /* Pointers can cast to any scalar (pointer or integer) */
        if (type_is_pointer(dst) || type_is_integer(dst) || type_is_bool(dst)) {
            return true;
        }
        return false;
    }

    /* Scalar types can be explicitly cast to any scalar type */
    bool src_scalar = type_is_arithmetic(src) || type_is_pointer(src);
    bool dst_scalar = type_is_arithmetic(dst) || type_is_pointer(dst);

    if (src_scalar && dst_scalar) {
        /*
         * Note: In C, float/double cannot be cast directly to pointer,
         * and pointer cannot be cast directly to float/double.
         */
        if (type_is_floating(src) && type_is_pointer(dst)) return false;
        if (type_is_pointer(src) && type_is_floating(dst)) return false;
        return true;
    }

    /* Otherwise, compatible types can cast (e.g. identical structs) */
    return type_is_compatible(src, dst);
}

bool type_compatible_assign(const TypeInfo *dst, const TypeInfo *src) {
    if (!dst || !src) return false;

    /* Both are arithmetic types */
    if (type_is_arithmetic(dst) && type_is_arithmetic(src)) {
        return true;
    }

    /* Both are compatible structure or union types */
    if ((dst->kind == TYPE_KIND_STRUCT || dst->kind == TYPE_KIND_UNION) &&
        (src->kind == TYPE_KIND_STRUCT || src->kind == TYPE_KIND_UNION)) {
        return type_is_compatible(dst, src);
    }

    /* Decay array or function src to pointer */
    const TypeInfo *effective_src = src;
    TypeInfo decay_src_buf;
    if (src->kind == TYPE_KIND_ARRAY) {
        decay_src_buf.kind = TYPE_KIND_POINTER;
        decay_src_buf.modifiers = TYPE_MOD_NONE;
        decay_src_buf.details.pointer.base = src->details.array.base;
        effective_src = &decay_src_buf;
    } else if (src->kind == TYPE_KIND_FUNCTION) {
        decay_src_buf.kind = TYPE_KIND_POINTER;
        decay_src_buf.modifiers = TYPE_MOD_NONE;
        decay_src_buf.details.pointer.base = (TypeInfo *)src;
        effective_src = &decay_src_buf;
    }

    /* If dst is _Bool, any arithmetic or pointer type can be assigned */
    if (type_is_bool(dst)) {
        return type_is_arithmetic(effective_src) || type_is_pointer(effective_src);
    }

    /* Pointer assignment rules */
    if (type_is_pointer(dst)) {
        /* Right operand is a pointer */
        if (type_is_pointer(effective_src)) {
            TypeInfo *dst_base = dst->details.pointer.base;
            TypeInfo *src_base = effective_src->details.pointer.base;

            /* Check qualifier constraints: dst must have all qualifiers of src target */
            if (dst_base && src_base) {
                uint32_t src_quals = src_base->modifiers & (TYPE_MOD_CONST | TYPE_MOD_VOLATILE | TYPE_MOD_RESTRICT | TYPE_MOD_ATOMIC);
                uint32_t dst_quals = dst_base->modifiers & (TYPE_MOD_CONST | TYPE_MOD_VOLATILE | TYPE_MOD_RESTRICT | TYPE_MOD_ATOMIC);
                if ((src_quals & ~dst_quals) != 0) {
                    /* Discards qualifiers */
                    return false;
                }
            }

            /* One operand is a pointer to void (void* or const void* etc.) */
            if ((dst_base && type_is_void(dst_base)) || (src_base && type_is_void(src_base))) {
                return true;
            }

            /* Pointers to compatible types */
            return type_is_compatible(dst_base, src_base);
        }

        /* Integer 0 or null constant assignment to pointer */
        return false;
    }

    return false;
}

TypeInfo *promote_binary(uint32_t op, const TypeInfo *l, const TypeInfo *r, Arena *perm, DiagBuf *diag) {
    if (!l || !r) {
        if (diag) diag_report(diag, 0, 0, "null operand in binary expression");
        return NULL;
    }

    /* Decay array and function types to pointers for operands */
    const TypeInfo *lp = l;
    if (l->kind == TYPE_KIND_ARRAY) {
        lp = type_pointer_to(perm, l->details.array.base);
    } else if (l->kind == TYPE_KIND_FUNCTION) {
        lp = type_pointer_to(perm, (TypeInfo *)l);
    }

    const TypeInfo *rp = r;
    if (r->kind == TYPE_KIND_ARRAY) {
        rp = type_pointer_to(perm, r->details.array.base);
    } else if (r->kind == TYPE_KIND_FUNCTION) {
        rp = type_pointer_to(perm, (TypeInfo *)r);
    }

    bool is_plus = (op == '+' || op == TOK_PLUS || op == PROMOTE_OP_ADD);
    bool is_minus = (op == '-' || op == TOK_MINUS || op == PROMOTE_OP_SUB);
    bool is_relational = (op == '<' || op == '>' || op == TOK_LT || op == TOK_GT ||
                          op == TOK_LE || op == TOK_GE ||
                          op == PROMOTE_OP_LT || op == PROMOTE_OP_GT ||
                          op == PROMOTE_OP_LE || op == PROMOTE_OP_GE);
    bool is_equality = (op == TOK_EQ || op == TOK_NE || op == PROMOTE_OP_EQ || op == PROMOTE_OP_NE);
    bool is_logical = (op == TOK_LOGAND || op == TOK_LOGOR || op == PROMOTE_OP_LOG_AND || op == PROMOTE_OP_LOG_OR);
    bool is_shift = (op == TOK_SHL || op == TOK_SHR || op == PROMOTE_OP_SHL || op == PROMOTE_OP_SHR);
    bool is_bitwise = (op == '&' || op == '|' || op == '^' ||
                       op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET ||
                       op == PROMOTE_OP_BIT_AND || op == PROMOTE_OP_BIT_OR || op == PROMOTE_OP_BIT_XOR);
    bool is_mod = (op == '%' || op == TOK_PERCENT || op == PROMOTE_OP_MOD);
    bool is_mult_div = (op == '*' || op == '/' || op == TOK_STAR || op == TOK_SLASH ||
                        op == PROMOTE_OP_MUL || op == PROMOTE_OP_DIV);

    /* 1. Pointer arithmetic cases */
    if (type_is_pointer(lp) || type_is_pointer(rp)) {
        if (is_plus) {
            /* pointer + integer -> pointer */
            if (type_is_pointer(lp) && type_is_integer(rp)) {
                return (TypeInfo *)lp;
            }
            /* integer + pointer -> pointer */
            if (type_is_integer(lp) && type_is_pointer(rp)) {
                return (TypeInfo *)rp;
            }
        } else if (is_minus) {
            /* pointer - integer -> pointer */
            if (type_is_pointer(lp) && type_is_integer(rp)) {
                return (TypeInfo *)lp;
            }
            /* pointer - pointer -> ptrdiff_t (long) */
            if (type_is_pointer(lp) && type_is_pointer(rp)) {
                if (type_is_compatible(lp->details.pointer.base, rp->details.pointer.base)) {
                    return type_long(perm);
                }
                if (diag) diag_report(diag, 0, 0, "subtraction of incompatible pointer types");
                return NULL;
            }
        } else if (is_relational || is_equality) {
            /* Comparison between pointers returns int */
            if (type_is_pointer(lp) && type_is_pointer(rp)) {
                return type_int(perm);
            }
        }

        if (diag) diag_report(diag, 0, 0, "invalid operands to binary operator with pointer");
        return NULL;
    }

    /* 2. Logical AND / OR (&&, ||) -> returns int */
    if (is_logical) {
        if (type_is_arithmetic(lp) && type_is_arithmetic(rp)) {
            return type_int(perm);
        }
        if (diag) diag_report(diag, 0, 0, "invalid operands to logical operator");
        return NULL;
    }

    /* 3. Relational and equality operators (<, <=, >, >=, ==, !=) -> returns int */
    if (is_relational || is_equality) {
        if (type_is_arithmetic(lp) && type_is_arithmetic(rp)) {
            return type_int(perm);
        }
        if (diag) diag_report(diag, 0, 0, "invalid operands to comparison operator");
        return NULL;
    }

    /* 4. Bitwise shift operators (<<, >>) */
    if (is_shift) {
        if (type_is_integer(lp) && type_is_integer(rp)) {
            /* Result is type of promoted left operand */
            return type_promote(perm, lp);
        }
        if (diag) diag_report(diag, 0, 0, "bitwise shift operands must be integers");
        return NULL;
    }

    /* 5. Bitwise & | ^ and % (modulo) */
    if (is_bitwise || is_mod) {
        if (!type_is_integer(lp) || !type_is_integer(rp)) {
            if (diag) diag_report(diag, 0, 0, "bitwise/modulo operands must be integers");
            return NULL;
        }
        return type_common_arithmetic_type(perm, lp, rp);
    }

    /* 6. Arithmetic operators (+, -, *, /) */
    if (is_plus || is_minus || is_mult_div) {
        if (!type_is_arithmetic(lp) || !type_is_arithmetic(rp)) {
            if (diag) diag_report(diag, 0, 0, "arithmetic operands must have arithmetic type");
            return NULL;
        }
        return type_common_arithmetic_type(perm, lp, rp);
    }

    /* Generic fallback for any other arithmetic binary op */
    if (type_is_arithmetic(lp) && type_is_arithmetic(rp)) {
        return type_common_arithmetic_type(perm, lp, rp);
    }

    if (diag) diag_report(diag, 0, 0, "unsupported binary operator %u", op);
    return NULL;
}
