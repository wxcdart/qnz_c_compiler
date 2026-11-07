#ifndef ZERO_CC_PARSER_ZERO_CC_DECLARATOR_STACK_H
#define ZERO_CC_PARSER_ZERO_CC_DECLARATOR_STACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/types/types.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/ast_expr.h"
#include "zero_cc/parser/ast_arena.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/semantic/decl.h"

#define ZERO_CC_DECLARATOR_MAX_NESTING 16
#define ZERO_CC_DECLARATOR_MAX_LEVELS  16
#define ZERO_CC_DECLARATOR_MAX_ITEMS   16

typedef enum ZeroCCDeclChunkKind {
    ZERO_CC_DECL_CHUNK_POINTER,
    ZERO_CC_DECL_CHUNK_ARRAY,
    ZERO_CC_DECL_CHUNK_FUNCTION
} ZeroCCDeclChunkKind;

typedef struct ZeroCCDeclaratorChunk {
    ZeroCCDeclChunkKind kind;
    /* Qualifiers */
    bool is_const;
    bool is_volatile;
    bool is_restrict;
    bool is_atomic;
    /* Pointer */
    bool is_func_ptr;
    /* Array */
    size_t array_len;           /* 0 = VLA or unsized */
    bool is_vla;
    bool is_static_array;       /* [static N] */
    ASTRef dim_expr;
    /* Function */
    ASTRef param_list;          /* AST linked list of NODE_DECL */
    size_t param_count;
    TypeInfo *param_tis[16];
    bool has_prototype;         /* true if prototype list or (void), false if K&R () */
    bool is_variadic;
} ZeroCCDeclaratorChunk;

typedef struct ZeroCCDeclaratorLevel {
    ZeroCCDeclaratorChunk prefixes[ZERO_CC_DECLARATOR_MAX_ITEMS];
    size_t prefix_count;
    ZeroCCDeclaratorChunk suffixes[ZERO_CC_DECLARATOR_MAX_ITEMS];
    size_t suffix_count;
} ZeroCCDeclaratorLevel;

typedef struct ZeroCCDeclarator {
    ZeroCCDeclaratorLevel levels[ZERO_CC_DECLARATOR_MAX_LEVELS];
    size_t num_levels;

    /* Flattened stack: TypeObj stack[MAX_NESTING]; int top = 0; */
    ZeroCCDeclaratorChunk stack[ZERO_CC_DECLARATOR_MAX_NESTING];
    int top;

    /* Identifier */
    const char *name;
    size_t name_tok_pos;
    size_t name_level;
    bool is_abstract;
    /* GNU asm label: `int x asm ("y");` / `register int v asm ("rax");`
     * - the concatenated string, empty when absent.  The name is a symbol
     * specifier (or register specifier); ZeroCC accepts it and records it,
     * but stores the object normally (c-torture asmgoto-5 / mangle-1 /
     * pr100278 / pr51447). */
    char asm_name[64];
    /* Sprint 122 2b: canonical ZeroCCType for each function parameter, stored by
     * zero_cc_parse_param_list so the caller can use it directly (avoiding the
     * lossy TypeInfo -> ZeroCCType round-trip that drops struct members).
     * Heap-allocated (NOT a per-chunk array: ZeroCCDeclarator is stack-allocated
     * and a per-chunk ZeroCCType[16] bloats it to ~2 MB).  NULL when the
     * declarator is not a function or has no parameters. */
    ZeroCCType *param_zt;
    size_t param_zt_cap;
} ZeroCCDeclarator;

/* Forward declarations */
static inline ZeroCCType zero_cc_resolve_base_type(ZeroCCCompiler *c, ZeroCCTokType t, const char *s);

/* Initialize declarator struct */
static inline void zero_cc_declarator_init(ZeroCCDeclarator *d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->num_levels = 1;
}

/* GNU asm label following a complete declarator:
 *   `asm ( "name" )` / `__asm__ ( "a" "b" )` - a run of string literals
 * inside one balanced paren group (the symbol/register name).  Consumes
 * the tokens and concatenates the strings into out.  Returns false (and
 * leaves the stream untouched) when the shape does not match. */
static inline bool zero_cc_parse_asm_label(ZeroCCCompiler *c, char *out, size_t outsz) {
    if (!c || !out || outsz == 0) return false;
    if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) {
        return false;
    }
    const char *an = c->tokens[c->token_pos].str_val;
    if (!an || (strcmp(an, "asm") != 0 && strcmp(an, "__asm__") != 0 &&
                strcmp(an, "__asm") != 0)) {
        return false;
    }
    if (c->token_pos + 1 >= c->token_count ||
        c->tokens[c->token_pos + 1].type != TK_LPAREN) {
        return false;
    }
    size_t p = c->token_pos + 2;
    int depth = 1;
    size_t o = 0;
    out[0] = '\0';
    while (p < c->token_count && depth > 0) {
        ZeroCCTokType pt = c->tokens[p].type;
        if (pt == TK_LPAREN) {
            depth++;
        } else if (pt == TK_RPAREN) {
            depth--;
            if (depth == 0) { p++; break; }
        } else if (pt == TK_STRING && depth == 1) {
            const char *s = c->tokens[p].str_val;
            if (s) {
                for (const char *ch = s; *ch && o + 1 < outsz; ch++) {
                    out[o++] = *ch;
                }
            }
        } else if (pt != TK_STRING) {
            return false; /* not a plain string-literal group */
        }
        p++;
    }
    if (depth != 0) return false;
    out[o] = '\0';
    c->token_pos = p;
    return true;
}

/* Forward declarations (defined further down in this header): the
 * function-pointer parameter branch below re-parses `(*f)(...)`
 * declarators with the full declarator-stack machinery. */
static inline bool zero_cc_parse_declarator(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx, ZeroCCDeclarator *d);
static inline ZeroCCType zero_cc_declarator_apply(
    const ZeroCCDeclarator *d, ZeroCCType base_type);

