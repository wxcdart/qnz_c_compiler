#ifndef ZERO_CC_PARSER_CONST_EVAL_H
#define ZERO_CC_PARSER_CONST_EVAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types/types.h"
#include "../types/registry.h"

/* Forward declarations for recursive functions */
static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_additive(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_unary(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_primary(ZeroCCCompiler *c, size_t *pos);

static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);

static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);
static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos);

static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
static inline int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);

static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_SIZEOF */
    if (c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty = zero_cc_type_of_equality(c, pos);
    if (c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.size;
}

static inline int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_ALIGNOF */
    if (c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty = zero_cc_type_of_equality(c, pos);
    if (c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.align ? ty.align : 1;
}

static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_relational(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            int64_t right = parse_relational(c, pos);
            if (op == TK_EQ)
                left = (left == right);
            else
                left = (left != right);
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            int64_t right = parse_additive(c, pos);
            int64_t res = 0;
            if (op == TK_LT) res = (left < right);
            else if (op == TK_GT) res = (left > right);
            else if (op == TK_LE) res = (left <= right);
            else if (op == TK_GE) res = (left >= right);
            left = res;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_additive(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_multiplicative(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_PLUS || op == TK_MINUS) {
            (*pos)++;
            int64_t right = parse_multiplicative(c, pos);
            if (op == TK_PLUS)
                left = left + right;
            else
                left = left - right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_multiplicative(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_unary(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT) {
            (*pos)++;
            int64_t right = parse_unary(c, pos);
            if (op == TK_STAR)
                left = left * right;
            else if (op == TK_SLASH)
                left = left / right;
            else
                left = left % right;
        } else {
            break;
        }
    }
    return left;
}

static inline int64_t parse_unary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_PLUS) {
        (*pos)++;
        return parse_unary(c, pos);
    }
    if (t == TK_MINUS) {
        (*pos)++;
        return -parse_unary(c, pos);
    }
    if (t == TK_TILDE) {
        (*pos)++;
        return ~parse_unary(c, pos);
    }
    if (t == TK_BANG) {
        (*pos)++;
        return !parse_unary(c, pos);
    }
    if (t == TK_SIZEOF) {
        return eval_sizeof(c, pos);
    }
    if (t == TK_ALIGNOF) {
        return eval_alignof(c, pos);
    }
    return parse_primary(c, pos);
}

static inline int64_t parse_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        int64_t val = c->tokens[*pos].num_val;
        (*pos)++;
        return val;
    }
    if (t == TK_TRUE) {
        (*pos)++;
        return 1;
    }
    if (t == TK_FALSE) {
        (*pos)++;
        return 0;
    }
    if (t == TK_NULLPTR) {
        (*pos)++;
        return 0;
    }
    if (t == TK_LPAREN) {
        (*pos)++;
        int64_t val = parse_equality(c, pos);
        if (c->tokens[*pos].type == TK_RPAREN) {
            (*pos)++;
        }
        return val;
    }
    /* Should not reach here for valid constant expression */
    return 0;
}

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
        if (operand.tag == TYPE_PTR && operand.base) {
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
        (*pos)++;
        return zero_cc_type_int();
    }
    if (t == TK_FLOAT) {
        (*pos)++;
        return zero_cc_type_double();
    }
    if (t == TK_CHAR_LIT) {
        (*pos)++;
        return zero_cc_type_char();
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
        (*pos)++;
        return zero_cc_type_ptr(NULL);
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
    if (t == TK_SHORT) { (*pos)++; return zero_cc_type_int16(); }
    if (t == TK_LONG) { (*pos)++; return zero_cc_type_int64(); }
    if (t == TK_SIGNED) { (*pos)++; return zero_cc_type_int(); }
    if (t == TK_UNSIGNED) { (*pos)++; return zero_cc_type_uint32(); }
    if (t == TK_ATOMIC) { (*pos)++; if (c->tokens[*pos].type == TK_LPAREN) { (*pos)++; } ZeroCCType ty = zero_cc_type_of_primary(c, pos); if (c->tokens[*pos].type == TK_RPAREN) (*pos)++; ty.is_atomic = true; return ty; }
    if (t == TK_LPAREN) {
        (*pos)++;
        /* Could be cast or parenthesized expression */
        /* Look ahead: if next token is a type keyword, treat as cast */
        ZeroCCTokType next = c->tokens[*pos].type;
        if (next == TK_IDENT || next == TK_INT || next == TK_CHAR || next == TK_VOID ||
            next == TK_STRUCT || next == TK_UNION ||
            next == TK_FLOAT_KW || next == TK_DOUBLE_KW || next == TK_BOOL_KW ||
            next == TK_SIGNED || next == TK_UNSIGNED || next == TK_SHORT || next == TK_LONG || next == TK_ATOMIC) {
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
        return ty;
    }
    if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        (*pos)++;
        /* Check for local variable */
        ZeroCCSym *sym = zero_cc_find_local(c, name);
        if (sym) {
            ZeroCCType ty = sym->type;
            /* Handle member access: sym->member or sym.member */
            while (true) {
                ZeroCCTokType mt = c->tokens[*pos].type;
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
        c->had_error = 1;
        ZeroCCType unknown_ty;
        memset(&unknown_ty, 0, sizeof(unknown_ty));
        return unknown_ty;
    }
    /* Should not reach here for valid constant expression */
    c->had_error = 1;
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
    if (base.tag != TYPE_STRUCT && base.tag != TYPE_UNION) {
        return zero_cc_type_int();
    }
    for (size_t i = 0; i < base.member_count; i++) {
        if (strcmp(base.members[i].name, member_name) == 0) {
            ZeroCCType member_ty;
            memset(&member_ty, 0, sizeof(member_ty));
            member_ty.tag = base.members[i].tag;
            member_ty.size = base.members[i].size;
            member_ty.align = base.members[i].align;
            member_ty.base = base.members[i].base;
            snprintf(member_ty.struct_name, sizeof(member_ty.struct_name), "%s", base.members[i].name);
            return member_ty;
        }
    }
    /* Member not found - return int */
    return zero_cc_type_int();
}

static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType ty;
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_STRUCT || t == TK_UNION) {
        (*pos)++;
        if (c->tokens[*pos].type == TK_IDENT) {
            const char *name = c->tokens[*pos].str_val;
            ZeroCCType *st = zero_cc_find_struct(c, name);
            if (!st) st = zero_cc_find_typedef(c, name);
            if (st) ty = *st;
            else ty = zero_cc_type_int64();
            (*pos)++;
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
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
        if (!tdef) tdef = zero_cc_find_enum(c, name);
        if (tdef) {
            ty = *tdef;
            (*pos)++;
        } else {
            ty = zero_cc_type_int();
            (*pos)++;
        }
    } else if (t == TK_INT) {
        ty = zero_cc_type_int(); (*pos)++;
    } else if (t == TK_CHAR) {
        ty = zero_cc_type_char(); (*pos)++;
    } else if (t == TK_VOID) {
        ty = zero_cc_type_void(); (*pos)++;
    } else if (t == TK_FLOAT_KW) {
        ty = zero_cc_type_float(); (*pos)++;
    } else if (t == TK_DOUBLE_KW) {
        ty = zero_cc_type_double(); (*pos)++;
    } else if (t == TK_BOOL_KW) {
        ty = zero_cc_type_bool(); (*pos)++;
    } else if (t == TK_SHORT) {
        ty = zero_cc_type_int16(); (*pos)++;
    } else if (t == TK_LONG) {
        ty = zero_cc_type_int64(); (*pos)++;
    } else if (t == TK_ATOMIC) {
        ty = zero_cc_type_int(); (*pos)++;
    } else {
        ty = zero_cc_type_int();
    }
    while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
        ty = zero_cc_type_ptr(&ty);
        (*pos)++;
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
    return ty;
}

static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c) {
    // Handle parentheses
    bool lparen = false;
    if (c->tokens[c->token_pos].type == TK_LPAREN) {
        lparen = true;
        c->token_pos++;
    }
    ZeroCCType ty;
    if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            const char *name = c->tokens[c->token_pos].str_val;
            ZeroCCType *st = zero_cc_find_struct(c, name);
            if (!st) st = zero_cc_find_typedef(c, name);
            if (st) ty = *st;
            else ty = zero_cc_type_int64();
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
    } else if (c->tokens[c->token_pos].type == TK_SHORT) {
        ty = zero_cc_type_int16();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_INT) {
        ty = zero_cc_type_int();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_CHAR) {
        ty = zero_cc_type_char();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_LONG) {
        // check for long long
        if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_LONG) {
            ty = zero_cc_type_int64();
            c->token_pos += 2;
        } else {
            ty = zero_cc_type_int64();
            c->token_pos++;
        }
    } else {
        // default
        ty = zero_cc_type_int64();
    }
    // Parse pointer stars
    while (c->tokens[c->token_pos].type == TK_STAR) {
        ty = zero_cc_type_ptr(&ty);
        c->token_pos++;
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
    return ty;
}

static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c)
{
    size_t pos = c->token_pos;
    int64_t result = parse_equality(c, &pos);
    c->token_pos = pos;
    return result;
}

#endif /* ZERO_CC_PARSER_CONST_EVAL_H */
