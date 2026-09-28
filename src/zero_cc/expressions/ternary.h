#ifndef ZERO_CC_EXPRESSIONS_TERNARY_H
#define ZERO_CC_EXPRESSIONS_TERNARY_H

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

/* Evaluate sizeof: returns size in bytes */


/* Type-only expression parser for sizeof/_Alignof operand */

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

#endif /* ZERO_CC_EXPRESSIONS_TERNARY_H */