/* Parse parameter list of a function suffix: (type param, ...) */
static inline void zero_cc_parse_param_list(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx, ZeroCCDeclaratorChunk *fn_chunk,
    ZeroCCDeclarator *decl)
{
    ASTRef param_head = AST_NULL;
    ASTRef param_tail = AST_NULL;
    bool first_param = true;
    while (c->token_pos < c->token_count) {
        if (c->tokens[c->token_pos].type == TK_RPAREN) {
            break;
        }
        if (!first_param) {
            if (c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
            } else {
                break;
            }
        }
        first_param = false;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            fn_chunk->is_variadic = true;
            fn_chunk->has_prototype = true;
            c->token_pos++;
            break;
        }
        /* void alone means no params */
        if (c->tokens[c->token_pos].type == TK_VOID) {
            if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_RPAREN) {
                fn_chunk->has_prototype = true;
                c->token_pos++;
                break;
            }
        }
        bool param_is_const = false;
        bool param_is_volatile = false;
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
            c->token_pos++;
        }
        size_t param_vec_size = 0;
        {
            size_t save = c->token_pos;
            size_t probe = 0;
            if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    param_vec_size = probe;
                } else {
                    c->token_pos = save;
                }
            }
        }
        /* GNU mode(X) marker: consumed (not applied) in parameter position -
         * mode-on-parameter was unsupported before marker emission, so
         * skipping preserves the old behavior. */
        zero_cc_skip_mode_attr(c);
        /* C99 6.9.1p5: storage-class specifiers in a param are not recognized
         * and are ignored (`register T x`, `static T x`, ...). */
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_REGISTER ||
                c->tokens[c->token_pos].type == TK_STATIC ||
                c->tokens[c->token_pos].type == TK_AUTO ||
                c->tokens[c->token_pos].type == TK_EXTERN)) {
            c->token_pos++;
        }
        /* C99 6.7.2.3 prefix complex: `_Complex double x` / `__complex__ float x`.
         * Consume the specifier; the real base type follows and gets wrapped. */
        bool p_prefix_complex = false;
        if (c->tokens[c->token_pos].type == TK_IDENT &&
            zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
            p_prefix_complex = true;
            c->token_pos++;
        }
        size_t ptype_pos = c->token_pos;
        ZeroCCTokType param_type_tok = c->tokens[c->token_pos].type;
        if (!is_type_specifier(c, param_type_tok)) {
            break;
        }
        c->token_pos++;
        ZeroCCType param_typeof_type;
        memset(&param_typeof_type, 0, sizeof(param_typeof_type));
        bool param_has_typeof = false;
        if (param_type_tok == TK_TYPEOF || param_type_tok == TK_TYPEOF_UNQUAL) {
            param_has_typeof = true;
            bool unqual = (param_type_tok == TK_TYPEOF_UNQUAL);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                if (zero_cc_tok_is_type_start(c, c->token_pos)) {
                    param_typeof_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                } else {
                    param_typeof_type = zero_cc_type_of_equality(c, &c->token_pos);
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            } else {
                param_typeof_type = zero_cc_type_int();
            }
            if (unqual) {
                param_typeof_type.is_const = false;
                param_typeof_type.is_volatile = false;
                param_typeof_type.is_restrict = false;
                param_typeof_type.is_atomic = false;
            }
        }
        bool param_is_unsigned = false;
        bool param_is_long_double = false;
        if (param_type_tok == TK_UNSIGNED || param_type_tok == TK_SIGNED) {
            param_is_unsigned = (param_type_tok == TK_UNSIGNED);
            if (c->token_pos < c->token_count &&
                (c->tokens[c->token_pos].type == TK_INT ||
                 c->tokens[c->token_pos].type == TK_CHAR ||
                 c->tokens[c->token_pos].type == TK_SHORT ||
                 c->tokens[c->token_pos].type == TK_LONG)) {
                ptype_pos = c->token_pos;
                param_type_tok = c->tokens[c->token_pos].type;
                c->token_pos++;
                if (param_type_tok == TK_SHORT && c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                if (param_type_tok == TK_LONG) {
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                }
            } else {
                param_type_tok = TK_INT;
            }
        } else if (param_type_tok == TK_LONG) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_DOUBLE_KW) {
                /* C99 6.7.2.1: long double / long long double - the double
                 * keyword follows the long run and becomes the base (modeled
                 * through the double value model with the is_long_double
                 * marker), matching the legacy prototype parser. */
                param_type_tok = TK_DOUBLE_KW;
                c->token_pos++;
                param_is_long_double = true;
            } else {
                if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                    param_is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
            }
        } else if (param_type_tok == TK_SHORT) {
            if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                param_is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        }
        const char *param_tag_name = NULL;
        if ((param_type_tok == TK_STRUCT || param_type_tok == TK_UNION || param_type_tok == TK_ENUM) &&
            c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            param_tag_name = c->tokens[c->token_pos].str_val;
            c->token_pos++;
        }
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
            c->token_pos++;
        }
        const char *ptname = param_tag_name ? param_tag_name : c->tokens[ptype_pos].str_val;
        ZeroCCType param_base = param_has_typeof ? param_typeof_type : zero_cc_resolve_base_type(c, param_type_tok, ptname);
        if (p_prefix_complex) {
            param_base = zero_cc_type_complex(&param_base);
        }
        /* C99 6.7.2.3 suffix complex: `double _Complex x` / `float __complex__ x`.
         * The complex spec follows the base type (before any pointer stars). */
        if (!p_prefix_complex && c->token_pos < c->token_count &&
            c->tokens[c->token_pos].type == TK_IDENT &&
            zero_cc_tok_is_complex_spec(c->tokens[c->token_pos].str_val)) {
            c->token_pos++;
            param_base = zero_cc_type_complex(&param_base);
        }
        if (param_is_unsigned) {
            if (param_base.tag == TYPE_INT8) param_base.tag = TYPE_UINT8;
            else if (param_base.tag == TYPE_INT16) param_base.tag = TYPE_UINT16;
            else if (param_base.tag == TYPE_INT32) param_base.tag = TYPE_UINT32;
            else if (param_base.tag == TYPE_INT64) param_base.tag = TYPE_UINT64;
        }
        param_base.is_const = param_is_const;
        param_base.is_volatile = param_is_volatile;
        param_base.is_long_double = param_is_long_double;
        /* C99 long double: 16-byte object, 16-byte aligned (x86-64 SysV).
         * The base was resolved through the 64-bit double value model
         * (8/8); widen the object size to match zero_cc_type_long_double()
         * - every other declaration path (locals, struct members, legacy
         * prototype parser) registers long double as 16/16, and the frame
         * layout / ABI marshalling keys off the registered size.  A wrong
         * 8/8 size shifted local slots and let a spill clobber a by-value
         * struct parameter (c-torture execute/20030914-1). */
        if (param_is_long_double) {
            param_base.size = 16;
            param_base.align = 16;
        }
        /* GNU vector parameter: the marker probed above wraps the scalar
         * element type in a vector type (pointers, if any, apply on top). */
        if (param_vec_size > 0) {
            param_base = zero_cc_type_vector(&param_base, param_vec_size);
        }

        int param_ptr_depth = 0;
        /* Per-star-level qualifiers (C99 6.7.3) for the plain-star branch
         * below; the `(*`/`((` branches re-parse or carry no qualifiers. */
        bool ppc_const[16] = {false}, ppc_vol[16] = {false}, ppc_restr[16] = {false};
        ZeroCCType ptype = param_base;  /* param type builder (dims/pointers apply on top) */
        ASTRef param_dim_expr = AST_NULL; /* VLA parameter dimension expr (C99 6.7.6.2) */
        bool is_param_func_ptr = false;
        bool ptype_full = false; /* set when the declarator re-parse rebuilt ptype */

        const char *pname = NULL;
        if (c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos].type == TK_LPAREN &&
            c->tokens[c->token_pos + 1].type == TK_STAR) {
            is_param_func_ptr = true;
            size_t decl_start = c->token_pos; /* '(*' - declarator re-parse anchor */
            c->token_pos += 2;
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            /* Qualifiers between the star run and the name: `(*const f)`. */
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT)) c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                /* Nested parens around the name: `(*const (funcp))`. */
                c->token_pos++;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                    pname = c->tokens[c->token_pos].str_val;
                    c->token_pos++;
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                pname = c->tokens[c->token_pos].str_val;
                c->token_pos++;
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
            }
            /* Trailing array dim on the parenthesized pointer: `(*p)[N]`
             * (pointer to array), not a function pointee. Consume it so the
             * declarator re-parse below stops where the quick scan left off. */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                int bdepth = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && bdepth > 0) {
                    if (c->tokens[c->token_pos].type == TK_LBRACKET) bdepth++;
                    else if (c->tokens[c->token_pos].type == TK_RBRACKET) bdepth--;
                    c->token_pos++;
                }
            }
            param_ptr_depth = 1;
            /* The quick scan above records only the pointer depth and
             * DROPS the function suffix's parameter types, so re-parse
             * the full declarator with the declarator-stack parser to
             * rebuild ptype with the pointee prototype (fn_params)
             * intact.  Without it, an indirect call through this
             * function-pointer parameter marshals arguments against no
             * signature: `void t(void (*f)(float, float)); f(1.0f,
             * 2.0f)` passed the floats as model doubles (the callee
             * read 0.0f) because the SysV float-arg narrowing is driven
             * by the declared parameter types.  The re-parse starts at
             * decl_start and must stop where the quick scan left off;
             * any failure or position mismatch falls back to the legacy
             * quick-built type below. */
            {
                size_t saved_pos = c->token_pos;
                c->token_pos = decl_start;
                ZeroCCDeclarator dp;
                bool reparse_ok = zero_cc_parse_declarator(c, arena, ctx, &dp) &&
                                  (c->token_pos == saved_pos);
                c->token_pos = saved_pos;
                if (reparse_ok) {
                    ptype = zero_cc_declarator_apply(&dp, param_base);
                    ptype_full = true;
                }
            }
        } else if (c->token_pos + 1 < c->token_count &&
                   c->tokens[c->token_pos].type == TK_LPAREN &&
                   c->tokens[c->token_pos + 1].type == TK_LPAREN) {
            /* Function-pointer declarator wrapped in an extra paren layer:
             * `((*f) (void ()))` (K&R idiom, c-torture execute/921215-1:
             * `void p(void ((*f) (void ())))`). The single-level `(*f)(...)`
             * branch above cannot see the `*` (it is nested one paren
             * deeper), so scan the balanced paren group for the star and
             * name, then consume the group plus the trailing (params). */
            int depth = 0;
            size_t close = c->token_count;
            size_t scan_name_pos = c->token_count;
            int stars = 0;
            const char *scan_name = NULL;
            for (size_t scan = c->token_pos; scan < c->token_count; scan++) {
                if (c->tokens[scan].type == TK_LPAREN) depth++;
                else if (c->tokens[scan].type == TK_RPAREN) {
                    depth--;
                    if (depth == 0) { close = scan; break; }
                } else if (c->tokens[scan].type == TK_STAR) stars++;
                else if (c->tokens[scan].type == TK_IDENT && scan_name == NULL) {
                    const char *s = c->tokens[scan].str_val;
                    if (strcmp(s, "const") != 0 && strcmp(s, "volatile") != 0 &&
                        strcmp(s, "restrict") != 0 && strcmp(s, "_Atomic") != 0) {
                        scan_name = s;
                        scan_name_pos = scan;
                    }
                }
            }
            if (stars > 0 && scan_name && close < c->token_count) {
                /* A `(` after the name inside the group is a function
                 * suffix: `((*f) (void ()))` -> pointer to function;
                 * `((*g))` -> a plain (double-parenthesized) pointer. */
                for (size_t a2 = scan_name_pos + 1; a2 < close; a2++) {
                    if (c->tokens[a2].type == TK_LPAREN) {
                        is_param_func_ptr = true;
                        break;
                    }
                }
                pname = scan_name;
                param_ptr_depth = stars;
                c->token_pos = close + 1;
            }
        } else {
            /* Per-star-level qualifiers (C99 6.7.3): `const char *const
             * *bar` is a pointer to (const pointer to const char) - the
             * const between the stars belongs to THAT star's pointer and
             * must not leak onto the outer one (strpbrk.c: `bar--` is
             * legal there; a single flag applied to the outermost level
             * made `bar` const and rejected the decrement). */
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR &&
                   param_ptr_depth < 16) {
                ppc_const[param_ptr_depth] = false;
                ppc_vol[param_ptr_depth] = false;
                ppc_restr[param_ptr_depth] = false;
                param_ptr_depth++;
                c->token_pos++;
                while (c->token_pos < c->token_count &&
                       (c->tokens[c->token_pos].type == TK_CONST ||
                        c->tokens[c->token_pos].type == TK_VOLATILE ||
                        c->tokens[c->token_pos].type == TK_RESTRICT)) {
                    if (c->tokens[c->token_pos].type == TK_CONST) ppc_const[param_ptr_depth - 1] = true;
                    if (c->tokens[c->token_pos].type == TK_VOLATILE) ppc_vol[param_ptr_depth - 1] = true;
                    if (c->tokens[c->token_pos].type == TK_RESTRICT) ppc_restr[param_ptr_depth - 1] = true;
                    c->token_pos++;
                }
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                pname = c->tokens[c->token_pos].str_val;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                /* Array parameter: `int f(int a[n])` decays to `int *a`,
                 * but the dimension expression is evaluated at call entry
                 * (C99 6.7.6.2) - parse and keep it (VLA parameter
                 * dimension) instead of discarding the brackets.
                 * Multiple suffixes (`int m[][4]`, `int[][fn3()]` pr77754):
                 * the OUTERMOST dimension decays to a pointer; the inner
                 * ones form the pointed-to array type. */
                                int n_suffs = 0;
                ssize_t p_dims[8] = {0};   /* constant dims; 0 = VLA expr */
                ASTRef p_dim_exprs[8] = {AST_NULL};
                while (c->token_pos < c->token_count &&
                       c->tokens[c->token_pos].type == TK_LBRACKET &&
                       n_suffs < 8) {
                    c->token_pos++;
                    size_t dim_start = c->token_pos;
                    int bdepth = 1;
                    while (c->token_pos < c->token_count && bdepth > 0) {
                        if (c->tokens[c->token_pos].type == TK_LBRACKET) bdepth++;
                        else if (c->tokens[c->token_pos].type == TK_RBRACKET) bdepth--;
                        c->token_pos++;
                    }
                    size_t dim_end = c->token_pos; /* just past ']' */
                    /* Keep every dimension (C99 6.7.6.2/6.7.6.3p7):
                     * `char a[2][++N]` decays to `char (*)[N]` - the
                     * OUTERMOST dim becomes the pointer, the INNER dims
                     * (incl. VLA exprs like `++N`) form the pointed-to
                     * array type and are evaluated at entry. */
                    /* A constant dimension is exactly two tokens in
                     * [dim_start, dim_end): the NUM literal followed by
                     * the closing ']' (dim_end sits just past ']').  The
                     * older `dim_start + 1 == dim_end` test could never
                     * match - it routed every `a[N]` through the VLA
                     * branch, building ARRAY{len=0, size=elem} with a
                     * wrong static size (latent since Sprint 66; masked
                     * on the parenthesized path by TypeInfo-based
                     * registration, exposed by Sprint 122 Stage 2's
                     * param_zt which uses the built ZeroCCType directly).
                     * C99 6.7.6.3p7: the dimension is a constant
                     * expression - a plain literal is the common case;
                     * non-literal constants still take the VLA-expr path
                     * below (evaluated at entry, same runtime result). */
                    if (dim_end - dim_start == 2 &&
                        c->tokens[dim_start].type == TK_NUM &&
                        c->tokens[dim_end - 1].type == TK_RBRACKET) {
                        p_dims[n_suffs] = (ssize_t)c->tokens[dim_start].num_val;
                    } else {
                        /* VLA (or const-expr) dimension (C99 6.7.6.2):
                         * keep the expression for entry evaluation. */
                        c->token_pos = dim_start;
                        ASTRef de = parse_expr_ast_internal(c, arena);
                        c->token_pos = dim_end;
                        p_dims[n_suffs] = 0;
                        p_dim_exprs[n_suffs] = de;
                    }
                    n_suffs++;
                }
                /* Build the pointed-to type from the INNER dimensions
                 * (innermost-first), then wrap with the decay pointer. */
                for (int i = n_suffs - 1; i >= 1; i--) {
                    if (p_dim_exprs[i] != AST_NULL) {
                        ptype = zero_cc_type_array(&ptype, 0);
                        ptype.is_vla = true;
                        ptype.vla_dim = (size_t)p_dim_exprs[i];
                        ptype.size = ptype.base->size > 0 ? ptype.base->size : 1;
                        ptype.align = 8;
                    } else {
                        ptype = zero_cc_type_array(&ptype, p_dims[i]);
                    }
                }
                if (n_suffs > 0) ptype = zero_cc_type_ptr(&ptype);
                /* Chain the VLA dim expressions (outermost-to-innermost)
                 * for entry evaluation + the registration save. */
                for (int i = 0; i < n_suffs; i++) {
                    if (p_dim_exprs[i] == AST_NULL) continue;
                    if (param_dim_expr == AST_NULL) {
                        param_dim_expr = p_dim_exprs[i];
                    } else {
                        ZeroCCASTNode *t2 = zero_cc_ast_get(arena, param_dim_expr);
                        while (t2 && t2->next != AST_NULL) t2 = zero_cc_ast_get(arena, t2->next);
                        if (t2) t2->next = p_dim_exprs[i];
                    }
                }
            }
        }
        /* `(void)` is an empty parameter list (C99 6.7.6.3p2): it
         * makes the declaration a prototype but adds no parameters. */
        if (first_param && param_type_tok == TK_VOID && param_ptr_depth == 0 && (pname == NULL || pname[0] == '\0')) {
            fn_chunk->has_prototype = true;
            break;
        }
        if (pname == NULL) pname = "";

        if (!ptype_full) {
            for (int pd = 0; pd < param_ptr_depth; pd++) {
                ptype = zero_cc_type_ptr(&ptype);
                ptype.is_const = (pd < 16) ? ppc_const[pd] : false;
                ptype.is_volatile = (pd < 16) ? ppc_vol[pd] : false;
                ptype.is_restrict = (pd < 16) ? ppc_restr[pd] : false;
            }
            if (is_param_func_ptr) {
                ptype.is_func_ptr = true;
            } else if (ptype.tag == TYPE_ARRAY) {
                /* C99 6.7.6.3p7: array parameter (via typedef/typeof) decays to pointer */
                ZeroCCType elem = ptype.base ? *ptype.base : zero_cc_type_int64();
                ptype = zero_cc_type_ptr(&elem);
                ptype.is_const = param_base.is_const;
                ptype.is_volatile = param_base.is_volatile;
            }
        }

        ASTRef p_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
        ZeroCCASTNode *p_node = zero_cc_ast_get(arena, p_ref);
        p_node->line = c->tokens[c->token_pos].line;
        p_node->col = c->tokens[c->token_pos].col;
        p_node->s.str = pname;
        p_node->s_len = strlen(pname);
        p_node->right = param_dim_expr; /* VLA dimension, evaluated at entry */

        TypeInfo *ptype_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ptype);
        decl_node_set_type(ctx, p_node, ptype_ti);

        if (fn_chunk->param_count < 16) {
            fn_chunk->param_tis[fn_chunk->param_count] = ptype_ti;
            /* Sprint 122 2b: store the canonical ZeroCCType (heap-allocated on
             * the top-level declarator, not the per-chunk struct). */
            if (decl && decl->param_zt_cap < 16) {
                decl->param_zt_cap = 16;
                decl->param_zt = (ZeroCCType *)malloc(16 * sizeof(ZeroCCType));
            }
            if (decl) decl->param_zt[fn_chunk->param_count] = ptype;
            fn_chunk->param_count++;
        }

        if (param_head == AST_NULL) {
            param_head = p_ref;
            param_tail = p_ref;
        } else {
            ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, param_tail);
            if (tail_node) tail_node->next = p_ref;
            param_tail = p_ref;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
        c->token_pos++;
    }
    fn_chunk->param_list = param_head;
    if (!first_param) {
        fn_chunk->has_prototype = true;
    }
}

