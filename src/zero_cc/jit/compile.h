#ifndef ZERO_CC_JIT_COMPILE_H
#define ZERO_CC_JIT_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/semantic.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/parser/zero_cc_declarator_stack.h"
#include "zero_cc/parser/definitions.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/ir/regalloc.h"

static inline void zero_cc_ensure_param_cap(SemContext *ctx, ZeroCCType **types, const char ***names, size_t *cap, size_t needed) {
    if (needed <= *cap) return;
    size_t ncap = (*cap < 16) ? 16 : *cap * 2;
    while (ncap < needed) ncap *= 2;
    Arena *arena = (ctx && ctx->tmp_arena) ? ctx->tmp_arena : NULL;
    ZeroCCType *ntypes = arena ? (ZeroCCType *)arena_alloc(arena, ncap * sizeof(ZeroCCType)) : (ZeroCCType *)malloc(ncap * sizeof(ZeroCCType));
    const char **nnames = arena ? (const char **)arena_alloc(arena, ncap * sizeof(const char *)) : (const char **)malloc(ncap * sizeof(const char *));
    if (*types && *cap > 0) {
        memcpy(ntypes, *types, *cap * sizeof(ZeroCCType));
        memcpy(nnames, *names, *cap * sizeof(const char *));
    }
    *types = ntypes;
    *names = nnames;
    *cap = ncap;
}

/* Sprint 65: K&R (old-style, C89 6.7.4.2) parameter-declaration statement.
 * Parses one declaration in the K&R block - a type-specifier run (any
 * order, `long long`, struct/union/enum tags, typedef names, pointers,
 * qualifiers), a comma list of declarators `name [dims]`, and the `;`.
 * Each declarator is recorded in out_names/out_types (array suffixes decay
 * to pointers - correct for parameters; aux local arrays are modeled as
 * pointer slots). Returns the number of declarators, or -1 on error (the
 * token stream is left at the faulting token). */
static int zero_cc_parse_kr_decl(ZeroCCCompiler *c, const char **out_names, ZeroCCType *out_types, int max) {
    /* C89 K&R blocks commonly carry storage classes (`register tree new, old;`). */
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_REGISTER || c->tokens[c->token_pos].type == TK_STATIC ||
            c->tokens[c->token_pos].type == TK_AUTO || c->tokens[c->token_pos].type == TK_EXTERN)) {
        c->token_pos++;
    }
    ZeroCCTokType t = c->tokens[c->token_pos].type;
    /* C99 6.7.2.3 prefix complex: `_Complex double *a` - consume the
     * specifier; the base type follows and gets wrapped below. */
    bool p_prefix_complex = false;
    if (t == TK_IDENT && zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
        p_prefix_complex = true;
        c->token_pos++;
        t = c->tokens[c->token_pos].type;
    }
    bool starts_type = (t == TK_INT || t == TK_CHAR || t == TK_SHORT || t == TK_LONG ||
                        t == TK_SIGNED || t == TK_UNSIGNED || t == TK_FLOAT_KW ||
                        t == TK_DOUBLE_KW || t == TK_VOID ||
                        t == TK_STRUCT || t == TK_UNION || t == TK_ENUM ||
                        t == TK_TYPEOF || t == TK_TYPEOF_UNQUAL ||
                        t == TK_CONST || t == TK_VOLATILE || t == TK_RESTRICT ||
                        (t == TK_IDENT && (zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val) ||
                         zero_cc_find_struct(c, c->tokens[c->token_pos].str_val) ||
                         zero_cc_find_enum(c, c->tokens[c->token_pos].str_val))));
    if (!starts_type) return -1;

    bool p_is_const = false, p_is_volatile = false;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_RESTRICT)) {
        if (c->tokens[c->token_pos].type == TK_CONST) p_is_const = true;
        if (c->tokens[c->token_pos].type == TK_VOLATILE) p_is_volatile = true;
        c->token_pos++;
    }
    size_t save = c->token_pos, probe = 0, vec_size = 0;
    if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
        if (c->token_pos < c->token_count &&
            (c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_CHAR ||
             c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG ||
             c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
             c->tokens[c->token_pos].type == TK_FLOAT_KW || c->tokens[c->token_pos].type == TK_DOUBLE_KW)) {
            vec_size = probe;
        } else {
            c->token_pos = save;
            vec_size = 0;
        }
    }
    ZeroCCType pt;
    if (c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL) {
        pt = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    } else {
        ZeroCCTokType t0 = c->tokens[c->token_pos].type;
        const char *nm = c->tokens[c->token_pos].str_val;
        ZeroCCType *found = (t0 == TK_IDENT) ? zero_cc_find_typedef(c, nm) : NULL;
        if (!found && t0 == TK_IDENT) found = zero_cc_find_struct(c, nm);
        if (!found && t0 == TK_IDENT) found = zero_cc_find_enum(c, nm);
        if (t0 != TK_IDENT && (t0 == TK_STRUCT || t0 == TK_UNION || t0 == TK_ENUM)) {
            c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                nm = c->tokens[c->token_pos].str_val;
                found = (t0 == TK_ENUM) ? zero_cc_find_enum(c, nm) : zero_cc_find_struct(c, nm);
                if (!found) found = zero_cc_find_typedef(c, nm);
                c->token_pos++;
            }
        } else if (found) {
            c->token_pos++;
        }
        bool p_has_unsigned = (t0 == TK_UNSIGNED), p_has_char = (t0 == TK_CHAR),
             p_has_short = (t0 == TK_SHORT), p_has_float = (t0 == TK_FLOAT_KW),
             p_has_double = (t0 == TK_DOUBLE_KW), p_has_void = (t0 == TK_VOID);
        int p_longs = (t0 == TK_LONG) ? 1 : 0;
        if (t0 == TK_SIGNED || t0 == TK_UNSIGNED || t0 == TK_SHORT || t0 == TK_LONG ||
            t0 == TK_INT || t0 == TK_CHAR) {
            found = NULL;
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_SIGNED ||
                    c->tokens[c->token_pos].type == TK_UNSIGNED ||
                    c->tokens[c->token_pos].type == TK_SHORT ||
                    c->tokens[c->token_pos].type == TK_LONG ||
                    c->tokens[c->token_pos].type == TK_INT ||
                    c->tokens[c->token_pos].type == TK_CHAR)) {
                switch (c->tokens[c->token_pos].type) {
                    case TK_UNSIGNED: p_has_unsigned = true; break;
                    case TK_CHAR: p_has_char = true; break;
                    case TK_SHORT: p_has_short = true; break;
                    case TK_LONG: p_longs++; break;
                    default: break;
                }
                c->token_pos++;
            }
        }
        if (c->tokens[c->token_pos].type == TK_FLOAT_KW) { p_has_float = true; c->token_pos++; }
        else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) { p_has_double = true; c->token_pos++; }
        else if (c->tokens[c->token_pos].type == TK_VOID) { p_has_void = true; c->token_pos++; }
        /* C99 6.7.2.3 suffix complex: `double _Complex *a`. */
        if (!p_prefix_complex && c->tokens[c->token_pos].type == TK_IDENT &&
            zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
            p_prefix_complex = true; /* reuse flag: complex wraps the base */
            c->token_pos++;
        }
        if (found) {
            pt = *found;
        } else if (p_has_void) {
            pt = zero_cc_type_void();
        } else if (p_has_float) {
            pt = zero_cc_type_float();
        } else if (p_has_double) {
            pt = zero_cc_type_double();
        } else if (p_has_char) {
            pt = zero_cc_type_char();
        } else if (p_has_short) {
            pt = zero_cc_type_int16();
        } else if (p_longs > 0) {
            pt = zero_cc_type_int64();
        } else {
            pt = zero_cc_type_int();
        }
        if (p_has_unsigned) {
            if (pt.tag == TYPE_INT8) pt.tag = TYPE_UINT8;
            else if (pt.tag == TYPE_INT16) pt.tag = TYPE_UINT16;
            else if (pt.tag == TYPE_INT32) pt.tag = TYPE_UINT32;
            else if (pt.tag == TYPE_INT64) pt.tag = TYPE_UINT64;
        }
        if (p_prefix_complex && !zero_cc_type_is_complex(&pt)) {
            pt = zero_cc_type_complex(&pt);
        }
    }
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE)) {
        if (c->tokens[c->token_pos].type == TK_CONST) p_is_const = true;
        if (c->tokens[c->token_pos].type == TK_VOLATILE) p_is_volatile = true;
        c->token_pos++;
    }
    int ptr_depth = 0;
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
        ptr_depth++;
        c->token_pos++;
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE)) {
            c->token_pos++;
        }
    }
    pt.is_const = p_is_const;
    pt.is_volatile = p_is_volatile;
    if (vec_size > 0 && !zero_cc_type_is_vector(&pt)) pt = zero_cc_type_vector(&pt, vec_size);
    for (int pd = 0; pd < ptr_depth; pd++) pt = zero_cc_type_ptr(&pt);

    int count = 0;
    for (;;) {
        /* Per-declarator pointer suffixes (`int *a, *b;`): each declarator
         * may carry its own `*` run with qualifiers. */
        int d_ptr = 0;
        bool d_const = pt.is_const, d_volatile = pt.is_volatile;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
            d_ptr++;
            c->token_pos++;
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE)) {
                if (c->tokens[c->token_pos].type == TK_CONST) d_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) d_volatile = true;
                c->token_pos++;
            }
        }
        if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) return -1;
        const char *dn = c->tokens[c->token_pos].str_val;
        c->token_pos++;
        bool is_array = false;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
            is_array = true;
            c->token_pos++;
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) c->token_pos++;
            if (c->token_pos < c->token_count) c->token_pos++; /* skip ']' */
        }
        if (count >= max) return -1;
        ZeroCCType dt = pt;
        dt.is_const = d_const;
        dt.is_volatile = d_volatile;
        for (int dp = 0; dp < d_ptr; dp++) dt = zero_cc_type_ptr(&dt);
        if (is_array && dt.tag != TYPE_PTR && !dt.is_func_ptr) {
            dt = zero_cc_type_ptr(&dt);
        }
        out_names[count] = dn;
        out_types[count] = dt;
        count++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            c->token_pos++;
            continue;
        }
        break;
    }
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_SEMI) return -1;
    c->token_pos++; /* skip ';' */
    return count;
}

