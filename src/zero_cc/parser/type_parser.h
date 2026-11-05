#ifndef ZERO_CC_PARSER_TYPE_PARSER_H
#define ZERO_CC_PARSER_TYPE_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/global_vars.h"

/* Forward declarations for constant-evaluation helpers */
int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);
int zero_cc_parse_aligned_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_align);
int zero_cc_parse_vector_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_size);
int zero_cc_parse_mode_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_bytes);

/* Apply a GNU `mode(X)` integer-width attribute (marker byte count) to a
 * base integer type, preserving signedness (c-torture execute/memclr:
 * `typedef unsigned int __attribute__((mode(QI))) int08_t;`). Non-integer
 * bases and unsupported widths (TI) are returned unchanged. */
static inline ZeroCCType zero_cc_type_apply_mode(ZeroCCType ty, size_t mode_bytes) {
    bool uns = (ty.tag == TYPE_UINT8 || ty.tag == TYPE_UINT16 ||
                ty.tag == TYPE_UINT32 || ty.tag == TYPE_UINT64);
    bool is_int = uns || ty.tag == TYPE_INT8 || ty.tag == TYPE_INT16 ||
                  ty.tag == TYPE_INT32 || ty.tag == TYPE_INT64;
    if (!is_int) return ty;
    switch (mode_bytes) {
        case 1:  return uns ? zero_cc_type_uint8()  : zero_cc_type_char();
        case 2:  return uns ? zero_cc_type_uint16() : zero_cc_type_int16();
        case 4:  return uns ? zero_cc_type_uint32() : zero_cc_type_int();
        case 8:  return uns ? zero_cc_type_uint64() : zero_cc_type_int64();
        default: return ty; /* TI and unknown: leave as-is */
    }
}
bool zero_cc_tok_is_type_start(ZeroCCCompiler *c, size_t pos);

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);

static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);
static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c);

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_relational(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            zero_cc_type_of_relational(c, pos);
            left = zero_cc_type_int(); /* comparison yields int */
        } else {
            break;
        }
    }
    /* C99 6.5.15: conditional expression in a type context
     * (`sizeof ((c ? a : b).c)`).  Both operands must be compatible;
     * the result type is that of the (struct/union) operands. */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_QUESTION) {
        (*pos)++;
        ZeroCCType t = zero_cc_type_of_equality(c, pos);
        if (*pos < c->token_count && c->tokens[*pos].type == TK_COLON) (*pos)++;
        ZeroCCType f = zero_cc_type_of_equality(c, pos);
        if ((t.tag == TYPE_STRUCT && f.tag == TYPE_STRUCT) ||
            (t.tag == TYPE_UNION && f.tag == TYPE_UNION)) {
            left = t;
        } else if (t.tag == f.tag) {
            left = t;
        } else {
            left = zero_cc_type_int();
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            zero_cc_type_of_additive(c, pos);
            left = zero_cc_type_int();
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_multiplicative(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_PLUS || op == TK_MINUS) {
            (*pos)++;
            ZeroCCType right = zero_cc_type_of_multiplicative(c, pos);
            /* Pointer arithmetic: if left is ptr and right is int, result is ptr */
            if (left.tag == TYPE_PTR && (right.tag == TYPE_INT32 || right.tag == TYPE_INT64 ||
                right.tag == TYPE_UINT32 || right.tag == TYPE_UINT64)) {
                /* result is same pointer type */
            } else {
                left = zero_cc_type_int();
            }
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType left = zero_cc_type_of_unary(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT) {
            (*pos)++;
            zero_cc_type_of_unary(c, pos);
            left = zero_cc_type_int();
        } else {
            break;
        }
    }
    return left;
}

static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_INC || t == TK_DEC) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_PLUS) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_MINUS) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_TILDE) {
        (*pos)++;
        return zero_cc_type_of_unary(c, pos);
    }
    if (t == TK_BANG) {
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_SIZEOF) {
        /* sizeof expression is size_t (uint64_t) */
        eval_sizeof(c, pos);
        return zero_cc_type_uint64();
    }
    if (t == TK_ALIGNOF) {
        eval_alignof(c, pos);
        return zero_cc_type_uint64();
    }
    if (t == TK_STAR) { /* dereference */
        (*pos)++;
        ZeroCCType operand = zero_cc_type_of_unary(c, pos);
        if ((operand.tag == TYPE_PTR || operand.tag == TYPE_ARRAY) && operand.base) {
            return *operand.base;
        }
        return operand;
    }
    if (t == TK_AMP) { /* address-of */
        (*pos)++;
        ZeroCCType operand = zero_cc_type_of_unary(c, pos);
        return zero_cc_type_ptr(&operand);
    }
    return zero_cc_type_of_primary(c, pos);
}