/* Evaluate an AST expression to a compile-time constant integer if possible */
static inline bool zero_cc_ast_eval_const(ZeroCCASTArena *arena, ASTRef ref, int64_t *out) {
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    if (n->tag == NODE_INT) {
        *out = n->int_val;
        return true;
    }
    if (n->tag == NODE_TERNARY) {
        int64_t cval, tval, eval;
        if (!zero_cc_ast_eval_const(arena, n->left, &cval)) return false;
        if (!zero_cc_ast_eval_const(arena, n->right, &tval)) return false;
        if (!zero_cc_ast_eval_const(arena, n->aux.alt, &eval)) return false;
        *out = cval != 0 ? tval : eval;
        return true;
    }
    if (n->tag == NODE_BINOP) {
        int64_t lval, rval;
        if (!zero_cc_ast_eval_const(arena, n->left, &lval)) return false;
        if (!zero_cc_ast_eval_const(arena, n->right, &rval)) return false;
        switch (n->aux.op) {
            case TK_PLUS: *out = lval + rval; return true;
            case TK_MINUS: *out = lval - rval; return true;
            case TK_STAR: *out = lval * rval; return true;
            case TK_SLASH: if (rval == 0) return false; *out = lval / rval; return true;
            case TK_PERCENT: if (rval == 0) return false; *out = lval % rval; return true;
            case TK_EQ: *out = (lval == rval); return true;
            case TK_NE: *out = (lval != rval); return true;
            case TK_LT: *out = (lval < rval); return true;
            case TK_GT: *out = (lval > rval); return true;
            case TK_LE: *out = (lval <= rval); return true;
            case TK_GE: *out = (lval >= rval); return true;
            case TK_AMP: *out = (lval & rval); return true;
            case TK_PIPE: *out = (lval | rval); return true;
            case TK_CARET: *out = (lval ^ rval); return true;
            case TK_SHL: *out = (lval << rval); return true;
            case TK_SHR: *out = (lval >> rval); return true;
            case TK_LOGAND: *out = (lval && rval); return true;
            case TK_LOGOR: *out = (lval || rval); return true;
            default: return false;
        }
    }
    if (n->tag == NODE_UNOP) {
        int64_t val;
        if (!zero_cc_ast_eval_const(arena, n->left, &val)) return false;
        switch (n->aux.op) {
            case TK_PLUS: *out = val; return true;
            case TK_MINUS: *out = -val; return true;
            case TK_TILDE: *out = ~val; return true;
            case TK_BANG: *out = !val; return true;
            default: return false;
        }
    }
    return false;
}

