/* Function compilation (Sprint 114 Tier B: converted from compile.h to this
 * translation unit). Holds zero_cc_compile_function (the per-function
 * compile entry: parameter parsing, body lowering hook, emission), the
 * fn-parameter-block parser, and the nested-function compile loop.
 *
 * compile.h now only declares the two cross-TU entry points (called from
 * compile_unit.c); jit_linker.h no longer pulls this implementation into
 * the main.c translation unit. */

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

/* Sprint 65: parse one function declarator's parameter list, starting
 * at the first token after `name (`.  Handles the K&R (old-style)
 * shape (bare identifiers + following declaration block) and the
 * usual prototype form.  Leaves the token at `;`, `{`, or `,`.
 * Returns false if parsing could not proceed (stream untouched or
 * advanced to an unrecoverable point - caller should give up).
 */
static bool zero_cc_try_parse_fn_param_block_kr(
    ZeroCCCompiler *c, SemContext *ctx, const char *fn_name,
    ZeroCCType **param_types, const char ***param_names, size_t *param_cap,
    size_t *param_count, bool *has_prototype, bool *is_variadic_fn) {
    size_t q = c->token_pos;
    int depth = 1;
    int bare_count = 0;
    bool kr_shape = true;
    while (q < c->token_count && depth > 0) {
        ZeroCCTokType qt = c->tokens[q].type;
        if (qt == TK_LPAREN) { kr_shape = false; break; }
        else if (qt == TK_RPAREN) { depth--; if (depth == 0) break; }
        else if (qt == TK_COMMA) { /* entry separator */ }
        else if (qt == TK_IDENT) {
            /* A bare identifier is a K&R parameter NAME only if it is not a
             * type name: `f(four_quarters j)` (typedef + name) is a normal
             * one-parameter prototype, not K&R - treating it as K&R would
             * register two int params and drop the struct type (c-torture
             * execute/921123-2). */
            if (zero_cc_find_typedef(c, c->tokens[q].str_val) ||
                zero_cc_find_struct(c, c->tokens[q].str_val) ||
                zero_cc_find_enum(c, c->tokens[q].str_val)) {
                kr_shape = false;
                break;
            }
            /* `_Complex`/`__complex__` is a type specifier, not a K&R
             * parameter name - its presence means a prototype, not an
             * old-style name list.  `void f(_Complex z) { }` must not be
             * read as a two-name K&R list (registering `z` as int). */
            if (zero_cc_tok_is_complex_spec(c->tokens[q].str_val)) {
                kr_shape = false;
                break;
            }
            /* GNU 128-bit base types (`__int128`/`__int128_t`/`__uint128_t`)
             * are type names tokenized as identifiers: `f(__int128 a)` is a
             * one-parameter prototype, NOT a two-name K&R list (which would
             * register `__int128` and `a` as two int parameters). */
            if (zc_name_int128_kind(c->tokens[q].str_val)) {
                kr_shape = false;
                break;
            }
            /* GNU __float128 (Sprint 112): same K&R hazard. */
            if (strcmp(c->tokens[q].str_val, "__float128") == 0) {
                kr_shape = false;
                break;
            }
            bare_count++;
        }
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
        /* C89 6.4.2: a K&R definition may have NO declaration list at all -
         * the parameter identifiers then default to int (`int f(a) { }`). */
        bool next_is_body = (at == TK_LBRACE);
        if (next_is_decl || next_is_body) {
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
            bool kr_matched[ZERO_CC_MAX_PARAMS];
            for (size_t mi = 0; mi < ZERO_CC_MAX_PARAMS; mi++) kr_matched[mi] = false;
            c->token_pos = q + 1; /* past ')' */
            *has_prototype = false; /* old-style: not a prototype */
            *is_variadic_fn = false;
            while (next_is_decl && c->token_pos < c->token_count &&
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
                            /* C89 6.4.2: each parameter shall be declared
                             * EXACTLY once (`int f(a) int a; int a; {}` ->
                             * "redefinition of parameter 'a'"). */
                            if (kr_matched[p]) {
                                diag_report_error_at_token(c, "redefinition of parameter '%s'", dn[k]);
                            } else {
                                kr_matched[p] = true;
                                (*param_types)[p] = dt[k];
                            }
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        /* C89 6.4.2: the declaration list declares PARAMETERs
                         * only - no aux locals (`int f(a) int a; char buf; {}`
                         * -> "declaration for parameter 'buf' but no such
                         * parameter"). */
                        diag_report_error_at_token(c, "declaration for parameter '%s' but no such parameter", dn[k]);
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool zero_cc_parse_fn_param_block(ZeroCCCompiler *c, SemContext *ctx,  /* Sprint 114: extern, called from compile_unit.c */
    const char *fn_name,
    ZeroCCType **param_types, const char ***param_names, size_t *param_cap,
    size_t *param_count, bool *is_variadic_fn, bool *has_prototype,
    ZeroCCASTArena **dim_arena_out, ASTRef dim_exprs[]) {
    bool kr_handled = zero_cc_try_parse_fn_param_block_kr(c, ctx, fn_name,
        param_types, param_names, param_cap, param_count, has_prototype,
        is_variadic_fn);

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
    /* GNU mode(X) marker: consumed (not applied) in parameter position -
     * mode-on-parameter was unsupported before marker emission, so
     * skipping preserves the old behavior. */
    zero_cc_skip_mode_attr(c);
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
        /* Standalone `_Complex` parameter (`_Complex z`): the identifier after
         * `_Complex` is the declarator NAME, not a typedef base - the real
         * type is `_Complex double` (GNU; strict C99 requires the real type).
         * Leave the name for the declarator below and default the base to
         * double instead of consuming it as a defaulted-int type. */
        bool p_cx_standalone = (p_is_complex && !is_typeof_param &&
                                ptt == TK_IDENT && !pfound);
        if (!is_typeof_param && !p_cx_standalone) c->token_pos++;
        if (p_cx_standalone) ptt = TK_DOUBLE_KW;
        /* Type-specifier run (C11 6.7.5): keywords may appear in any
         * order and `long long` is two tokens — consume the whole
         * run so e.g. `unsigned long long int *v` does not leave the
         * trailing `int` behind: the next loop iteration would then
         * re-parse it as a fresh unnamed parameter and the pointer
         * param would be recorded as `int *`, mis-sizing every
         * dereference (c-torture execute/961213-1). */
        bool p_is_unsigned = (!is_typeof_param && ptt == TK_UNSIGNED);
        bool p_is_long_double = false;
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
            /* C99 6.7.2.1: `long double` / `long long double` - the
             * double keyword follows the long run and turns it into the
             * extended-precision type (modeled through the double value
             * model with the is_long_double marker). */
            if (p_longs >= 1 && c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
                c->token_pos++;
                ptt = TK_DOUBLE_KW;
                p_is_long_double = true;
            }
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
        bool p_ptr_const[16], p_ptr_vol[16], p_ptr_restr[16];
        /* Each star level carries its own qualifier: `const char *const
         * *bar` is a pointer to (const pointer to const char) - the inner
         * const must not leak onto bar's own type (strpbrk.c: `bar--` is
         * legal there). */
        while (c->tokens[c->token_pos].type == TK_STAR) {
            if (ptr_depth < 16) {
                p_ptr_const[ptr_depth] = false;
                p_ptr_vol[ptr_depth] = false;
                p_ptr_restr[ptr_depth] = false;
            }
            ptr_depth++;
            c->token_pos++;
            while (c->tokens[c->token_pos].type == TK_CONST ||
                   c->tokens[c->token_pos].type == TK_VOLATILE ||
                   c->tokens[c->token_pos].type == TK_RESTRICT) {
                if (ptr_depth <= 16) {
                    if (c->tokens[c->token_pos].type == TK_CONST) p_ptr_const[ptr_depth - 1] = true;
                    if (c->tokens[c->token_pos].type == TK_VOLATILE) p_ptr_vol[ptr_depth - 1] = true;
                    if (c->tokens[c->token_pos].type == TK_RESTRICT) p_ptr_restr[ptr_depth - 1] = true;
                }
                c->token_pos++;
            }
        }
        /* Compute the parameter's base type NOW (before the function-
         * pointer branch) so `ret (*name)(params)` can use `ret` as the
         * pointed-to function's return type (921208-1). */
        ZeroCCType pt;
        if (is_typeof_param) {
            pt = typeof_pt;
        } else if (ptt == TK_IDENT && !pfound && zc_name_int128_kind(ptname) > 0) {
            /* GNU 128-bit base type in parameter position (`__int128 a` /
             * `__uint128_t a`): tokenized as identifiers, not typedefs. */
            pt = (zc_name_int128_kind(ptname) == 2) ? zero_cc_type_uint128() : zero_cc_type_int128();
        } else if (ptt == TK_IDENT && !pfound && strcmp(ptname, "__float128") == 0) {
            /* GNU __float128 base type in parameter position (Sprint 112). */
            pt = zero_cc_type_float128();
        } else if (pfound) {
            pt = *pfound;
            /* An explicit `struct`/`union` keyword means ptname IS the tag
             * (adopt it - e.g. the struct was found via an alias).  For a
             * plain identifier (typedef name), keep the type's own tag:
             * overwriting it with the alias broke compatibility with the
             * real tag (c-torture pr49474: `gfc_actual_arglist *lst` param
             * vs `struct gfc_formal_arglist *` return - incompatible). */
            if (ptt == TK_STRUCT || ptt == TK_UNION) {
                snprintf(pt.struct_name, sizeof(pt.struct_name), "%s", ptname);
            }
        } else if (ptt == TK_FLOAT_KW) {
            pt = zero_cc_type_float();
        } else if (ptt == TK_DOUBLE_KW) {
            pt = p_is_long_double ? zero_cc_type_long_double() : zero_cc_type_double();
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
            if (pd < 16) {
                pt.is_const = p_ptr_const[pd];
                pt.is_volatile = p_ptr_vol[pd];
                pt.is_restrict = p_ptr_restr[pd];
            }
        }
        bool fn_ptr_param_done = false;
        if (c->tokens[c->token_pos].type == TK_LPAREN &&
            c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos + 1].type == TK_STAR) {
            /* Function-pointer parameter: `ret_type (*name)(params...)` */
            size_t fp_decl_start = c->token_pos; /* '(*' - declarator re-parse anchor */
            c->token_pos++; /* ( */
            while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            /* Qualifier between `*` and the name: `(*const f)(...)` - a const
             * function-pointer parameter (c-torture 20040909-1). */
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT)) {
                c->token_pos++;
            }
            const char *fp_name = "";
            bool fp_wrapped_name = false;
            if (c->tokens[c->token_pos].type == TK_LPAREN) {
                /* Parenthesized name: `(* const (funcp)(time_t))` - the
                 * declarator name is wrapped in its own paren group. */
                size_t wp = c->token_pos;
                int wd = 0;
                bool wp_ok = true;
                const char *wp_name = "";
                while (wp < c->token_count) {
                    if (c->tokens[wp].type == TK_LPAREN) wd++;
                    else if (c->tokens[wp].type == TK_RPAREN) {
                        wd--;
                        if (wd == 0) break;
                    } else if (c->tokens[wp].type == TK_IDENT && wp_name[0] == '\0' &&
                               strcmp(c->tokens[wp].str_val, "const") != 0 &&
                               strcmp(c->tokens[wp].str_val, "volatile") != 0 &&
                               strcmp(c->tokens[wp].str_val, "restrict") != 0) {
                        wp_name = c->tokens[wp].str_val;
                    } else if (c->tokens[wp].type != TK_CONST &&
                               c->tokens[wp].type != TK_VOLATILE &&
                               c->tokens[wp].type != TK_RESTRICT) {
                        wp_ok = false;
                        break;
                    }
                    wp++;
                }
                if (wp_ok && wp_name[0] != '\0') {
                    fp_name = wp_name;
                    fp_wrapped_name = true;
                    c->token_pos = wp + 1; /* past the group's ')' */
                }
            }
            if (fp_name[0] == '\0' && c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_IDENT) {
                fp_name = c->tokens[c->token_pos].str_val;
                c->token_pos++; /* name */
            }
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
            /* Wrapped-name form: the outer `(* ... )` group closes AFTER
             * the parameter suffix (`(* (funcp)(time_t))`), so consume its
             * ')' here (the plain form consumed it right after the name). */
            if (fp_wrapped_name && c->token_pos < c->token_count &&
                c->tokens[c->token_pos].type == TK_RPAREN) {
                c->token_pos++;
            }
            /* ABI: an 8-byte pointer in a register.  The pointee
             * carries the function's RETURN type (the declared
             * base type `ret` in `ret (*name)(params)`) so an
             * indirect call `(*name)(args)` knows whether the
             * result arrives in XMM0 (float/double) or RAX
             * (c-torture execute/921208-1, pr42231). */
            ZeroCCType fpt = zero_cc_type_ptr(&pt);
            fpt.is_func_ptr = true;
            /* The quick scan above DROPS the function suffix's parameter
             * types, so re-parse the declarator with the declarator-stack
             * parser to keep the pointee prototype (fn_params) on the
             * pointer's base.  Without it, an indirect call through this
             * function-pointer parameter marshals arguments against no
             * signature: `void t(void (*f)(float, float)); f(1.0f, 2.0f)`
             * passed the floats as model doubles (the callee read 0.0f)
             * because the SysV float-arg narrowing is driven by the
             * declared parameter types.  The re-parse starts at
             * fp_decl_start and must stop where the quick scan left off;
             * any failure or position mismatch keeps the legacy type. */
            if (ctx && c->tu_ast_arena) {
                size_t saved_pos = c->token_pos;
                c->token_pos = fp_decl_start;
                ZeroCCDeclarator dp;
                bool reparse_ok = zero_cc_parse_declarator(c, c->tu_ast_arena, ctx, &dp, false) &&
                                  (c->token_pos == saved_pos);
                c->token_pos = saved_pos;
                if (reparse_ok) {
                    fpt = zero_cc_declarator_apply(&dp, pt);
                }
            }
            zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
            (*param_names)[*param_count] = zero_cc_intern_string(c, fp_name);
            (*param_types)[*param_count] = fpt;
            if (fp_name[0] != '\0' && *param_count < ZERO_CC_MAX_PARAMS) {
                zero_cc_add_local(c, (*param_names)[*param_count], fpt);
            }
            (*param_count)++;
            fn_ptr_param_done = true; /* the named branch must not re-record */
            /* fall through to the comma/stuck-token handling below */
        } else if (c->token_pos + 1 < c->token_count &&
                   c->tokens[c->token_pos].type == TK_LPAREN &&
                   c->tokens[c->token_pos + 1].type == TK_LPAREN) {
            /* Function-pointer parameter wrapped in an extra paren layer:
             * `ret_type ((*name) (params...))` (K&R idiom, c-torture
             * execute/921215-1: `void p(void ((*f) (void ())))`). The
             * single-level `(*name)(...)` branch above cannot see the `*`
             * (it is nested one paren deeper), so scan the balanced paren
             * group for the star and name. */
            int depth = 0;
            size_t close = c->token_count;
            size_t scan_name_pos = c->token_count;
            int stars = 0;
            const char *fp_name = "";
            for (size_t scan = c->token_pos; scan < c->token_count; scan++) {
                if (c->tokens[scan].type == TK_LPAREN) depth++;
                else if (c->tokens[scan].type == TK_RPAREN) {
                    depth--;
                    if (depth == 0) { close = scan; break; }
                } else if (c->tokens[scan].type == TK_STAR) stars++;
                else if (c->tokens[scan].type == TK_IDENT && fp_name[0] == '\0') {
                    const char *s = c->tokens[scan].str_val;
                    if (strcmp(s, "const") != 0 && strcmp(s, "volatile") != 0 &&
                        strcmp(s, "restrict") != 0 && strcmp(s, "_Atomic") != 0) {
                        fp_name = s;
                        scan_name_pos = scan;
                    }
                }
            }
            if (stars > 0 && fp_name[0] != '\0' && close < c->token_count) {
                /* A `(` after the name inside the group is a function
                 * suffix: `((*f) (void ()))` -> pointer to function;
                 * `((*g))` -> a plain (double-parenthesized) pointer. */
                bool is_fn_suffix = false;
                for (size_t a2 = scan_name_pos + 1; a2 < close; a2++) {
                    if (c->tokens[a2].type == TK_LPAREN) { is_fn_suffix = true; break; }
                }
                zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                (*param_names)[*param_count] = zero_cc_intern_string(c, fp_name);
                ZeroCCType fpt = zero_cc_type_ptr(&pt);
                fpt.is_func_ptr = is_fn_suffix;
                (*param_types)[*param_count] = fpt;
                if (fp_name[0] != '\0' && *param_count < ZERO_CC_MAX_PARAMS) {
                    zero_cc_add_local(c, (*param_names)[*param_count], fpt);
                }
                (*param_count)++;
                fn_ptr_param_done = true;
                c->token_pos = close + 1;
            }
        }
        /* `(void)` is an empty parameter list (C99 6.7.6.3p2): it
         * makes the declaration a prototype but adds no parameter. */
        bool is_void_param = (!fn_ptr_param_done && ptt == TK_VOID && !is_typeof_param &&
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
            {
                zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                (*param_names)[*param_count] = "";
                ZeroCCType ptt2 = zero_cc_type_int64();
                ZeroCCType fpt2 = zero_cc_type_ptr(&ptt2);
                fpt2.is_func_ptr = true;
                (*param_types)[*param_count] = fpt2;
                (*param_count)++;
            }
        } else if (!fn_ptr_param_done && !is_void_param && c->tokens[c->token_pos].type == TK_IDENT) {
            {
                zero_cc_ensure_param_cap(ctx, param_types, param_names, param_cap, *param_count + 1);
                (*param_names)[*param_count] = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
                (*param_types)[*param_count] = pt;
                if (*param_count < ZERO_CC_MAX_PARAMS) {
                    zero_cc_add_local(c, (*param_names)[*param_count], pt);
                }
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
                        if (dim_start < dim_end && (*param_count - 1) < ZERO_CC_MAX_PARAMS &&
                            dim_exprs[*param_count - 1] == AST_NULL) {
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
                    /* VLA dimension (dims[i] == 0): mark the array as VLA
                     * (carrying the dim expression) and use the element
                     * size as the static factor (runtime sizeof walks this
                     * chain), mirroring zero_cc_chunk_apply. */
                    if (dims[i] == 0) {
                        t.is_vla = true;
                        t.vla_dim = (size_t)dim_exprs[*param_count - 1];
                        t.size = t.base->size > 0 ? t.base->size : 1;
                    }
                }
                (*param_types)[*param_count - 1] = zero_cc_type_ptr(&t);
                if (c->local_count > 0 && (*param_count - 1) < ZERO_CC_MAX_PARAMS) {
                    c->locals[c->local_count - 1].type = (*param_types)[*param_count - 1];
                }
            } else if (*param_count > 0 && (*param_types)[*param_count - 1].tag == TYPE_ARRAY) {
                /* C99 6.7.6.3p7: parameter declared via an array typedef
                 * (e.g. `jmp_buf env`) adjusts its outermost array dimension
                 * to a pointer to the element type. */
                ZeroCCType at = (*param_types)[*param_count - 1];
                ZeroCCType elem = at.base ? *at.base : zero_cc_type_int64();
                ZeroCCType adj = zero_cc_type_ptr(&elem);
                adj.is_const = at.is_const;
                adj.is_volatile = at.is_volatile;
                (*param_types)[*param_count - 1] = adj;
                if (c->local_count > 0 && (*param_count - 1) < ZERO_CC_MAX_PARAMS) {
                    c->locals[c->local_count - 1].type = adj;
                }
            }
        } else if (!fn_ptr_param_done && !is_void_param &&
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
            } else if (pt.tag == TYPE_ARRAY) {
                ZeroCCType elem = pt.base ? *pt.base : zero_cc_type_int64();
                ZeroCCType adj = zero_cc_type_ptr(&elem);
                adj.is_const = pt.is_const;
                adj.is_volatile = pt.is_volatile;
                pt = adj;
            }
            (*param_types)[*param_count] = pt;
            (*param_count)++;
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

void zero_cc_compile_function(ZeroCCCompiler *c, SemContext *ctx, Arena *perm) {  /* Sprint 114: extern, called from compile_unit.c */
    /* Skip static / inline / const / _Noreturn */
    bool fn_noreturn = false;
    bool fn_inline = false;
    bool fn_static = false;
    bool fn_ret_const = false; /* leading `const` qualifies the pointee of a pointer return type */
    bool fn_ret_volatile = false;
    while (c->tokens[c->token_pos].type == TK_STATIC || c->tokens[c->token_pos].type == TK_INLINE ||
           c->tokens[c->token_pos].type == TK_NORETURN || c->tokens[c->token_pos].type == TK_CONST ||
           c->tokens[c->token_pos].type == TK_VOLATILE ||
           c->tokens[c->token_pos].type == TK_ALIGNAS ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0) ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "volatile") == 0)) {
        if (c->tokens[c->token_pos].type == TK_NORETURN) fn_noreturn = true;
        if (c->tokens[c->token_pos].type == TK_INLINE) fn_inline = true;
        if (c->tokens[c->token_pos].type == TK_STATIC) fn_static = true;
        if (c->tokens[c->token_pos].type == TK_ALIGNAS) {
            /* C11 `_Alignas(N)` / `_Alignas(T)` leading a function
             * definition/prototype (e.g. after the file-scope global branch
             * backs up on a function declarator): consume it here; the
             * value feeds fn_align below. */
            size_t pa = 0;
            if (!zero_cc_parse_aligned_attr(c, &c->token_pos, &pa)) c->token_pos++;
        }
        if (c->tokens[c->token_pos].type == TK_CONST ||
            (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0))
            fn_ret_const = true;
        if (c->tokens[c->token_pos].type == TK_VOLATILE ||
            (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "volatile") == 0))
            fn_ret_volatile = true;
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
    if (c->tokens[c->token_pos].type == TK_IDENT &&
        zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
        /* C99 6.7.2.3: `_Complex` / `__complex__` return type prefix. */
        c->token_pos++;
        ZeroCCType cx_inner;
        if (zero_cc_token_is_complex_inner_base(c, c->token_pos)) {
            cx_inner = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
            /* `_Complex double *f(void)` returns a POINTER TO complex: the
             * type-name parser's pointer-suffix consumption must be rewound
             * so the declarator below owns the stars (the base specifier's
             * last token is a type keyword, so the walk stops at its end). */
            while (c->token_pos > 0 &&
                   (c->tokens[c->token_pos - 1].type == TK_STAR ||
                    c->tokens[c->token_pos - 1].type == TK_CONST ||
                    c->tokens[c->token_pos - 1].type == TK_VOLATILE ||
                    c->tokens[c->token_pos - 1].type == TK_RESTRICT)) {
                c->token_pos--;
            }
            /* The parser returned the suffix-included type (e.g. ptr to
             * double); the complex element is the scalar base only - the
             * declarator below re-applies the pointer. */
            while (cx_inner.tag == TYPE_PTR && cx_inner.base) {
                cx_inner = *cx_inner.base;
            }
        } else {
            /* Standalone `_Complex` return type means `_Complex double`
             * (GNU); the next token is the function declarator. */
            cx_inner = zero_cc_type_double();
        }
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
        const char *rtname = c->tokens[c->token_pos].str_val;
        if (rtname && (strcmp(rtname, "__int128") == 0 || strcmp(rtname, "__int128_t") == 0 ||
                       strcmp(rtname, "__uint128_t") == 0)) {
            /* GNU 128-bit return type. __uint128_t is always unsigned;
             * __int128/__int128_t are signed here (a leading unsigned/signed
             * keyword would have routed to the basic-specifier window). */
            fn_ret_type = (strcmp(rtname, "__uint128_t") == 0) ? zero_cc_type_uint128() : zero_cc_type_int128();
            c->token_pos++;
        } else if (rtname && strcmp(rtname, "__float128") == 0) {
            /* GNU __float128 return type (Sprint 112). */
            fn_ret_type = zero_cc_type_float128();
            c->token_pos++;
        } else {
        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
        if (!tdef) tdef = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
        if (!tdef) tdef = zero_cc_find_enum(c, c->tokens[c->token_pos].str_val);
        /* C89 6.4.4.1: `IDENT (params)` - the identifier is the function
         * name with a defaulted int return type, even when it is a known
         * tag/typedef name (C keeps tags in a separate namespace; typedef
         * names and function names share the ordinary one).  A function
         * pointer declarator opens with `(*`, so that shape keeps the
         * type-name reading (`T (*f)(...)` returns T). */
        bool ident_is_fn_name = (c->token_pos + 2 < c->token_count &&
            c->tokens[c->token_pos + 1].type == TK_LPAREN &&
            c->tokens[c->token_pos + 2].type != TK_STAR);
        if (tdef && !ident_is_fn_name) {
            fn_ret_type = *tdef;
            c->token_pos++;
        } else if (ident_is_fn_name) {
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
        }
    } else if (c->tokens[c->token_pos].type == TK_CHAR ||
               c->tokens[c->token_pos].type == TK_BOOL_KW) {
        fn_ret_type = zero_cc_type_char();
        c->token_pos++;
    } else if (c->tokens[c->token_pos].type == TK_FLOAT_KW ||
               c->tokens[c->token_pos].type == TK_DOUBLE_KW ||
               c->tokens[c->token_pos].type == TK_SHORT ||
               c->tokens[c->token_pos].type == TK_INT ||
               c->tokens[c->token_pos].type == TK_SIGNED ||
               c->tokens[c->token_pos].type == TK_UNSIGNED ||
               c->tokens[c->token_pos].type == TK_LONG) {
        /* Basic type specifier window in any order (C99 6.5.9):
         * `short int`, `int unsigned`, `long long unsigned`, ... */
        fn_ret_type = zc_parse_basic_type_spec(c, &c->token_pos);
    } else if (c->tokens[c->token_pos].type == TK_VOID) {
        fn_ret_type = zero_cc_type_void();
        c->token_pos++;
    }
    /* Leading `const`/`volatile` (e.g. `const char f(void)`, `volatile foo_t *f()`)
     * qualifies the BASE scalar, not a later pointer (C99 6.7.3): apply before the
     * pointer wraps below. */
    if (fn_ret_const) fn_ret_type.is_const = true;
    if (fn_ret_volatile) fn_ret_type.is_volatile = true;
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_RESTRICT)) {
        if (c->tokens[c->token_pos].type == TK_CONST) fn_ret_type.is_const = true;
        if (c->tokens[c->token_pos].type == TK_VOLATILE) fn_ret_type.is_volatile = true;
        if (c->tokens[c->token_pos].type == TK_RESTRICT) fn_ret_type.is_restrict = true;
        c->token_pos++;
    }
    while (c->tokens[c->token_pos].type == TK_STAR) {
        fn_ret_type = zero_cc_type_ptr(&fn_ret_type);
        c->token_pos++;
        /* Qualifiers immediately after a `*` apply to THAT pointer
         * level: `const char * const *` = pointer to (const pointer to
         * const char). */
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_CONST) fn_ret_type.is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) fn_ret_type.is_volatile = true;
            if (c->tokens[c->token_pos].type == TK_RESTRICT) fn_ret_type.is_restrict = true;
            c->token_pos++;
        }
    }
    /* Trailing qualifiers after the return type: `void volatile f();`
     * (c-torture compile/20000605-1) - no effect on the scalar type. */
    while (c->token_pos < c->token_count &&
           (c->tokens[c->token_pos].type == TK_CONST ||
            c->tokens[c->token_pos].type == TK_VOLATILE ||
            c->tokens[c->token_pos].type == TK_RESTRICT)) {
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
    char fn_asm[64] = {0}; /* GNU `__asm("sym")` label on the declarator */
    bool multi_decl = false; /* comma-separated declarator list */
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
        if (!zero_cc_parse_declarator(c, &darena, ctx, &d, false) || !d.name ||
            !zero_cc_declarator_is_function(&d)) {
            zero_cc_ast_arena_free(&darena);
            return;
        }
        snprintf(fn_name, sizeof(fn_name), "%s", d.name);
        if (d.asm_name[0]) {
            /* zero_cc_parse_declarator consumed the trailing asm label */
            snprintf(fn_asm, sizeof(fn_asm), "%s", d.asm_name);
        }
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
                    pl = pn->next;
                    continue;
                }
                zero_cc_ensure_param_cap(ctx, &param_types, &param_names, &param_cap, pi + 1);
                param_types[pi] = zero_cc_type_int();
                if (pi < fc->param_count) {
                    TypeInfo *pti = ast_node_get_typeinfo(c, pn);
                    if (pti) param_types[pi] = typeinfo_to_zero_cc_type(pti);
                }
                param_names[pi] = zero_cc_intern_string(c, (pn->s.str && pn->s.str[0]) ? pn->s.str : "");
                /* C99 6.7.6.2: a VLA parameter's dimension expression is
                 * carried on the NODE_DECL's `right` field (parsed into
                 * darena).  Clone it into the surviving heap dim_arena so
                 * the function record owns it - it is evaluated at entry
                 * to fill the per-param runtime size slot that sizeof(a[0])
                 * reads, exactly like the legacy plain-name path.  Without
                 * this, a parenthesized declarator such as
                 * `int (*f(char a[2][N]))(int)` yields a definition-time
                 * sizeof(a[0]) instead of the runtime row size.  (The
                 * subscript *stride* is unaffected - it uses the param
                 * type's vla_dim, set in zero_cc_parse_param_list.)
                 * Sprint 122 Stage 1: capability parity for the
                 * declarator-stack path; the plain-name path reuses this
                 * in Stage 2. */
                if (pn->right != AST_NULL) {
                    if (!dim_arena) {
                        dim_arena = (ZeroCCASTArena *)calloc(1, sizeof(ZeroCCASTArena));
                        if (dim_arena) zero_cc_ast_arena_init(dim_arena, 64);
                    }
                    if (dim_arena) {
                        dim_exprs[pi] = zero_cc_ast_clone(&darena, pn->right, dim_arena);
                    }
                }
                pl = pn->next;
                pi++;
            }
            param_count = pi;
        }
        zero_cc_ast_arena_free(&darena);
    } else if (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_IDENT ||
                c->tokens[c->token_pos].type == TK_TRUE ||
                c->tokens[c->token_pos].type == TK_FALSE)) {
        size_t name_pos = c->token_pos;  /* declarator start (Stage 2 rewind) */
        snprintf(fn_name, sizeof(fn_name), "%s", c->tokens[c->token_pos].str_val);
        c->token_pos++;

        if (c->tokens[c->token_pos].type != TK_LPAREN) return;
        c->token_pos++;

        /* Sprint 122 Stage 2 (2b): route the common `name(params)` top-level
         * function through the declarator stack.  The K&R old-style form
         * (identifiers only, followed by a declaration block) is still handled
         * by the legacy helper.  The declarator path now carries full type
         * info (long double, __attribute__((mode(...))), (*const fnptr), etc.)
         * and VLA parameter dim expressions (Stage 1). */
        {
            bool params_parsed = zero_cc_try_parse_fn_param_block_kr(
                c, ctx, fn_name, &param_types, &param_names, &param_cap,
                &param_count, &has_prototype, &is_variadic_fn);
            if (!params_parsed) {
                c->token_pos = name_pos;  /* rewind to the function name */
                ZeroCCASTArena darena;
                ZeroCCDeclarator d;
                zero_cc_ast_arena_init(&darena, 256);
                bool decl_ok =
                    zero_cc_parse_declarator(c, &darena, ctx, &d, true) && d.name &&
                    zero_cc_declarator_is_function(&d);
                if (decl_ok) {
                    /* Terminal-position sanity: the declarator must have
                     * consumed exactly the first function declarator, so the
                     * next token is the declaration terminator (';'), the
                     * definition body ('{'), a comma (multi-declarator list -
                     * `double sqrt(double),fabs(double),sin(double);`; the
                     * comma loop below re-parses each remaining declarator),
                     * or a trailing attribute/asm label (an identifier).
                     * Anything else means the param-list parser stopped early
                     * and the leftover is NOT a parameter - e.g. `T decl
                     * (T * (*) (T *), ..)` leaves the `(*)` group (LPAREN),
                     * and the file-scope `struct S {..} __attribute__((
                     * aligned(N)));` form reaches here through
                     * zero_cc_struct_def_is_fn with the preprocessor marker
                     * `__zc_aligned__(N)` masquerading as `name(N)`, leaving
                     * the numeric argument (NUM) - the legacy parser's
                     * stuck-token guard skips it and recovers, so fall back
                     * to it for those shapes. */
                    ZeroCCTokType next_tk = (c->token_pos < c->token_count)
                        ? c->tokens[c->token_pos].type : TK_EOF;
                    if (next_tk != TK_SEMI && next_tk != TK_LBRACE &&
                        next_tk != TK_IDENT && next_tk != TK_COMMA) {
                        decl_ok = false;
                    }
                    if (!decl_ok && getenv("ZC_FB_TRACE")) {
                        fprintf(stderr, "[FB] terminal fn=%s line=%d next=%d\n",
                                fn_name, (int)c->tokens[name_pos].line, (int)next_tk);
                    }
                    /* Param-list completeness: a param list that did not
                     * stop on its ')' is a partial parse - the legacy parser
                     * is the reference.  The residual shapes after the
                     * Sprint 122 parity burn-down (fallback hits: 51 -> 3
                     * across the c-torture corpus, all on SKIP-C89-baseline
                     * files): multi-dim VLA pointer-array params
                     * (`int (*)[f()][f()+1][f()+2]`, pr77754-6), named
                     * anonymous-struct-definition params (`struct S {..} *`),
                     * and the GCC-only `__GIMPLE` builtin (pr115143).  The
                     * `param_total > 16` cap guard is gone: the 2b
                     * param_zt table now grows to ZERO_CC_MAX_PARAMS
                     * (c-torture compile/asmgoto-5 declares a 17-param
                     * function; limits-fnargs's 10,000-param prototypes
                     * parse fine and register the first 256, matching the
                     * legacy path's own cap). */
                    if (decl_ok) {
                        const ZeroCCDeclaratorChunk *fc2 =
                            zero_cc_declarator_get_function_chunk(&d);
                        if (fc2 && !fc2->param_list_complete) {
                            decl_ok = false;
                        }
                        if (!decl_ok && getenv("ZC_FB_TRACE")) {
                            fprintf(stderr,
                                    "[FB] incomplete fn=%s line=%d params=%zu complete=%d\n",
                                    fn_name, (int)c->tokens[name_pos].line,
                                    fc2 ? fc2->param_total : 0,
                                    fc2 ? (int)fc2->param_list_complete : 0);
                        }
                    }
                }
                if (decl_ok && d.asm_name[0]) {
                    /* GNU `__asm("sym")` label: the declarator consumed it,
                     * so the later zero_cc_parse_asm_label call (which the
                     * legacy plain-name path relies on) would see nothing
                     * and the symbol alias would be dropped (c-torture
                     * execute/builtins/memops-asm: `extern void *memcpy(..)
                     * __asm("my_memcpy")` must redirect calls). */
                    snprintf(fn_asm, sizeof(fn_asm), "%s", d.asm_name);
                }
                if (decl_ok) {
                    const ZeroCCDeclaratorChunk *fc =
                        zero_cc_declarator_get_function_chunk(&d);
                    if (fc) {
                        has_prototype = fc->has_prototype;
                        is_variadic_fn = fc->is_variadic;
                        param_count = 0;
                        for (size_t pi = 0;
                             pi < fc->param_count && pi < ZERO_CC_MAX_PARAMS; pi++) {
                            zero_cc_ensure_param_cap(ctx, &param_types,
                                                     &param_names, &param_cap,
                                                     pi + 1);
                            /* param_zt covers every parsed param (up to
                             * ZERO_CC_MAX_PARAMS); the param_tis[16]
                             * TypeInfo table is the fallback for callers
                             * that did not opt in. */
                            param_types[param_count] =
                                (d.param_zt && d.param_zt_cap > pi)
                                    ? d.param_zt[pi]
                                    : (pi < 16
                                         ? typeinfo_to_zero_cc_type(fc->param_tis[pi])
                                         : zero_cc_type_int());
                            /* Recover the param name from the AST param_list
                             * (in darena, not c->arena). */
                            ASTRef pn2 = fc->param_list;
                            for (size_t k = 0; k < pi && pn2 != AST_NULL; k++) {
                                ZeroCCASTNode *n = zero_cc_ast_get(&darena, pn2);
                                if (!n) break;
                                pn2 = n->next;
                            }
                            ZeroCCASTNode *pn_node = zero_cc_ast_get(&darena, pn2);
                            param_names[param_count] =
                                (pn_node && pn_node->tag == NODE_DECL && pn_node->s.str)
                                    ? pn_node->s.str : "";
                            /* VLA dim expression capture (Stage 1). */
                            if (pn_node && pn_node->tag == NODE_DECL &&
                                pn_node->right != AST_NULL) {
                                if (!dim_arena) {
                                    dim_arena = (ZeroCCASTArena *)calloc(1, sizeof(ZeroCCASTArena));
                                    if (dim_arena) zero_cc_ast_arena_init(dim_arena, 64);
                                }
                                if (dim_arena) {
                                    dim_exprs[param_count] = zero_cc_ast_clone(
                                        &darena, pn_node->right, dim_arena);
                                }
                            }
                            param_count++;
                        }
                        params_parsed = true;
                    }
                }
                zero_cc_ast_arena_free(&darena);
                /* Free the heap param-type table (Sprint 122 2b): it is
                 * stack-local to this declarator and the param_types[]
                 * copies above already took the values. */
                zero_cc_declarator_free(&d);
                if (!params_parsed) {
                    /* Legacy prototype parser fallback: the declarator
                     * stack does not yet cover every prototype shape the
                     * legacy hand-written parser handles; try it before
                     * giving up on the function. */
                    c->token_pos = name_pos + 2;  /* right after '(' */
                    if (!zero_cc_parse_fn_param_block(c, ctx, fn_name,
                            &param_types, &param_names, &param_cap,
                            &param_count, &is_variadic_fn, &has_prototype,
                            &dim_arena, dim_exprs)) return;
                }
            }
        }

        /* C99 6.7.5: a declaration may introduce several declarators
         * (`void d0(void), d1(void);`, `double bar(void), c;`).  Each
         * comma-separated declarator after the first reuses the base
         * return type.  Function declarators re-parse their own
         * parameter list; variable declarators get their own storage. */
        first_param_count = param_count;
        while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_COMMA) {
            multi_decl = true;
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
                if (add_global_var(c, vname, vt, voff, vsize, false, true) < 0) {
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
    /* GNU asm label: `int f(void) __asm("g");` - the label trails the
     * complete (single) declarator and renames the emitted symbol; call
     * sites and the definition emission both follow it (c-torture
     * execute/builtins/{strstr,memops}-asm.c).  The parenthesized-declarator
     * path already consumed it into d.asm_name; the plain-name path parses
     * it here.  Multi-declarator lists are left alone (the label would
     * attach to the last declarator, which may be a variable).  fn_asm is
     * applied to pre-existing records now AND inherited by the record
     * created below (prototype or definition). */
    if (!fn_asm[0] && !multi_decl &&
        c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
        (void)zero_cc_parse_asm_label(c, fn_asm, sizeof(fn_asm));
    }
    if (fn_asm[0]) {
        zero_cc_fn_set_asm_name(c, fn_name, fn_asm);
    }

    size_t fn_align = 0;
    {
        size_t probe = 0;
        const char *alias_tgt = NULL;
        while (c->token_pos < c->token_count &&
               c->tokens[c->token_pos].type == TK_IDENT &&
               c->tokens[c->token_pos].str_val[0] != '\0') {
            if (strcmp(c->tokens[c->token_pos].str_val, "__zc_aligned__") == 0) {
                if (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe) && probe > fn_align) {
                    fn_align = probe;
                }
            } else if (strcmp(c->tokens[c->token_pos].str_val, "__zc_alias__") == 0) {
                if (!zero_cc_parse_alias_attr(c, &c->token_pos, &alias_tgt)) break;
            } else {
                break;
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
            if (fn_asm[0]) {
                snprintf(fn->asm_name, sizeof(fn->asm_name), "%s", fn_asm);
            }
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
        /* Sprint 99: top-level functions terminate the nested-enclosing
         * chain; the zero-init default (0) would make ancestor walks
         * (following nested_enclosing_fn_idx) loop on index 0 forever. */
        fn->nested_enclosing_fn_idx = (size_t)-1;
        snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
        if (fn_asm[0]) {
            snprintf(fn->asm_name, sizeof(fn->asm_name), "%s", fn_asm);
        }
        /* Record text_offset BEFORE emitting any prologue bytes for this function */
        fn->text_offset = c->text_section.length;
        /* Sprint 108: GNU __attribute__((section("name"))) on a function.  The
         * file-scope loop already stripped the __zc_section__("name") marker and
         * recorded the name in c->pending_section.  Record it here so the AOT -c
         * emitter places the function's code in the named section.  (The code is
         * still emitted into text_section so JIT execution is unaffected.) */
        fn->section[0] = '\0';
        if (c->pending_section[0]) {
            snprintf(fn->section, sizeof(fn->section), "%s", c->pending_section);
        }
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
                size_t dim_cap = param_count < ZERO_CC_MAX_PARAMS ? param_count : ZERO_CC_MAX_PARAMS;
                fn->param_dim_exprs = (ASTRef *)arena_alloc(farena, param_count * sizeof(ASTRef));
                for (size_t i = 0; i < param_count; i++) {
                    fn->param_dim_exprs[i] = (i < dim_cap) ? dim_exprs[i] : AST_NULL;
                }
            }
        }

        c->local_count = 0;
        c->local_lookup_bound = -1;
        c->nested_fn_base = c->function_count;
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

        for (size_t p = 0; p < param_count && p < ZERO_CC_MAX_PARAMS; p++) {
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
            /* Skip duplicates already in scope: scope_put_symbol would
             * return NULL anyway, but we avoid the type conversion work
             * up front (this loop runs once per function, making it
             * O(n²) over the TU otherwise — significant for files with
             * hundreds of static functions such as c-torture memclr.c). */
            if (scope_lookup_current(global_scope_root, f->name) != NULL) continue;
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
            size_t gparam_count = f->param_count;
            const ZeroCCType *gparam_types = f->param_types;
            if (f->is_nested && (f->nested_captured_count > 0 || f->goto_pair_count > 0)) {
                /* GNU nested functions are BLOCK-SCOPED and their
                 * source-level signature excludes the hidden capture
                 * params (nf->param_count includes them). Register the
                 * user-parameter prototype: a sibling nested function
                 * calling it (body re-parsed against the global scope)
                 * must not get "argument count mismatch: prototype
                 * declares 4 parameter(s), 2 argument(s) given"
                 * (c-torture execute/nestfunc-3.c).
                 * Sprint 99: the hidden nonlocal-goto pair params are
                 * excluded the same way. */
                gparam_count = f->param_count - f->nested_captured_count -
                               2 * (size_t)f->goto_pair_count;
                gparam_types = f->param_types + f->nested_captured_count +
                               2 * f->goto_pair_count;
            }
            TypeInfo **params = NULL;
            if (gparam_count > 0) {
                params = (TypeInfo **)arena_alloc(perm, gparam_count * sizeof(TypeInfo *));
                for (size_t j = 0; j < gparam_count; j++) {
                    params[j] = zero_cc_type_to_typeinfo(perm, gparam_types[j]);
                }
            }
            /* Extern entries (incl. __builtin_* routines registered during
             * lowering, e.g. memcpy/abort) must appear as FUNCTIONS, not
             * variables: a later function calling the same name must see a
             * callable type (registering the return type as SYMBOL_VAR made
             * `__builtin_popcount` an "int variable" and broke its second
             * use - c-torture compile/fold-popcount-1.c). */
            TypeInfo *fn_t = type_function(perm, ret_t, params, gparam_count, f->is_variadic);
            /* No-prototype declaration: a phantom extern registered during
             * lowering has no parameter list, so it must not constrain call
             * argument counts (C99 6.7.5.3). */
            fn_t->details.function.is_prototype = f->has_prototype;
            scope_put_symbol(global_scope_root, f->name, SYMBOL_FUNC, fn_t);
        }
        for (size_t i = 0; i < c->global_var_count; i++) {
            if (scope_lookup_current(global_scope_root, c->global_vars[i].name) != NULL) continue;
            TypeInfo *gv_ti = zero_cc_type_to_typeinfo(perm, c->global_vars[i].type);

            scope_put_symbol(global_scope_root, c->global_vars[i].name, SYMBOL_VAR, gv_ti);
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

        size_t err_count_before = ctx->diags ? diag_error_count(ctx->diags) : 0;
        /* Pre-collect user labels so goto targets validate in one pass
         * (a label may textually follow its goto). */
        sem_collect_labels(ctx, body);
        /* Sprint 99: record this function's user labels so a later-compiled
         * nested function can resolve `goto X` against an ancestor. */
        if (fn->nl_label_count == 0) {
            int n = (int)(ctx->label_count < 32 ? ctx->label_count : 32);
            for (int i = 0; i < n; i++) {
                fn->nl_labels[i] = ctx->labels[i];
                fn->nl_label_count++;
            }
        }
        (void)sem_analyze_ref(ctx, body);
        /* Only ERRORS fail the semantic pass: warnings (e.g. the gnu89
         * implicit-function-declaration warning) are advisory and must not
         * abort lowering of the function. */
        bool sem_ok = (!ctx->diags ||
                       diag_error_count(ctx->diags) == err_count_before);

        if (sem_ok) {
            /* Reset register allocator for this function */
            regalloc_init(&c->regalloc);
            c->next_temp_id = VREG_USER_START;
            c->ir_next_label = 0;

            /* Reset IR buffer for this function */
            ir_reset(c);

            /* Pass 1: Lower AST to IR (instruction selection) */
            c->has_pending_lowering_fn = true;
            c->pending_lowering_fn_idx = (size_t)(fn - c->functions);
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
             *  returns, and nested inlining (depth < 2) are verified safe.
             *  Sprint 110: 128-bit scalar params/returns are safe to inline - the
             *  inlined param substitution sets last_expr_is_128, the arg loop
             *  normalizes 128-bit width, and the inlined return widens + MOV128s
             *  into the target temp. */
            bool uses_va_arg_pack = fn->is_variadic && fn_inline &&
                (zero_cc_ast_has_call(&arena, body, "__builtin_va_arg_pack") ||
                 zero_cc_ast_has_call(&arena, body, "__builtin_va_arg_pack_len"));
            bool ret_is_128_scalar = (fn_ret_type.tag == TYPE_INT128 ||
                                      fn_ret_type.tag == TYPE_UINT128);
            if (c->tu_ast_arena && (fn_inline || fn_static) &&
                (!fn->is_variadic || uses_va_arg_pack) &&
                !fn->returns_struct && (fn->ret_struct_sz <= 8 || ret_is_128_scalar)) {
                ZeroCCASTArena *tu_arena = (ZeroCCASTArena *)c->tu_ast_arena;
                size_t node_count = zero_cc_ast_node_count(&arena, body);
                bool inline_ok = (node_count > 0 && node_count <= 35 &&
                                  !zero_cc_ast_has_call(&arena, body, fn->name));
                if (inline_ok) {
                    if (fn->param_count > 8)
                        inline_ok = false;
                    if (fn->returns_struct || fn->return_type.tag == TYPE_STRUCT ||
                        fn->return_type.tag == TYPE_UNION || fn->return_type.is_complex ||
                        (fn->return_type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn->return_type)))
                        inline_ok = false;
                    for (size_t p = 0; inline_ok && p < fn->param_count; p++) {
                        if (fn->param_types[p].tag == TYPE_FLOAT || fn->param_types[p].tag == TYPE_DOUBLE ||
                            fn->param_types[p].tag == TYPE_STRUCT || fn->param_types[p].tag == TYPE_UNION ||
                            fn->param_types[p].tag == TYPE_ARRAY || fn->param_types[p].is_complex ||
                            (fn->param_types[p].tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn->param_types[p])))
                            inline_ok = false;
                    }
                }
                if (inline_ok) {
                    if (zero_cc_ast_has_local_decl(&arena, body) ||
                        zero_cc_ast_has_label_goto(&arena, body) ||
                        /* __builtin_apply_args snapshots the CURRENT
                         * function's parameters - an inlined body would
                         * capture the caller's table (or none) instead of
                         * the callee's, so such functions keep their own
                         * frame and are never inlined. */
                        zero_cc_ast_has_call(&arena, body, "__builtin_apply_args"))
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

            /* Phase 1: Compile GNU nested functions registered during this
             * enclosing function's compilation.  Each nested function's body
             * was skipped at parse time (token range saved in
             * nested_body_start/nested_body_end).  Re-parse, lower, and emit
             * each one as a separate function in the same text section.
             * No closure capture in Phase 1 - nested functions that reference
             * enclosing locals will miscompile (acceptable limitation). */
            for (size_t ni = c->nested_fn_base; ni < c->function_count; ni++) {
                ZeroCCFunction *nf = &c->functions[ni];
                if (!nf->is_nested || nf->text_offset != 0 || nf->is_tramp_stub) continue;

                nf->text_offset = c->text_section.length;

                /* Save compiler state */
                size_t save_token_pos = c->token_pos;
                size_t save_function_count = c->function_count;
                size_t save_local_count = c->local_count;
                int32_t save_lookup_bound = c->local_lookup_bound;

                /* Do NOT truncate function_count to ni+1: zero_cc_lower_ast
                 * is pinned to this function via has_pending_lowering_fn /
                 * pending_lowering_fn_idx, and a truncation would discard
                 * the trampoline-stub record registered right after this
                 * nested function (at parse time) - a re-parsed inner
                 * nested function would then be appended onto the stub's
                 * slot, overwriting it (c-torture execute/20061220-1:
                 * baz/nested/nested2 emitted the stub under the inner
                 * function's name and never emitted the real body). */

                /* Set up the nested function's local table BEFORE re-parsing
                 * the body. Two reasons:
                 *  (1) zero_cc_add_local assigns stack_offset from
                 *      c->current_stack_frame at call time, so the frame
                 *      must be reset before params are added (adding them
                 *      against the enclosing function's leftover frame left
                 *      the slots pointing outside the nested frame).
                 *  (2) declaration nodes bake c->local_count into
                 *      node->aux.step at parse time; lowering resolves the
                 *      symbol as c->locals[aux.step - 1]. Re-parsing while
                 *      the enclosing table was still active made body locals
                 *      carry indices valid for the ENCLOSING table, so e.g.
                 *      the first body local resolved to the nested table's
                 *      second entry (a promoted param with stack_offset 0)
                 *      and was stored at [fp+0], clobbering the caller's
                 *      saved RBP. Parsing with the nested table active gives
                 *      every body declaration its own index after the
                 *      params.
                 * The first nested_captured_count params are captured values
                 * (passed in registers by the caller); the rest are user
                 * params. */
                c->local_count = 0;
                c->local_lookup_bound = -1;
                if (c->local_hash_buckets && c->local_hash_bucket_count > 0) {
                    for (size_t b = 0; b < c->local_hash_bucket_count; b++) c->local_hash_buckets[b] = -1;
                }
                c->current_stack_frame = BE(c)->callee_save_frame_reserve;
                /* System V AMD64 ABI: >16 byte struct returns use the hidden
                 * pointer in RDI; reserve its slot before the params (same
                 * as the top-level function path). */
                if (nf->ret_struct_sz > 16) {
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    nf->sret_stack_offset = c->current_stack_frame;
                } else {
                    nf->sret_stack_offset = 0;
                }
                for (size_t p = 0; p < nf->param_count && p < ZERO_CC_MAX_PARAMS; p++) {
                    ZeroCCSym *sym = zero_cc_add_local(c, nf->param_names[p], nf->param_types[p]);
                    sym->is_param = true;
                    sym->param_reg_idx = (int)p;
                }
                /* Phase 3b: by-reference captures. Each scalar capture's
                 * hidden param is a POINTER to the enclosing storage.
                 * Mark it is_capture_mirror and retype the symbol to the
                 * POINTEE type: body paths key widths, inc-steps and int
                 * extensions off sym->type (a pointer type would make
                 * `i++` step by 4 and stores write 8 bytes), while
                 * zc_sym_load/store/addr route every access through the
                 * slot's pointer value (a mirror local would break
                 * aliasing with &capture passed as a user argument). */
                for (size_t ci = 0; ci < nf->nested_captured_count && ci < nf->param_count; ci++) {
                    if (!nf->nested_capture_byref || !nf->nested_capture_byref[ci]) continue;
                    c->locals[ci].is_capture_mirror = true;
                    c->locals[ci].type = nf->nested_capture_types[ci];
                }

                /* Re-parse the nested function body from saved tokens.
                 * Publish this function's by-ref capture flags so a nested
                 * function REGISTERED during this re-parse can rename
                 * pointer-param captures (see enc_byref_captures). */
                c->enc_byref_captures = nf->nested_capture_byref;
                c->enc_capture_count = nf->nested_captured_count;
                c->token_pos = nf->nested_body_start;
                ZeroCCASTArena n_arena;
                zero_cc_ast_arena_init(&n_arena, 1024);
                ASTRef n_body = zero_cc_parse_stmt_ast(c, &n_arena, ctx);
                c->enc_byref_captures = NULL;
                c->enc_capture_count = 0;
                if (n_body == AST_NULL) {
                    c->token_pos = save_token_pos;
                    c->function_count = save_function_count;
                    c->local_count = save_local_count;
                    c->local_lookup_bound = save_lookup_bound;
                    ctx->ast_arena = &arena;
                    zero_cc_ast_arena_free(&n_arena);
                    continue;
                }

                /* Set up semantic context for this nested function.
                 * The re-parsed body is fresh AST: it carries NO semantic
                 * annotations (member offsets in aux.alt, TypeInfo,
                 * member_name) until sem_analyze_ref runs - the top-level
                 * path does exactly this before lowering, and the nested
                 * path must too, or member accesses lower with garbage
                 * offsets.  Enter a fresh scope holding the nested
                 * function's params (captured + user) so identifiers
                 * validate, mirroring the top-level param scope. */
                ctx->ast_arena = &n_arena;
                TypeInfo *save_fn_ret_ti = ctx->current_fn_ret_type;
                Scope *n_param_scope = scope_enter(&ctx->current_scope, perm);
                for (size_t p = 0; p < nf->param_count && p < ZERO_CC_MAX_PARAMS; p++) {
                    /* Phase 3a: a by-reference capture's hidden param is a
                     * POINTER, but the body must see the ORIGINAL type
                     * (the mirror local's type) - otherwise `i++` on a
                     * captured int would lower as pointer arithmetic. */
                    ZeroCCType st = nf->param_types[p];
                    if (p < nf->nested_captured_count && nf->nested_capture_byref &&
                        nf->nested_capture_byref[p]) {
                        st = nf->nested_capture_types[p];
                    }
                    TypeInfo *pti = zero_cc_type_to_typeinfo(perm, st);
                    scope_put_symbol(n_param_scope, nf->param_names[p], SYMBOL_VAR, pti);
                }
                ctx->current_fn_ret_type = zero_cc_type_to_typeinfo(perm, nf->return_type);
                sem_collect_labels(ctx, n_body);
                /* Sprint 99: record this nested function's labels (a deeper
                 * nesting level may nonlocal-goto into it). */
                if (nf->nl_label_count == 0) {
                    int n = (int)(ctx->label_count < 32 ? ctx->label_count : 32);
                    for (int i = 0; i < n; i++) {
                        nf->nl_labels[i] = ctx->labels[i];
                        nf->nl_label_count++;
                    }
                }
                /* Sprint 99: expose the ANCESTOR labels (outermost first) so
                 * `goto X` into an enclosing function validates. */
                {
                    int al = 0;
                    for (size_t ai = nf->nested_enclosing_fn_idx;
                         ai != (size_t)-1 && al < 64; ) {
                        ZeroCCFunction *af = &c->functions[ai];
                        for (int li = 0; li < af->nl_label_count && al < 64; li++) {
                            ctx->ancestor_labels[al++] = af->nl_labels[li];
                        }
                        if (!af->is_nested) break;
                        ai = af->nested_enclosing_fn_idx;
                    }
                    ctx->ancestor_label_count = al;
                }
                (void)sem_analyze_ref(ctx, n_body);
                ctx->ancestor_label_count = 0;

                /* Lower and emit */
                c->has_pending_lowering_fn = true;
                c->pending_lowering_fn_idx = ni;
                zero_cc_lower_ast(c, &n_arena, n_body);

                scope_leave(&ctx->current_scope);
                ctx->current_fn_ret_type = save_fn_ret_ti;

                /* Restore compiler state. Keep any functions registered DURING
                 * this re-parse (multi-level nesting: a nested function
                 * defined inside another nested function): restoring the
                 * old count would discard them and the enclosing function's
                 * call to it would lower as a direct call to a
                 * non-existent function (segfault - c-torture
                 * execute/20061220-1.c baz/nested/nested2). */
                c->token_pos = save_token_pos;
                if (c->function_count < save_function_count) c->function_count = save_function_count;
                c->local_count = save_local_count;
                c->local_lookup_bound = save_lookup_bound;
                ctx->ast_arena = &arena;

                zero_cc_ast_arena_free(&n_arena);

                /* Phase 3b: emit the trampoline stub right after the
                 * nested function. The stub dispatches to the nested fn
                 * with a relative `call` (both live in this text image, so
                 * the offset is known at emission time - no relocation
                 * needed) and reads its captures from the static ctx object
                 * (x86-64: movabs + absolute data reloc; AArch64: adrp+add
                 * pair reloc; RISC-V64: auipc+addi pair reloc).
                 * Machine-code paths only (JIT + raw AOT). */
                if (nf->tramp_stub_fn_idx >= 0) {
                    ZeroCCFunction *sf = &c->functions[nf->tramp_stub_fn_idx];
                    sf->text_offset = c->text_section.length;
                    if (c->isa == ISA_X86_64)
                        zero_cc_x86_emit_tramp_stub(c, nf);
                    else if (c->isa == ISA_AARCH64)
                        zero_cc_aarch64_emit_tramp_stub(c, nf);
                    else if (c->isa == ISA_RISCV64)
                        zero_cc_riscv_emit_tramp_stub(c, nf);
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