static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        /* C99 6.4.4.1p6: the suffix (bit0=u, bit1=l, bit2=ll) fixes the
         * literal's type and an unsuffixed constant takes the first type in
         * which its value fits, so sizeof(0x12345678ABCDEF0LLU) is 8, not
         * sizeof(int). */
        uint8_t suf = c->tokens[*pos].int_suffix;
        int64_t v = c->tokens[*pos].num_val;
        (*pos)++;
        if (suf & 0x06) return (suf & 0x01) ? zero_cc_type_uint64() : zero_cc_type_int64();
        if (suf & 0x01) return (v > (int64_t)0xFFFFFFFF) ? zero_cc_type_uint64() : zero_cc_type_uint32();
        if (v < 0) return zero_cc_type_uint64(); /* wrapped 64-bit hex literal */
        if (v > (int64_t)0xFFFFFFFF) return zero_cc_type_int64();
        if (v > INT32_MAX) return zero_cc_type_uint32();
        return zero_cc_type_int();
    }
    if (t == TK_FLOAT) {
        uint8_t suf = c->tokens[*pos].int_suffix;
        (*pos)++;
        if (suf == 1) return zero_cc_type_float();
        return zero_cc_type_double();
    }
    if (t == TK_CHAR_LIT) {
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_TRUE || t == TK_FALSE) {
        (*pos)++;
        return zero_cc_type_int(); /* _Bool */
    }
    if (t == TK_NULLPTR) {
        (*pos)++;
        return zero_cc_type_nullptr();
    }
    if (t == TK_STRING) {
        bool is_wide = c->tokens[*pos].is_wide;
        size_t slen = zero_cc_token_str_len(&c->tokens[*pos]);
        (*pos)++;
        if (is_wide) {
            ZeroCCType elem = zero_cc_type_int();
            size_t num_elems = (slen / 4) + 1;
            return zero_cc_type_array(&elem, (ssize_t)num_elems);
        } else {
            size_t len = slen + 1;
            ZeroCCType elem = zero_cc_type_char();
            return zero_cc_type_array(&elem, (ssize_t)len);
        }
    }
    /* Type keywords as primary (for sizeof(type)/_Alignof(type)) */
    if (t == TK_STRUCT || t == TK_UNION) {
        (*pos)++;
        if (c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            (*pos)++;
            ZeroCCType *st = zero_cc_find_struct(c, name);
            if (!st) st = zero_cc_find_typedef(c, name);
            if (st) return *st;
            return zero_cc_type_int64();
        }
    } else if (t == TK_ENUM) {
        (*pos)++;
        if (c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            (*pos)++;
            ZeroCCType *et = zero_cc_find_enum(c, name);
            if (!et) et = zero_cc_find_typedef(c, name);
            if (et) return *et;
            return zero_cc_type_enum(name);
        }
    }
    if (t == TK_INT) { (*pos)++; return zero_cc_type_int(); }
    if (t == TK_CHAR) { (*pos)++; return zero_cc_type_char(); }
    if (t == TK_VOID) { (*pos)++; return zero_cc_type_void(); }
    if (t == TK_FLOAT_KW) { (*pos)++; return zero_cc_type_float(); }
    if (t == TK_DOUBLE_KW) { (*pos)++; return zero_cc_type_double(); }
    if (t == TK_BOOL_KW) { (*pos)++; return zero_cc_type_bool(); }
    if (t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        return zero_cc_parse_type_name_at_pos(c, pos);
    }
    if (t == TK_SHORT || t == TK_LONG || t == TK_SIGNED || t == TK_UNSIGNED) {
        /* Multi-word types (long long, unsigned long, signed short, ...) */
        return zero_cc_parse_type_name_at_pos(c, pos);
    }
    if (t == TK_ATOMIC) { (*pos)++; if (c->tokens[*pos].type == TK_LPAREN) { (*pos)++; } ZeroCCType ty = zero_cc_type_of_primary(c, pos); if (c->tokens[*pos].type == TK_RPAREN) (*pos)++; ty.is_atomic = true; return ty; }
    if (t == TK_LPAREN) {
        (*pos)++;
        /* Could be cast or parenthesized expression */
        ZeroCCTokType next = c->tokens[*pos].type;
        bool is_type = (next == TK_INT || next == TK_CHAR || next == TK_VOID ||
                        next == TK_STRUCT || next == TK_UNION || next == TK_ENUM ||
                        next == TK_FLOAT_KW || next == TK_DOUBLE_KW || next == TK_BOOL_KW ||
                        next == TK_SIGNED || next == TK_UNSIGNED || next == TK_SHORT || next == TK_LONG || next == TK_ATOMIC ||
                        next == TK_TYPEOF || next == TK_TYPEOF_UNQUAL);
        if (!is_type && next == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            if (zero_cc_find_typedef(c, name)) {
                is_type = true;
            } else if (strcmp(name, "__int128") == 0 || strcmp(name, "__int128_t") == 0 ||
                       strcmp(name, "__uint128_t") == 0 || strcmp(name, "__float128") == 0) {
                /* GNU 128-bit base / __float128 cast: `(__int128)x`,
                 * `(__uint128_t)x`, `(__float128)x`. */
                is_type = true;
            }
        }
        if (is_type) {
            /* Cast: (type) expr */
            ZeroCCType cast_ty = zero_cc_parse_type_name_at_pos(c, pos);
            if (c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            ZeroCCType expr_ty = zero_cc_type_of_unary(c, pos);
            (void)expr_ty; /* ignore - cast result is cast_ty */
            return cast_ty;
        }
        /* Parenthesized expression */
        ZeroCCType ty = zero_cc_type_of_equality(c, pos);
        if (c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        /* Postfix after the closing paren: `++/--` and member access
         * (`((c ? a : b).c)` - c-torture compile/20011106-2). */
        while (*pos < c->token_count) {
            ZeroCCTokType pt = c->tokens[*pos].type;
            if (pt == TK_INC || pt == TK_DEC) { (*pos)++; continue; }
            if (pt == TK_DOT || pt == TK_ARROW) {
                (*pos)++;
                if (c->tokens[*pos].type == TK_IDENT) {
                    const char *mem_name = c->tokens[*pos].str_val;
                    (*pos)++;
                    ty = zero_cc_type_find_member(c, ty, mem_name);
                }
                continue;
            }
            break;
        }
        return ty;
    }
    if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        (*pos)++;
        /* Check for function call: func(...) */
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            (*pos)++;
            int pdepth = 1;
            while (*pos < c->token_count && pdepth > 0) {
                if (c->tokens[*pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[*pos].type == TK_RPAREN) pdepth--;
                (*pos)++;
            }
            ZeroCCFunction *fn = zero_cc_find_function(c, name);
            if (fn) return fn->return_type;
            return zero_cc_type_int();
        }
        /* Check for local variable */
        ZeroCCSym *sym = zero_cc_find_local(c, name);
        if (sym) {
            ZeroCCType ty = sym->type;
            /* Handle member access and postfix: sym->member, sym.member, sym[i], sym++, sym-- */
            while (true) {
                ZeroCCTokType mt = c->tokens[*pos].type;
                if (mt == TK_INC || mt == TK_DEC) {
                    (*pos)++;
                    continue;
                }
                if (mt == TK_DOT || mt == TK_ARROW) {
                    (*pos)++;
                    if (c->tokens[*pos].type == TK_IDENT) {
                        const char *mem_name = c->tokens[*pos].str_val;
                        (*pos)++;
                        ty = zero_cc_type_find_member(c, ty, mem_name);
                    }
                    continue;
                }
                if (mt == TK_LBRACKET) { /* array indexing */
                    (*pos)++;
                    zero_cc_type_of_equality(c, pos); /* consume index */
                    if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    if (ty.tag == TYPE_ARRAY && ty.base) {
                        ty = *ty.base;
                    } else if (ty.tag == TYPE_PTR && ty.base) {
                        ty = *ty.base;
                    }
                    continue;
                }
                break;
            }
            return ty;
        }
        /* Check for thread-local variable */
        void *tls = zero_cc_find_tls(c, name);
        if (tls) {
            /* Get type from TLS slots array */
            for (size_t i = 0; i < c->thread_local_count; i++) {
                if (strcmp(c->thread_locals[i].name, name) == 0) {
                    ZeroCCType ty = c->thread_locals[i].type;
                    /* Handle member access and postfix on TLS variable */
                    while (true) {
                        ZeroCCTokType mt = c->tokens[*pos].type;
                        if (mt == TK_INC || mt == TK_DEC) {
                            (*pos)++;
                            continue;
                        }
                        if (mt == TK_DOT || mt == TK_ARROW) {
                            (*pos)++;
                            if (c->tokens[*pos].type == TK_IDENT) {
                                const char *mem_name = c->tokens[*pos].str_val;
                                (*pos)++;
                                ty = zero_cc_type_find_member(c, ty, mem_name);
                            }
                            continue;
                        }
                        if (mt == TK_LBRACKET) { /* array indexing */
                            (*pos)++;
                            zero_cc_type_of_equality(c, pos); /* consume index */
                            if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                            if (ty.tag == TYPE_ARRAY && ty.base) {
                                ty = *ty.base;
                            } else if (ty.tag == TYPE_PTR && ty.base) {
                                ty = *ty.base;
                            }
                            continue;
                        }
                        break;
                    }
                    return ty;
                }
            }
        }
        /* Check for global variable */
        ZeroCCGlobalVar *gv = find_global_var(c, name);
        if (gv) {
            ZeroCCType ty = gv->type;
            while (true) {
                ZeroCCTokType mt = c->tokens[*pos].type;
                if (mt == TK_INC || mt == TK_DEC) {
                    (*pos)++;
                    continue;
                }
                if (mt == TK_DOT || mt == TK_ARROW) {
                    (*pos)++;
                    if (c->tokens[*pos].type == TK_IDENT) {
                        const char *mem_name = c->tokens[*pos].str_val;
                        (*pos)++;
                        ty = zero_cc_type_find_member(c, ty, mem_name);
                    }
                    continue;
                }
                if (mt == TK_LBRACKET) { /* array indexing */
                    (*pos)++;
                    zero_cc_type_of_equality(c, pos); /* consume index */
                    if (c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
                    if (ty.tag == TYPE_ARRAY && ty.base) {
                        ty = *ty.base;
                    } else if (ty.tag == TYPE_PTR && ty.base) {
                        ty = *ty.base;
                    }
                    continue;
                }
                break;
            }
            return ty;
        }
        /* Check for function: the operand decays to a function pointer
         * (C99 6.3.2.1) - sizeof of a function name is the pointer size. */
        ZeroCCFunction *fn = zero_cc_find_function(c, name);
        if (fn) {
            return zero_cc_type_ptr(&fn->return_type);
        }
        /* Check for typedef/struct */
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (tdef) {
            ZeroCCType ty = *tdef;
            /* Handle member access on type name (e.g., S.x) */
            while (c->tokens[*pos].type == TK_DOT) {
                (*pos)++;
                if (c->tokens[*pos].type == TK_IDENT) {
                    const char *mem_name = c->tokens[*pos].str_val;
                    (*pos)++;
                    ty = zero_cc_type_find_member(c, ty, mem_name);
                }
            }
            return ty;
        }
        /* Unknown identifier */
        diag_report_error_at_token(c, "unknown identifier '%s'", c->tokens[*pos - 1].str_val);
        ZeroCCType unknown_ty;
        memset(&unknown_ty, 0, sizeof(unknown_ty));
        return unknown_ty;
    }
    /* Should not reach here for valid constant expression */
    diag_report_error_at_token(c, "invalid constant expression");
    ZeroCCType unknown_ty;
    memset(&unknown_ty, 0, sizeof(unknown_ty));
    return unknown_ty;
}

static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name)
{
    (void)c;
    if (base.tag == TYPE_PTR && base.base) {
        base = *base.base;
    }
    if (!zero_cc_type_is_struct_or_union(&base)) {
        return zero_cc_type_int();
    }
    const ZeroCCStructMember *m = zero_cc_type_find_member_info(&base, member_name);
    if (m) {
        ZeroCCType member_ty;
        memset(&member_ty, 0, sizeof(member_ty));
        member_ty.tag = m->tag;
        member_ty.size = m->size;
        member_ty.align = m->align;
        member_ty.base = m->base;
        snprintf(member_ty.struct_name, sizeof(member_ty.struct_name), "%s", m->name);
        return member_ty;
    }
    /* Member not found - return int */
    return zero_cc_type_int();
}