/* Recursive declarator parse */
static inline bool zero_cc_parse_declarator_internal(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx,
    ZeroCCDeclarator *d, size_t level)
{
    if (level >= ZERO_CC_DECLARATOR_MAX_LEVELS) {
        diag_report_error_at_token(c, "declarator nesting depth exceeds limit");
        return false;
    }
    if (level >= d->num_levels) {
        d->num_levels = level + 1;
    }
    ZeroCCDeclaratorLevel *lvl = &d->levels[level];

    /* 1. Parse pointer asterisks and qualifiers at this level */
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
        c->token_pos++;
        ZeroCCDeclaratorChunk ptr_chunk;
        memset(&ptr_chunk, 0, sizeof(ptr_chunk));
        ptr_chunk.kind = ZERO_CC_DECL_CHUNK_POINTER;
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT ||
                c->tokens[c->token_pos].type == TK_ATOMIC)) {
            if (c->tokens[c->token_pos].type == TK_CONST) ptr_chunk.is_const = true;
            else if (c->tokens[c->token_pos].type == TK_VOLATILE) ptr_chunk.is_volatile = true;
            else if (c->tokens[c->token_pos].type == TK_RESTRICT) ptr_chunk.is_restrict = true;
            else if (c->tokens[c->token_pos].type == TK_ATOMIC) ptr_chunk.is_atomic = true;
            c->token_pos++;
        }
        /* GNU aligned(N) marker between pointer chunks:
         * `int *__attribute__((aligned(16))) *p;` - alignment applies to
         * the object, not the pointer; consume and discard here (the
         * declaration-level probes already handled the usual positions). */
        {
            size_t probe = 0;
            while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) { /* skip */ }
        }
        if (lvl->prefix_count < ZERO_CC_DECLARATOR_MAX_ITEMS) {
            lvl->prefixes[lvl->prefix_count++] = ptr_chunk;
        }
    }

    /* 2. Direct declarator: parenthesized declarator, identifier, or abstract */
    bool is_paren = false;
    if (d->name == NULL && c->token_pos < c->token_count &&
        c->tokens[c->token_pos].type == TK_LPAREN) {
        if (c->token_pos + 1 < c->token_count) {
            ZeroCCTokType next_tok = c->tokens[c->token_pos + 1].type;
            if (next_tok == TK_STAR || next_tok == TK_LPAREN) {
                is_paren = true;
            } else if (next_tok == TK_IDENT) {
                const char *s = c->tokens[c->token_pos + 1].str_val;
                if (!zero_cc_find_typedef(c, s)) {
                    is_paren = true;
                }
            } else if (next_tok == TK_RPAREN) {
                is_paren = false;
            }
        }
    }

    if (is_paren) {
        c->token_pos++; /* consume '(' */
        if (!zero_cc_parse_declarator_internal(c, arena, ctx, d, level + 1)) {
            return false;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++; /* consume ')' */
        } else {
            diag_report_error_at_token(c, "expected ')' closing parenthesized declarator");
            return false;
        }
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT && d->name == NULL) {
        ZeroCCToken *tok = &c->tokens[c->token_pos];
        d->name = tok->allocated_str ? tok->allocated_str : tok->str_val;
        d->name_tok_pos = c->token_pos;
        d->name_level = level;
        c->token_pos++;
    } else {
        d->is_abstract = true;
    }

    /* 3. Suffixes (arrays and functions) at this level */
    while (c->token_pos < c->token_count) {
        if (c->tokens[c->token_pos].type == TK_LBRACKET) {
            c->token_pos++; /* consume '[' */
            ZeroCCDeclaratorChunk arr_chunk;
            memset(&arr_chunk, 0, sizeof(arr_chunk));
            arr_chunk.kind = ZERO_CC_DECL_CHUNK_ARRAY;
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT ||
                    c->tokens[c->token_pos].type == TK_STATIC)) {
                if (c->tokens[c->token_pos].type == TK_CONST) arr_chunk.is_const = true;
                else if (c->tokens[c->token_pos].type == TK_VOLATILE) arr_chunk.is_volatile = true;
                else if (c->tokens[c->token_pos].type == TK_RESTRICT) arr_chunk.is_restrict = true;
                else if (c->tokens[c->token_pos].type == TK_STATIC) arr_chunk.is_static_array = true;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                c->token_pos++;
                arr_chunk.array_len = 0;
            } else {
                ASTRef dim_expr = parse_expr_ast_internal(c, arena);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                    c->token_pos++;
                } else {
                    diag_report_error_at_token(c, "expected ']' after array dimension");
                    return false;
                }
                int64_t cval = 0;
                if (zero_cc_ast_eval_const(arena, dim_expr, &cval) && cval >= 0) {
                    arr_chunk.array_len = (size_t)cval;
                } else {
                    arr_chunk.is_vla = true;
                    arr_chunk.dim_expr = dim_expr;
                }
            }
            if (lvl->suffix_count < ZERO_CC_DECLARATOR_MAX_ITEMS) {
                lvl->suffixes[lvl->suffix_count++] = arr_chunk;
            }
        } else if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++; /* consume '(' */
            ZeroCCDeclaratorChunk fn_chunk;
            memset(&fn_chunk, 0, sizeof(fn_chunk));
            fn_chunk.kind = ZERO_CC_DECL_CHUNK_FUNCTION;
            zero_cc_parse_param_list(c, arena, ctx, &fn_chunk, d);
            if (lvl->suffix_count < ZERO_CC_DECLARATOR_MAX_ITEMS) {
                lvl->suffixes[lvl->suffix_count++] = fn_chunk;
            }
        } else {
            break;
        }
    }
    return true;
}