/* Sprint 65: parse one function declarator's parameter list, starting
 * at the first token after `name (`.  Handles the K&R (old-style)
 * shape (bare identifiers + following declaration block) and the
 * usual prototype form.  Leaves the token at `;`, `{`, or `,`.
 * Returns false if parsing could not proceed (stream untouched or
 * advanced to an unrecoverable point - caller should give up).
 */
static bool zero_cc_parse_fn_param_block(ZeroCCCompiler *c, SemContext *ctx,
    const char *fn_name,
    ZeroCCType **param_types, const char ***param_names, size_t *param_cap,
    size_t *param_count, bool *is_variadic_fn, bool *has_prototype,
    ZeroCCASTArena **dim_arena_out, ASTRef dim_exprs[]) {
    bool kr_handled = false;
    {
    size_t q = c->token_pos;
    int depth = 1;
    int bare_count = 0;
    bool kr_shape = true;
    while (q < c->token_count && depth > 0) {
        ZeroCCTokType qt = c->tokens[q].type;
        if (qt == TK_LPAREN) { kr_shape = false; break; }
        else if (qt == TK_RPAREN) { depth--; if (depth == 0) break; }
        else if (qt == TK_COMMA) { /* entry separator */ }
        else if (qt == TK_IDENT) { bare_count++; }
        else { kr_shape = false; break; }
        q++;
    }
    if (kr_shape && q < c->token_count && depth == 0 && bare_count >= 1 &&
        c->tokens[c->token_pos].type == TK_IDENT &&
        c->tokens[q - 1].type == TK_IDENT) {
        size_t after = q + 1; /* first token after ')' */
        while (after < c->token_count &&
               (c->tokens[after].type == TK_REGISTER || c->tokens[after].type == TK_STATIC ||
                c->tokens[after].type == TK_AUTO || c->tokens[after].type == TK_EXTERN ||
                c->tokens[after].type == TK_CONST || c->tokens[after].type == TK_VOLATILE ||
                c->tokens[after].type == TK_RESTRICT)) {
            after++;
        }
        ZeroCCTokType at = (after < c->token_count) ? c->tokens[after].type : TK_EOF;
        bool next_is_decl = (at == TK_INT || at == TK_CHAR || at == TK_SHORT || at == TK_LONG ||
                             at == TK_SIGNED || at == TK_UNSIGNED || at == TK_FLOAT_KW ||
                             at == TK_DOUBLE_KW || at == TK_VOID ||
                             at == TK_STRUCT || at == TK_UNION || at == TK_ENUM ||
                             at == TK_TYPEOF || at == TK_TYPEOF_UNQUAL ||
                             at == TK_CONST || at == TK_VOLATILE || at == TK_RESTRICT ||
                             (at == TK_IDENT && (zero_cc_find_typedef(c, c->tokens[after].str_val) ||
                              zero_cc_find_struct(c, c->tokens[after].str_val) ||
                              zero_cc_find_enum(c, c->tokens[after].str_val))));
        if (next_is_decl) {
            c->kr_aux_count = 0;
            size_t r = c->token_pos;
            while (r < q) {
                if (c->tokens[r].type == TK_IDENT && *param_count < ZERO_CC_MAX_PARAMS) {
                    zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                    (*param_names)[*param_count] = zero_cc_intern_string(c, c->tokens[r].str_val);
                    (*param_types)[*param_count] = zero_cc_type_int(); /* filled by the decl block; int default (C89) */
                    (*param_count)++;
                }
                r++;
            }
            c->token_pos = q + 1; /* past ')' */
            *has_prototype = false; /* old-style: not a prototype */
            *is_variadic_fn = false;
            while (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type != TK_LBRACE &&
                   c->tokens[c->token_pos].type != TK_SEMI &&
                   c->tokens[c->token_pos].type != TK_EOF) {
                const char *dn[16];
                ZeroCCType dt[16];
                int n = zero_cc_parse_kr_decl(c, dn, dt, 16);
                if (n < 0) {
                    diag_report_error_at_token(c, "invalid K&R parameter declaration in function '%s'", fn_name);
                    while (c->token_pos < c->token_count &&
                           c->tokens[c->token_pos].type != TK_LBRACE &&
                           c->tokens[c->token_pos].type != TK_SEMI) c->token_pos++;
                    break;
                }
                for (int k = 0; k < n; k++) {
                    bool matched = false;
                    for (size_t p = 0; p < *param_count; p++) {
                        if (strcmp((*param_names)[p], dn[k]) == 0) {
                            (*param_types)[p] = dt[k];
                            matched = true;
                            break;
                        }
                    }
                    if (!matched && c->kr_aux_count < 64) {
                        snprintf(c->kr_aux_names[c->kr_aux_count], 64, "%s", dn[k]);
                        c->kr_aux_types[c->kr_aux_count] = dt[k];
                        c->kr_aux_count++;
                    }
                }
            }
            kr_handled = true;
        }
    }
}

if (!kr_handled) {
while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
size_t ppos = c->token_pos;
*has_prototype = true; /* any entry (param or ...) makes this a prototype */
if (c->tokens[c->token_pos].type == TK_ELLIPSIS) {
    *is_variadic_fn = true;
    c->token_pos++;
} else {
    bool p_is_const = false;
    bool p_is_volatile = false;
    while (c->tokens[c->token_pos].type == TK_CONST ||
           c->tokens[c->token_pos].type == TK_VOLATILE ||
           c->tokens[c->token_pos].type == TK_RESTRICT) {
        if (c->tokens[c->token_pos].type == TK_CONST) p_is_const = true;
        if (c->tokens[c->token_pos].type == TK_VOLATILE) p_is_volatile = true;
        c->token_pos++;
    }
    /* GNU vector parameter: probe the `__zc_vector_size__(N)`
     * marker emitted by the preprocessor (`vector int t`). */
    size_t p_vec_size = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            if (c->token_pos < c->token_count &&
                (c->tokens[c->token_pos].type == TK_INT ||
                 c->tokens[c->token_pos].type == TK_CHAR ||
                 c->tokens[c->token_pos].type == TK_SHORT ||
                 c->tokens[c->token_pos].type == TK_LONG ||
                 c->tokens[c->token_pos].type == TK_SIGNED ||
                 c->tokens[c->token_pos].type == TK_UNSIGNED ||
                 c->tokens[c->token_pos].type == TK_FLOAT_KW ||
                 c->tokens[c->token_pos].type == TK_DOUBLE_KW)) {
                p_vec_size = probe;
            } else {
                c->token_pos = save;
            }
        }
    }
    ZeroCCType typeof_pt;
    bool is_typeof_param = false;
    bool p_is_complex = false;
    /* C99 6.7.2.3 prefix complex parameter: `_Complex double x` / `__complex__ float x`. */
    if (c->tokens[c->token_pos].type == TK_IDENT &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
        p_is_complex = true;
        c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL) {
        is_typeof_param = true;
        typeof_pt = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    }
    if (is_typeof_param || c->tokens[c->token_pos].type == TK_IDENT || c->tokens[c->token_pos].type == TK_INT ||
        c->tokens[c->token_pos].type == TK_CHAR || c->tokens[c->token_pos].type == TK_VOID ||
        c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION ||
        c->tokens[c->token_pos].type == TK_ENUM ||
        c->tokens[c->token_pos].type == TK_FLOAT_KW ||
        c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
        c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
        c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
        ZeroCCTokType ptt = is_typeof_param ? TK_TYPEOF : c->tokens[c->token_pos].type;
        const char *ptname = is_typeof_param ? "" : c->tokens[c->token_pos].str_val;
        ZeroCCType *pfound = is_typeof_param ? NULL : zero_cc_find_typedef(c, ptname);
        if (!pfound && !is_typeof_param) pfound = zero_cc_find_struct(c, ptname);
        if (!pfound && !is_typeof_param) pfound = zero_cc_find_enum(c, ptname);
        if (!is_typeof_param) c->token_pos++;
        /* Type-specifier run (C11 6.7.5): keywords may appear in any
         * order and `long long` is two tokens — consume the whole
         * run so e.g. `unsigned long long int *v` does not leave the
         * trailing `int` behind: the next loop iteration would then
         * re-parse it as a fresh unnamed parameter and the pointer
         * param would be recorded as `int *`, mis-sizing every
         * dereference (c-torture execute/961213-1). */
        bool p_is_unsigned = (!is_typeof_param && ptt == TK_UNSIGNED);
        if (!is_typeof_param && (ptt == TK_SIGNED || ptt == TK_UNSIGNED ||
                                 ptt == TK_SHORT || ptt == TK_LONG ||
                                 ptt == TK_CHAR)) {
            pfound = NULL;
            bool p_has_unsigned = (ptt == TK_UNSIGNED);
            bool p_has_char = (ptt == TK_CHAR);
            bool p_has_short = (ptt == TK_SHORT);
            int p_longs = (ptt == TK_LONG) ? 1 : 0;
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_SIGNED ||
                    c->tokens[c->token_pos].type == TK_UNSIGNED ||
                    c->tokens[c->token_pos].type == TK_SHORT ||
                    c->tokens[c->token_pos].type == TK_LONG ||
                    c->tokens[c->token_pos].type == TK_INT ||
                    c->tokens[c->token_pos].type == TK_CHAR)) {
                switch (c->tokens[c->token_pos].type) {
                    case TK_SIGNED: break;
                    case TK_UNSIGNED: p_has_unsigned = true; break;
                    case TK_CHAR: p_has_char = true; break;
                    case TK_SHORT: p_has_short = true; break;
                    case TK_LONG: p_longs++; break;
                    case TK_INT: break;
                    default: break;
                }
                c->token_pos++;
            }
            if (p_has_char) ptt = TK_CHAR;
            else if (p_has_short) ptt = TK_SHORT;
            else if (p_longs > 0) ptt = TK_LONG;
            else ptt = TK_INT;
            p_is_unsigned = p_has_unsigned;
        }
        if ((ptt == TK_STRUCT || ptt == TK_UNION) && c->tokens[c->token_pos].type == TK_IDENT) {
            ptname = c->tokens[c->token_pos].str_val;
            pfound = zero_cc_find_struct(c, ptname);
            if (!pfound) pfound = zero_cc_find_typedef(c, ptname);
            c->token_pos++;
        } else if (ptt == TK_ENUM && c->tokens[c->token_pos].type == TK_IDENT) {
            ptname = c->tokens[c->token_pos].str_val;
            pfound = zero_cc_find_enum(c, ptname);
            if (!pfound) pfound = zero_cc_find_typedef(c, ptname);
            c->token_pos++;
        }
        while (c->tokens[c->token_pos].type == TK_CONST ||
               c->tokens[c->token_pos].type == TK_VOLATILE ||
               c->tokens[c->token_pos].type == TK_RESTRICT) {
            if (c->tokens[c->token_pos].type == TK_CONST) p_is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) p_is_volatile = true;
            c->token_pos++;
        }
        /* C99 6.7.2.3 suffix complex parameter: `double _Complex x`. */
        if (!p_is_complex && c->tokens[c->token_pos].type == TK_IDENT &&
            zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
            p_is_complex = true;
            c->token_pos++;
        }
        int ptr_depth = 0;
        bool p_ptr_is_const = false;
        bool p_ptr_is_volatile = false;
        bool p_ptr_is_restrict = false;
        while (c->tokens[c->token_pos].type == TK_STAR) {
            ptr_depth++;
            c->token_pos++;
            while (c->tokens[c->token_pos].type == TK_CONST ||
                   c->tokens[c->token_pos].type == TK_VOLATILE ||
                   c->tokens[c->token_pos].type == TK_RESTRICT) {
                if (c->tokens[c->token_pos].type == TK_CONST) p_ptr_is_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) p_ptr_is_volatile = true;
                if (c->tokens[c->token_pos].type == TK_RESTRICT) p_ptr_is_restrict = true;
                c->token_pos++;
            }
        }
        /* Compute the parameter's base type NOW (before the function-
         * pointer branch) so `ret (*name)(params)` can use `ret` as the
         * pointed-to function's return type (921208-1). */
        ZeroCCType pt;
        if (is_typeof_param) {
            pt = typeof_pt;
        } else if (pfound) {
            pt = *pfound;
            snprintf(pt.struct_name, sizeof(pt.struct_name), "%s", ptname);
        } else if (ptt == TK_FLOAT_KW) {
            pt = zero_cc_type_float();
        } else if (ptt == TK_DOUBLE_KW) {
            pt = zero_cc_type_double();
        } else if (ptt == TK_CHAR || ptt == TK_BOOL_KW) {
            pt = zero_cc_type_char();
        } else if (ptt == TK_SHORT) {
            pt = zero_cc_type_int16();
        } else if (ptt == TK_INT || ptt == TK_SIGNED || ptt == TK_UNSIGNED) {
            pt = zero_cc_type_int();
        } else if (ptt == TK_LONG) {
            pt = zero_cc_type_int64();
        } else {
            pt = zero_cc_type_int64();
        }
        if (p_is_unsigned) {
            if (pt.tag == TYPE_INT8) pt.tag = TYPE_UINT8;
            else if (pt.tag == TYPE_INT16) pt.tag = TYPE_UINT16;
            else if (pt.tag == TYPE_INT32) pt.tag = TYPE_UINT32;
            else if (pt.tag == TYPE_INT64) pt.tag = TYPE_UINT64;
        }
        pt.is_const = p_is_const;
        pt.is_volatile = p_is_volatile;
        if (p_is_complex) {
            pt = zero_cc_type_complex(&pt);
        }
        if (p_vec_size > 0) {
            pt = zero_cc_type_vector(&pt, p_vec_size);
        }
        for (int pd = 0; pd < ptr_depth; pd++) {
            pt = zero_cc_type_ptr(&pt);
            if (pd == ptr_depth - 1) {
                pt.is_const = p_ptr_is_const;
                pt.is_volatile = p_ptr_is_volatile;
                pt.is_restrict = p_ptr_is_restrict;
            }
        }
        bool fn_ptr_param_done = false;
        if (c->tokens[c->token_pos].type == TK_LPAREN &&
            c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos + 1].type == TK_STAR) {
            /* Function-pointer parameter: `ret_type (*name)(params...)` */
            c->token_pos++; /* ( */
            while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                if (*param_count >= ZERO_CC_MAX_PARAMS) {
                    diag_report_error_at_token(c,
                        "function '%s' has too many parameters (max %d)",
                        fn_name, ZERO_CC_MAX_PARAMS);
                } else {
                    zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                    (*param_names)[*param_count] = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
                    /* ABI: an 8-byte pointer in a register.  The pointee
                     * carries the function's RETURN type (the declared
                     * base type `ret` in `ret (*name)(params)`) so an
                     * indirect call `(*name)(args)` knows whether the
                     * result arrives in XMM0 (float/double) or RAX
                     * (c-torture execute/921208-1, pr42231). */
                    ZeroCCType fpt = zero_cc_type_ptr(&pt);
                    fpt.is_func_ptr = true;
                    (*param_types)[*param_count] = fpt;
                    zero_cc_add_local(c, (*param_names)[*param_count], fpt);
                    (*param_count)++;
                    fn_ptr_param_done = true; /* the named branch must not re-record */
                }
            }
            c->token_pos++; /* name */
            if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++; /* ) */
            /* Skip the parameter list (depth-counted) */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                int pdepth = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && pdepth > 0) {
                    if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                    else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                    c->token_pos++;
                }
            }
            /* fall through to the comma/stuck-token handling below */
        }
        /* `(void)` is an empty parameter list (C99 6.7.6.3p2): it
         * makes the declaration a prototype but adds no parameter. */
        bool is_void_param = (ptt == TK_VOID && !is_typeof_param &&
                              ptr_depth == 0);
        /* Abstract declarator: `int ( int foo )` - a parenthesized
         * function parameter list on an anonymous declarator (C99
         * 6.7.6.1); the parameter is a pointer to function
         * (20010313-1). */
        if (!fn_ptr_param_done && !is_void_param && c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type == TK_LPAREN &&
            (c->token_pos + 1 >= c->token_count ||
             c->tokens[c->token_pos + 1].type != TK_STAR)) {
            int pdepth = 0;
            while (c->token_pos < c->token_count) {
                if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                else if (c->tokens[c->token_pos].type == TK_RPAREN) {
                    pdepth--;
                    if (pdepth == 0) { c->token_pos++; break; }
                }
                c->token_pos++;
            }
            if (*param_count < ZERO_CC_MAX_PARAMS) {
                zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                (*param_names)[*param_count] = "";
                ZeroCCType ptt2 = zero_cc_type_int64();
                ZeroCCType fpt2 = zero_cc_type_ptr(&ptt2);
                fpt2.is_func_ptr = true;
                (*param_types)[*param_count] = fpt2;
                (*param_count)++;
            }
        } else if (!fn_ptr_param_done && !is_void_param && c->tokens[c->token_pos].type == TK_IDENT) {
            if (*param_count >= ZERO_CC_MAX_PARAMS) {
                diag_report_error_at_token(c,
                    "function '%s' has too many parameters (max %d)",
                    fn_name, ZERO_CC_MAX_PARAMS);
            } else {
                zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                (*param_names)[*param_count] = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
                (*param_types)[*param_count] = pt;
                zero_cc_add_local(c, (*param_names)[*param_count], pt);
                (*param_count)++;
            }
            c->token_pos++;
            /* Array parameter (C99 6.7.6.3p7): the OUTERMOST array
             * dimension is adjusted to a pointer; remaining
             * dimensions form the pointed-to array type
             * (`int m[][4]` -> `int (*)[4]`).  Previously the
             * `[...]` suffixes were skipped by the stuck-token
             * guard below and the param kept its element type, so
             * the pointer value was later sign-extended as a
             * 32-bit int (movslq) and truncated at runtime. */
            if (*param_count > 0 && c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_LBRACKET) {
                ssize_t dims[8];
                int ndims = 0;
                while (c->token_pos < c->token_count &&
                       c->tokens[c->token_pos].type == TK_LBRACKET &&
                       ndims < 8) {
                    ssize_t d = 0;
                    c->token_pos++; /* '[' */
                    if (c->token_pos < c->token_count &&
                        c->tokens[c->token_pos].type == TK_NUM &&
                        c->token_pos + 1 < c->token_count &&
                        c->tokens[c->token_pos + 1].type == TK_RBRACKET) {
                        d = (ssize_t)c->tokens[c->token_pos].num_val;
                        c->token_pos++;
                    } else {
                        /* VLA dimension expression (C99 6.7.6.2): parse
                         * the bracket content as an expression; it is
                         * evaluated in parameter order at function entry
                         * (for its side effects - the storage lives in
                         * the caller's frame). */
                        size_t dim_start = c->token_pos;
                        while (c->token_pos < c->token_count &&
                               c->tokens[c->token_pos].type != TK_RBRACKET) c->token_pos++;
                        size_t dim_end = c->token_pos; /* the ']' */
                        d = 0;
                        if (dim_start < dim_end && dim_exprs[*param_count - 1] == AST_NULL) {
                            if (!*dim_arena_out) {
                                *dim_arena_out = (ZeroCCASTArena *)calloc(1, sizeof(ZeroCCASTArena));
                                if (*dim_arena_out) zero_cc_ast_arena_init(*dim_arena_out, 64);
                            }
                            if (*dim_arena_out) {
                                size_t saved_pos = c->token_pos;
                                c->token_pos = dim_start;
                                dim_exprs[*param_count - 1] = parse_expr_ast_internal(c, *dim_arena_out);
                                c->token_pos = (dim_end < c->token_count) ? dim_end : saved_pos;
                            }
                        }
                    }
                    if (c->token_pos < c->token_count &&
                        c->tokens[c->token_pos].type == TK_RBRACKET) {
                        c->token_pos++; /* ']' */
                    }
                    dims[ndims++] = d;
                }
                ZeroCCType t = (*param_types)[*param_count - 1];
                /* Later dimension tokens are inner: build the
                 * pointed-to array type innermost-first. */
                for (int i = ndims - 1; i >= 1; i--) {
                    t = zero_cc_type_array(&t, dims[i]);
                }
                (*param_types)[*param_count - 1] = zero_cc_type_ptr(&t);
            }
        } else if (!fn_ptr_param_done && !is_void_param && *param_count < ZERO_CC_MAX_PARAMS &&
                   c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_LBRACKET ||
                    c->tokens[c->token_pos].type == TK_COMMA ||
                    c->tokens[c->token_pos].type == TK_RPAREN)) {
            /* Unnamed parameter: record the type with an empty
             * name so the prototype keeps its parameter count.
             * No add_local (nothing to bind).  Array suffixes are
             * consumed and adjusted like the named case (C99
             * 6.7.6.3p7): `int m[]` -> `int *`, `int[][N]` ->
             * `int (*)[N]`, `int[][fn3()]` (pr77754) -> pointer to
             * an (inner) array. */
            ssize_t u_dims[8];
            int u_ndims = 0;
            while (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_LBRACKET &&
                   u_ndims < 8) {
                ssize_t d = 0;
                c->token_pos++; /* '[' */
                if (c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_NUM &&
                    c->token_pos + 1 < c->token_count &&
                    c->tokens[c->token_pos + 1].type == TK_RBRACKET) {
                    d = (ssize_t)c->tokens[c->token_pos].num_val;
                    c->token_pos++;
                } else {
                    /* VLA / empty dimension: skip to the matching ']' */
                    int bdepth = 1;
                    while (c->token_pos < c->token_count && bdepth > 0) {
                        if (c->tokens[c->token_pos].type == TK_LBRACKET) bdepth++;
                        else if (c->tokens[c->token_pos].type == TK_RBRACKET) bdepth--;
                        c->token_pos++;
                    }
                    d = 0;
                }
                if (c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_RBRACKET) {
                    c->token_pos++; /* ']' */
                }
                u_dims[u_ndims++] = d;
            }
            zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
            (*param_names)[*param_count] = "";
            if (u_ndims > 0) {
                ZeroCCType t = pt;
                for (int i = u_ndims - 1; i >= 1; i--) {
                    t = zero_cc_type_array(&t, u_dims[i] > 0 ? u_dims[i] : 1);
                }
                pt = zero_cc_type_ptr(&t);
            }
            (*param_types)[*param_count] = pt;
            (*param_count)++;
        } else if (!is_void_param && *param_count >= ZERO_CC_MAX_PARAMS &&
                   c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_COMMA ||
                    c->tokens[c->token_pos].type == TK_RPAREN)) {
            diag_report_error_at_token(c,
                "function '%s' has too many parameters (max %d)",
                fn_name, ZERO_CC_MAX_PARAMS);
        }
    }
}
if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
if (c->token_pos == ppos) c->token_pos++;
}
if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
}
    return true;
}

