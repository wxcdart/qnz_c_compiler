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
    if (c->tokens[c->token_pos].type == TK_TYPEOF || c->tokens[c->token_pos].type == TK_TYPEOF_UNQUAL) {
        fn_ret_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
    } else if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCType *st = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
            if (!st) st = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
            if (st) fn_ret_type = *st;
            c->token_pos++;
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
        fn_ret_type = zero_cc_type_int64();
        c->token_pos++; }
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
    bool is_variadic_fn = false;
    bool has_prototype = false; /* set once the param list holds at least one entry */
    c->local_count = 0;
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

        while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
        size_t ppos = c->token_pos;
        has_prototype = true; /* any entry (param or ...) makes this a prototype */
        if (c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            is_variadic_fn = true;
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
            ZeroCCType typeof_pt;
            bool is_typeof_param = false;
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
                /* signed/unsigned qualifier (C11 6.7.5): consume and remember */
                bool p_is_unsigned = (!is_typeof_param && ptt == TK_UNSIGNED);
                if (!is_typeof_param && (ptt == TK_SIGNED || ptt == TK_UNSIGNED)) {
                    if (c->token_pos < c->token_count &&
                        (c->tokens[c->token_pos].type == TK_INT ||
                         c->tokens[c->token_pos].type == TK_CHAR ||
                         c->tokens[c->token_pos].type == TK_SHORT ||
                         c->tokens[c->token_pos].type == TK_LONG)) {
                        ptt = c->tokens[c->token_pos].type;
                        c->token_pos++;
                        if (ptt == TK_LONG && c->token_pos < c->token_count &&
                            c->tokens[c->token_pos].type == TK_LONG) c->token_pos++; /* long long */
                    } else {
                        ptt = TK_INT;
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
                if (c->tokens[c->token_pos].type == TK_LPAREN &&
                    c->token_pos + 1 < c->token_count &&
                    c->tokens[c->token_pos + 1].type == TK_STAR) {
                    /* Function-pointer parameter: `ret_type (*name)(params...)` */
                    c->token_pos++; /* ( */
                    while (c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
                    if (c->tokens[c->token_pos].type == TK_IDENT) {
                        if (param_count >= ZERO_CC_MAX_PARAMS) {
                            diag_report_error_at_token(c,
                                "function '%s' has too many parameters (max %d)",
                                fn_name, ZERO_CC_MAX_PARAMS);
                        } else {
                            zero_cc_ensure_param_cap(ctx, &param_types, &param_names, &param_cap, param_count + 1);
                            param_names[param_count] = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
                            /* ABI: an 8-byte pointer in a register; the pointed-to
                             * signature is not modeled (calls go through the
                             * value). Marked func-ptr for later codegen hooks. */
                            ZeroCCType pt = zero_cc_type_int64();
                            ZeroCCType fpt = zero_cc_type_ptr(&pt);
                            fpt.is_func_ptr = true;
                            param_types[param_count] = fpt;
                            zero_cc_add_local(c, param_names[param_count], fpt);
                            param_count++;
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
                /* Parameter type - shared by named and unnamed parameters
                 * (C99 6.7.6.3): an unnamed parameter still constrains
                 * the caller. Previously unnamed params were silently
                 * dropped, so `int f(int);` parsed as a 0-parameter
                 * prototype and legal calls were rejected. */
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
                for (int pd = 0; pd < ptr_depth; pd++) {
                    pt = zero_cc_type_ptr(&pt);
                    if (pd == ptr_depth - 1) {
                        pt.is_const = p_ptr_is_const;
                        pt.is_volatile = p_ptr_is_volatile;
                        pt.is_restrict = p_ptr_is_restrict;
                    }
                }
                /* `(void)` is an empty parameter list (C99 6.7.6.3p2): it
                 * makes the declaration a prototype but adds no parameter. */
                bool is_void_param = (ptt == TK_VOID && !is_typeof_param &&
                                      ptr_depth == 0);
                if (!is_void_param && c->tokens[c->token_pos].type == TK_IDENT) {
                    if (param_count >= ZERO_CC_MAX_PARAMS) {
                        diag_report_error_at_token(c,
                            "function '%s' has too many parameters (max %d)",
                            fn_name, ZERO_CC_MAX_PARAMS);
                    } else {
                        zero_cc_ensure_param_cap(ctx, &param_types, &param_names, &param_cap, param_count + 1);
                        param_names[param_count] = zero_cc_intern_string(c, c->tokens[c->token_pos].str_val);
                        param_types[param_count] = pt;
                        zero_cc_add_local(c, param_names[param_count], pt);
                        param_count++;
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
                    if (param_count > 0 && c->token_pos < c->token_count &&
                        c->tokens[c->token_pos].type == TK_LBRACKET) {
                        ssize_t dims[8];
                        int ndims = 0;
                        while (c->token_pos < c->token_count &&
                               c->tokens[c->token_pos].type == TK_LBRACKET &&
                               ndims < 8) {
                            ssize_t d = 0;
                            c->token_pos++; /* '[' */
                            if (c->token_pos < c->token_count &&
                                c->tokens[c->token_pos].type == TK_NUM) {
                                d = (ssize_t)c->tokens[c->token_pos].num_val;
                                c->token_pos++;
                            }
                            if (c->token_pos < c->token_count &&
                                c->tokens[c->token_pos].type == TK_RBRACKET) {
                                c->token_pos++; /* ']' */
                            }
                            dims[ndims++] = d;
                        }
                        ZeroCCType t = param_types[param_count - 1];
                        /* Later dimension tokens are inner: build the
                         * pointed-to array type innermost-first. */
                        for (int i = ndims - 1; i >= 1; i--) {
                            t = zero_cc_type_array(&t, dims[i]);
                        }
                        param_types[param_count - 1] = zero_cc_type_ptr(&t);
                    }
                } else if (!is_void_param && param_count < ZERO_CC_MAX_PARAMS &&
                           c->token_pos < c->token_count &&
                           (c->tokens[c->token_pos].type == TK_COMMA ||
                            c->tokens[c->token_pos].type == TK_RPAREN)) {
                    /* Unnamed parameter: record the type with an empty
                     * name so the prototype keeps its parameter count.
                     * No add_local (nothing to bind); the degenerate
                     * `f(int [])` unnamed-array form skips the array
                     * adjustment above. */
                    zero_cc_ensure_param_cap(ctx, &param_types, &param_names, &param_cap, param_count + 1);
                    param_names[param_count] = "";
                    param_types[param_count] = pt;
                    param_count++;
                } else if (!is_void_param && param_count >= ZERO_CC_MAX_PARAMS &&
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
    } else {
        return;
    }

    if (c->tokens[c->token_pos].type == TK_SEMI) {
        /* Function prototype declaration: record prototype, no code emit */
        if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
            ZeroCCFunction *fn = &c->functions[c->function_count++];
            memset(fn, 0, sizeof(*fn));
            snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
            fn->text_offset = 0;
            fn->param_count = param_count;
            fn->return_type = fn_ret_type;
            fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT || fn_ret_type.tag == TYPE_UNION);
            fn->ret_struct_sz = fn_ret_type.size;
            fn->is_variadic = is_variadic_fn;
            fn->noreturn = fn_noreturn;
            fn->is_prototype = true;
            fn->has_prototype = has_prototype;
            fn->is_inline = fn_inline;
            fn->is_static = fn_static;
            if (param_count > 0) {
                Arena *farena = zero_cc_get_fn_arena(c);
                fn->param_names = (const char **)arena_alloc(farena, param_count * sizeof(const char *));
                fn->param_types = (ZeroCCType *)arena_alloc(farena, param_count * sizeof(ZeroCCType));
                for (size_t i = 0; i < param_count; i++) {
                    fn->param_types[i] = param_types[i];
                    fn->param_names[i] = param_names[i];
                }
            }
        }
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
        fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT || fn_ret_type.tag == TYPE_UNION);
        fn->ret_struct_sz = fn_ret_type.size;
        fn->is_variadic = is_variadic_fn;
        fn->noreturn = fn_noreturn;
        fn->is_prototype = false;
        fn->has_prototype = has_prototype;
        fn->is_inline = fn_inline;
        fn->is_static = fn_static;
        /* Copy parameter types and names to function struct */
        if (param_count > 0) {
            Arena *farena = zero_cc_get_fn_arena(c);
            fn->param_names = (const char **)arena_alloc(farena, param_count * sizeof(const char *));
            fn->param_types = (ZeroCCType *)arena_alloc(farena, param_count * sizeof(ZeroCCType));
            for (size_t i = 0; i < param_count; i++) {
                fn->param_types[i] = param_types[i];
                fn->param_names[i] = param_names[i];
            }
        }

        c->local_count = 0;
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
                 * `extern struct A a;`) register a function-table entry with
                 * the variable's type in return_type; they must appear as
                 * variables.  Extern *functions* (incl. __builtin_* routines
                 * registered during lowering, e.g. memcpy/abort) fall through
                 * to the function registration below: a later function
                 * calling the same name must see a callable type
                 * (registering the return type as SYMBOL_VAR made
                 * `__builtin_popcount` an "int variable" and broke its second
                 * use - c-torture compile/fold-popcount-1.c). */
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
             * are executed inside zero_cc_lower_ast(). */

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