/* C99 6.5.9: the basic type specifiers may appear in ANY order, so
 * `short int`, `short unsigned`, `unsigned short int`, `long unsigned`,
 * `long signed`, `int unsigned`, `int signed`, `long long signed`, ...
 * are all legal.  Consume the whole specifier window and derive the type
 * from the SET of specifiers seen (order-insensitive), instead of the
 * previous one-token-per-branch handling that left trailing specifiers
 * unconsumed ("expected identifier in declaration" on `short int a;`). */
/* GNU 128-bit base type names (tokenized as identifiers). Returns 0 if the
 * name is not one of them, 1 for the signed forms (`__int128` / `__int128_t`),
 * 2 for the always-unsigned `__uint128_t`. */
static inline int zc_name_int128_kind(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "__int128") == 0 || strcmp(name, "__int128_t") == 0) return 1;
    if (strcmp(name, "__uint128_t") == 0) return 2;
    return 0;
}

static inline ZeroCCType zc_parse_basic_type_spec(ZeroCCCompiler *c, size_t *pos) {
    int has_char = 0, has_short = 0, has_long = 0;
    int has_float = 0, has_double = 0;
    int n_unsigned = 0, n_signed = 0;
    int has_int128 = 0, int128_kind = 0, has_float128 = 0;
    for (;;) {
        ZeroCCTokType u = (*pos < c->token_count) ? c->tokens[*pos].type : TK_EOF;
        if (u == TK_INT) { /* int - carries no extra information */ }
        else if (u == TK_CHAR) has_char = 1;
        else if (u == TK_SHORT) has_short = 1;
        else if (u == TK_LONG) has_long++;
        else if (u == TK_FLOAT_KW) has_float = 1;
        else if (u == TK_DOUBLE_KW) has_double = 1;
        else if (u == TK_UNSIGNED) n_unsigned++;
        else if (u == TK_SIGNED) n_signed++;
        else if (u == TK_IDENT) {
            /* GNU 128-bit base type (`__int128` / `__uint128_t` / `__int128_t`). */
            int k = zc_name_int128_kind(c->tokens[*pos].str_val);
            if (k) { has_int128 = 1; int128_kind = k; }
            /* GNU __float128 (Sprint 112). */
            else if (strcmp(c->tokens[*pos].str_val, "__float128") == 0) has_float128 = 1;
            else break;
        }
        /* Type qualifiers interleave with specifiers in any order
         * (C99 6.7.1): `unsigned long long const`, `volatile unsigned
         * int`, ... - they carry no effect on the scalar type here. */
        else if (u == TK_CONST || u == TK_VOLATILE || u == TK_RESTRICT) { /* skip */ }
        else break;
        (*pos)++;
    }
    bool is_unsigned = (n_unsigned > 0) && (n_signed == 0);
    if (has_int128) {
        /* `__uint128_t` is always unsigned; `__int128`/`__int128_t` follow
         * the `unsigned`/`signed` specifier. */
        bool u = (int128_kind == 2) || is_unsigned;
        return u ? zero_cc_type_uint128() : zero_cc_type_int128();
    }
    if (has_float128) return zero_cc_type_float128();
    if (has_double || has_float) {
        if (has_double) return has_long ? zero_cc_type_long_double() : zero_cc_type_double();
        return has_long ? zero_cc_type_long_double() : zero_cc_type_float();
    }
    if (has_char) return is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char();
    if (has_short) return is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
    if (has_long) return is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
    return is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
}

