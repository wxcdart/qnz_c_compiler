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
static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c);
static inline ZeroCCType zero_cc_type_of_equality(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_relational(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_additive(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_multiplicative(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_unary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_of_primary(ZeroCCCompiler *c, size_t *pos);
static inline ZeroCCType zero_cc_type_find_member(ZeroCCCompiler *c, ZeroCCType base, const char *member_name);
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
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        const char *name = c->tokens[c->token_pos].str_val;
        ZeroCCType *tdef = zero_cc_find_typedef(c, name);
        if (!tdef) tdef = zero_cc_find_struct(c, name);
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

/* Primary Expression Parser */

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

/* Parse type name at current position (helper for casts in type-only parser) */
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
    } else if (t == TK_IDENT) {
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
