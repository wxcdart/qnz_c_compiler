#ifndef ZERO_CC_JIT_COMPILE_H
#define ZERO_CC_JIT_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/core/core.h"
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic/semantic.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/ir/regalloc.h"

static inline void zero_cc_compile_function(ZeroCCCompiler *c, SemContext *ctx, Arena *perm) {
    /* Skip static / inline / const / _Noreturn */
    bool fn_noreturn = false;
    bool fn_inline = false;
    bool fn_static = false;
    while (c->tokens[c->token_pos].type == TK_STATIC || c->tokens[c->token_pos].type == TK_INLINE ||
           c->tokens[c->token_pos].type == TK_NORETURN || c->tokens[c->token_pos].type == TK_CONST ||
           c->tokens[c->token_pos].type == TK_VOLATILE ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0)) {
        if (c->tokens[c->token_pos].type == TK_NORETURN) fn_noreturn = true;
        if (c->tokens[c->token_pos].type == TK_INLINE) fn_inline = true;
        if (c->tokens[c->token_pos].type == TK_STATIC) fn_static = true;
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
    if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
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
        if (tdef) fn_ret_type = *tdef;
        c->token_pos++;
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

    if (c->tokens[c->token_pos].type != TK_IDENT) return;
    char fn_name[128];
    snprintf(fn_name, sizeof(fn_name), "%s", c->tokens[c->token_pos].str_val);
    c->token_pos++;

    if (c->tokens[c->token_pos].type != TK_LPAREN) return;
    c->token_pos++;

    /* Parse Parameters in signature */
    ZeroCCType param_types[8];
    char param_names[8][128];
    size_t param_count = 0;
    bool is_variadic_fn = false;

    while (c->tokens[c->token_pos].type != TK_RPAREN && c->tokens[c->token_pos].type != TK_EOF) {
        size_t ppos = c->token_pos;
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
            if (c->tokens[c->token_pos].type == TK_IDENT || c->tokens[c->token_pos].type == TK_INT ||
                c->tokens[c->token_pos].type == TK_CHAR || c->tokens[c->token_pos].type == TK_VOID ||
                c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION ||
                c->tokens[c->token_pos].type == TK_ENUM ||
                c->tokens[c->token_pos].type == TK_FLOAT_KW ||
                c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
                c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
                c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
                ZeroCCTokType ptt = c->tokens[c->token_pos].type;
                const char *ptname = c->tokens[c->token_pos].str_val;
                ZeroCCType *pfound = zero_cc_find_typedef(c, ptname);
                if (!pfound) pfound = zero_cc_find_struct(c, ptname);
                if (!pfound) pfound = zero_cc_find_enum(c, ptname);
                c->token_pos++;
                /* signed/unsigned qualifier (C11 6.7.5): consume and remember */
                bool p_is_unsigned = (ptt == TK_UNSIGNED);
                if (ptt == TK_SIGNED || ptt == TK_UNSIGNED) {
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
                if (c->tokens[c->token_pos].type == TK_IDENT) {
                    if (param_count < 8) {
                        snprintf(param_names[param_count], sizeof(param_names[param_count]), "%s", c->tokens[c->token_pos].str_val);
                        ZeroCCType pt;
                        if (pfound) {
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
                        param_types[param_count] = pt;
                        param_count++;
                    }
                    c->token_pos++;
                }
            }
        }
        if (c->tokens[c->token_pos].type == TK_COMMA) c->token_pos++;
        if (c->token_pos == ppos) c->token_pos++;
    }
    if (c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;

    if (c->tokens[c->token_pos].type == TK_SEMI) {
        /* Function prototype declaration: record prototype, no code emit */
        if (c->function_count < ZERO_CC_MAX_FUNCTIONS) {
            ZeroCCFunction *fn = &c->functions[c->function_count++];
            snprintf(fn->name, sizeof(fn->name), "%s", fn_name);
            fn->text_offset = 0;
            fn->param_count = param_count;
            fn->return_type = fn_ret_type;
            fn->returns_struct = (fn_ret_type.tag == TYPE_STRUCT || fn_ret_type.tag == TYPE_UNION);
            fn->ret_struct_sz = fn_ret_type.size;
            fn->is_variadic = is_variadic_fn;
            fn->noreturn = fn_noreturn;
            fn->is_prototype = true;
            fn->is_inline = fn_inline;
            fn->is_static = fn_static;
            for (size_t i = 0; i < param_count && i < 8; i++) {
                fn->param_types[i] = param_types[i];
                snprintf(fn->param_names[i], sizeof(fn->param_names[i]), "%s", param_names[i]);
            }
        }
        c->token_pos++;
        return;
    }

    if (c->tokens[c->token_pos].type == TK_LBRACE) {
        if (c->function_count >= ZERO_CC_MAX_FUNCTIONS) return;
        ZeroCCFunction *fn = &c->functions[c->function_count++];
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
        fn->is_inline = fn_inline;
        fn->is_static = fn_static;
        /* Copy parameter types and names to function struct */
        for (size_t i = 0; i < param_count && i < 8; i++) {
            fn->param_types[i] = param_types[i];
            snprintf(fn->param_names[i], sizeof(fn->param_names[i]), "%s", param_names[i]);
        }

        c->local_count = 0;
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

        /* Variadic spill area: reserve stack frame for remaining integer registers */
        if (is_variadic_fn) {
            for (size_t p = param_count; p < 6; p++) {
                c->current_stack_frame += 8;
            }
        }

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
            TypeInfo *ret_t = zero_cc_type_to_typeinfo(perm, f->return_type);
            if (f->is_external) {
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
            TypeInfo *fn_t = type_function(perm, ret_t, params, f->param_count, f->is_variadic);
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
        } else {
            if (ctx->diags) semantic_print_diagnostics(ctx->diags, stderr);
            /* Error already reported via semantic_print_diagnostics */
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