/* Consume one array-dimension bracket group `[...]` in a type-NAME context
 * (*pos at '[').  Returns the length when the dimension is a single numeric
 * literal, else -1: an expression dimension (`sizeof (T)`, `2*8`, `n`) has
 * a runtime value that is irrelevant to a type name (casts reinterpret the
 * pointer; named declarations go through the declarator stack, which keeps
 * the real dimension expression). */
static inline ssize_t zc_consume_type_dim(ZeroCCCompiler *c, size_t *pos) {
    (*pos)++; /* '[' */
    ssize_t alen = 0;
    bool numeric = false;
    if (*pos < c->token_count && c->tokens[*pos].type == TK_NUM) {
        alen = (ssize_t)c->tokens[*pos].num_val;
        numeric = true;
        (*pos)++;
    } else if (*pos < c->token_count && c->tokens[*pos].type != TK_RBRACKET) {
        int pd = 0, bd = 0;
        while (*pos < c->token_count) {
            ZeroCCTokType dt = c->tokens[*pos].type;
            if (dt == TK_LPAREN) pd++;
            else if (dt == TK_RPAREN) { if (pd == 0) break; pd--; }
            else if (dt == TK_LBRACKET) bd++;
            else if (dt == TK_RBRACKET) { if (bd == 0) break; bd--; }
            (*pos)++;
        }
    }
    if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
    return numeric ? alen : -1;
}