/* Flatten the hierarchy into DeclaratorChunk stack[MAX_NESTING]; int top = 0;
 * Order: inner level before outer level; within a level: suffixes forward, prefixes reverse. */
static inline void zero_cc_declarator_flatten(ZeroCCDeclarator *d) {
    d->top = 0;
    for (size_t lvl = d->num_levels; lvl > 0; lvl--) {
        size_t l = lvl - 1;
        /* Suffixes forward */
        for (size_t s = 0; s < d->levels[l].suffix_count; s++) {
            if (d->top < ZERO_CC_DECLARATOR_MAX_NESTING) {
                d->stack[d->top++] = d->levels[l].suffixes[s];
            }
        }
        /* Prefixes reverse (innermost pointer first) */
        for (size_t p = d->levels[l].prefix_count; p > 0; p--) {
            if (d->top < ZERO_CC_DECLARATOR_MAX_NESTING) {
                d->stack[d->top++] = d->levels[l].prefixes[p - 1];
            }
        }
    }
    /* Mark function pointer modifiers */
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_POINTER) {
            if (i + 1 < d->top && d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
                d->stack[i].is_func_ptr = true;
            }
        }
    }
}

/* Parse a full declarator starting from level 0 */
static inline bool zero_cc_parse_declarator(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx, ZeroCCDeclarator *d)
{
    zero_cc_declarator_init(d);
    if (!zero_cc_parse_declarator_internal(c, arena, ctx, d, 0)) {
        return false;
    }
    zero_cc_declarator_flatten(d);
    /* GNU asm label: `int x asm ("y");` - legal after the complete
     * declarator (name + any pointer/array/function suffixes). */
    (void)zero_cc_parse_asm_label(c, d->asm_name, sizeof(d->asm_name));
    return true;
}

