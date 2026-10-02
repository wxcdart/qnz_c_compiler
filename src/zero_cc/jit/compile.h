#ifndef ZERO_CC_JIT_COMPILE_H
#define ZERO_CC_JIT_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "zero_cc/core.h"
#include "zero_cc/types_lexer.h"
#include "zero_cc/semantic/decl.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/semantic/scope.h"
#include "zero_cc/semantic.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/parser/ir_emitter.h"

static inline void zero_cc_compile_function(ZeroCCCompiler *c, SemContext *ctx, Arena *perm) {
    /* Skip static / inline / const / _Noreturn */
    bool fn_noreturn = false;
    while (c->tokens[c->token_pos].type == TK_STATIC || c->tokens[c->token_pos].type == TK_INLINE ||
           c->tokens[c->token_pos].type == TK_NORETURN ||
           (c->tokens[c->token_pos].type == TK_IDENT && strcmp(c->tokens[c->token_pos].str_val, "const") == 0)) {
        if (c->tokens[c->token_pos].type == TK_NORETURN) fn_noreturn = true;
        c->token_pos++;
    }

    /* Return type */
    ZeroCCType fn_ret_type = zero_cc_type_int();
    if (c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION) {
        c->token_pos++;
        if (c->tokens[c->token_pos].type == TK_IDENT) {
            ZeroCCType *st = zero_cc_find_struct(c, c->tokens[c->token_pos].str_val);
            if (!st) st = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
            if (st) fn_ret_type = *st;
            c->token_pos++;
        }
    } else if (c->tokens[c->token_pos].type == TK_IDENT) {
        ZeroCCType *tdef = zero_cc_find_typedef(c, c->tokens[c->token_pos].str_val);
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
        } else if (c->tokens[c->token_pos].type == TK_IDENT || c->tokens[c->token_pos].type == TK_INT ||
            c->tokens[c->token_pos].type == TK_CHAR || c->tokens[c->token_pos].type == TK_VOID ||
            c->tokens[c->token_pos].type == TK_STRUCT || c->tokens[c->token_pos].type == TK_UNION ||
            c->tokens[c->token_pos].type == TK_FLOAT_KW ||
            c->tokens[c->token_pos].type == TK_DOUBLE_KW || c->tokens[c->token_pos].type == TK_BOOL_KW ||
            c->tokens[c->token_pos].type == TK_SIGNED || c->tokens[c->token_pos].type == TK_UNSIGNED ||
            c->tokens[c->token_pos].type == TK_SHORT || c->tokens[c->token_pos].type == TK_LONG) {
            ZeroCCTokType ptt = c->tokens[c->token_pos].type;
            const char *ptname = c->tokens[c->token_pos].str_val;
            ZeroCCType *pfound = zero_cc_find_typedef(c, ptname);
            if (!pfound) pfound = zero_cc_find_struct(c, ptname);
            c->token_pos++;
            if ((ptt == TK_STRUCT || ptt == TK_UNION) && c->tokens[c->token_pos].type == TK_IDENT) {
                ptname = c->tokens[c->token_pos].str_val;
                pfound = zero_cc_find_struct(c, ptname);
                if (!pfound) pfound = zero_cc_find_typedef(c, ptname);
                c->token_pos++;
            }
            bool is_ptr = false;
            while (c->tokens[c->token_pos].type == TK_STAR) { is_ptr = true; c->token_pos++; }
            if (c->tokens[c->token_pos].type == TK_IDENT) {
                if (param_count < 8) {
                    snprintf(param_names[param_count], sizeof(param_names[param_count]), "%s", c->tokens[c->token_pos].str_val);
                    if (pfound) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(pfound) : *pfound;
                        snprintf(param_types[param_count].struct_name, sizeof(param_types[param_count].struct_name), "%s", ptname);
                    } else if (ptt == TK_FLOAT_KW) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_float();
                    } else if (ptt == TK_DOUBLE_KW) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_double();
                    } else if (ptt == TK_CHAR || ptt == TK_BOOL_KW) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_char();
                    } else if (ptt == TK_SHORT) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_int16();
                    } else if (ptt == TK_INT || ptt == TK_SIGNED || ptt == TK_UNSIGNED) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_int();
                    } else if (ptt == TK_LONG) {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_int64();
                    } else {
                        param_types[param_count] = is_ptr ? zero_cc_type_ptr(NULL) : zero_cc_type_int64();
                    }
                    param_count++;
                }
                c->token_pos++;
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
        /* Copy parameter types and names to function struct */
        for (size_t i = 0; i < param_count && i < 8; i++) {
            fn->param_types[i] = param_types[i];
            snprintf(fn->param_names[i], sizeof(fn->param_names[i]), "%s", param_names[i]);
        }

        c->local_count = 0;
        c->current_stack_frame = 16;

        /* Reset IR buffer for this function */
        ir_reset();

        /* Function Prologue */
        zero_cc_emit_u8(&c->text_section, 0x55);          /* push rbp */
        zero_cc_emit_u8(&c->text_section, 0x48);          /* mov rbp, rsp */
        zero_cc_emit_u8(&c->text_section, 0x89);
        zero_cc_emit_u8(&c->text_section, 0xE5);
        zero_cc_emit_u8(&c->text_section, 0x48);          /* sub rsp, 256 */
        zero_cc_emit_u8(&c->text_section, 0x81);
        zero_cc_emit_u8(&c->text_section, 0xEC);
        zero_cc_emit_u32(&c->text_section, 256);

        /* Save parameter registers to stack */
        /* Integer arg regs: rdi(7), rsi(6), rdx(2), rcx(1), r8(8), r9(9) */
        /* Float arg regs: xmm0-xmm7 */
        static const uint8_t save_int_regs[6][3] = {
            {0x48, 0x89, 0xBD}, /* mov [rbp - offset], rdi */
            {0x48, 0x89, 0xB5}, /* mov [rbp - offset], rsi */
            {0x48, 0x89, 0x95}, /* mov [rbp - offset], rdx */
            {0x48, 0x89, 0x8D}, /* mov [rbp - offset], rcx */
            {0x4C, 0x89, 0x85}, /* mov [rbp - offset], r8  */
            {0x4C, 0x89, 0x8D}  /* mov [rbp - offset], r9  */
        };

        /* Track which xmm register to use for float params (independent of int regs) */
        int xmm_reg_idx = 0;
        /* Track which integer register to use for int params (independent of float regs) */
        int int_reg_idx = 0;

        for (size_t p = 0; p < param_count; p++) {
            ZeroCCSym *sym = zero_cc_add_local(c, param_names[p], param_types[p]);
            sym->is_param = true;
            sym->param_reg_idx = (int)p;
            bool is_fp = (param_types[p].tag == TYPE_FLOAT || param_types[p].tag == TYPE_DOUBLE);
            
            if (is_fp && xmm_reg_idx < 8) {
                /* Save from xmm register: movsd [rbp - offset], xmmN */
                zero_cc_emit_u8(&c->text_section, 0xF2);
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0x11);
                zero_cc_emit_u8(&c->text_section, 0x85 | ((xmm_reg_idx & 7) << 3));
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                xmm_reg_idx++;
            } else if (!is_fp && int_reg_idx < 6) {
                /* Save from integer register (based on int param order, not overall param index) */
                zero_cc_emit_u8(&c->text_section, save_int_regs[int_reg_idx][0]);
                zero_cc_emit_u8(&c->text_section, save_int_regs[int_reg_idx][1]);
                zero_cc_emit_u8(&c->text_section, save_int_regs[int_reg_idx][2]);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                int_reg_idx++;
            }
        }

        /* Variadic spill area: spill remaining integer registers for va_start */
        if (is_variadic_fn) {
            for (size_t p = param_count; p < 6; p++) {
                c->current_stack_frame += 8;
                int32_t spill_offset = c->current_stack_frame;
                zero_cc_emit_u8(&c->text_section, save_int_regs[p][0]);
                zero_cc_emit_u8(&c->text_section, save_int_regs[p][1]);
                zero_cc_emit_u8(&c->text_section, save_int_regs[p][2]);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-spill_offset));
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
            /* Reset register allocator for this function */
            regalloc_init(&c->regalloc);
            c->next_temp_id = 0;
            c->ir_next_label = 0;
            
            /* Reset IR buffer for this function */
            ir_reset();
            
            /* Pass 1: Lower AST to IR (instruction selection) */
            zero_cc_lower_ast(c, &arena, body);
            
            /* Pass 2: Compute live ranges and allocate registers */
            /* TODO: Enable when register allocation is fully integrated */
            /* LiveRangeAnalyzer lra;
            live_range_init(&lra);
            live_range_scan_ir(&lra, ir_get_buffer(), ir_get_count());
            live_range_compute(&lra, &c->regalloc);
            regalloc_allocate(&c->regalloc);
            live_range_free(&lra); */
            
            /* Pass 3: Generate machine code from IR using allocated registers */
            /* (Machine code emission will be implemented in future) */
        } else {
            if (ctx->diags) semantic_print_diagnostics(ctx->diags, stderr);
            c->had_error = true;
        }

        // Restore scope and return type after analysis
        scope_leave(&ctx->current_scope);
        ctx->current_scope = global_scope_root;
        ctx->current_fn_ret_type = NULL;
// Consume closing brace if present

        if (c->tokens[c->token_pos].type == TK_RBRACE) c->token_pos++;
    }
}


#endif /* ZERO_CC_JIT_COMPILE_H */