static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType ty;
    /* Type qualifiers may precede the type name (C99 6.7.3: const int, etc.) */
    bool q_const = false, q_volatile = false, q_restrict = false;
    while (*pos < c->token_count &&
           (c->tokens[*pos].type == TK_CONST || c->tokens[*pos].type == TK_VOLATILE ||
            c->tokens[*pos].type == TK_RESTRICT)) {
        if (c->tokens[*pos].type == TK_CONST) q_const = true;
        else if (c->tokens[*pos].type == TK_VOLATILE) q_volatile = true;
        else q_restrict = true;
        (*pos)++;
    }
    /* GNU vector attribute marker (`__attribute__((__vector_size__(N)))`,
     * preprocessed into `__zc_vector_size__(N)`) may lead the type name:
     * `sizeof(__attribute__((__vector_size__(16))) short)`. Same for the
     * `mode(X)` integer-width marker (`__zc_mode__(QI)`). */
    size_t vec_attr_size = 0;
    size_t mode_attr_bytes = 0;
    {
        size_t probe = 0;
        for (int mk = 0; mk < 4; mk++) {
            bool got = false;
            if (vec_attr_size == 0 &&
                zero_cc_parse_vector_attr(c, pos, &probe)) {
                vec_attr_size = probe;
                got = true;
            }
            if (mode_attr_bytes == 0 &&
                zero_cc_parse_mode_attr(c, pos, &probe)) {
                mode_attr_bytes = probe;
                got = true;
            }
            if (!got) break;
        }
    }
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_STRUCT || t == TK_UNION) {
        (*pos)++;
        if (c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            ZeroCCType *st = zero_cc_find_struct(c, name);
            if (!st) st = zero_cc_find_typedef(c, name);
            if (st) {
                ty = *st;
            } else if (strcmp(name, "struct") != 0 && strcmp(name, "union") != 0 &&
                       strcmp(name, "enum") != 0) {
                /* Forward reference: materialize an incomplete (zero-size)
                 * type so globals/pointees keep the struct's identity
                 * instead of silently becoming long (c-torture pr46388). */
                memset(&ty, 0, sizeof(ty));
                ty.tag = (t == TK_UNION) ? TYPE_UNION : TYPE_STRUCT;
                ty.size = 0;
                ty.align = 1;
                snprintf(ty.struct_name, sizeof(ty.struct_name), "%s", name);
                zero_cc_register_struct(c, name, ty);
            } else {
                ty = zero_cc_type_int64();
            }
            (*pos)++;
        } else if (c->tokens[*pos].type == TK_LBRACE && c->parse_struct_body) {
            size_t save = c->token_pos;
            c->token_pos = *pos;
            ty = c->parse_struct_body(c, t == TK_UNION);
            *pos = c->token_pos;
            c->token_pos = save;
        } else {
            ty = zero_cc_type_int64();
        }
    } else if (t == TK_ENUM) {
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            ZeroCCType *et = zero_cc_find_enum(c, name);
            if (!et) et = zero_cc_find_typedef(c, name);
            if (et) ty = *et;
            else ty = zero_cc_type_enum(name);
            (*pos)++;
        } else {
            ty = zero_cc_type_enum(NULL);
        }
    } else if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        /* C99 6.7.2.3: `_Complex double` / `double _Complex`. */
        if (zero_cc_tok_is_complex_spec(name)) {
            (*pos)++;
            /* Standalone `_Complex` (no inner base) means `_Complex double`;
             * otherwise parse the explicit inner real type. */
            ZeroCCType inner = zero_cc_token_is_complex_inner_base(c, *pos)
                ? zero_cc_parse_type_name_at_pos(c, pos)
                : zero_cc_type_double();
            return zero_cc_type_complex(&inner);
        }
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (!tdef) tdef = zero_cc_find_enum(c, name);
        if (tdef) {
            ty = *tdef;
            (*pos)++;
        } else {
            /* GNU 128-bit base type (`__int128` / `__uint128_t` / `__int128_t`). */
            int i128k = zc_name_int128_kind(name);
            ty = (i128k == 2) ? zero_cc_type_uint128()
               : (i128k == 1) ? zero_cc_type_int128()
               : (strcmp(name, "__float128") == 0) ? zero_cc_type_float128()
               : zero_cc_type_int();
            (*pos)++;
        }
    } else if (t == TK_INT || t == TK_CHAR || t == TK_SHORT || t == TK_LONG ||
               t == TK_FLOAT_KW || t == TK_DOUBLE_KW ||
               t == TK_SIGNED || t == TK_UNSIGNED) {
        /* Basic type specifier window in any order (C99 6.5.9). */
        ty = zc_parse_basic_type_spec(c, pos);
    } else if (t == TK_VOID) {
        ty = zero_cc_type_void(); (*pos)++;
    } else if (t == TK_BOOL_KW) {
        ty = zero_cc_type_bool(); (*pos)++;
    } else if (t == TK_ATOMIC) {
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            (*pos)++;
            ty = zero_cc_parse_type_name_at_pos(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        } else {
            ty = zero_cc_parse_type_name_at_pos(c, pos);
        }
        ty.is_atomic = true;
    } else if (t == TK_SIGNED || t == TK_UNSIGNED) {
        /* signed/unsigned [short|int|long|long long] */
        bool is_unsigned = (t == TK_UNSIGNED);
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_SHORT) {
            ty = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++;
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_CHAR) {
            ty = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char();
            (*pos)++;
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_LONG) {
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LONG) (*pos)++; /* long long */
            if (*pos < c->token_count &&
                (c->tokens[*pos].type == TK_DOUBLE_KW || c->tokens[*pos].type == TK_FLOAT_KW)) {
                (*pos)++;
                ty = zero_cc_type_long_double();
            } else {
                ty = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++; /* unsigned long int */
        } else {
            if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++; /* [int] */
            ty = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
        }
    } else if (t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        bool unqual = (t == TK_TYPEOF_UNQUAL);
        (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            (*pos)++;
            if (zero_cc_tok_is_type_start(c, *pos)) {
                ty = zero_cc_parse_type_name_at_pos(c, pos);
            } else {
                ty = zero_cc_type_of_equality(c, pos);
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
                (*pos)++;
            }
        } else {
            ty = zero_cc_type_int();
        }
        if (unqual) {
            ty.is_const = false;
            ty.is_volatile = false;
            ty.is_restrict = false;
            ty.is_atomic = false;
        }
    } else {
        ty = zero_cc_type_int();
    }
    /* Leading qualifiers attach to the BASE type, not the outermost
     * pointer: `const char *ss` = pointer to const char (C99 6.7.3).
     * Applied now, before pointer/array wrapping. */
    if (q_const) ty.is_const = true;
    if (q_volatile) ty.is_volatile = true;
    if (q_restrict) ty.is_restrict = true;
    /* Vector attribute trailing the base specifier:
     * `short __attribute__((__vector_size__(16)))` / `V __zc_vector_size__(16)`.
     * Wraps the base type in a 128-bit vector before any pointer/array
     * declarator suffixes are applied. Same for `mode(X)`:
     * `unsigned int __attribute__((mode(QI)))` resizes the integer base.
     * The loop handles chained attributes in any order. */
    for (int mk = 0; mk < 4; mk++) {
        size_t probe = 0;
        bool got = false;
        if (vec_attr_size == 0 &&
            zero_cc_parse_vector_attr(c, pos, &probe)) {
            vec_attr_size = probe;
            got = true;
        }
        if (mode_attr_bytes == 0 &&
            zero_cc_parse_mode_attr(c, pos, &probe)) {
            mode_attr_bytes = probe;
            got = true;
        }
        if (!got) break;
    }
    if (mode_attr_bytes > 0) {
        ty = zero_cc_type_apply_mode(ty, mode_attr_bytes);
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ty)) {
        ty = zero_cc_type_vector(&ty, vec_attr_size);
    }
    /* Declarator suffixes (C99 6.7.5): parenthesized abstract
     * declarators (`(*)(void)`, `(*)[4]`, `(*[3])(float)`) and pointer
     * stars may alternate in any order - `int *(*)(long)`,
     * `struct A *(*) (long)` (c-torture compile/20010102-1).  Stars
     * BEFORE a function-pointer paren block belong to the function's
     * RETURN type, so the is_func_ptr layer is always the wrap applied
     * directly around the already-starred base. */
    for (;;) {
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN &&
            *pos + 1 < c->token_count && c->tokens[*pos + 1].type == TK_STAR) {
        (*pos)++; /* consume '(' */
        int ptr_cnt = 0;
        bool is_func = false;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
            ptr_cnt++;
            (*pos)++;
            while (*pos < c->token_count &&
                   (c->tokens[*pos].type == TK_CONST ||
                    c->tokens[*pos].type == TK_VOLATILE ||
                    c->tokens[*pos].type == TK_RESTRICT)) (*pos)++;
        }
        ssize_t inner_arr_len = -1;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            ssize_t alen = zc_consume_type_dim(c, pos);
            if (alen < 0) alen = 0;
            inner_arr_len = (inner_arr_len < 0) ? alen : inner_arr_len * alen;
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
            int pdepth = 1;
            (*pos)++;
            while (*pos < c->token_count && pdepth > 0) {
                if (c->tokens[*pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[*pos].type == TK_RPAREN) pdepth--;
                (*pos)++;
            }
            is_func = true;
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
                ssize_t arr_len = zc_consume_type_dim(c, pos);
                if (arr_len < 0) arr_len = 0;
                ZeroCCType elem_ty = ty;
                ty = zero_cc_type_array(&elem_ty, arr_len);
            }
        }
        for (int p = 0; p < ptr_cnt; p++) {
            ty = zero_cc_type_ptr(&ty);
            if (is_func && p == 0) ty.is_func_ptr = true;
        }
        if (inner_arr_len >= 0) {
            ZeroCCType elem_ty = ty;
            ty = zero_cc_type_array(&elem_ty, inner_arr_len);
        }
        }
        int star_waits = 0;
        while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
            star_waits++;
            ty = zero_cc_type_ptr(&ty);
            (*pos)++;
            while (*pos < c->token_count &&
                   (c->tokens[*pos].type == TK_CONST ||
                    c->tokens[*pos].type == TK_VOLATILE ||
                    c->tokens[*pos].type == TK_RESTRICT)) (*pos)++;
            {
                size_t probe = 0;
                while (zero_cc_parse_aligned_attr(c, pos, &probe)) { /* skip */ }
            }
        }
        if (star_waits == 0 &&
            !(*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN &&
              *pos + 1 < c->token_count && c->tokens[*pos + 1].type == TK_STAR)) {
            break;
        }
    }
    /* Suffix complex: `double _Complex` (C99 6.7.2.3). */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT &&
        c->tokens[*pos].str_val &&
        zero_cc_tok_is_complex_spec(c->tokens[*pos].str_val) &&
        !ty.is_complex) {
        (*pos)++;
        ty = zero_cc_type_complex(&ty);
    }
    while (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
        (*pos)++;
        ssize_t arr_len = 0;
        if (*pos < c->token_count && c->tokens[*pos].type == TK_NUM) {
            arr_len = (ssize_t)c->tokens[*pos].num_val;
            (*pos)++;
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) {
            (*pos)++;
        }
        ZeroCCType elem_ty = ty;
        ty = zero_cc_type_array(&elem_ty, arr_len);
    }
    (void)q_const; (void)q_volatile; (void)q_restrict; /* applied at the base type */
    return ty;
}