/* Check if declarator is a function declaration/definition. The name-level
 * direct declarator must be a function suffix AND the name level must have
 * no pointer prefix: `int f(void)` is a function, but `int *f(void)` and
 * `int (*f)(void)` are pointer-to-function VARIABLES - a name-level `*`
 * turns the function into a pointer-to-function (c-torture 930608-1:
 * `double (* const a[])(double) = {&f};` was mis-parsed as a function). */
static inline bool zero_cc_declarator_is_function(const ZeroCCDeclarator *d) {
    if (!d || d->name == NULL) return false;
    size_t nl = d->name_level;
    if (nl < d->num_levels && d->levels[nl].suffix_count > 0) {
        if (d->levels[nl].suffixes[0].kind != ZERO_CC_DECL_CHUNK_FUNCTION) {
            return false;
        }
        /* Pointer prefixes are allowed when the function chunk is the
         * innermost suffix: `void *x()` / `int *f(void)` - a function
         * returning a pointer (GNU nested functions, c-torture
         * 920415-1).  A function POINTER declarator (`(*f)(void)`) has
         * its function chunk at the OUTER level, so the name level
         * itself has no function suffix there. */
        for (size_t pi = 0; pi < d->levels[nl].prefix_count; pi++) {
            if (d->levels[nl].prefixes[pi].kind != ZERO_CC_DECL_CHUNK_POINTER) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/* Get the primary function chunk corresponding to a function declarator */
static inline const ZeroCCDeclaratorChunk *zero_cc_declarator_get_function_chunk(const ZeroCCDeclarator *d) {
    if (!d || d->name == NULL) return NULL;
    size_t nl = d->name_level;
    if (nl < d->num_levels && d->levels[nl].suffix_count > 0) {
        if (d->levels[nl].suffixes[0].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
            for (int i = 0; i < d->top; i++) {
                if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_FUNCTION &&
                    d->stack[i].param_list == d->levels[nl].suffixes[0].param_list) {
                    return &d->stack[i];
                }
            }
            return &d->levels[nl].suffixes[0];
        }
    }
    return NULL;
}

/* Check if declarator has any array modifier */
static inline bool zero_cc_declarator_has_array(const ZeroCCDeclarator *d) {
    if (!d) return false;
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_ARRAY) return true;
    }
    return false;
}

