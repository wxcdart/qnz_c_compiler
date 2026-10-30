#ifndef ZERO_CC_PARSER_CONST_EVAL_H
#define ZERO_CC_PARSER_CONST_EVAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/global_vars.h"

/* Forward declarations for recursive functions */
static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_bitand(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_bitxor(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_bitor(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos);
static inline int64_t parse_shift(ZeroCCCompiler *c, size_t *pos);
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

static inline int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);

/* Parse a `__zc_vector_size__( <const-expr> )` marker - emitted by the
 * preprocessor in place of `__attribute__((__vector_size__(N)))` (see the
 * __attribute__ special case in core/preprocessor.h). Operates on the
 * caller's token position *pos (either &c->token_pos or a local). On
 * success consumes the marker and returns 1 with *out_size = N (bytes);
 * returns 0 (and leaves *pos untouched) when the current token is not the
 * marker. Called both BEFORE the base type specifier (the attribute may
 * lead: `__attribute__((__vector_size__(16))) short`) and AFTER it
 * (`signed char __attribute__((__vector_size__(16))) V`). N is evaluated
 * as a C constant expression (so `2 * sizeof(long long)` works); N must
 * be positive and <= 512 bytes. */
static inline int zero_cc_parse_vector_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_size) {
    if (!c || !out_size || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
    if (!id || strcmp(id, "__zc_vector_size__") != 0) return 0;
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    size_t save = *pos;
    size_t save_tp = c->token_pos;
    c->token_pos = save + 2; /* consume ident + '(' */
    int64_t n = zero_cc_eval_const_expr(c);
    size_t after = c->token_pos;
    c->token_pos = save_tp; /* restore: advance *pos on success below */
    if (after >= c->token_count || c->tokens[after].type != TK_RPAREN) {
        *pos = save;
        return 0;
    }
    *pos = after + 1; /* consume ')' */
    if (n <= 0 || n > 512) {
        *pos = save;
        return 0;
    }
    *out_size = (size_t)n;
    return 1;
}

/* Parse a `__zc_aligned__( <const-expr> )` marker - emitted by the
 * preprocessor in place of `__attribute__((aligned(N)))` / `__aligned__(N)`
 * (see the __attribute__ special case in core/preprocessor.h). Same
 * shape as zero_cc_parse_vector_attr: on success consumes the marker and
 * returns 1 with *out_align = N (bytes); returns 0 (leaving *pos
 * untouched) when the current token is not the marker. N must be positive
 * and <= 4096 bytes. */
static inline int zero_cc_parse_aligned_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_align) {
    if (!c || !out_align || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
    if (!id || strcmp(id, "__zc_aligned__") != 0) return 0;
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    size_t save = *pos;
    size_t save_tp = c->token_pos;
    c->token_pos = save + 2; /* consume ident + '(' */
    int64_t n = zero_cc_eval_const_expr(c);
    size_t after = c->token_pos;
    c->token_pos = save_tp; /* restore: advance *pos on success below */
    if (after >= c->token_count || c->tokens[after].type != TK_RPAREN) {
        *pos = save;
        return 0;
    }
    *pos = after + 1; /* consume ')' */
    if (n <= 0 || n > 4096) {
        *pos = save;
        return 0;
    }
    *out_align = (size_t)n;
    return 1;
}

/* Parse a `__zc_alias__( "target" )` marker - emitted by the preprocessor
 * in place of `__attribute__((alias("target")))` (GNU symbol aliasing).
 * On success consumes the marker and returns 1 with *out_target pointing
 * at the target name (a token string valid for the compilation); returns 0
 * (leaving *pos untouched) when the current token is not the marker. */
static inline int zero_cc_parse_alias_attr(ZeroCCCompiler *c, size_t *pos, const char **out_target) {
    if (!c || !out_target || !pos || *pos >= c->token_count) return 0;
    ZeroCCToken *t = &c->tokens[*pos];
    const char *id = t->type == TK_IDENT ? (t->allocated_str ? t->allocated_str : t->str_val) : NULL;
    if (!id || strcmp(id, "__zc_alias__") != 0) return 0;
    if (*pos + 1 >= c->token_count || c->tokens[*pos + 1].type != TK_LPAREN) return 0;
    if (*pos + 2 >= c->token_count || c->tokens[*pos + 2].type != TK_STRING) return 0;
    if (*pos + 3 >= c->token_count || c->tokens[*pos + 3].type != TK_RPAREN) return 0;
    const ZeroCCToken *st = &c->tokens[*pos + 2];
    *out_target = st->allocated_str ? st->allocated_str : st->str_val;
    *pos = *pos + 4;
    return 1;
}

static inline bool zero_cc_tok_is_type_start(ZeroCCCompiler *c, size_t pos) {
    ZeroCCTokType t = c->tokens[pos].type;
    if (t == TK_INT || t == TK_CHAR || t == TK_VOID || t == TK_STRUCT ||
        t == TK_UNION || t == TK_ENUM || t == TK_FLOAT_KW || t == TK_DOUBLE_KW ||
        t == TK_BOOL_KW || t == TK_SIGNED || t == TK_UNSIGNED ||
        t == TK_SHORT || t == TK_LONG || t == TK_ATOMIC ||
        t == TK_CONST || t == TK_VOLATILE || t == TK_RESTRICT ||
        t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL) {
        return true;
    }
    if (t == TK_IDENT) {
        const char *name = c->tokens[pos].str_val;
        return zero_cc_find_typedef(c, name) != NULL;
    }
    return false;
}

/* Size/alignment of a plain-identifier operand for sizeof(expr)/_Alignof(expr):
 * resolve against local symbols first, then the global variable table.
 * An optional [index] suffix selects the array element size.
 * Returns 0 when the operand's type cannot be determined at parse time.
 * On success the operand (incl. any [..] suffix) is consumed from *pos. */
static inline size_t zero_cc_sizeof_ident_size(ZeroCCCompiler *c, size_t *pos) {
    if (c->tokens[*pos].type != TK_IDENT) return 0;
    const char *name = c->tokens[*pos].str_val;
    ZeroCCType ty;
    bool found = false;
    ZeroCCSym *ls = zero_cc_find_local(c, name);
    if (ls && ls->type.size > 0) { ty = ls->type; found = true; }
    if (!found) {
        void *tls = zero_cc_find_tls(c, name);
        if (tls) {
            for (size_t i = 0; i < c->thread_local_count; i++) {
                if (strcmp(c->thread_locals[i].name, name) == 0) {
                    ty = c->thread_locals[i].type;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        ZeroCCGlobalVar *gv = find_global_var(name);
        if (gv && gv->type.size > 0) { ty = gv->type; found = true; }
    }
    if (!found) return 0;
    /* Only a plain `ident` or `ident[...]` operand is handled here.  A
     * member access (`->`/`.`), call, or inc/dec suffix means the operand
     * is a full postfix expression: consume only the identifier here and
     * the rest dangles, stalling the parser (and yielding the pointer's
     * size instead of the member's).  Fall through to the expression-type
     * walker (zero_cc_type_of_equality), which consumes the whole chain. */
    if (*pos + 1 < c->token_count) {
        ZeroCCTokType after = c->tokens[*pos + 1].type;
        if (after == TK_DOT || after == TK_ARROW || after == TK_LPAREN ||
            after == TK_INC || after == TK_DEC) {
            return 0;
        }
    }
    (*pos)++; /* consume the identifier */
    if (c->tokens[*pos].type == TK_LBRACKET) {
        /* a[expr] -> element size */
        size_t q = *pos;
        q++; /* [ */
        int depth = 1;
        while (q < c->token_count && depth > 0) {
            if (c->tokens[q].type == TK_LBRACKET) depth++;
            else if (c->tokens[q].type == TK_RBRACKET) depth--;
            q++;
        }
        *pos = q; /* past ] */
        if (ty.tag == TYPE_ARRAY && ty.base && ty.base->size > 0) {
            return ty.base->size;
        }
        return ty.size;
    }
    return ty.size;
}

static inline size_t zero_cc_sizeof_ident_align(ZeroCCCompiler *c, size_t *pos) {
    if (c->tokens[*pos].type != TK_IDENT) return 0;
    const char *name = c->tokens[*pos].str_val;
    ZeroCCType ty;
    bool found = false;
    ZeroCCSym *ls = zero_cc_find_local(c, name);
    if (ls && ls->type.align > 0) { ty = ls->type; found = true; }
    if (!found) {
        void *tls = zero_cc_find_tls(c, name);
        if (tls) {
            for (size_t i = 0; i < c->thread_local_count; i++) {
                if (strcmp(c->thread_locals[i].name, name) == 0) {
                    ty = c->thread_locals[i].type;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        ZeroCCGlobalVar *gv = find_global_var(name);
        if (gv && gv->type.align > 0) { ty = gv->type; found = true; }
    }
    if (!found) return 0;
    /* See zero_cc_sizeof_ident_size: postfix continuations after the
     * identifier must be handled by the expression-type walker. */
    if (*pos + 1 < c->token_count) {
        ZeroCCTokType after = c->tokens[*pos + 1].type;
        if (after == TK_DOT || after == TK_ARROW || after == TK_LPAREN ||
            after == TK_INC || after == TK_DEC) {
            return 0;
        }
    }
    (*pos)++; /* consume the identifier */
    if (c->tokens[*pos].type == TK_LBRACKET) {
        size_t q = *pos;
        q++;
        int depth = 1;
        while (q < c->token_count && depth > 0) {
            if (c->tokens[q].type == TK_LBRACKET) depth++;
            else if (c->tokens[q].type == TK_RBRACKET) depth--;
            q++;
        }
        *pos = q;
    }
    return ty.align ? ty.align : 1;
}
static inline int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_SIZEOF */
    bool has_paren = false;
    if (c->tokens[*pos].type == TK_LPAREN) {
        has_paren = true;
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty;
    /* `sizeof(type)` / `sizeof(type *)` / `sizeof(t arr[])`: a type name
     * (including pointer/array suffixes) needs the type parser; the
     * expression grammar treats `*` as multiplication and fails. */
    if (zero_cc_tok_is_type_start(c, *pos)) {
        ty = zero_cc_parse_type_name_at_pos(c, pos);
    } else {
        size_t id_sz = zero_cc_sizeof_ident_size(c, pos);
        if (id_sz > 0) {
            if (has_paren && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return (int64_t)id_sz;
        }
        ty = zero_cc_type_of_equality(c, pos);
    }
    if (has_paren && c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.size;
}

static inline int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume TK_ALIGNOF */
    bool has_paren = false;
    if (c->tokens[*pos].type == TK_LPAREN) {
        has_paren = true;
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty;
    if (zero_cc_tok_is_type_start(c, *pos)) {
        ty = zero_cc_parse_type_name_at_pos(c, pos);
    } else {
        size_t id_al = zero_cc_sizeof_ident_align(c, pos);
        if (id_al > 0) {
            if (has_paren && c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            return (int64_t)id_al;
        }
        ty = zero_cc_type_of_equality(c, pos);
    }
    if (has_paren && c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++;
    }
    return ty.align ? ty.align : 1;
}

static inline int64_t parse_equality(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_bitand(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_EQ || op == TK_NE) {
            (*pos)++;
            int64_t right = parse_bitand(c, pos);
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

static inline int64_t parse_bitand(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_bitxor(c, pos);
    while (c->tokens[*pos].type == TK_AMP) {
        (*pos)++;
        left &= parse_bitxor(c, pos);
    }
    return left;
}

static inline int64_t parse_bitxor(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_bitor(c, pos);
    while (c->tokens[*pos].type == TK_CARET) {
        (*pos)++;
        left ^= parse_bitor(c, pos);
    }
    return left;
}

static inline int64_t parse_bitor(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_relational(c, pos);
    while (c->tokens[*pos].type == TK_PIPE) {
        (*pos)++;
        left |= parse_relational(c, pos);
    }
    return left;
}

static inline int64_t parse_relational(ZeroCCCompiler *c, size_t *pos)
{
    int64_t left = parse_shift(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            (*pos)++;
            int64_t right = parse_shift(c, pos);
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

static inline int64_t parse_shift(ZeroCCCompiler *c, size_t *pos)
{
    size_t left_begin = *pos;
    int64_t left = parse_additive(c, pos);
    while (true) {
        ZeroCCTokType op = c->tokens[*pos].type;
        if (op == TK_SHL || op == TK_SHR) {
            (*pos)++;
            int64_t right = parse_additive(c, pos);
            unsigned long ul = (unsigned long)left;
            unsigned long ur = (unsigned long)right;
            if (ur >= 64) ur = 0; /* avoid UB; C99 leaves it undefined anyway */
            if (op == TK_SHL) {
                left = (int64_t)(ul << ur);
            } else {
                /* C99 6.5.7: >> on a SIGNED operand is an arithmetic (sign-
                 * extending) shift; on unsigned it is logical. The evaluator
                 * has no operand types, so use the literal's suffix when the
                 * left operand is a bare integer literal (bit0 of
                 * int_suffix = U) and default to arithmetic - the signed
                 * case, matching GCC for `0x...LL >> n` initializers. */
                bool l_unsigned = (c->tokens[left_begin].type == TK_INT) &&
                    (c->tokens[left_begin].int_suffix & 0x1);
                if (l_unsigned) {
                    left = (int64_t)(ul >> ur);
                } else {
                    left = (int64_t)left >> ur;
                }
            }
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

static inline int64_t zero_cc_eval_builtin_offsetof(ZeroCCCompiler *c, size_t *pos)
{
    (*pos)++; /* consume __builtin_offsetof */
    if (*pos < c->token_count && c->tokens[*pos].type == TK_LPAREN) {
        (*pos)++; /* consume '(' */
    }
    ZeroCCType ty = zero_cc_parse_type_name_at_pos(c, pos);
    if (*pos < c->token_count && c->tokens[*pos].type == TK_COMMA) {
        (*pos)++; /* consume ',' */
    }

    int64_t total_offset = 0;
    while (*pos < c->token_count && c->tokens[*pos].type != TK_RPAREN) {
        if (c->tokens[*pos].type == TK_DOT) {
            (*pos)++;
        }
        if (*pos < c->token_count && c->tokens[*pos].type == TK_IDENT) {
            const char *mname = c->tokens[*pos].str_val;
            (*pos)++;
            bool found = false;
            if (ty.tag == TYPE_STRUCT || ty.tag == TYPE_UNION) {
                for (size_t m = 0; m < ty.member_count; m++) {
                    if (strcmp(ty.members[m].name, mname) == 0) {
                        total_offset += (int64_t)ty.members[m].offset;
                        if (ty.members[m].base && (ty.members[m].tag == TYPE_STRUCT || ty.members[m].tag == TYPE_UNION)) {
                            ty = *ty.members[m].base;
                        } else if (ty.members[m].base && ty.members[m].tag == TYPE_ARRAY) {
                            ty.base = ty.members[m].base;
                            ty.tag = TYPE_ARRAY;
                            ty.size = ty.members[m].size;
                        } else {
                            ty.tag = ty.members[m].tag;
                            ty.size = ty.members[m].size;
                            ty.base = ty.members[m].base;
                            ty.member_count = 0;
                        }
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                break;
            }
        } else if (*pos < c->token_count && c->tokens[*pos].type == TK_LBRACKET) {
            (*pos)++; /* consume '[' */
            int64_t idx = parse_equality(c, pos);
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) {
                (*pos)++; /* consume ']' */
            }
            size_t elem_sz = (ty.base && ty.base->size > 0) ? ty.base->size : (ty.size > 0 ? ty.size : 4);
            total_offset += idx * (int64_t)elem_sz;
            if (ty.base) {
                ty = *ty.base;
            }
        } else {
            break;
        }
    }

    if (*pos < c->token_count && c->tokens[*pos].type == TK_RPAREN) {
        (*pos)++; /* consume ')' */
    }
    return total_offset;
}

static inline int64_t parse_primary(ZeroCCCompiler *c, size_t *pos)
{
    ZeroCCTokType t = c->tokens[*pos].type;
    if (t == TK_NUM) {
        int64_t val = c->tokens[*pos].num_val;
        (*pos)++;
        return val;
    }
    if (t == TK_CHAR_LIT) {
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
    if (t == TK_IDENT) {
        if (strcmp(c->tokens[*pos].str_val, "__builtin_offsetof") == 0) {
            return zero_cc_eval_builtin_offsetof(c, pos);
        }
        /* Enum constants are valid in constant expressions (C99 6.6). */
        const ZeroCCEnumConst *ec = zero_cc_find_enum_const(c, c->tokens[*pos].str_val);
        if (ec) {
            (*pos)++;
            return ec->value;
        }
        return 0; /* unknown identifier in constant expression */
    }
    if (t == TK_LPAREN) {
        (*pos)++;
        if (zero_cc_tok_is_type_start(c, *pos)) {
            /* Cast: (type) expr - skip the type, evaluate the operand. */
            ZeroCCType cast_ty = zero_cc_parse_type_name_at_pos(c, pos);
            (void)cast_ty;
            if (c->tokens[*pos].type == TK_RPAREN) (*pos)++;
            int64_t val = parse_unary(c, pos);
            return val;
        }
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
        while (*pos < c->token_count && (c->tokens[*pos].type == TK_INC || c->tokens[*pos].type == TK_DEC)) {
            (*pos)++;
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
        ZeroCCGlobalVar *gv = find_global_var(name);
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
        /* Check for function */
        ZeroCCFunction *fn = zero_cc_find_function(c, name);
        if (fn) {
            return fn->return_type;
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
     * `sizeof(__attribute__((__vector_size__(16))) short)`. */
    size_t vec_attr_size = 0;
    {
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, pos, &probe)) vec_attr_size = probe;
    }
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
        if (*pos < c->token_count && c->tokens[*pos].type == TK_LONG) (*pos)++; /* long long */
        if (*pos < c->token_count && c->tokens[*pos].type == TK_INT) (*pos)++; /* long int */
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
            ty = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
            (*pos)++;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_LONG) (*pos)++; /* long long */
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
    /* Vector attribute trailing the base specifier:
     * `short __attribute__((__vector_size__(16)))` / `V __zc_vector_size__(16)`.
     * Wraps the base type in a 128-bit vector before any pointer/array
     * declarator suffixes are applied. */
    if (vec_attr_size == 0) {
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, pos, &probe)) vec_attr_size = probe;
    }
    if (vec_attr_size > 0 && !zero_cc_type_is_vector(&ty)) {
        ty = zero_cc_type_vector(&ty, vec_attr_size);
    }
    /* Parenthesized abstract declarator: (*)(void), (*)[4], or (*[3])(float) */
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
            (*pos)++;
            ssize_t alen = 0;
            if (*pos < c->token_count && c->tokens[*pos].type == TK_NUM) {
                alen = (ssize_t)c->tokens[*pos].num_val;
                (*pos)++;
            }
            if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
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
                (*pos)++;
                ssize_t arr_len = 0;
                if (*pos < c->token_count && c->tokens[*pos].type == TK_NUM) {
                    arr_len = (ssize_t)c->tokens[*pos].num_val;
                    (*pos)++;
                }
                if (*pos < c->token_count && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
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
    while (*pos < c->token_count && c->tokens[*pos].type == TK_STAR) {
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
    ty.is_const = q_const;
    ty.is_volatile = q_volatile;
    ty.is_restrict = q_restrict;
    return ty;
}

static inline ZeroCCType zero_cc_parse_type_name(ZeroCCCompiler *c) {
    // Handle parentheses
    bool lparen = false;
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
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            if (c->token_pos < c->token_count && zero_cc_tok_is_type_start(c, c->token_pos)) {
                vec_attr_size = probe;
            } else {
                c->token_pos = save;
            }
        }
        if (vec_attr_size == 0) {
            size_t save2 = c->token_pos;
            if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) {
                if (c->token_pos < c->token_count && zero_cc_tok_is_type_start(c, c->token_pos)) {
                    align_attr_val = probe;
                } else {
                    c->token_pos = save2;
                }
            }
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
    } else if (c->tokens[c->token_pos].type == TK_SHORT) {
        ty = zero_cc_type_int16();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED) {
        bool is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_SHORT) {
            ty = is_unsigned ? zero_cc_type_uint16() : zero_cc_type_int16();
            c->token_pos++;
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_CHAR) {
            ty = is_unsigned ? zero_cc_type_uint8() : zero_cc_type_char();
            c->token_pos++;
        } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) {
            ty = is_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++; /* long long */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++; /* long [long] int */
        } else {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
            ty = is_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
        }
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
        if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
            if (c->tokens[c->token_pos].type == TK_UNSIGNED) ty = zero_cc_type_uint64();
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
    } else {
        // default
        ty = zero_cc_type_int64();
    }
    /* Vector attribute trailing the base specifier: consume it (otherwise
     * the marker identifier would be read as the variable name) and wrap
     * the base type in a vector. Same for aligned(N): raise the type's
     * alignment (and pad struct sizes) to the given value. */
    if (vec_attr_size == 0) {
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) vec_attr_size = probe;
    }
    if (align_attr_val == 0) {
        size_t probe = 0;
        if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) align_attr_val = probe;
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
    /* Parenthesized abstract declarator: (*)(void), (*)[4], or (*[3])(float).
     * A parenthesized declarator that contains an identifier (`(*fp)(void)`)
     * is a NAMED declarator, not an abstract type: leave the tokens for the
     * declarator parser instead of swallowing the name. */
    {
        size_t probe = c->token_pos + 2;
        while (probe < c->token_count && c->tokens[probe].type == TK_STAR) probe++;
        while (probe < c->token_count &&
               (c->tokens[probe].type == TK_CONST ||
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
    ty.is_const = q_const;
    ty.is_volatile = q_volatile;
    ty.is_restrict = q_restrict;
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