static inline void zero_cc_compile_function(ZeroCCCompiler *c, SemContext *ctx, Arena *perm) {
    /* Skip static / inline / const / _Noreturn */
    bool fn_noreturn = false;
    bool fn_inline = false;
    bool fn_static = false;
    bool fn_ret_const = false; /* leading `const` qualifies the pointee of a pointer return type */
    while (c->tokens[c->token_pos].type == TK_STATIC || c->tokens[c->token_pos].type == TK_INLINE ||
           c->tokens[c->token_pos].type == TK_NORETURN || c->tokens[c->token_pos].type == TK_CONST ||
           c->tokens[c->token_pos].type == TK_VOLATILE ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0)) {
        if (c->tokens[c->token_pos].type == TK_NORETURN) fn_noreturn = true;
        if (c->tokens[c->token_pos].type == TK_INLINE) fn_inline = true;
        if (c->tokens[c->token_pos].type == TK_STATIC) fn_static = true;
        if (c->tokens[c->token_pos].type == TK_CONST ||
            (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0))
            fn_ret_const = true;
        c->token_pos++;
    }

    /* Return type */
    ZeroCCType fn_ret_type = zero_cc_type_int();
    bool fn_ret_unsigned = false;
    /* GNU vector return type: `__attribute__((__vector_size__(N))) float g(void)`
     * - probe the preprocessor marker before the base specifier. */
    size_t fn_ret_vec_size = 0;
    {
        size_t save = c->token_pos;
        size_t probe = 0;
        if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
            if (c->token_pos < c->token_count &&
                (c->tokens[c->token_pos].type == TK_INT ||
                 c->tokens[c->token_pos].type == TK_CHAR ||
                 c->tokens[c->token_pos].type == TK_SHORT ||
                 c->tokens[c->token_pos].type == TK_LONG ||
                 c->tokens[c->token_pos].type == TK_SIGNED ||
                 c->tokens[c->token_pos].type == TK_UNSIGNED ||
                 c->tokens[c->token_pos].type == TK_FLOAT_KW ||
                 c->tokens[c->token_pos].type == TK_DOUBLE_KW)) {
                fn_ret_vec_size = probe;
            } else {
                c->token_pos = save;
            }
        }
    }
    if (c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED) {
        fn_ret_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
        c->token_pos++;
        if (c->token_pos < c->token_count &&
            (c->tokens[c->token_pos].type == TK_INT ||
             c->tokens[c->token_pos].type == TK_CHAR ||
             c->tokens[c->token_pos].type == TK_SHORT ||
             c->tokens[c->token_pos].type == TK_LONG)) {
            /* fall through to the base-specifier dispatch below */
        } else {
            /* bare `unsigned` / `signed` */
            fn_ret_type = fn_ret_unsigned ? zero_cc_type_uint32() : zero_cc_type_int();
        }
    }
    if (c->tokens[c->token_pos].type == TK_IDENT &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
        /* C99 6.7.2.3: `_Complex` / `__complex__` return type prefix. */
        c->token_pos++;
        ZeroCCType cx_inner = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
        fn_ret_type = zero_cc_type_complex(&cx_inner);
    } else if (c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL) {
        fn_ret_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    } else if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
        bool fn_ret_is_union = (c->tokens[c->token_pos].type == TK_UNION);
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_LBRACE) {
            /* Sprint 65: inline struct/union definition as the return type:
             * `struct st { ... } f (params) { ... }` (the `;` after the
             * body is omitted - the declarator continues the declaration). */
            fn_ret_type = zero_cc_parse_struct_body(c, fn_ret_is_union);
        } else if (c->tokens[c->token_pos].type == TK_IDENT) {
            char fn_st_name[128];
            snprintf(fn_st_name, sizeof(fn_st_name), "%s", c->tokens[c->token_pos].str_val);
            c->token_pos++;
            if (c->tokens[c->token_pos].type == TK_LBRACE) {
                /* Named inline definition: register the tag from the body. */
                ZeroCCType st = zero_cc_parse_struct_body(c, fn_ret_is_union);
                if (st.struct_name[0] == '\0') {
                    snprintf(st.struct_name, sizeof(st.struct_name), "%s", fn_st_name);
                }
                zero_cc_register_struct(c, fn_st_name, st);
                zero_cc_sync_globals_with_struct(c, fn_st_name, &st);
                fn_ret_type = st;
            } else {
                ZeroCCType *st = zero_cc_find_struct(c, fn_st_name);
                if (!st) st = zero_cc_find_typedef(c, fn_st_name);
                if (st) fn_ret_type = *st;
            }
        }
    } else if (c->tokens[c->token_pos].type == TK_ENUM) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCType *et = zero_cc_find_enum(c, c->tokens[c->token_pos].str_val);
            if (!et) et = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
            if (et) fn_ret_type = *et;
            else fn_ret_type = zero_cc_type_enum(c->tokens[c->token_pos].str_val);
            c->token_pos++;
        } else {
            fn_ret_type = zero_cc_type_enum(NULL);
        }
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
        if (!tdef) tdef = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
        if (!tdef) tdef = zero_cc_find_enum(c, c->tokens[c->token_pos].str_val);
        if (tdef) {
            fn_ret_type = *tdef;
            c->token_pos++;
        } else if (c->token_pos + 1 < c->token_count &&
                   c->tokens[c->token_pos + 1].type == TK_LPAREN) {
            /* C89-style implicit-int function definition: `main(){ ... }`.
             * The identifier is the function name, not a type; the return
             * type defaults to int. C99 6.7p6 requires an explicit type
             * specifier — gcc accepts this with -Wimplicit-int, so we warn
             * and keep compiling (fn_ret_type stays int, token not consumed). */
            fprintf(stderr, "warning: return type of '%s' defaults to 'int' (C99 requires an explicit return type)\n",
                    c->tokens[c->token_pos].str_val);
        } else {
            /* Unknown type name: legacy lenient behavior (default int, skip). */
            c->token_pos++;
        }
    } else if (c->tokens[c->token_pos].type == TK_FLOAT_KW) {
        fn_ret_type = zero_cc_type_float();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
        fn_ret_type = zero_cc_type_double();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_CHAR || c->tokens[c->token_pos].type == TK_BOOL_KW) {
        fn_ret_type = zero_cc_type_char();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_SHORT) {
        fn_ret_type = zero_cc_type_int16();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_VOID) {
        fn_ret_type = zero_cc_type_void();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_INT || c->tokens[c->token_pos].type == TK_SIGNED ||
               c->tokens[c->token_pos].type == TK_UNSIGNED) {
        fn_ret_type = zero_cc_type_int();
        c->token_pos++; }
    else if (c->tokens[c->token_pos].type == TK_LONG) {
        c->token_pos++;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
        if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
            fn_ret_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
            c->token_pos++;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        fn_ret_type = fn_ret_unsigned ? zero_cc_type_uint64() : zero_cc_type_int64();
    }
    if (fn_ret_const && c->tokens[c->token_pos].type == TK_STAR) fn_ret_type.is_const = true;
    while (c->tokens[c->token_pos].type == TK_STAR) {
        fn_ret_type = zero_cc_type_ptr(&fn_ret_type);
        c->token_pos++;
    }
    /* Apply the signedness qualifier to a plain int return type */
    if (fn_ret_unsigned) {
        if (fn_ret_type.tag == TYPE_INT8) fn_ret_type.tag = TYPE_UINT8;
        else if (fn_ret_type.tag == TYPE_INT16) fn_ret_type.tag = TYPE_UINT16;
        else if (fn_ret_type.tag == TYPE_INT32) fn_ret_type.tag = TYPE_UINT32;
        else if (fn_ret_type.tag == TYPE_INT64) fn_ret_type.tag = TYPE_UINT64;
    }
    if (fn_ret_vec_size > 0 && !zero_cc_type_is_vector(&fn_ret_type)) {
        fn_ret_type = zero_cc_type_vector(&fn_ret_type, fn_ret_vec_size);
    }
    /* C99 6.7.2.3 suffix complex: `float _Complex foo(void)`. */
    if (c->tokens[c->token_pos].type == TK_IDENT &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val) &&
        !fn_ret_type.is_complex) {
        c->token_pos++;
        fn_ret_type = zero_cc_type_complex(&fn_ret_type);
    }
    /* Skip _Noreturn that may appear after return type */
    while (c->tokens[c->token_pos].type == TK_NORETURN) {
        fn_noreturn = true;
        c->token_pos++;
    }

    /* Function name and parameter list. Two shapes:
     *   name ( params )            - the common form
     *   (*name(params))(params2)   - parenthesized declarator (a function
     *                                 returning a (function) pointer) */
    char fn_name[128];
    /* Parse Parameters in signature */
    size_t param_cap = 16;
    ZeroCCType *param_types = (ZeroCCType *)arena_alloc(ctx->tmp_arena, param_cap * sizeof(ZeroCCType));
    const char **param_names = (const char **)arena_alloc(ctx->tmp_arena, param_cap * sizeof(const char *));
    size_t param_count = 0;
    size_t first_param_count = 0; /* params of the 1st declarator (multi-decl) */
    /* C99 6.7.6.2: VLA parameter dimension expressions (one per param,
     * 0 = none) - parsed into a private arena, evaluated at entry. */
    ZeroCCASTArena *dim_arena = NULL;
    ASTRef dim_exprs[ZERO_CC_MAX_PARAMS];
    for (size_t di = 0; di < ZERO_CC_MAX_PARAMS; di++) dim_exprs[di] = AST_NULL;
    bool is_variadic_fn = false;
    bool has_prototype = false; /* set once the param list holds at least one entry */
    c->local_count = 0;
    c->local_lookup_bound = -1;
    if (c->local_hash_buckets && c->local_hash_bucket_count > 0) {
        for (size_t b = 0; b < c->local_hash_bucket_count; b++) c->local_hash_buckets[b] = -1;
    }

    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN &&
        c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_STAR) {
        /* Parenthesized declarator: parse the full declarator and extract
         * the name, its parameter list, and the (pointer) return type. */
        ZeroCCASTArena darena;
        ZeroCCDeclarator d;
        zero_cc_ast_arena_init(&darena, 256);
        if (!zero_cc_parse_declarator(c, &darena, ctx, &d) || !d.name ||
            !zero_cc_declarator_is_function(&d)) {
            zero_cc_ast_arena_free(&darena);
            return;
        }
        snprintf(fn_name, sizeof(fn_name), "%s", d.name);
        fn_ret_type = zero_cc_declarator_apply_return(&d, fn_ret_type);
        if (fn_ret_type.is_func) {
            diag_report_error_at_token(c, "function '%s' cannot return a function", d.name);
            zero_cc_ast_arena_free(&darena);
            return;
        }
        const ZeroCCDeclaratorChunk *fc = zero_cc_declarator_get_function_chunk(&d);
        if (fc) {
            is_variadic_fn = fc->is_variadic;
            has_prototype = fc->has_prototype;
            ASTRef pl = fc->param_list;
            size_t pi = 0;
            while (pl != AST_NULL) {
                ZeroCCASTNode *pn = zero_cc_ast_get(&darena, pl);
                if (!pn) break;
                if (pi >= ZERO_CC_MAX_PARAMS) {
                    diag_report_error_at_token(c, "function '%s' has too many parameters (max %d)", fn_name, ZERO_CC_MAX_PARAMS);
                    break;
                }
                zero_cc_ensure_param_cap(ctx, &param_types, &param_names, &param_cap, pi + 1);
                param_types[pi] = zero_cc_type_int();
                if (pi < fc->param_count) {
                    TypeInfo *pti = ast_node_get_typeinfo(c, pn);
                    if (pti) param_types[pi] = typeinfo_to_zero_cc_type(pti);
                }
                param_names[pi] = zero_cc_intern_string(c, (pn->s.str && pn->s.str[0]) ? pn->s.str : "");
                pl = pn->next;
                pi++;
            }
            param_count = pi;
        }
        zero_cc_ast_arena_free(&darena);
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
        snprintf(fn_name, sizeof(fn_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;

        if (c->tokens[c->token_pos].type != TK_LPAREN) return;
        c->token_pos++;

        /* Sprint 65: parameter list (incl. K&R old-style). */
        if (!zero_cc_parse_fn_param_block(c, ctx, fn_name,
                &param_types, &param_names, &param_cap, &param_count,
                &is_variadic_fn, &has_prototype, &dim_arena, dim_exprs)) return;

        /* C99 6.7.5: a declaration may introduce several declarators
         * (`void d0(void), d1(void);`, `double bar(void), c;`).  Each
         * comma-separated declarator after the first reuses the base
         * return type.  Function declarators re-parse their own
         * parameter list; variable declarators get their own storage. */
        first_param_count = param_count;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            size_t save = c->token_pos;
            c->token_pos++; /* ',' */
            {
                size_t probe = 0;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT &&
                       strcmp(c->tokens[c->token_pos].str_val, "__zc_aligned__") == 0) {
                    zero_cc_parse_aligned_attr(c, &c->token_pos, &probe);
                }
            }
            /* A pointer-leading declarator re-derives its own chain:
             * `T *a, *b;` and `T *f1(void), *f2(void);` (the star is part
             * of THIS declarator, not the shared base type). Skip stars +
             * qualifiers to find the name; stars2 is applied to the type
             * in the variable branch below. */
            int stars2 = 0;
            {
                size_t p2 = c->token_pos;
                while (p2 < c->token_count && c->tokens[p2].type == TK_STAR) {
                    stars2++;
                    p2++;
                    while (p2 < c->token_count &&
                           (c->tokens[p2].type == TK_CONST ||
                            c->tokens[p2].type == TK_VOLATILE ||
                            c->tokens[p2].type == TK_RESTRICT)) p2++;
                }
                if (p2 < c->token_count && c->tokens[p2].type == TK_IDENT) {
                    c->token_pos = p2; /* position at the name */
                }
            }
            if (c->token_pos + 1 < c->token_count &&
                c->tokens[c->token_pos].type == TK_IDENT &&
                c->tokens[c->token_pos + 1].type == TK_LPAREN) {
                char fn2_name[128];
                snprintf(fn2_name, sizeof(fn2_name), "%s", c->tokens[c->token_pos].str_val);
                /* Pointer-to-function declarator: the skipped leading
                 * stars apply to the return type of THIS declarator. */
                ZeroCCType fn2_ret = fn_ret_type;
                for (int s2 = 0; s2 < stars2; s2++) fn2_ret = zero_cc_type_ptr(&fn2_ret);
                c->token_pos++; /* name */
                c->token_pos++; /* ( */
                size_t p2_base = param_count;
                if (!zero_cc_parse_fn_param_block(c, ctx, fn2_name,
                        &param_types, &param_names, &param_cap, &param_count,
                        &is_variadic_fn, &has_prototype, &dim_arena, dim_exprs)) return;
                /* Register this declarator's own prototype (C99 6.7.5);
                 * its params occupy [p2_base, param_count) in the shared
                 * buffers, so the next declarator appends cleanly. */
                if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
                    ZeroCCFunction *fn2 = &c->functions[c->function_count++];
                    memset(fn2, 0, sizeof(*fn2));
                    snprintf(fn2->name, sizeof(fn2->name), "%s", fn2_name);
                    fn2->text_offset = 0;
                    fn2->param_count = param_count - p2_base;
                    fn2->return_type = fn2_ret;
                    fn2->returns_struct = (fn2_ret.tag == TYPE_STRUCT || fn2_ret.tag == TYPE_UNION ||
                                          (fn2_ret.tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn2_ret)));
                    fn2->ret_struct_sz = fn2_ret.size;
                    fn2->is_prototype = true;
                    fn2->has_prototype = has_prototype;
                    fn2->is_inline = fn_inline;
                    fn2->is_static = fn_static;
                    if (fn2->param_count > 0) {
                        Arena *farena = zero_cc_get_fn_arena(c);
                        fn2->param_names = (const char **)arena_alloc(farena, fn2->param_count * sizeof(const char *));
                        fn2->param_types = (ZeroCCType *)arena_alloc(farena, fn2->param_count * sizeof(ZeroCCType));
                        for (size_t i = 0; i < fn2->param_count; i++) {
                            fn2->param_types[i] = param_types[p2_base + i];
                            fn2->param_names[i] = param_names[p2_base + i];
                        }
                    }
                }
            } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                /* Variable declarator sharing the base type: name [*] [dims...].
                 * Allocate storage (file-scope non-extern object). */
                char vname[128];
                snprintf(vname, sizeof(vname), "%s", c->tokens[c->token_pos].str_val);
                c->token_pos++;
                ZeroCCType vt = fn_ret_type;
                for (int s2 = 0; s2 < stars2; s2++) vt = zero_cc_type_ptr(&vt);
                int pd = 0;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                    vt = zero_cc_type_ptr(&vt);
                    pd++;
                    c->token_pos++;
                    while (c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_CONST || c->tokens[c->token_pos].type == TK_VOLATILE))
                        c->token_pos++;
                }
                int64_t vlen = 1;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                    c->token_pos++;
                    int64_t d = 1;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) {
                        d = zero_cc_eval_const_expr(c);
                    }
                    if (d > 1) vlen *= d;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) c->token_pos++;
                }
                size_t vsize = (vt.size > 0 ? vt.size : 8) * (size_t)vlen;
                size_t valign = vt.align > 0 ? vt.align : 8;
                if (valign < 8) valign = 8;
                size_t vpad = (valign - (c->data_section.length % valign)) % valign;
                for (size_t i = 0; i < vpad; i++) zero_cc_emit_u8(&c->data_section, 0);
                size_t voff = c->data_section.length;
                for (size_t i = 0; i < vsize; i++) zero_cc_emit_u8(&c->data_section, 0);
                if (add_global_var(vname, vt, voff, vsize, false, true) < 0) {
                    diag_report_error_at_token(c, "out of memory registering global variable");
                }
                (void)pd;
                /* After a variable declarator the remaining list items are
                 * variables too - keep the loop (same branch). */
            } else {
                c->token_pos = save;
                break;
            }
        }
    } else {
        return;
    }

    /* Trailing function alignment: `void f(void) __attribute__((aligned(N)));`
     * (preprocessor marker __zc_aligned__(N)).  Kept on the function record
     * so `__alignof__(f)` sees it (c-torture execute/align-3).  A later
     * definition without the attribute inherits the prototype's value. */
    size_t fn_align = 0;
    {
        size_t probe = 0;
        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type == TK_IDENT &&
               c->tokens[c->token_pos].str_val &&
               strcmp(c->tokens[c->token_pos].str_val, "__zc_aligned__") == 0) {
            if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe) && probe > fn_align) {
                fn_align = probe;
            }
        }
        if (fn_align == 0) {
            ZeroCCFunction *prev = zero_cc_find_function(c, fn_name);
            if (prev && prev->align > 0) fn_align = prev->align;
        }
    }

    if (c->tokens[c->token_pos].type == TK_SEMI) {
        /* Function prototype declaration: record prototype, no code emit */
        if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
            ZeroCCFunction *fn = &c->functions[c->function_count++];
            memset(fn, 0, sizeof(*fn));
            snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
            fn->text_offset = 0;
            fn->param_count = first_param_count;
            fn->return_type = fn_ret_type;
            fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT || fn_ret_type.tag == TYPE_UNION ||
                                  (fn_ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn_ret_type)));
            fn->ret_struct_sz = fn_ret_type.size;
            fn->is_variadic = is_variadic_fn;
            fn->noreturn = fn_noreturn;
            fn->is_prototype = true;
            fn->has_prototype = has_prototype;
            fn->is_inline = fn_inline;
            fn->is_static = fn_static;
            fn->align = fn_align;
            if (first_param_count > 0) {
                Arena *farena = zero_cc_get_fn_arena(c);
                fn->param_names = (const char **)arena_alloc(farena, first_param_count * sizeof(const char *));
                fn->param_types = (ZeroCCType *)arena_alloc(farena, first_param_count * sizeof(ZeroCCType));
                for (size_t i = 0; i < first_param_count; i++) {
                    fn->param_types[i] = param_types[i];
                    fn->param_names[i] = param_names[i];
                }
            }
        }
        if (dim_arena) { zero_cc_ast_arena_free(dim_arena); free(dim_arena); dim_arena = NULL; }
        c->token_pos++;
        return;
    }

    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        if (c->function_count >= ZERO_CC_MAX_FUNCTIONS) return;
        ZeroCCFunction *fn = &c->functions[c->function_count++];
        memset(fn, 0, sizeof(*fn));
        snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
        /* Record text_offset BEFORE emitting any prologue bytes for this function */
        fn->text_offset = c->text_section.length;
        fn->param_count = param_count;
        fn->return_type = fn_ret_type;
        fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT || fn_ret_type.tag == TYPE_UNION ||
                              (fn_ret_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn_ret_type)));
        fn->ret_struct_sz = fn_ret_type.size;
        fn->is_variadic = is_variadic_fn;
        fn->noreturn = fn_noreturn;
        fn->is_prototype = false;
        fn->has_prototype = has_prototype;
        fn->is_inline = fn_inline;
        fn->is_static = fn_static;
        fn->align = fn_align;
        if (param_count > 0) {
            Arena *farena = zero_cc_get_fn_arena(c);
            fn->param_names = (const char **)arena_alloc(farena, param_count * sizeof(const char *));
            fn->param_types = (ZeroCCType *)arena_alloc(farena, param_count * sizeof(ZeroCCType));
            for (size_t i = 0; i < param_count; i++) {
                fn->param_types[i] = param_types[i];
                fn->param_names[i] = param_names[i];
            }
            fn->param_dim_arena = dim_arena;
            if (dim_arena) {
                fn->param_dim_exprs = (ASTRef *)arena_alloc(farena, param_count * sizeof(ASTRef));
                for (size_t i = 0; i < param_count; i++) {
                    fn->param_dim_exprs[i] = dim_exprs[i];
                }
            }
        }

        c->local_count = 0;
        c->local_lookup_bound = -1;
        if (c->local_hash_buckets && c->local_hash_bucket_count > 0) {
            for (size_t b = 0; b < c->local_hash_bucket_count; b++) c->local_hash_buckets[b] = -1;
        }
        c->current_stack_frame = BE(c)->callee_save_frame_reserve; /* Reserve top of frame for callee-saves */

        /* System V AMD64 ABI: >16 byte struct returns use hidden pointer in RDI.
         * Reserve an 8-byte stack slot for the hidden pointer before params. */
        if (fn->ret_struct_sz > 16) {
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            fn->sret_stack_offset = c->current_stack_frame;
        } else {
            fn->sret_stack_offset = 0;
        }

        /* Parameters and locals are allocated stack offsets in c->current_stack_frame.
         * The function prologue and parameter save instructions will be emitted into IR in zero_cc_lower_ast(). */
        int xmm_reg_idx = 0;
        int int_reg_idx = 0;
        int stack_arg_idx = 0;

        /* Sprint 65: K&R aux locals (non-parameter names declared in the
         * parameter-declaration block, e.g. `int f(a) int a; char buf[10];`). */
        for (int a = 0; a < c->kr_aux_count; a++) {
            zero_cc_add_local(c, c->kr_aux_names[a], c->kr_aux_types[a]);
        }
        c->kr_aux_count = 0;

        for (size_t p = 0; p < param_count; p++) {
            ZeroCCSym *sym = zero_cc_add_local(c, param_names[p], param_types[p]);
            sym->is_param = true;
            sym->param_reg_idx = (int)p;
            bool is_fp = (param_types[p].tag == TYPE_FLOAT || param_types[p].tag == TYPE_DOUBLE);

            if (is_fp && xmm_reg_idx < 8) {
                xmm_reg_idx++;
            } else if (!is_fp && int_reg_idx < 6) {
                int_reg_idx++;
            } else if (!is_fp) {
                stack_arg_idx++;
            }
        }

        /* (The old parse-time variadic spill-area reservation lived here;
         * the save area is now placed above all locals at lowering time in
         * zero_cc_lower_ast, which also extends the frame.) */

        // Parse function body into AST and lower to machine code
        ZeroCCASTArena arena;
        zero_cc_ast_arena_init(&arena, 1024);
        ASTRef body = zero_cc_parse_stmt_ast(c, &arena, ctx);
        // Register previously compiled functions in the GLOBAL scope
        // Find the global scope (root of scope chain)
        Scope *global_scope_root = ctx->current_scope;
        while (global_scope_root && global_scope_root->parent) {
            global_scope_root = global_scope_root->parent;
        }
        for (size_t i = 0; i < c->function_count; i++) {
            ZeroCCFunction *f = &c->functions[i];
            /* Phantoms registered during lowering (extern calls whose symbol
             * the ELF writer needs, e.g. __builtin_memcpy -> memcpy and
             * __sync_* / __atomic_* names) are codegen-level symbol entries,
             * not semantic declarations: re-registering them would let a
             * zero-typed void function shadow the analyzer's implicit
             * int-returning declaration for later functions (c-torture
             * compile/sync-2.c: `sc = __sync_fetch_and_add(...)` became an
             * assignment from void). */
            if (f->is_external && !f->is_object) continue;
            TypeInfo *ret_t = zero_cc_type_to_typeinfo(perm, f->return_type);
            if (f->is_external && f->is_object) {
                /* Extern *variable* declarations (`extern int j;`,
                 * `extern unsigned long aa[];`) register a function-table
                 * entry with the variable's type in return_type; they must
                 * appear as variables.  Array externs keep their array type
                 * so `*aa` lowers to a single load, not a value-load +
                 * deref (c-torture execute/20000403-1). */
                scope_put_symbol(global_scope_root, f->name, SYMBOL_VAR, ret_t);
                continue;
            }
            TypeInfo **params = NULL;
            if (f->param_count > 0) {
                params = (TypeInfo **)arena_alloc(perm, f->param_count * sizeof(TypeInfo *));
                for (size_t j = 0; j < f->param_count; j++) {
                    params[j] = zero_cc_type_to_typeinfo(perm, f->param_types[j]);
                }
            }
            /* Extern entries (incl. __builtin_* routines registered during
             * lowering, e.g. memcpy/abort) must appear as FUNCTIONS, not
             * variables: a later function calling the same name must see a
             * callable type (registering the return type as SYMBOL_VAR made
             * `__builtin_popcount` an "int variable" and broke its second
             * use - c-torture compile/fold-popcount-1.c). */
            TypeInfo *fn_t = type_function(perm, ret_t, params, f->param_count, f->is_variadic);
            /* No-prototype declaration: a phantom extern registered during
             * lowering has no parameter list, so it must not constrain call
             * argument counts (C99 6.7.5.3). */
            fn_t->details.function.is_prototype = f->has_prototype;
            scope_put_symbol(global_scope_root, f->name, SYMBOL_FUNC, fn_t);
        }
        for (size_t i = 0; i < global_var_count; i++) {
            TypeInfo *gv_ti = zero_cc_type_to_typeinfo(perm, global_vars[i].type);

            scope_put_symbol(global_scope_root, global_vars[i].name, SYMBOL_VAR, gv_ti);
        }

        // Set the AST arena for this function body
        ctx->ast_arena = &arena;
        ctx->current_scope = global_scope_root;

        // Enter function parameter scope
        Scope *fn_param_scope = scope_enter(&ctx->current_scope, perm);
        for (size_t p = 0; p < param_count; p++) {
            TypeInfo *pti = zero_cc_type_to_typeinfo(perm, param_types[p]);
            scope_put_symbol(fn_param_scope, param_names[p], SYMBOL_VAR, pti);
        }

        // Set current function return type for validation in return statements
        TypeInfo *fn_ret_ti = zero_cc_type_to_typeinfo(perm, fn_ret_type);
        ctx->current_fn_ret_type = fn_ret_ti;

        size_t err_count_before = ctx->diags ? ctx->diags->count : 0;
        /* Pre-collect user labels so goto targets validate in one pass
         * (a label may textually follow its goto). */
        sem_collect_labels(ctx, body);
        (void)sem_analyze_ref(ctx, body);
        bool sem_ok = (!ctx->diags || ctx->diags->count == err_count_before);

        if (sem_ok) {
            /* Set active compiler for IR emission */
            ir_set_compiler(c);

            /* Reset register allocator for this function */
            regalloc_init(&c->regalloc);
            c->next_temp_id = VREG_USER_START;
            c->ir_next_label = 0;

            /* Reset IR buffer for this function */
            ir_reset();

            /* Pass 1: Lower AST to IR (instruction selection) */
            zero_cc_lower_ast(c, &arena, body);

            /* Pass 1 (Lower AST -> IR), Register Allocation, and Pass 2 (IR -> Machine Code)
             * are executed inside zero_cc_lower_ast(). The VLA parameter
             * dimension expressions were consumed at entry during Pass 1. */
            if (dim_arena) {
                zero_cc_ast_arena_free(dim_arena);
                free(dim_arena);
                dim_arena = NULL;
            }

            /* Record small static/inline function body in translation-unit arena for inlining.
             * Sprint 35 Phase 1 (inline audit) conservative candidate filters - the
             * call-site expander re-lowers the body in the CALLER's context, where the
             * callee's parse-time state is unavailable or collides:
             *  - no float/double params or return (param temps carry no FP class; the
             *    param-substitution path forces last_expr_is_float=0, so FP params/
             *    returns miscompile at -O2),
             *  - no local declarations in the body (locals are registered at parse
             *    time in the callee's per-function table; at the call site they lower
             *    to TEMP_NONE and corrupt the caller),
             *  - no labels/goto (user labels resolve through a global name-based map;
             *    a same-named caller label would alias the inlined body's target),
             *  - no &param (value params bind to virtual registers with no address).
             *  Real calls from the inlined body, struct params/returns <=8B, multiple
             *    returns, and nested inlining (depth < 2) are verified safe. */
            if (c->tu_ast_arena && (fn_inline || fn_static) && !fn->is_variadic &&
                !fn->returns_struct && fn->ret_struct_sz <= 8) {
                ZeroCCASTArena *tu_arena = (ZeroCCASTArena *)c->tu_ast_arena;
                size_t node_count = zero_cc_ast_node_count(&arena, body);
                bool inline_ok = (node_count > 0 && node_count <= 35 &&
                                  !zero_cc_ast_has_call(&arena, body, fn->name));
                if (inline_ok) {
                    if (fn->param_count > 8)
                        inline_ok = false;
                    if (fn->return_type.tag == TYPE_FLOAT || fn->return_type.tag == TYPE_DOUBLE)
                        inline_ok = false;
                    for (size_t p = 0; inline_ok && p < fn->param_count; p++) {
                        if (fn->param_types[p].tag == TYPE_FLOAT || fn->param_types[p].tag == TYPE_DOUBLE)
                            inline_ok = false;
                    }
                }
                if (inline_ok) {
                    if (zero_cc_ast_has_local_decl(&arena, body) ||
                        zero_cc_ast_has_label_goto(&arena, body))
                        inline_ok = false;
                }
                if (inline_ok && fn->param_count > 0) {
                    if (zero_cc_ast_addr_of_param(&arena, body, fn->param_names, (int)fn->param_count))
                        inline_ok = false;
                    /* (Sprint 41) A write to a value param (x = / x op=) has no
                     * stack slot in the inlined body, so the store is dropped and
                     * later reads see the stale argument. Do not inline such fns. */
                    if (inline_ok && zero_cc_ast_assigns_to_param(&arena, body, fn->param_names, (int)fn->param_count))
                        inline_ok = false;
                }
                if (inline_ok) {
                    ASTRef cloned_body = zero_cc_ast_clone(&arena, body, tu_arena);
                    if (cloned_body != AST_NULL) {
                        fn->can_inline = true;
                        fn->inline_body = cloned_body;
                        fn->ast_arena = tu_arena;
                    }
                }
            }
        } else {
            c->had_error = 1;
            /* Errors are recorded in ctx->diags and merged into c->diag_buf at TU end */
        }

        // Restore scope and return type after analysis
        scope_leave(&ctx->current_scope);
        ctx->current_scope = global_scope_root;
        ctx->current_fn_ret_type = NULL;
        ctx->ast_arena = NULL;
        zero_cc_ast_arena_free(&arena);

        // Consume closing brace if present
        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
    }
}


#endif /* ZERO_CC_JIT_COMPILE_H */