/* Check if declarator has any VLA modifier */
static inline bool zero_cc_declarator_has_vla(const ZeroCCDeclarator *d) {
    if (!d) return false;
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_ARRAY && d->stack[i].is_vla) return true;
    }
    return false;
}

/* Return the outermost VLA's dimension AST expression */
static inline ASTRef zero_cc_declarator_vla_dim_expr(const ZeroCCDeclarator *d) {
    if (!d) return AST_NULL;
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_ARRAY && d->stack[i].is_vla) {
            return d->stack[i].dim_expr;
        }
    }
    return AST_NULL;
}

/* Return the OUTERMOST array dimension of the declarator: a VLA dim as its
 * expression, or a constant dim as a fresh NODE_INT (in arena). The VLA
 * declaration lowering uses this as the allocation "count" (the inner dims
 * are walked from the type chain). */
static inline bool zero_cc_declarator_outer_dim(const ZeroCCDeclarator *d,
                                                ZeroCCASTArena *arena,
                                                int64_t *const_val, ASTRef *expr) {
    *const_val = 0;
    *expr = AST_NULL;
    if (!d || d->top == 0) return false;
    const ZeroCCDeclaratorChunk *c0 = &d->stack[0];
    if (c0->kind != ZERO_CC_DECL_CHUNK_ARRAY) return false;
    if (c0->is_vla) {
        *expr = c0->dim_expr;
    } else {
        *const_val = (int64_t)c0->array_len;
        if (arena) {
            ASTRef r = zero_cc_ast_alloc_node(arena, NODE_INT);
            ZeroCCASTNode *rn = zero_cc_ast_get(arena, r);
            if (rn) rn->int_val = (int64_t)c0->array_len;
            *expr = r;
        }
    }
    return true;
}

/* Validate declarator against standard C constraints */
static inline bool zero_cc_declarator_validate(ZeroCCCompiler *c, const ZeroCCDeclarator *d) {
    if (!d) return false;
    for (int i = 0; i < d->top; i++) {
        const ZeroCCDeclaratorChunk *chunk = &d->stack[i];
        if (chunk->kind == ZERO_CC_DECL_CHUNK_ARRAY) {
            /* C99 6.7.5.2: array of functions is invalid */
            if (i + 1 < d->top && d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
                diag_report_error_at_token(c, "declaration of array of functions is not allowed");
                return false;
            }
        } else if (chunk->kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
            /* C99 6.7.5.3: function returning function or array is invalid */
            if (i + 1 < d->top) {
                if (d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
                    diag_report_error_at_token(c, "function cannot return a function");
                    return false;
                }
                if (d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_ARRAY) {
                    diag_report_error_at_token(c, "function cannot return an array");
                    return false;
                }
            }
        }
    }
    return true;
}

/* Apply a single DeclaratorChunk modifier to a base/target type */
static inline ZeroCCType zero_cc_chunk_apply(const ZeroCCDeclaratorChunk *chunk, ZeroCCType target) {
    if (chunk->kind == ZERO_CC_DECL_CHUNK_POINTER) {
        ZeroCCType p = zero_cc_type_ptr(&target);
        p.is_const = chunk->is_const;
        p.is_volatile = chunk->is_volatile;
        p.is_restrict = chunk->is_restrict;
        p.is_atomic = chunk->is_atomic;
        /* A pointer whose pointee is (or became) a function type is a
         * function pointer: `int (*fp)(void)` and `func *fp` (where
         * `typedef int func(void);`) both need is_func_ptr set. */
        p.is_func_ptr = chunk->is_func_ptr || target.is_func;
        return p;
    }
    if (chunk->kind == ZERO_CC_DECL_CHUNK_ARRAY) {
        ZeroCCType arr;
        if (chunk->is_vla) {
            arr = zero_cc_type_array(&target, 0);
            arr.is_vla = true;
            arr.vla_dim = (size_t)chunk->dim_expr;
            /* VLA array "size" = the static factor so far (the element
             * size): runtime `sizeof` walks the array chain and divides
             * constant dims out of it (the VLA dim is carried in the
             * runtime slot, not the type). Historically 8 (pointer size),
             * which broke the static-factor division for non-8-byte
             * elements. */
            arr.size = target.size > 0 ? target.size : 1;
            arr.align = 8;
        } else {
            arr = zero_cc_type_array(&target, (ssize_t)chunk->array_len);
        }
        arr.is_const = chunk->is_const;
        arr.is_volatile = chunk->is_volatile;
        return arr;
    }
    if (chunk->kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
        /* A function suffix makes the type a function type.  When this
         * chunk is not the name's own (it is skipped in apply_return),
         * it builds an outer function type, e.g. `void (*f(void))(void)`
         * — a function returning a function pointer: the outer suffix
         * marks is_func and the inner pointer chunk converts it. */
        target.is_func = true;
        /* Record the declared prototype on the function type so a
         * wrapping pointer keeps it in its `base` and the ZeroCCType ->
         * TypeInfo conversion can rebuild the pointee's parameter list.
         * Without this, `int (*f)(double, int)` degrades to
         * pointer->function(int, ...): indirect calls through f marshal
         * arguments against no signature (c-torture execute/930702-1:
         * `f(33, 11)` passed 33 in EDI instead of XMM0). */
        target.fn_has_prototype = chunk->has_prototype;
        target.fn_is_variadic = chunk->is_variadic;
        if (chunk->has_prototype && chunk->param_count > 0 &&
            target.fn_params == NULL) {
            ZeroCCType *params = (ZeroCCType *)malloc(chunk->param_count * sizeof(ZeroCCType));
            if (params) {
                for (size_t p = 0; p < chunk->param_count; p++) {
                    params[p] = chunk->param_tis[p] ?
                        typeinfo_to_zero_cc_type(chunk->param_tis[p]) : zero_cc_type_int();
                }
                target.fn_params = params;
                target.fn_param_count = chunk->param_count;
            }
        }
        return target;
    }
    return target;
}

