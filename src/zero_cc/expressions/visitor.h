#ifndef ZERO_CC_EXPRESSIONS_VISITOR_H
#define ZERO_CC_EXPRESSIONS_VISITOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations */
static inline void zero_cc_parse_expr(ZeroCCCompiler *c);
static inline void zero_cc_parse_stmt(ZeroCCCompiler *c);
static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);


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
        return zero_cc_type_int();
    }
    /* Should not reach here for valid constant expression */
    return zero_cc_type_int();
}

/* Helper: find member type in struct/union */
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name)
{
    (void)c;
    if (base.tag == TYPE_PTR && base.base) {
        base = *base.base;
    }
    if (base.tag != TYPE_STRUCT) {
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

/* Parse type name at current position (helper for casts in type-only parser) */
static inline ZeroCCType zero_cc_parse_type_name_at_pos(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCType ty;
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_IDENT) {
        const char *name = c->tokens[*pos].str_val;
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
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
    while (c->tokens[*pos].type == TK_STAR) {
        ty = zero_cc_type_ptr(&ty);
        (*pos)++;
    }
    return ty;
}

/* Updated eval_sizeof using type-only parser */
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

/* Updated eval_alignof using type-only parser */
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

static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c)
{
    size_t pos = c->token_pos;
    int64_t result = parse_equality(c, &pos);
    c->token_pos = pos;
    return result;
}

#endif /* ZERO_CC_EXPRESSIONS_VISITOR_H */