static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c) {
    // Handle parentheses
    bool lparen = false;
    if (c->last_type_ptr_depth != 0) c->last_type_ptr_depth = 0;
    if (c->tokens[c->token_pos].type == TK_LPAREN) {
        lparen = true;
        c->token_pos++;
    }
    /* Handle type qualifiers (const, volatile, restrict) at the start */
    bool q_const = false, q_volatile = false, q_restrict = false;
    while (c->tokens[c->token_pos].type == TK_CONST ||
           c->tokens[c->token_pos].type == TK_VOLATILE ||
           c->tokens[c->token_pos].type == TK_RESTRICT) {
        if (c->tokens[c->token_pos].type == TK_CONST) q_const = true;
        else if (c->tokens[c->token_pos].type == TK_VOLATILE) q_volatile = true;
        else q_restrict = true;
        c->token_pos++;
    }
    /* GNU vector attribute marker (`__attribute__((__vector_size__(N)))`),
     * either leading (`__attribute__(...) short g;`) or trailing the base
     * specifier (`short __attribute__(...) g;`). */
    size_t vec_attr_size = 0;
    size_t align_attr_val = 0;
    size_t mode_attr_bytes = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        bool consumed = false;
        /* Markers may chain in any order: consume them all, then require a
         * type start (rewind otherwise). */
        for (int mk = 0; mk < 4; mk++) {
            bool got = false;
            if (vec_attr_size == 0 &&
                zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                vec_attr_size = probe;
                got = true;
            }
            if (align_attr_val == 0 &&
                zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                align_attr_val = probe;
                got = true;
            }
            if (mode_attr_bytes == 0 &&
                zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) {
                mode_attr_bytes = probe;
                got = true;
            }
            if (!got) break;
            consumed = true;
        }
        if (consumed &&
            (c->token_pos >= c->token_count ||
             !zero_cc_tok_is_type_start(c, c->token_pos))) {
            c->token_pos = save;
            vec_attr_size = 0;
            align_attr_val = 0;
            mode_attr_bytes = 0;
        }
    }
    ZeroCCType ty;
    if (c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL) {
        ty = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    } else if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            const char *name = c->tokens[c->token_pos].str_val;
            ZeroCCType *st = zero_cc_find_struct(c, name);
            if (!st) st = zero_cc_find_typedef(c, name);
            if (st) {
                ty = *st;
            } else if (strcmp(name, "struct") != 0 && strcmp(name, "union") != 0 &&
                       strcmp(name, "enum") != 0) {
                /* Forward reference: register an incomplete (zero-size)
                 * type so the struct keeps its identity instead of
                 * silently becoming long (c-torture pr46388). */
                memset(&ty, 0, sizeof(ty));
                ty.tag = (c->tokens[c->token_pos - 1].type == TK_UNION) ? TYPE_UNION : TYPE_STRUCT;
                ty.size = 0;
                ty.align = 1;
                snprintf(ty.struct_name, sizeof(ty.struct_name), "%s", name);
                zero_cc_register_struct(c, name, ty);
            } else {
                ty = zero_cc_type_int64();
            }
            c->token_pos++;
        } else {
            ty = zero_cc_type_int64();
        }
    } else if (c->tokens[c->token_pos].type == TK_ENUM) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            const char *name = c->tokens[c->token_pos].str_val;
            ZeroCCType *et = zero_cc_find_enum(c, name);
            if (!et) et = zero_cc_find_typedef(c, name);
            if (et) ty = *et;
            else ty = zero_cc_type_enum(name);
            c->token_pos++;
        } else {
            ty = zero_cc_type_enum(NULL);
        }
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        const char *name = c->tokens[c->token_pos].str_val;
        /* C99 6.7.2.3: the _Complex / __complex__ type specifier.
         * Prefix form: `_Complex double`; the suffix form is handled
         * after the base specifier below. */
        if (zero_cc_tok_is_complex_spec(name)) {
            c->token_pos++;
            /* Standalone `_Complex` (no inner base) means `_Complex double`;
             * otherwise parse the explicit inner real type. */
            ZeroCCType inner = zero_cc_token_is_complex_inner_base(c, c->token_pos)
                ? zero_cc_parse_type_name(c)
                : zero_cc_type_double();
            return zero_cc_type_complex(&inner);
        }
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (!tdef) tdef = zero_cc_find_enum(c, name);
        if (tdef) {
            ty = *tdef;
            c->token_pos++;
        } else if (strcmp(name, "int") == 0) {
            ty = zero_cc_type_int();
            c->token_pos++;
        } else if (strcmp(name, "char") == 0) {
            ty = zero_cc_type_char();
            c->token_pos++;
        } else if (strcmp(name, "long") == 0) {
            // check if next is also long
            if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_LONG) {
                ty = zero_cc_type_int64();
                c->token_pos += 2;
            } else {
                ty = zero_cc_type_int64(); // assume long is 64-bit
                c->token_pos++;
            }
        } else if (strcmp(name, "__int128") == 0 || strcmp(name, "__int128_t") == 0 ||
                   strcmp(name, "__uint128_t") == 0) {
            // GNU 128-bit base type reached as a bare identifier (no leading
            // unsigned/signed keyword, which would have routed the specifier
            // window to zc_parse_basic_type_spec instead). `__uint128_t` is
            // always unsigned; `__int128`/`__int128_t` are signed here.
            bool u128 = (strcmp(name, "__uint128_t") == 0);
            ty = u128 ? zero_cc_type_uint128() : zero_cc_type_int128();
            c->token_pos++;
        } else if (strcmp(name, "__float128") == 0) {
            // GNU __float128 cast target (Sprint 112).
            ty = zero_cc_type_float128();
            c->token_pos++;
        } else if (strcmp(name, "y") == 0) {
            // lookup local variable y
            ZeroCCSym *sym = zero_cc_find_local(c, "y");
            if (sym) {
                ty = sym->type;
            } else {
                ty = zero_cc_type_int64();
            }
            c->token_pos++;
        } else {
            // unknown identifier, default to int64
            ty = zero_cc_type_int64();
            c->token_pos++;
        }
    } else if (c->tokens[c->token_pos].type == TK_VOID) {
        ty = zero_cc_type_void();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) {
        ty = zero_cc_type_float();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
        ty = zero_cc_type_double();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_BOOL_KW) {
        ty = zero_cc_type_bool();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_ATOMIC) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++;
            ty = zero_cc_parse_type_name(c);
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        } else {
            ty = zero_cc_parse_type_name(c);
        }
        ty.is_atomic = true;
    } else if (c->tokens[c->token_pos].type == TK_INT ||
               c->tokens[c->token_pos].type == TK_CHAR ||
               c->tokens[c->token_pos].type == TK_SHORT ||
               c->tokens[c->token_pos].type == TK_LONG ||
               c->tokens[c->token_pos].type == TK_FLOAT_KW ||
               c->tokens[c->token_pos].type == TK_DOUBLE_KW ||
               c->tokens[c->token_pos].type == TK_SIGNED ||
               c->tokens[c->token_pos].type == TK_UNSIGNED) {
        /* Basic type specifier window in any order (C99 6.5.9). */
        ty = zc_parse_basic_type_spec(c, &c->token_pos);
    } else {
        // default
        ty = zero_cc_type_int64();
    }
    /* Leading qualifiers attach to the BASE type, not the outermost
     * pointer: `const char *ss` = pointer to const char (C99 6.7.3).
     * Applied now, before pointer/array wrapping. */
    if (q_const) ty.is_const = true;
    if (q_volatile) ty.is_volatile = true;
    if (q_restrict) ty.is_restrict = true;
    /* Vector attribute trailing the base specifier: consume it (otherwise
     * the marker identifier would be read as the variable name) and wrap
     * the base type in a vector. Same for aligned(N): raise the type's
     * alignment (and pad struct sizes) to the given value. Same for
     * mode(X): resize an integer base type (c-torture execute/memclr).
     * The loop handles chained attributes in any order. */
    for (int mk = 0; mk < 4; mk++) {
        size_t probe = 0;
        bool got = false;
        if (vec_attr_size == 0 &&
            zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            vec_attr_size = probe;
            got = true;
        }
        if (align_attr_val == 0 &&
            zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
            align_attr_val = probe;
            got = true;
        }
        if (mode_attr_bytes == 0 &&
            zero_cc_parse_mode_attr(c, &c->token_pos, &probe)) {
            mode_attr_bytes = probe;
            got = true;
        }
        if (!got) break;
    }
    if (mode_attr_bytes > 0) {
        ty = zero_cc_type_apply_mode(ty, mode_attr_bytes);
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ty)) {
        ty = zero_cc_type_vector(&ty, vec_attr_size);
    }
    if (align_attr_val > 0) {
        if (ty.tag == TYPE_STRUCT || ty.tag == TYPE_UNION) {
            ZeroCCType *st = zero_cc_find_struct(c, ty.struct_name);
            if (!st) st = zero_cc_find_typedef(c, ty.struct_name);
            if (st && align_attr_val > st->align) {
                st->align = align_attr_val;
                if (st->size < align_attr_val) st->size = align_attr_val;
                if ((st->size % align_attr_val) != 0) {
                    st->size = (st->size + align_attr_val - 1) / align_attr_val * align_attr_val;
                }
                ty = *st;
            }
        } else if (align_attr_val > ty.align) {
            ty.align = align_attr_val;
            if (ty.size < align_attr_val) ty.size = align_attr_val;
            if ((ty.size % align_attr_val) != 0) {
                ty.size = (ty.size + align_attr_val - 1) / align_attr_val * align_attr_val;
            }
        }
    }
    /* Suffix complex: `double _Complex` (C99 6.7.2.3). */
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT &&
        c->tokens[c->token_pos].str_val &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val) &&
        !ty.is_complex) {
        c->token_pos++;
        ty = zero_cc_type_complex(&ty);
    }
    /* Parenthesized abstract declarator: (*)(void), (*)[4], or (*[3])(float).
     * A parenthesized declarator that contains an identifier (`(*fp)(void)`)
     * is a NAMED declarator, not an abstract type: leave the tokens for the
     * declarator parser instead of swallowing the name. */
    {
        size_t probe = c->token_pos + 2;
        while (probe < c->token_count &&
               (c->tokens[probe].type == TK_LPAREN ||
                c->tokens[probe].type == TK_STAR ||
                c->tokens[probe].type == TK_CONST ||
                c->tokens[probe].type == TK_VOLATILE ||
                c->tokens[probe].type == TK_RESTRICT)) probe++;
        bool named_declarator = (probe < c->token_count && c->tokens[probe].type == TK_IDENT);
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN &&
            c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_STAR &&
            !named_declarator) {
            c->token_pos++; /* consume '(' */
        int ptr_cnt = 0;
        bool is_func = false;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
            ptr_cnt++;
            c->token_pos++;
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT)) c->token_pos++;
        }
        ssize_t inner_arr_len = -1;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
            c->token_pos++;
            ssize_t alen = 0;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
                alen = (ssize_t)c->tokens[c->token_pos].num_val;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
            inner_arr_len = (inner_arr_len < 0) ? alen : inner_arr_len * alen;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
            int pdepth = 1;
            c->token_pos++;
            while (c->token_pos < c->token_count && pdepth > 0) {
                if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                c->token_pos++;
            }
            is_func = true;
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                c->token_pos++;
                ssize_t arr_len = 0;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_NUM) {
                    arr_len = (ssize_t)c->tokens[c->token_pos].num_val;
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                ZeroCCType elem_ty = ty;
                ty = zero_cc_type_array(&elem_ty, arr_len);
            }
        }
        for (int p = 0; p < ptr_cnt; p++) {
            ty = zero_cc_type_ptr(&ty);
            if (is_func && p == 0) ty.is_func_ptr = true;
            c->last_type_ptr_depth++;
        }
        if (inner_arr_len >= 0) {
            ZeroCCType elem_ty = ty;
            ty = zero_cc_type_array(&elem_ty, inner_arr_len);
        }
        }
    }
    // Parse pointer stars (qualifiers and GNU aligned(N) markers may sit
    // between chunks: `int *const *p;`, `int *__attr__((aligned(16))) *p;`)
    while (c->tokens[c->token_pos].type == TK_STAR) {
        ty = zero_cc_type_ptr(&ty);
        c->last_type_ptr_depth++;
        c->token_pos++;
        while (c->tokens[c->token_pos].type == TK_CONST ||
               c->tokens[c->token_pos].type == TK_VOLATILE ||
               c->tokens[c->token_pos].type == TK_RESTRICT) c->token_pos++;
        {
            size_t probe = 0;
            while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) { /* skip */ }
        }
    }
    // Parse array brackets: we ignore size for alignment
    while (c->tokens[c->token_pos].type == TK_LBRACKET) {
        // skip until matching ]
        int bracket_depth = 1;
        c->token_pos++;
        while (c->tokens[c->token_pos].type != TK_EOF && bracket_depth > 0) {
            if (c->tokens[c->token_pos].type == TK_LBRACKET) bracket_depth++;
            else if (c->tokens[c->token_pos].type == TK_RBRACKET) bracket_depth--;
            c->token_pos++;
        }
        // array alignment is same as element type, so ty unchanged
    }
    if (lparen) {
        if (c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++;
        }
    }
    (void)q_const; (void)q_volatile; (void)q_restrict; /* applied at the base type */
    return ty;
}


#endif /* ZERO_CC_PARSER_TYPE_PARSER_H */