/* Build the complete variable type:
 * for i = top-1..0: apply DeclaratorChunk[i] to base_type */
static inline ZeroCCType zero_cc_declarator_apply(
    const ZeroCCDeclarator *d, ZeroCCType base_type)
{
    ZeroCCType cur = base_type;
    for (int i = d->top - 1; i >= 0; i--) {
        cur = zero_cc_chunk_apply(&d->stack[i], cur);
    }
    return cur;
}

/* Build the return type for a function declarator (skipping its primary function chunk) */
static inline ZeroCCType zero_cc_declarator_apply_return(
    const ZeroCCDeclarator *d, ZeroCCType base_type)
{
    ZeroCCType cur = base_type;
    const ZeroCCDeclaratorChunk *fn_chunk = zero_cc_declarator_get_function_chunk(d);
    for (int i = d->top - 1; i >= 0; i--) {
        if (&d->stack[i] == fn_chunk) continue;
        cur = zero_cc_chunk_apply(&d->stack[i], cur);
    }
    return cur;
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
    /* C89 6.4.2: only `register` may prefix a parameter declaration;
     * `static`/`auto`/`extern` are errors ("storage class specified for
     * parameter 'x'" - c-torture probe vs gcc -std=gnu89). */
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_REGISTER) {
        c->token_pos++;
    } else if (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_STATIC ||
                c->tokens[c->token_pos].type == TK_AUTO ||
                c->tokens[c->token_pos].type == TK_EXTERN)) {
        return -1;
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
    /* Pointer stars are consumed PER-DECLARATOR in the loop below
     * (`int *a, b;` - each declarator owns its own `*` run).  Applying a
     * base `*` run here as well would over-count: `int *o1, *o2;` would
     * type o1 as int* but o2 as int** (Sprint 70, c-torture 961004-1: the
     * K&R params came out with mismatched types and every cross
     * assignment REJECTed as incompatible). */
    pt.is_const = p_is_const;
    pt.is_volatile = p_is_volatile;
    if (vec_size > 0 && !zero_cc_type_is_vector(&pt)) pt = zero_cc_type_vector(&pt, vec_size);

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
        /* Sprint 72: function-pointer K&R parameter (`void * (*chunkfun)
         * ();` - c-torture compile/cp.c): the declarator is
         * `(*name)(params)`; the parameter's type is a pointer to a
         * function whose return type is the base + this declarator's
         * star run. */
        bool d_func_ptr = false;
        int d_inner_stars = 0;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN &&
            c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos + 1].type == TK_STAR) {
            c->token_pos++; /* ( */
            /* All stars inside the group: `(*f)()`, `(**f)()` (pointer to
             * pointer to function - c-torture compile/callind). */
            while (c->token_pos < c->token_count &&
                   c->tokens[c->token_pos].type == TK_STAR) {
                d_inner_stars++;
                c->token_pos++;
            }
            d_func_ptr = true;
            /* Qualifier between `*` and the name: `(*const f)(...)` / `(*const f)()`
             * (const function-pointer parameter, c-torture 20040909-1). */
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE)) {
                if (c->tokens[c->token_pos].type == TK_CONST) d_const = true;
                if (c->tokens[c->token_pos].type == TK_VOLATILE) d_volatile = true;
                c->token_pos++;
            }
        }
        if (c->token_pos >= c->token_count || c->tokens[c->token_pos].type != TK_IDENT) return -1;
        const char *dn = c->tokens[c->token_pos].str_val;
        c->token_pos++;
        bool is_array = false;
        if (d_func_ptr) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            /* Skip the parameter list `( ... )` (depth-counted) */
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                int pdepth = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && pdepth > 0) {
                    if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                    else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                    c->token_pos++;
                }
            }
        } else {
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                is_array = true;
                c->token_pos++;
                while (c->token_pos < c->token_count && c->tokens[c->token_pos].type != TK_RBRACKET) c->token_pos++;
                if (c->token_pos < c->token_count) c->token_pos++; /* skip ']' */
            }
        }
        if (count >= max) return -1;
        ZeroCCType dt = pt;
        dt.is_const = d_const;
        dt.is_volatile = d_volatile;
        for (int dp = 0; dp < d_ptr; dp++) dt = zero_cc_type_ptr(&dt);
        if (d_func_ptr) {
            /* `ret * (*name) ()` - pointer to function returning `ret *`.
             * Extra stars in the group (`(**name) ()`) wrap the function
             * pointer in more pointer levels. */
            ZeroCCType fpt = zero_cc_type_ptr(&dt);
            fpt.is_func_ptr = true;
            dt = fpt;
            for (int isp = 1; isp < d_inner_stars; isp++) {
                dt = zero_cc_type_ptr(&dt);
            }
        } else if (is_array && dt.tag != TYPE_PTR && !dt.is_func_ptr) {
            dt = zero_cc_type_ptr(&dt);
        } else if (dt.tag == TYPE_ARRAY) {
            /* C99 6.7.6.3p7: array parameter (via typedef/typeof) decays to pointer */
            ZeroCCType elem = dt.base ? *dt.base : zero_cc_type_int64();
            dt = zero_cc_type_ptr(&elem);
            dt.is_const = d_const;
            dt.is_volatile = d_volatile;
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


#endif /* ZERO_CC_PARSER_ZERO_CC_DECLARATOR_STACK_H */
