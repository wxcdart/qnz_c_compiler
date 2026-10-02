/* AST Lowering - AST to Machine Code (with IR Emitter State Management) */
#ifndef ZERO_CC_PARSER_AST_LOWERING_H
#define ZERO_CC_PARSER_AST_LOWERING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../parser.h"
#include "ir_emitter.h"
#include "ir_emit_ops.h"
#include "machine_emitter.h"
#include "../isa_backend.h"
#include "../types/vregs.h"
#include "../types/global_vars.h"
#include "../semantic/type.h"
#include "../utils/arena.h"
#include "../abi_classify.h"

/* Helper: Get current function's return type (last function added to compiler) */
static inline ZeroCCType zero_cc_get_current_fn_ret_type(ZeroCCCompiler *c) {
    if (c->function_count > 0) {
        return c->functions[c->function_count - 1].return_type;
    }
    return zero_cc_type_int();
}

/* IR Emitter Context for state management (replaces static globals) */
static IREmitterContext g_ir_emitter_ctx;

/* Virtual registers are defined in ../types/vregs.h
 * They use the simple index-based approach where each ISA backend
 * interprets the numbers as its native register encodings.
 * 
 * Usage:
 *   VREG_0  - result / return value
 *   VREG_1-7 - argument passing / temporaries
 *   VREG_8-15 - general temporaries
 *   VREG_F0 - float result
 *   VREG_F1-7 - float temporaries
 *   BE(c)->fp_reg - frame pointer (ISA-specific)
 *   BE(c)->sp_reg - stack pointer (ISA-specific)
 */

/* =========================================================================
 * Forward Declarations (matching ast_dump.h)
 * ========================================================================= */

static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
static int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
static void emit_ir_to_machine_code(ZeroCCCompiler *c);

/* =========================================================================
 * Statement Handlers
 * ========================================================================= */

static void lower_stmt_block(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    ASTRef stmt = ref;
    while (stmt != AST_NULL) {
        lower_node(c, arena, stmt);
        ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
        stmt = snode ? snode->next : AST_NULL;
    }
}

static void lower_stmt_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        (void)lower_expr(c, arena, node->left);  /* Result not used */
    }
}

static void lower_stmt_decl(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->flags & NODE_DECL_IS_VLA) {
        /* Variable Length Array (C99 VLA) */
        const char *var = node->s.str;
        ZeroCCSym *sym = zero_cc_find_local(c, var);
        if (sym && node->right != AST_NULL) {
            /* 1. Evaluate dimension expression */
            int count_temp = lower_expr(c, arena, node->right);

            /* 2. Multiply count by element size */
            size_t elem_size = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 4;
            int size_temp;
            if (elem_size == 1) {
                size_temp = count_temp;
            } else {
                int sz_imm = c->next_temp_id++;
                ir_emit_mov_imm(sz_imm, (int32_t)elem_size);
                size_temp = c->next_temp_id++;
                ir_emit_mul(size_temp, count_temp, sz_imm);
            }

            /* 3. Align size to 16 bytes: (size + 15) & ~15 */
            int add15 = c->next_temp_id++;
            ir_emit_mov_imm(add15, 15);
            int size_p15 = c->next_temp_id++;
            ir_emit_add(size_p15, size_temp, add15);
            int mask16 = c->next_temp_id++;
            ir_emit_mov_imm(mask16, -16);
            int aligned_size = c->next_temp_id++;
            ir_emit_and(aligned_size, size_p15, mask16);

            /* 4. Allocate dynamically on stack via sub rsp, reg */
            int vla_ptr = c->next_temp_id++;
            ir_emit_vla_alloc(vla_ptr, aligned_size);

            /* 5. Store the buffer pointer in the local variable's stack slot */
            ir_emit_store(vla_ptr, BE(c)->fp_reg, -sym->stack_offset);
        }
        return;
    }
    if (node->left != AST_NULL) {
        int result_temp = lower_expr(c, arena, node->left);
        const char *var = node->s.str;
        ZeroCCSym *sym = zero_cc_find_local(c, var);
        if (sym) {
            bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            if (target_is_float) {
                if (!c->last_expr_is_float) {
                    /* Convert int (in rax / result_temp) to float in xmm0 */
                    if (BE(c)->emit_fp_cvtsf) {
                        BE(c)->emit_fp_cvtsf(c, 0, 0); /* xmm0, rax */
                    }
                }
                /* Float assignment: store xmm0 to [rbp - offset] */
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, 0, BE(c)->fp_reg, -sym->stack_offset);
                }
            } else {
                if (c->last_expr_is_float) {
                    /* Convert float (in xmm0) to int (in eax) */
                    if (BE(c)->emit_fp_cvtsi) {
                        BE(c)->emit_fp_cvtsi(c, 0, 0); /* eax, xmm0 */
                    }
                    /* mov [rbp - offset], eax */
                    BE(c)->emit_store(c, 0, BE(c)->fp_reg, -sym->stack_offset);
                } else {
                    /* Integer or struct assignment: use IR store */
                    ir_emit_store(result_temp, BE(c)->fp_reg, -sym->stack_offset);
                    if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16) {
                        /* System V AMD64 ABI: second eightbyte returned in rdx (physical register 2) */
                        ir_emit_store(2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                    }
                }
            }
            c->last_expr_is_float = 0;
        }
    }
}

static void lower_stmt_static_assert(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        lower_expr(c, arena, node->left);
    }
}

/* Synthetic epilogue label used by return statements */
static uint16_t g_ir_epilogue_label = 0;

static void lower_stmt_return(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        ZeroCCASTNode *ret_expr = zero_cc_ast_get(arena, node->left);
        ZeroCCSym *sym = (ret_expr && ret_expr->tag == NODE_IDENT) ? zero_cc_find_local(c, ret_expr->s.str) : NULL;
        if (sym && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16) {
            /* System V AMD64 ABI: 2-eightbyte struct returned across rax and rdx */
            int32_t struct_offset = -sym->stack_offset;
            
            // 1. Load lower 8 bytes into rax (VREG_0) via IR
            ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);
            
            // 2. Load upper 8 bytes into rdx (physical register 2) via IR
            ir_emit_load(2, BE(c)->fp_reg, struct_offset + 8);
            
            // 3. Jump to epilogue (which will emit leave; ret)
            ir_emit_jmp(g_ir_epilogue_label);
            return;
        }

        int result_temp = lower_expr(c, arena, node->left);
        /* Ensure result is in rax (VREG_0) for return */
        if (!c->last_expr_is_float) {
            /* Move result to rax if not already there - emit IR */
            if (result_temp != VREG_0) {
                ir_emit_mov_reg(VREG_0, result_temp);
            }
            /* If function returns float/double, convert int to float */
            ZeroCCType fn_ret_type = zero_cc_get_current_fn_ret_type(c);
            if (fn_ret_type.tag == TYPE_FLOAT || fn_ret_type.tag == TYPE_DOUBLE) {
                int xmm0_temp = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint8_t)xmm0_temp, (uint8_t)result_temp);
                c->last_expr_is_float = 1;
            }
        }
        /* Float result is already in xmm0, no move needed */
    }
    ir_emit_jmp(g_ir_epilogue_label);
}

/* Emit return jumps after IR-to-MC */
static void emit_return_jumps(ZeroCCCompiler *c) {
    (void)c;
}

static void lower_stmt_if(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    int cond_temp = lower_expr(c, arena, node->left);
    uint16_t else_label = c->ir_next_label++;
    uint16_t end_label = c->ir_next_label++;
    ir_emit_je((uint8_t)cond_temp, else_label);
    lower_node(c, arena, node->right);
    ir_emit_jmp(end_label);
    ir_emit_label(else_label);
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(end_label);
}

static void lower_stmt_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    ir_emit_label(loop_start);
    int cond_temp = lower_expr(c, arena, node->left);
    ir_emit_je((uint8_t)cond_temp, loop_exit);
    lower_node(c, arena, node->right);
    ir_emit_jmp(loop_start);
    ir_emit_label(loop_exit);
}

static void lower_stmt_do_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    ir_emit_label(loop_start);
    lower_node(c, arena, node->right);
    int cond_temp = lower_expr(c, arena, node->left);
    ir_emit_jne((uint8_t)cond_temp, loop_start);
    ir_emit_label(loop_exit);
}

static void lower_stmt_for(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    ir_emit_label(loop_start);
    if (node->right != AST_NULL) {
        int cond_temp = lower_expr(c, arena, node->right);
        ir_emit_je((uint8_t)cond_temp, loop_exit);
    }
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    if (node->aux.step != AST_NULL) {
        lower_expr(c, arena, node->aux.step);
    }
    ir_emit_jmp(loop_start);
    ir_emit_label(loop_exit);
}

static void lower_stmt_switch(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        lower_expr(c, arena, node->left);
    }
    /* TODO: Implement switch statement */
}

static void lower_stmt_break(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    if (g_ir_emitter_ctx.loop_depth > 0) {
        size_t patch = BE(c)->emit_jmp(c, ""); /* TODO: label */;
        ir_emitter_add_break_patch(&g_ir_emitter_ctx, patch);
    }
}

static void lower_stmt_continue(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    if (g_ir_emitter_ctx.loop_depth > 0) {
        size_t patch = BE(c)->emit_jmp(c, ""); /* TODO: label */;
        ir_emitter_add_continue_patch(&g_ir_emitter_ctx, patch);
    }
}

static void lower_stmt_label(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    int idx = ir_emitter_find_label(&g_ir_emitter_ctx, node->s.str);
    if (idx < 0) {
        idx = ir_emitter_add_label(&g_ir_emitter_ctx, node->s.str, me_get_offset(c));
    } else {
        g_ir_emitter_ctx.label_table[idx].position = me_get_offset(c);
    }
    /* Patch any pending gotos to this label */
    for (int i = 0; i < g_ir_emitter_ctx.goto_patch_count[idx]; i++) {
        size_t patch_pos = g_ir_emitter_ctx.goto_patch_list[idx][i];
        BE(c)->patch_jmp(c, patch_pos, g_ir_emitter_ctx.label_table[idx].position);
    }
    g_ir_emitter_ctx.goto_patch_count[idx] = 0;
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
}

static void lower_stmt_goto(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused)), ZeroCCASTNode *node) {
    if (!node) return;
    int idx = ir_emitter_find_label(&g_ir_emitter_ctx, node->s.str);
    if (idx >= 0) {
        size_t patch = BE(c)->emit_jmp(c, ""); /* TODO: label */;
        BE(c)->patch_jmp(c, patch, g_ir_emitter_ctx.label_table[idx].position);
    } else {
        size_t patch = BE(c)->emit_jmp(c, ""); /* TODO: label */;
        idx = ir_emitter_add_label(&g_ir_emitter_ctx, node->s.str, 0);
        if (idx >= 0) {
            if (!g_ir_emitter_ctx.goto_patch_list[idx]) {
                g_ir_emitter_ctx.goto_patch_list[idx] = (size_t*)malloc(sizeof(size_t) * 16);
                g_ir_emitter_ctx.goto_patch_count[idx] = 0;
            }
            if (g_ir_emitter_ctx.goto_patch_list[idx] && g_ir_emitter_ctx.goto_patch_count[idx] < 16) {
                g_ir_emitter_ctx.goto_patch_list[idx][g_ir_emitter_ctx.goto_patch_count[idx]++] = patch;
            }
        }
    }
}

/* =========================================================================
 * Main Statement Dispatcher
 * ========================================================================= */

static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_BLOCK:
            lower_stmt_block(c, arena, node->left);
            break;
        case NODE_EXPR_STMT:
            lower_stmt_expr(c, arena, node);
            break;
        case NODE_DECL:
            /* If this is a function definition (has body in right), lower the body */
            if (!(node->flags & NODE_DECL_IS_VLA) && node->right != AST_NULL) {
                lower_stmt_block(c, arena, node->right);
            } else {
                lower_stmt_decl(c, arena, node);
            }
            break;
        case NODE_STATIC_ASSERT:
            lower_stmt_static_assert(c, arena, node);
            break;
        case NODE_RETURN:
            lower_stmt_return(c, arena, node);
            break;
        case NODE_IF:
            lower_stmt_if(c, arena, node);
            break;
        case NODE_WHILE:
            lower_stmt_while(c, arena, node);
            break;
        case NODE_DO_WHILE:
            lower_stmt_do_while(c, arena, node);
            break;
        case NODE_FOR:
            lower_stmt_for(c, arena, node);
            break;
        case NODE_SWITCH:
            lower_stmt_switch(c, arena, node);
            break;
        case NODE_CASE:
        case NODE_DEFAULT:
            break;
        case NODE_BREAK:
            lower_stmt_break(c, arena);
            break;
        case NODE_CONTINUE:
            lower_stmt_continue(c, arena);
            break;
        case NODE_LABEL:
            lower_stmt_label(c, arena, node);
            break;
        case NODE_GOTO:
            lower_stmt_goto(c, arena, node);
            break;
        default:
            break;
    }
}

/* =========================================================================
 * Expression Lowering
 * ========================================================================= */

/* =========================================================================
 * Expression Lowering (IR-Based)
 * ========================================================================= */

/* 
 * IR-based expression lowering
 * Returns: temp ID containing the result (0xFF if no result)
 * Sets c->last_expr_is_float for float results
 */
static int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return 0xFF;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return 0xFF;
    
    switch (node->tag) {
        case NODE_INT: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, node->int_val);
            c->last_expr_is_float = 0;
            return temp;
        }
        
        case NODE_BOOL: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, node->int_val);
            c->last_expr_is_float = 0;
            return temp;
        }
        
        case NODE_NULLPTR: {
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, 0);
            c->last_expr_is_float = 0;
            return temp;
        }
        
        case NODE_FLOAT: {
            /* Float literals load via backend emit_load_fp_imm */
            double val = node->float_val;
            if (BE(c)->emit_load_fp_imm) {
                BE(c)->emit_load_fp_imm(c, 0, val);
            }
            c->last_expr_is_float = 1;
            return VREG_F0;
        }
        
        case NODE_STRING: {
            if (node->s.str) {
                size_t len = strlen(node->s.str);
                size_t rodata_offset = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                uint8_t zero = 0;
                zero_cc_buf_put_bytes(&c->rodata_section, &zero, 1);
                if (BE(c)->emit_lea_rip) {
                    BE(c)->emit_lea_rip(c, 0, rodata_offset);
                }
            }
            c->last_expr_is_float = 0;
            return VREG_0;
        }
        
        case NODE_IDENT: {
            const char *name = node->s.str;
            ZeroCCSym *sym = zero_cc_find_local(c, name);
            if (sym) {
                if (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE) {
                    /* Float load via backend emit_load_fp */
                    if (BE(c)->emit_load_fp) {
                        BE(c)->emit_load_fp(c, 0, BE(c)->fp_reg, -sym->stack_offset);
                    }
                    c->last_expr_is_float = 1;
                    return VREG_F0;
                } else if (sym->type.tag == TYPE_ARRAY && !sym->is_vla) {
                    /* Fixed-size array decays to pointer via LEA */
                    int temp = c->next_temp_id++;
                    ir_emit_lea(temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return temp;
                } else {
                    int temp = c->next_temp_id++;
                    ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return temp;
                }
            } else {
                ZeroCCGlobalVar *gv = find_global_var(name);
                if (gv) {
                    int temp = c->next_temp_id++;
                    /* Global load requires direct machine code with relocation */
                    BE(c)->emit_mov_imm(c, 0, 0);
                    size_t patch_pos = me_get_offset(c) - 8;
                    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
                        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                        r->text_patch_offset = patch_pos;
                        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                        r->is_call = false;
                    }
                    c->last_expr_is_float = 0;
                    return temp;
                }
                return 0xFF;
            }
        }
        
        case NODE_BINOP: {
            /* Evaluate left operand first - result lands in xmm0 for floats */
            int left_temp = lower_expr(c, arena, node->left);
            bool left_is_float = c->last_expr_is_float;

            /* For float operands: spill xmm0 (= left) to stack NOW, before
             * evaluating right, which would otherwise overwrite xmm0.
             * Use an %rbp-relative stack slot within the allocated frame
             * rather than red-zone [rsp-8] so it is safe across nested calls. */
            int32_t spill_offset = 0;
            if (left_is_float) {
                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                c->current_stack_frame += 8;
                spill_offset = c->current_stack_frame;

                /* movsd [rbp - spill_offset], xmm0 */
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, 0, BE(c)->fp_reg, -spill_offset);
                }
            }

            /* Evaluate right operand - result lands in xmm0 for floats */
            int right_temp = lower_expr(c, arena, node->right);
            bool right_is_float = c->last_expr_is_float;

            /* Float arithmetic requires direct machine code */
            if (left_is_float && right_is_float) {
                /* At this point: xmm0 = right
                 * Reload left and set up: xmm0 = left, xmm1 = right
                 * so that "xmm0 op xmm1" gives "left op right" for all ops. */

                /* movsd xmm1, xmm0   -- xmm1 = right */
                if (BE(c)->emit_fp_mov) {
                    BE(c)->emit_fp_mov(c, 1, 0);
                }

                /* movsd xmm0, [rbp - spill_offset] -- xmm0 = left (reloaded) */
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, 0, BE(c)->fp_reg, -spill_offset);
                }

                /* Perform: xmm0 = left op right */
                switch (node->aux.op) {
                    case TK_PLUS:
                        if (BE(c)->emit_fp_add) BE(c)->emit_fp_add(c, 0, 0, 1);
                        break;
                    case TK_MINUS:
                        if (BE(c)->emit_fp_sub) BE(c)->emit_fp_sub(c, 0, 0, 1);
                        break;
                    case TK_STAR:
                        if (BE(c)->emit_fp_mul) BE(c)->emit_fp_mul(c, 0, 0, 1);
                        break;
                    case TK_SLASH:
                        if (BE(c)->emit_fp_div) BE(c)->emit_fp_div(c, 0, 0, 1);
                        break;
                    default:
                        break;
                }
                c->last_expr_is_float = 1;
                return VREG_F0;
            }
            
            /* Integer arithmetic - use IR */
            int result_temp = c->next_temp_id++;
            switch (node->aux.op) {
                case TK_PLUS:
                    ir_emit_add(result_temp, left_temp, right_temp);
                    break;
                case TK_MINUS:
                    ir_emit_sub(result_temp, left_temp, right_temp);
                    break;
                case TK_STAR:
                    ir_emit_mul(result_temp, left_temp, right_temp);
                    break;
                case TK_SLASH:
                    ir_emit_div(result_temp, left_temp, right_temp);
                    break;
                case TK_PERCENT:
                    ir_emit_mod(result_temp, left_temp, right_temp);
                    break;
                case TK_AMP:
                    ir_emit_and(result_temp, left_temp, right_temp);
                    break;
                case TK_PIPE:
                    ir_emit_or(result_temp, left_temp, right_temp);
                    break;
                case TK_CARET:
                    ir_emit_xor(result_temp, left_temp, right_temp);
                    break;
                case TK_SHL:
                    ir_emit_shl(result_temp, left_temp, right_temp);
                    break;
                case TK_SHR:
                    ir_emit_shr(result_temp, left_temp, right_temp);
                    break;
                case TK_EQ:
                case TK_NE:
                case TK_LT:
                case TK_LE:
                case TK_GT:
                case TK_GE: {
                    /* Comparison: emit cmp + setcc */
                    ir_emit_cmp(left_temp, right_temp);
                    switch (node->aux.op) {
                        case TK_EQ: ir_emit_sete(result_temp); break;
                        case TK_NE: ir_emit_setne(result_temp); break;
                        case TK_LT: ir_emit_setl(result_temp); break;
                        case TK_LE: ir_emit_setle(result_temp); break;
                        case TK_GT: ir_emit_setg(result_temp); break;
                        case TK_GE: ir_emit_setge(result_temp); break;
                        default: break;
                    }
                    break;
                }
                default:
                    ir_emit_mov_reg(result_temp, left_temp);
                    break;
            }
            c->last_expr_is_float = 0;
            return result_temp;
        }
        
        case NODE_UNOP: {
            if (node->aux.op == TK_INC || node->aux.op == TK_DEC) {
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        if (sym->is_const || sym->type.is_const) {
                            c->had_error = 1;
                            snprintf(c->error_msg, sizeof(c->error_msg), "cannot modify const-qualified variable '%s'", var);
                            return 0xFF;
                        }
                        if (sym->is_atomic || sym->type.is_atomic) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                            int delta_temp = c->next_temp_id++;
                            int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                            if (node->aux.op == TK_INC) {
                                ir_emit_mov_imm(delta_temp, 1);
                            } else {
                                ir_emit_mov_imm(delta_temp, -1);
                            }
                            int old_temp = c->next_temp_id++;
                            ir_emit_atomic_xadd(old_temp, addr_temp, delta_temp, width);
                            c->last_expr_is_float = 0;
                            if (node->flags == 1) {
                                /* Postfix x++ / x--: return old value */
                                return old_temp;
                            } else {
                                /* Prefix ++x / --x: return new value (old + 1 / -1) */
                                int new_temp = c->next_temp_id++;
                                int step_temp = c->next_temp_id++;
                                ir_emit_mov_imm(step_temp, (node->aux.op == TK_INC) ? 1 : -1);
                                ir_emit_add(new_temp, old_temp, step_temp);
                                return new_temp;
                            }
                        }

                        int val_temp = c->next_temp_id++;
                        int one_temp = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;

                        /* Load current variable value from stack */
                        ir_emit_load(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                        /* Constant 1 */
                        ir_emit_mov_imm(one_temp, 1);
                        /* res = val +/- 1 */
                        if (node->aux.op == TK_INC) {
                            ir_emit_add(res_temp, val_temp, one_temp);
                        } else {
                            ir_emit_sub(res_temp, val_temp, one_temp);
                        }
                        /* Store new value back to stack */
                        ir_emit_store(res_temp, BE(c)->fp_reg, -sym->stack_offset);

                        c->last_expr_is_float = 0;
                        return res_temp;
                    }
                }
                return 0xFF;
            }

            if (node->aux.op == '&' || node->aux.op == TK_AMP) {
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, operand->s.str);
                    if (sym) {
                        int addr_temp = c->next_temp_id++;
                        if (sym->is_vla) {
                            ir_emit_load(addr_temp, BE(c)->fp_reg, -sym->stack_offset);
                        } else {
                            ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                        }
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                }
            }

            int operand_temp = lower_expr(c, arena, node->left);
            int result_temp = c->next_temp_id++;
            
            switch (node->aux.op) {
                case TK_STAR:  /* Dereference */
                    ir_emit_deref(result_temp, operand_temp);
                    break;
                case TK_MINUS:  /* Negation */
                    ir_emit_neg(result_temp, operand_temp);
                    break;
                case TK_BANG:  /* Logical NOT */
                    ir_emit_lnot(result_temp, operand_temp);
                    break;
                case TK_TILDE:  /* Bitwise NOT */
                    ir_emit_not(result_temp, operand_temp);
                    break;
                default:
                    ir_emit_mov_reg(result_temp, operand_temp);
                    break;
            }
            c->last_expr_is_float = 0;
            return result_temp;
        }
        
        case NODE_CAST: {
            int result = lower_expr(c, arena, node->left);
            /* Float to int conversion: emit via IR for correct sequencing */
            if (c->last_expr_is_float) {
                int int_res_temp = c->next_temp_id++;
                ir_emit_cvttsd2si((uint8_t)int_res_temp, (uint8_t)result);
                c->last_expr_is_float = 0;
                return int_res_temp;
            }
            return result;
        }
        
        case NODE_VA_START: {
            /* va_start(ap, last) - initialize va_list to point to variadic args */
            /* The variadic args are in the spill area after the fixed params */
            /* For System V AMD64: after saving rdi, rsi, rdx, rcx, r8, r9 */
            /* The first variadic arg is at offset after the last fixed param */
            
            /* Get the ap variable name - node->left is an ASTRef to the ap identifier */
            ZeroCCASTNode *ap_node = zero_cc_ast_get(arena, node->left);
            const char *ap_var = (ap_node && ap_node->tag == NODE_IDENT) ? ap_node->s.str : "";
            ZeroCCSym *ap_sym = zero_cc_find_local(c, ap_var);
            
            if (ap_sym) {
                /* Get the 'last' parameter to find where variadic args start */
                ZeroCCASTNode *last_node = zero_cc_ast_get(arena, node->right);
                const char *last_var = (last_node && last_node->tag == NODE_IDENT) ? last_node->s.str : "";
                ZeroCCSym *last_sym = zero_cc_find_local(c, last_var);
                
                if (last_sym) {
                    /* variadic args start 8 bytes after the last fixed param in stack */
                    int32_t va_start_offset = -(last_sym->stack_offset + 8);
                    
                    /* Store address into ap: ap = (char*)rbp + va_start_offset */
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, 0xFF, va_start_offset);
                    ir_emit_store(addr_temp, BE(c)->fp_reg, -ap_sym->stack_offset);
                }
            }
            
            c->last_expr_is_float = 0;
            return VREG_0;
        }
        
        case NODE_VA_ARG: {
            /* va_arg(ap, type) - extract next variadic argument */
            /* Simplified: bump pointer by 8 bytes and load */
            /* Full implementation would check gp_offset for register args */
            
            /* Get the va_list pointer */
            int ap_temp = lower_expr(c, arena, node->left);
            
            /* Load the value from [ap] */
            int result_temp = c->next_temp_id++;
            ir_emit_deref(result_temp, ap_temp);
            
            /* Bump ap by 8 bytes downward: ap -= 8 */
            int eight_temp = c->next_temp_id++;
            ir_emit_mov_imm(eight_temp, -8);
            int new_ap_temp = c->next_temp_id++;
            ir_emit_add(new_ap_temp, ap_temp, eight_temp);
            
            /* Store back to ap variable */
            ZeroCCASTNode *ap_node = zero_cc_ast_get(arena, node->left);
            const char *ap_var = ap_node ? ap_node->s.str : "";
            ZeroCCSym *ap_sym = zero_cc_find_local(c, ap_var);
            if (ap_sym) {
                ir_emit_store(new_ap_temp, BE(c)->fp_reg, -ap_sym->stack_offset);
            }
            
            c->last_expr_is_float = 0;
            return result_temp;
        }
        
        case NODE_ASSIGN: {
            /* Check if this is a compound assignment (+=, -=, etc.) */
            bool is_compound = (node->aux.op != TK_ASSIGN && node->aux.op != '=');
            
            int right_temp = lower_expr(c, arena, node->right);
            ZeroCCASTNode *left_node = zero_cc_ast_get(arena, node->left);
            
            if (left_node && left_node->tag == NODE_MEMBER) {
                /* Struct member assignment: traverse chain of member accesses */
                int32_t member_offset = 0;
                ZeroCCASTNode *curr = left_node;
                while (curr && curr->tag == NODE_MEMBER) {
                    member_offset += (int32_t)curr->aux.alt;
                    curr = zero_cc_ast_get(arena, curr->left);
                }
                ZeroCCASTNode *base_node = curr;
                int32_t base_stack_offset = 0;
                
                // Find the base variable's stack offset if it's a local identifier
                if (base_node && base_node->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                    if (!sym) {
                        fprintf(stderr, "ERROR: Undefined local variable '%s' in struct assignment\n", base_node->s.str);
                        return 0xFF;
                    }
                    if (sym->is_const || sym->type.is_const) {
                        c->had_error = 1;
                        snprintf(c->error_msg, sizeof(c->error_msg), "cannot assign to member of const-qualified struct '%s'", base_node->s.str);
                        return 0xFF;
                    }
                    if (c->sem_ctx && left_node->aux.extra > 0) {
                        TypeInfo *ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(left_node->aux.extra - 1));
                        if (ti && (ti->modifiers & TYPE_MOD_CONST)) {
                            c->had_error = 1;
                            snprintf(c->error_msg, sizeof(c->error_msg), "cannot assign to const-qualified member");
                            return 0xFF;
                        }
                    }
                    for (size_t m_i = 0; m_i < sym->type.member_count; m_i++) {
                        if (sym->type.members[m_i].offset == (size_t)member_offset) {
                            if (sym->type.members[m_i].is_const) {
                                c->had_error = 1;
                                snprintf(c->error_msg, sizeof(c->error_msg), "cannot assign to const-qualified member '%s'", sym->type.members[m_i].name);
                                return 0xFF;
                            }
                            break;
                        }
                    }
                    base_stack_offset = -sym->stack_offset;
                } else {
                    fprintf(stderr, "ERROR: Unsupported base expression in struct member assignment\n");
                    return 0xFF;
                }
                
                int32_t total_offset = base_stack_offset + member_offset;
                
                if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                    uint8_t bit_offset = (uint8_t)(left_node->aux.step & 0xFF);
                    uint8_t bit_width = (uint8_t)((left_node->aux.step >> 8) & 0xFF);
                    bool is_signed = ((left_node->aux.step >> 16) & 1) != 0;
                    uint64_t val_mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);

                    /* 1. Load current storage unit */
                    int curr_temp = c->next_temp_id++;
                    ir_emit_load(curr_temp, BE(c)->fp_reg, total_offset);

                    /* Handle compound assignment on bitfield */
                    if (is_compound) {
                        int curr_val = curr_temp;
                        if (bit_offset > 0) {
                            int shift_amt = c->next_temp_id++;
                            ir_emit_mov_imm(shift_amt, (int32_t)bit_offset);
                            int shifted = c->next_temp_id++;
                            ir_emit_shr(shifted, curr_val, shift_amt);
                            curr_val = shifted;
                        }
                        int mask_t = c->next_temp_id++;
                        ir_emit_mov_imm(mask_t, (int32_t)val_mask);
                        int masked = c->next_temp_id++;
                        ir_emit_and(masked, curr_val, mask_t);
                        curr_val = masked;
                        if (is_signed && bit_width < 32) {
                            int32_t sign_bit = (int32_t)(1ULL << (bit_width - 1));
                            int s_temp = c->next_temp_id++;
                            ir_emit_mov_imm(s_temp, sign_bit);
                            int x_temp = c->next_temp_id++;
                            ir_emit_xor(x_temp, curr_val, s_temp);
                            int e_temp = c->next_temp_id++;
                            ir_emit_sub(e_temp, x_temp, s_temp);
                            curr_val = e_temp;
                        }
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                            case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                            default: ir_emit_mov_reg(op_res, right_temp); break;
                        }
                        right_temp = op_res;
                    }

                    /* 2. Clear target bits */
                    uint64_t field_mask = (bit_width >= 64) ? ~0ULL : (val_mask << bit_offset);
                    uint64_t clear_mask = ~field_mask;
                    int clear_mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(clear_mask_temp, (int32_t)clear_mask);
                    int cleared_temp = c->next_temp_id++;
                    ir_emit_and(cleared_temp, curr_temp, clear_mask_temp);

                    /* 3. Mask incoming value to bit_width */
                    int val_mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(val_mask_temp, (int32_t)val_mask);
                    int masked_val = c->next_temp_id++;
                    ir_emit_and(masked_val, right_temp, val_mask_temp);

                    /* 4. Shift into position */
                    if (bit_offset > 0) {
                        int shl_amt = c->next_temp_id++;
                        ir_emit_mov_imm(shl_amt, (int32_t)bit_offset);
                        int shifted_val = c->next_temp_id++;
                        ir_emit_shl(shifted_val, masked_val, shl_amt);
                        masked_val = shifted_val;
                    }

                    /* 5. Combine and store */
                    int combined_temp = c->next_temp_id++;
                    ir_emit_or(combined_temp, cleared_temp, masked_val);
                    ir_emit_store(combined_temp, BE(c)->fp_reg, total_offset);

                    c->last_expr_is_float = 0;
                    return right_temp;
                }

                if (c->last_expr_is_float) {
                    /* Float value in xmm0 - store to [rbp + total_offset] */
                    if (BE(c)->emit_store_fp) {
                        BE(c)->emit_store_fp(c, 0, BE(c)->fp_reg, total_offset);
                    }
                } else {
                    /* Integer/pointer value in GPR - use IR store */
                    ir_emit_store(right_temp, BE(c)->fp_reg, total_offset);
                }
                c->last_expr_is_float = 0;
                return right_temp;
            }

            if (left_node && left_node->tag == NODE_INDEX) {
                /* Array element assignment: arr[idx] = expr or arr[idx] op= expr */
                int array_temp = lower_expr(c, arena, left_node->left);
                int index_temp = lower_expr(c, arena, left_node->right);

                size_t elem_size = 4;
                TypeInfo *elem_ti = NULL;
                if (c->sem_ctx && left_node->aux.extra > 0) {
                    elem_ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(left_node->aux.extra - 1));
                }
                if (elem_ti) {
                    elem_size = type_size(elem_ti);
                } else {
                    ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                    if (ln && ln->tag == NODE_IDENT) {
                        ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                        if (sym && sym->type.base && sym->type.base->size > 0) {
                            elem_size = sym->type.base->size;
                        }
                    }
                }
                if (elem_size == 0) elem_size = 4;

                int scaled_index;
                if (elem_size == 1) {
                    scaled_index = index_temp;
                } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
                    int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
                    int sh_temp = c->next_temp_id++;
                    ir_emit_mov_imm(sh_temp, shift);
                    scaled_index = c->next_temp_id++;
                    ir_emit_shl(scaled_index, index_temp, sh_temp);
                } else {
                    int sz_temp = c->next_temp_id++;
                    ir_emit_mov_imm(sz_temp, (int32_t)elem_size);
                    scaled_index = c->next_temp_id++;
                    ir_emit_mul(scaled_index, index_temp, sz_temp);
                }

                int addr_temp = c->next_temp_id++;
                ir_emit_add(addr_temp, array_temp, scaled_index);

                if (is_compound) {
                    int curr_val = c->next_temp_id++;
                    if (elem_size == 1) {
                        ir_emit_load_byte(curr_val, addr_temp);
                    } else if (elem_size == 2) {
                        ir_emit_load_16(curr_val, addr_temp);
                    } else if (elem_size == 4) {
                        ir_emit_load_32(curr_val, addr_temp);
                    } else {
                        ir_emit_deref(curr_val, addr_temp);
                    }
                    int op_res = c->next_temp_id++;
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, right_temp); break;
                        default: ir_emit_mov_reg(op_res, right_temp); break;
                    }
                    right_temp = op_res;
                }

                if (elem_size == 1) {
                    ir_emit_store_byte(right_temp, addr_temp);
                } else if (elem_size == 2) {
                    ir_emit_store_16(right_temp, addr_temp);
                } else if (elem_size == 4) {
                    ir_emit_store_32(right_temp, addr_temp);
                } else {
                    ir_emit_store_64(right_temp, addr_temp);
                }

                c->last_expr_is_float = 0;
                return right_temp;
            }

            if (left_node && left_node->tag == NODE_UNOP && (left_node->aux.op == '*' || left_node->aux.op == TK_STAR)) {
                ZeroCCASTNode *inner = zero_cc_ast_get(arena, left_node->left);
                if (inner && inner->tag == NODE_IDENT) {
                    ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                    if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->is_const) {
                        c->had_error = 1;
                        snprintf(c->error_msg, sizeof(c->error_msg), "cannot assign to const-qualified pointer target '%s'", inner->s.str);
                        return 0xFF;
                    }
                }
                int ptr_temp = lower_expr(c, arena, left_node->left);
                ir_emit_store_64(right_temp, ptr_temp);
                c->last_expr_is_float = 0;
                return right_temp;
            }

            const char *var = left_node ? left_node->s.str : "";
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym) {
                if (sym->is_const || sym->type.is_const) {
                    c->had_error = 1;
                    snprintf(c->error_msg, sizeof(c->error_msg), "cannot assign to const-qualified variable '%s'", var);
                    return 0xFF;
                }
                bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                if (target_is_float) {
                    if (!c->last_expr_is_float) {
                        /* Convert int to float in xmm0 */
                        if (BE(c)->emit_fp_cvtsf) {
                            BE(c)->emit_fp_cvtsf(c, 0, 0); /* cvtsi2sd xmm0, rax */
                        }
                    }
                    /* For compound assignment, we need to load current value first */
                    if (is_compound) {
                        /* Load current value into xmm1 */
                        if (BE(c)->emit_load_fp) {
                            BE(c)->emit_load_fp(c, 1, BE(c)->fp_reg, -sym->stack_offset);
                        }
                        /* Perform operation in xmm0 */
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN:
                                if (BE(c)->emit_fp_add) BE(c)->emit_fp_add(c, 0, 0, 1);
                                break;
                            case TK_SUB_ASSIGN:
                                if (BE(c)->emit_fp_sub) BE(c)->emit_fp_sub(c, 0, 0, 1);
                                break;
                            case TK_MUL_ASSIGN:
                                if (BE(c)->emit_fp_mul) BE(c)->emit_fp_mul(c, 0, 0, 1);
                                break;
                            case TK_DIV_ASSIGN:
                                if (BE(c)->emit_fp_div) BE(c)->emit_fp_div(c, 0, 0, 1);
                                break;
                        }
                    }
                    /* movsd [rbp - offset], xmm0 */
                    if (BE(c)->emit_store_fp) {
                        BE(c)->emit_store_fp(c, 0, BE(c)->fp_reg, -sym->stack_offset);
                    }
                    c->last_expr_is_float = 1;
                    return VREG_F0;
                } else {
                    if (c->last_expr_is_float) {
                        /* Convert float to int in eax */
                        if (BE(c)->emit_fp_cvtsi) {
                            BE(c)->emit_fp_cvtsi(c, 0, 0); /* cvttsd2si eax, xmm0 */
                        }
                        if (is_compound) {
                            /* Load current value and perform operation */
                            int left_temp = c->next_temp_id++;
                            ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_add(right_temp, left_temp, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_sub(right_temp, left_temp, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_mul(right_temp, left_temp, right_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_div(right_temp, left_temp, right_temp); break;
                                case TK_MOD_ASSIGN: ir_emit_mod(right_temp, left_temp, right_temp); break;
                                case TK_AND_ASSIGN: ir_emit_and(right_temp, left_temp, right_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(right_temp, left_temp, right_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(right_temp, left_temp, right_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(right_temp, left_temp, right_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(right_temp, left_temp, right_temp); break;
                            }
                        }
                        /* mov [rbp - offset], eax */
                        BE(c)->emit_store(c, 0, BE(c)->fp_reg, -sym->stack_offset);
                        c->last_expr_is_float = 0;
                        return VREG_0;
                    } else {
                        if (sym && (sym->is_atomic || sym->type.is_atomic)) {
                            int addr_temp = c->next_temp_id++;
                            ir_emit_lea(addr_temp, 0xFF, -sym->stack_offset);
                            int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                            if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
                                int delta_temp = right_temp;
                                if (node->aux.op == TK_SUB_ASSIGN) {
                                    delta_temp = c->next_temp_id++;
                                    ir_emit_neg(delta_temp, right_temp);
                                }
                                int delta_copy = c->next_temp_id++;
                                ir_emit_mov_reg(delta_copy, delta_temp);
                                int old_temp = c->next_temp_id++;
                                ir_emit_atomic_xadd(old_temp, addr_temp, delta_temp, width);
                                int new_temp = c->next_temp_id++;
                                ir_emit_add(new_temp, old_temp, delta_copy);
                                c->last_expr_is_float = 0;
                                return new_temp;
                            } else if (!is_compound) {
                                int old_temp = c->next_temp_id++;
                                ir_emit_atomic_xchg(old_temp, addr_temp, right_temp, width);
                                c->last_expr_is_float = 0;
                                return right_temp;
                            }
                        }

                        if (is_compound) {
                            /* Load current value and perform operation */
                            int left_temp = c->next_temp_id++;
                            ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                            switch (node->aux.op) {
                                case TK_ADD_ASSIGN: ir_emit_add(right_temp, left_temp, right_temp); break;
                                case TK_SUB_ASSIGN: ir_emit_sub(right_temp, left_temp, right_temp); break;
                                case TK_MUL_ASSIGN: ir_emit_mul(right_temp, left_temp, right_temp); break;
                                case TK_DIV_ASSIGN: ir_emit_div(right_temp, left_temp, right_temp); break;
                                case TK_MOD_ASSIGN: ir_emit_mod(right_temp, left_temp, right_temp); break;
                                case TK_AND_ASSIGN: ir_emit_and(right_temp, left_temp, right_temp); break;
                                case TK_OR_ASSIGN:  ir_emit_or(right_temp, left_temp, right_temp); break;
                                case TK_XOR_ASSIGN: ir_emit_xor(right_temp, left_temp, right_temp); break;
                                case TK_SHL_ASSIGN: ir_emit_shl(right_temp, left_temp, right_temp); break;
                                case TK_SHR_ASSIGN: ir_emit_shr(right_temp, left_temp, right_temp); break;
                            }
                        }
                        ir_emit_store(right_temp, BE(c)->fp_reg, -sym->stack_offset);
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16 && !is_compound) {
                            /* System V AMD64 ABI: second eightbyte returned in rdx (physical register 2) */
                            ir_emit_store(2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                        }
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                }
            }
            c->last_expr_is_float = 0;
            return right_temp;
        }
        
        case NODE_TERNARY: {
            /* Ternary (cond ? then : else) using standard IR */
            int cond_temp = lower_expr(c, arena, node->left);
            int res_temp = c->next_temp_id++;
            uint16_t else_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            ir_emit_je((uint8_t)cond_temp, else_label);

            int then_temp = lower_expr(c, arena, node->right);
            ir_emit_mov_reg(res_temp, then_temp);
            ir_emit_jmp(end_label);

            ir_emit_label(else_label);
            int else_temp = lower_expr(c, arena, node->aux.alt);
            ir_emit_mov_reg(res_temp, else_temp);

            ir_emit_label(end_label);
            c->last_expr_is_float = 0;
            return res_temp;
        }
        
        case NODE_COMPOUND_LIT: {
            if (node->left != AST_NULL) {
                return lower_expr(c, arena, node->left);
            }
            return 0xFF;
        }
        
        case NODE_INDEX: {
            /* Array indexing: base[index] */
            int array_temp = lower_expr(c, arena, node->left);
            int index_temp = lower_expr(c, arena, node->right);

            /* Determine element size */
            size_t elem_size = 4;
            TypeInfo *elem_ti = NULL;
            if (c->sem_ctx && node->aux.extra > 0) {
                elem_ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
            }
            if (elem_ti) {
                elem_size = type_size(elem_ti);
            } else {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, ln->s.str);
                    if (sym && sym->type.base && sym->type.base->size > 0) {
                        elem_size = sym->type.base->size;
                    }
                }
            }
            if (elem_size == 0) elem_size = 4;

            int scaled_index;
            if (elem_size == 1) {
                scaled_index = index_temp;
            } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
                int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
                int sh_temp = c->next_temp_id++;
                ir_emit_mov_imm(sh_temp, shift);
                scaled_index = c->next_temp_id++;
                ir_emit_shl(scaled_index, index_temp, sh_temp);
            } else {
                int sz_temp = c->next_temp_id++;
                ir_emit_mov_imm(sz_temp, (int32_t)elem_size);
                scaled_index = c->next_temp_id++;
                ir_emit_mul(scaled_index, index_temp, sz_temp);
            }

            int addr_temp = c->next_temp_id++;
            ir_emit_add(addr_temp, array_temp, scaled_index);  /* addr = base + scaled_index */

            int result_temp = c->next_temp_id++;
            if (elem_size == 1) {
                ir_emit_load_byte(result_temp, addr_temp);
            } else if (elem_size == 2) {
                ir_emit_load_16(result_temp, addr_temp);
            } else if (elem_size == 4) {
                ir_emit_load_32(result_temp, addr_temp);
            } else {
                ir_emit_deref(result_temp, addr_temp);
            }

            c->last_expr_is_float = 0;
            return result_temp;
        }
        
        case NODE_MEMBER: {
            /* Load member value from memory (Rvalue context) */
            int32_t member_offset = 0;
            ZeroCCASTNode *curr = node;
            while (curr && curr->tag == NODE_MEMBER) {
                member_offset += (int32_t)curr->aux.alt;
                curr = zero_cc_ast_get(arena, curr->left);
            }
            ZeroCCASTNode *base_node = curr;
            int32_t base_stack_offset = 0;
            
            if (base_node && base_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                if (sym) {
                    base_stack_offset = -sym->stack_offset;
                }
            }
            
            int32_t total_offset = base_stack_offset + member_offset;
            int val_temp = c->next_temp_id++;
            ir_emit_load(val_temp, BE(c)->fp_reg, total_offset);

            if (node->flags & NODE_MEMBER_IS_BITFIELD) {
                uint8_t bit_offset = (uint8_t)(node->aux.step & 0xFF);
                uint8_t bit_width = (uint8_t)((node->aux.step >> 8) & 0xFF);
                bool is_signed = ((node->aux.step >> 16) & 1) != 0;

                if (bit_offset > 0) {
                    int shift_temp = c->next_temp_id++;
                    ir_emit_mov_imm(shift_temp, (int32_t)bit_offset);
                    int shifted_temp = c->next_temp_id++;
                    ir_emit_shr(shifted_temp, val_temp, shift_temp);
                    val_temp = shifted_temp;
                }

                uint64_t mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
                int mask_temp = c->next_temp_id++;
                ir_emit_mov_imm(mask_temp, (int32_t)mask);
                int masked_temp = c->next_temp_id++;
                ir_emit_and(masked_temp, val_temp, mask_temp);
                val_temp = masked_temp;

                if (is_signed && bit_width < 32) {
                    int32_t sign_bit = (int32_t)(1ULL << (bit_width - 1));
                    int sign_temp = c->next_temp_id++;
                    ir_emit_mov_imm(sign_temp, sign_bit);
                    int xor_temp = c->next_temp_id++;
                    ir_emit_xor(xor_temp, val_temp, sign_temp);
                    int ext_temp = c->next_temp_id++;
                    ir_emit_sub(ext_temp, xor_temp, sign_temp);
                    val_temp = ext_temp;
                }
            } else {
                TypeInfo *ti = NULL;
                if (c->sem_ctx && node->aux.extra > 0) {
                    ti = (TypeInfo *)arena_index_to_ptr(c->sem_ctx->perm_arena, (size_t)(node->aux.extra - 1));
                }
                size_t sz = ti ? type_size(ti) : 4;
                bool is_u = ti && (ti->modifiers & TYPE_MOD_UNSIGNED);
                if (sz == 1) {
                    int mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(mask_temp, 0xFF);
                    int masked = c->next_temp_id++;
                    ir_emit_and(masked, val_temp, mask_temp);
                    val_temp = masked;
                    if (!is_u) {
                        int sign_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sign_temp, 0x80);
                        int xor_temp = c->next_temp_id++;
                        ir_emit_xor(xor_temp, val_temp, sign_temp);
                        int ext_temp = c->next_temp_id++;
                        ir_emit_sub(ext_temp, xor_temp, sign_temp);
                        val_temp = ext_temp;
                    }
                } else if (sz == 2) {
                    int mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(mask_temp, 0xFFFF);
                    int masked = c->next_temp_id++;
                    ir_emit_and(masked, val_temp, mask_temp);
                    val_temp = masked;
                    if (!is_u) {
                        int sign_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sign_temp, 0x8000);
                        int xor_temp = c->next_temp_id++;
                        ir_emit_xor(xor_temp, val_temp, sign_temp);
                        int ext_temp = c->next_temp_id++;
                        ir_emit_sub(ext_temp, xor_temp, sign_temp);
                        val_temp = ext_temp;
                    }
                } else if (sz == 4) {
                    int mask_temp = c->next_temp_id++;
                    ir_emit_mov_imm(mask_temp, 0xFFFFFFFFLL);
                    int masked = c->next_temp_id++;
                    ir_emit_and(masked, val_temp, mask_temp);
                    val_temp = masked;
                    if (!is_u) {
                        int sign_temp = c->next_temp_id++;
                        ir_emit_mov_imm(sign_temp, 0x80000000LL);
                        int xor_temp = c->next_temp_id++;
                        ir_emit_xor(xor_temp, val_temp, sign_temp);
                        int ext_temp = c->next_temp_id++;
                        ir_emit_sub(ext_temp, xor_temp, sign_temp);
                        val_temp = ext_temp;
                    }
                }
            }

            c->last_expr_is_float = 0;
            return val_temp;
        }
        
        case NODE_CALL: {
            /* Function call - handle float args directly in machine code, int args via IR */
            ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
            if (callee && callee->tag == NODE_IDENT) {
                const char *name = callee->s.str;

                /* Intercept atomic builtins */
                if (strcmp(name, "__sync_fetch_and_add") == 0 || strcmp(name, "__atomic_fetch_add") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(res_temp, ptr_temp, val_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_add_and_fetch") == 0 || strcmp(name, "__atomic_add_fetch") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int val_copy = c->next_temp_id++;
                    ir_emit_mov_reg(val_copy, val_temp);
                    int old_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(old_temp, ptr_temp, val_temp, 4);
                    int res_temp = c->next_temp_id++;
                    ir_emit_add(res_temp, old_temp, val_copy);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_fetch_and_sub") == 0 || strcmp(name, "__atomic_fetch_sub") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int neg_temp = c->next_temp_id++;
                    ir_emit_neg(neg_temp, val_temp);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(res_temp, ptr_temp, neg_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_sub_and_fetch") == 0 || strcmp(name, "__atomic_sub_fetch") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int val_copy = c->next_temp_id++;
                    ir_emit_mov_reg(val_copy, val_temp);
                    int neg_temp = c->next_temp_id++;
                    ir_emit_neg(neg_temp, val_temp);
                    int old_temp = c->next_temp_id++;
                    ir_emit_atomic_xadd(old_temp, ptr_temp, neg_temp, 4);
                    int res_temp = c->next_temp_id++;
                    ir_emit_sub(res_temp, old_temp, val_copy);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_val_compare_and_swap") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    ASTRef arg2 = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int old_temp = lower_expr(c, arena, arg1);
                    int new_temp = lower_expr(c, arena, arg2);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_cmpxchg(res_temp, ptr_temp, old_temp, new_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_bool_compare_and_swap") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    ASTRef arg2 = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int old_temp = lower_expr(c, arena, arg1);
                    int new_temp = lower_expr(c, arena, arg2);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_cmpxchg_bool(res_temp, ptr_temp, old_temp, new_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_lock_test_and_set") == 0 || strcmp(name, "__atomic_exchange_n") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int res_temp = c->next_temp_id++;
                    ir_emit_atomic_xchg(res_temp, ptr_temp, val_temp, 4);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__sync_lock_release") == 0) {
                    ASTRef arg0 = node->right;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int zero_temp = c->next_temp_id++;
                    ir_emit_mov_imm(zero_temp, 0);
                    ir_emit_store_32(zero_temp, ptr_temp);
                    c->last_expr_is_float = 0;
                    return zero_temp;
                }
                if (strcmp(name, "__sync_synchronize") == 0) {
                    ir_emit_mfence();
                    int res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(res_temp, 0);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__atomic_load_n") == 0) {
                    ASTRef arg0 = node->right;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int res_temp = c->next_temp_id++;
                    ir_emit_load_32(res_temp, ptr_temp);
                    c->last_expr_is_float = 0;
                    return res_temp;
                }
                if (strcmp(name, "__atomic_store_n") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int val_temp = lower_expr(c, arena, arg1);
                    int dummy_res = c->next_temp_id++;
                    ir_emit_atomic_xchg(dummy_res, ptr_temp, val_temp, 4);
                    c->last_expr_is_float = 0;
                    return dummy_res;
                }
                if (strcmp(name, "__atomic_compare_exchange_n") == 0) {
                    ASTRef arg0 = node->right;
                    ASTRef arg1 = (arg0 != AST_NULL) ? zero_cc_ast_get(arena, arg0)->next : AST_NULL;
                    ASTRef arg2 = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1)->next : AST_NULL;
                    int ptr_temp = lower_expr(c, arena, arg0);
                    int exp_ptr_temp = lower_expr(c, arena, arg1);
                    int exp_val_temp = c->next_temp_id++;
                    ir_emit_load_32(exp_val_temp, exp_ptr_temp);
                    int desired_temp = lower_expr(c, arena, arg2);
                    int actual_old = c->next_temp_id++;
                    ir_emit_atomic_cmpxchg(actual_old, ptr_temp, exp_val_temp, desired_temp, 4);
                    ir_emit_store_32(actual_old, exp_ptr_temp);
                    int bool_res = c->next_temp_id++;
                    ir_emit_cmp(actual_old, exp_val_temp);
                    ir_emit_sete(bool_res);
                    c->last_expr_is_float = 0;
                    return bool_res;
                }
                
                /* Collect argument temps for integer args only */
                ASTRef arg = node->right;
                int int_arg_temps[6];
                int int_arg_count = 0;
                int fp_reg_idx = 0;  /* Tracks which xmm register for next float arg */
                int32_t fp_spill_offsets[8];
                
                while (arg != AST_NULL) {
                    int temp = lower_expr(c, arena, arg);
                    if (c->last_expr_is_float) {
                        /* Float argument: value is in xmm0. Spill it to a stack slot NOW
                         * so that subsequent argument evaluations (or calls) do not clobber it. */
                        if (fp_reg_idx < 8) {
                            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                            c->current_stack_frame += 8;
                            fp_spill_offsets[fp_reg_idx] = c->current_stack_frame;
                            /* movsd [rbp - spill_offset], xmm0 */
                            if (BE(c)->emit_store_fp) {
                                BE(c)->emit_store_fp(c, 0, BE(c)->fp_reg, -fp_spill_offsets[fp_reg_idx]);
                            }
                            fp_reg_idx++;
                        }
                    } else {
                        /* Integer argument: collect for IR emission */
                        if (int_arg_count < 6) {
                            int_arg_temps[int_arg_count] = temp;
                            int_arg_count++;
                        }
                    }
                    ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                    arg = arg_node ? arg_node->next : AST_NULL;
                }
                
                /* Emit IR call with only integer args */
                ir_emit_call(name);
                
                /* Pack integer arg info into the instruction */
                if (ir_get_count() > 0) {
                    Instr *last_instr = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                    last_instr->temp_dest = (uint8_t)int_arg_count;
                    if (int_arg_count > 0) last_instr->temp_src1 = (uint8_t)int_arg_temps[0];
                    if (int_arg_count > 1) last_instr->temp_src2 = (uint8_t)int_arg_temps[1];
                    if (int_arg_count > 2) {
                        last_instr->imm = 0;
                        if (int_arg_count > 2) last_instr->imm |= ((int16_t)(uint8_t)int_arg_temps[2]) << 0;
                        if (int_arg_count > 3) last_instr->imm |= ((int16_t)(uint8_t)int_arg_temps[3]) << 8;
                    }
                    /* Store fp_reg_idx in label_idx for variadic %al handling */
                    last_instr->label_idx = (uint16_t)fp_reg_idx;
                }
                
                /* Record float spill offsets for this call */
                if (g_ir_emitter_ctx.call_count < 64) {
                    int cidx = g_ir_emitter_ctx.call_count++;
                    g_ir_emitter_ctx.call_fp_spill_counts[cidx] = fp_reg_idx;
                    for (int f = 0; f < fp_reg_idx && f < 8; f++) {
                        g_ir_emitter_ctx.call_fp_spills[cidx][f] = fp_spill_offsets[f];
                    }
                }
                
                ZeroCCFunction *callee_fn = zero_cc_find_function(c, name);
                if (callee_fn && (callee_fn->return_type.tag == TYPE_FLOAT || callee_fn->return_type.tag == TYPE_DOUBLE)) {
                    c->last_expr_is_float = 1;
                    return VREG_F0;  /* Return value in xmm0 */
                }

                c->last_expr_is_float = 0;
                
                /* Check if function returns a 9-16 byte struct/union */
                if (callee_fn && (callee_fn->return_type.tag == TYPE_STRUCT || callee_fn->return_type.tag == TYPE_UNION) && 
                    callee_fn->return_type.size > 8 && callee_fn->return_type.size <= 16) {
                    /* Store struct return info in label_idx: high byte = 1, low byte = size */
                    if (ir_get_count() > 0) {
                        Instr *last_instr = (Instr*)&ir_get_buffer()[ir_get_count() - 1];
                        last_instr->label_idx = 0x0100 | (callee_fn->return_type.size & 0xFF);
                    }
                }
                
                return VREG_0;  /* Return value in rax */
            }
            c->last_expr_is_float = 0;
            return VREG_0;
        }
        
        default:
            return 0xFF;
    }
}


/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* Initialize IR emitter context */
    ir_emitter_init(&g_ir_emitter_ctx);
    
    /* Reset register allocator for this function */
    regalloc_init(&c->regalloc);
    c->next_temp_id = 0;
    c->ir_next_label = 0;
    
    /* Reset IR buffer */
    ir_reset();
    g_ir_epilogue_label = c->ir_next_label++;
    
    /* Don't reset text section - accumulate all function code */
    /* c->text_section.length = 0; */
    
    /* NOTE: The function prologue is emitted in compile.h before calling this
     * function. emit_prologue() is NOT called here to avoid a double prologue. */
    
    /* Lower AST to IR (instruction selection) */
    lower_node(c, arena, root);
    ir_emit_label(g_ir_epilogue_label);
    
    /* Compute live ranges and allocate registers */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get_buffer(), ir_get_count());
    live_range_compute(&lra, &c->regalloc);
    regalloc_allocate(&c->regalloc);
    live_range_free(&lra);
    
    /* Debug: print interval details */
    for (size_t i = 0; i < c->regalloc.interval_count && i < 10; i++) {
    }
    
    /* Emit machine code from IR with allocated registers */
    emit_ir_to_machine_code(c);
    
    /* Emit return jumps after IR-to-MC */
    emit_return_jumps(c);
    
    /* Resolve all label patches */
    if (BE(c)->resolve_labels) {
        BE(c)->resolve_labels(c);
    }
    
    /* Patch epilogue jumps - target is the epilogue (before leave/ret) */
    size_t epilogue_target = me_get_offset(c);
    
    /* Emit epilogue */
    me_emit_leave(c);
    BE(c)->emit_ret(c);
    for (int i = 0; i < g_ir_emitter_ctx.epilogue_patch_count; i++) {
        size_t patch_pos = g_ir_emitter_ctx.epilogue_patches[i];
        int32_t rel = (int32_t)(epilogue_target - (patch_pos + 4));
        me_patch_u32(c, patch_pos, (uint32_t)rel);
    }
    g_ir_emitter_ctx.epilogue_patch_count = 0;
    regalloc_destroy(&c->regalloc);
    
    /* text_offset was already set in compile.h BEFORE the prologue was emitted,
     * so fn->text_offset correctly points to the function's first byte. */
}


/* =========================================================================
 * IR to Machine Code Emission
 * ========================================================================= */

/* Emit machine code from IR with allocated registers */
static void emit_ir_to_machine_code(ZeroCCCompiler *c) {
    const Instr *instrs = ir_get_buffer();
    size_t count = ir_get_count();
    size_t label_count = c->ir_next_label;
    bool *label_defined = label_count ? (bool *)calloc(label_count, sizeof(bool)) : NULL;
    size_t *label_offsets = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    size_t **label_patches = label_count ? (size_t **)calloc(label_count, sizeof(size_t *)) : NULL;
    size_t *label_patch_counts = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    size_t *label_patch_caps = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    int call_idx = 0;

    for (size_t i = 0; i < count; i++) {
        const Instr *instr = &instrs[i];

        /* Frame pointer (0xFF) should NOT be allocated - use actual FP register */
        int phys_dst = (instr->temp_dest == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
        int phys_src1 = (instr->temp_src1 == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
        int phys_src2 = (instr->temp_src2 == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src2);

        switch (instr->op) {
            case OP_MOV_IMM:
                BE(c)->emit_mov_imm(c, phys_dst, instr->imm64);
                break;
            
            case OP_MOV_MEM_RAX:
                BE(c)->emit_store(c, phys_src1, phys_src2, instr->imm);
                break;
            
            case OP_MOV_RAX_MEM:
                BE(c)->emit_load(c, phys_dst, phys_src1, instr->imm);
                break;
            
            case OP_MOV_RAX_RBX:
                BE(c)->emit_mov_reg(c, phys_dst, phys_src1);
                break;
            
            case OP_LEA_RAX_MEM:
                BE(c)->emit_lea(c, phys_dst, phys_src1, instr->imm);
                break;
            
            case OP_ADD:
                BE(c)->emit_add(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_SUB:
                BE(c)->emit_sub(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_NEG:
                if (BE(c)->emit_neg) {
                    BE(c)->emit_neg(c, phys_dst, phys_src1);
                }
                break;
            
            case OP_MUL:
                BE(c)->emit_mul(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_DIV:
                BE(c)->emit_div(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_MOD:
                BE(c)->emit_mod(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_AND:
                BE(c)->emit_and(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_OR:
                BE(c)->emit_or(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_XOR:
                BE(c)->emit_xor(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_SHL:
                BE(c)->emit_shl(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_SHR:
                BE(c)->emit_shr(c, phys_dst, phys_src1, phys_src2);
                break;
            
            case OP_CMP:
                BE(c)->emit_cmp(c, phys_src1, phys_src2);
                break;
            
            case OP_TEST:
                BE(c)->emit_test(c, phys_src1, phys_src2);
                break;
            
            case OP_SETL:
                BE(c)->emit_setl(c, phys_dst);
                break;
            
            case OP_SETLE:
                BE(c)->emit_setle(c, phys_dst);
                break;
            
            case OP_SETG:
                BE(c)->emit_setg(c, phys_dst);
                break;
            
            case OP_SETGE:
                BE(c)->emit_setge(c, phys_dst);
                break;
            
            case OP_SET_E:
                BE(c)->emit_sete(c, phys_dst);
                break;
            
            case OP_SET_NE:
                BE(c)->emit_setne(c, phys_dst);
                break;
            
            case OP_DEREF:
                BE(c)->emit_deref(c, phys_dst, phys_src1);
                break;
            
            case OP_LOAD_BYTE:
                BE(c)->emit_load_byte(c, phys_dst, phys_src1);
                break;
            
            case OP_CMP_ZERO:
                BE(c)->emit_cmp_zero(c, phys_src1);
                break;
            
            case OP_CVTTSD2SI:
                if (BE(c)->emit_fp_cvtsi) {
                    BE(c)->emit_fp_cvtsi(c, phys_dst, 0);  /* convert xmm0 to GPR phys_dst */
                }
                break;
            
            case OP_CVTSI2SD:
                if (BE(c)->emit_fp_cvtsf) {
                    BE(c)->emit_fp_cvtsf(c, 0, phys_src1);  /* convert GPR phys_src1 to xmm0 */
                }
                break;

            case OP_LOAD_16:
                if (BE(c)->emit_load_16) {
                    BE(c)->emit_load_16(c, phys_dst, phys_src1);
                }
                break;

            case OP_LOAD_32:
                if (BE(c)->emit_load_32) {
                    BE(c)->emit_load_32(c, phys_dst, phys_src1);
                }
                break;

            case OP_STORE_BYTE:
                if (BE(c)->emit_store_byte) {
                    BE(c)->emit_store_byte(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_16:
                if (BE(c)->emit_store_16) {
                    BE(c)->emit_store_16(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_32:
                if (BE(c)->emit_store_32) {
                    BE(c)->emit_store_32(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_64:
                if (BE(c)->emit_store_64) {
                    BE(c)->emit_store_64(c, phys_src1, phys_src2);
                }
                break;

            case OP_VLA_ALLOC:
                if (BE(c)->emit_vla_alloc) {
                    BE(c)->emit_vla_alloc(c, phys_dst, phys_src1);
                }
                break;

            case OP_ATOMIC_XADD:
                if (BE(c)->emit_atomic_xadd) {
                    BE(c)->emit_atomic_xadd(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_CMPXCHG: {
                int phys_expected = (instr->label_idx == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg) {
                    BE(c)->emit_atomic_cmpxchg(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_CMPXCHG_BOOL: {
                int phys_expected = (instr->label_idx == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg_bool) {
                    BE(c)->emit_atomic_cmpxchg_bool(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_XCHG:
                if (BE(c)->emit_atomic_xchg) {
                    BE(c)->emit_atomic_xchg(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_MFENCE:
                if (BE(c)->emit_mfence) {
                    BE(c)->emit_mfence(c);
                }
                break;

            case OP_CALL: {
                /* Extract function name from imm64 (stored as pointer during lowering) */
                const char *fn_name = (const char *)(intptr_t)instr->imm64;
                
                /* Extract integer argument info packed into the instruction */
                int arg_count = instr->temp_dest;
                int arg_temps[6] = {0};
                if (arg_count > 0) arg_temps[0] = instr->temp_src1;
                if (arg_count > 1) arg_temps[1] = instr->temp_src2;
                if (arg_count > 2) {
                    /* Unpack remaining temps from imm (16-bit) */
                    arg_temps[2] = (int)((instr->imm >> 0) & 0xFF);
                    if (arg_count > 3) arg_temps[3] = (int)((instr->imm >> 8) & 0xFF);
                }
                
                /* Extract fp_reg_idx (number of float args) from label_idx */
                int fp_reg_idx = (int)instr->label_idx;
                
                /* Safe parallel move: push all src registers then pop into dst registers */
                static const int int_arg_regs[6] = {7, 6, 2, 1, 8, 9};  /* rdi, rsi, rdx, rcx, r8, r9 */
                int n_args = (arg_count < 6) ? arg_count : 6;
                int src_regs[6];
                for (int i = 0; i < n_args; i++) {
                    src_regs[i] = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                    BE(c)->emit_push_reg(c, src_regs[i]);
                }
                for (int i = n_args - 1; i >= 0; i--) {
                    int dst_reg = int_arg_regs[i];
                    BE(c)->emit_pop_reg(c, dst_reg);
                }
                
                /* Load float arguments from spill slots into xmm0..xmmN */
                if (call_idx < g_ir_emitter_ctx.call_count) {
                    int fp_count = g_ir_emitter_ctx.call_fp_spill_counts[call_idx];
                    for (int f = 0; f < fp_count && f < 8; f++) {
                        int32_t spill_off = g_ir_emitter_ctx.call_fp_spills[call_idx][f];
                        /* movsd xmm<f>, [rbp - spill_off] */
                        if (BE(c)->emit_load_fp) {
                            BE(c)->emit_load_fp(c, f, BE(c)->fp_reg, -spill_off);
                        }
                    }
                    call_idx++;
                }
                
                /* System V AMD64 ABI: For variadic functions, %al must hold number of SSE registers used */
                /* TODO: Check if function is variadic - for now emit for all calls */
                if (BE(c)->emit_variadic_al_setup) {
                    BE(c)->emit_variadic_al_setup(c, fp_reg_idx);
                }
                
                /* Emit the call */
                BE(c)->emit_call(c, fn_name);

                /* Handle struct return (9-16 bytes) unpacking */
                if ((instr->label_idx & 0xFF00) == 0x0100) {
                    int struct_size = instr->label_idx & 0xFF;
                    if (struct_size > 8 && struct_size <= 16) {
                        /* Physical rdx contains upper 8 bytes */
                        /* This is handled by caller store logic */
                    }
                }
                break;
            }
            
            case OP_NOT:
                BE(c)->emit_not(c, phys_dst, phys_src1);
                break;
            
            case OP_LNOT:
                BE(c)->emit_lnot(c, phys_dst, phys_src1);
                break;

            case OP_LABEL:
                if (instr->label_idx < label_count) {
                    size_t label_idx = instr->label_idx;
                    label_defined[label_idx] = true;
                    label_offsets[label_idx] = me_get_offset(c);
                    for (size_t j = 0; j < label_patch_counts[label_idx]; j++) {
                        BE(c)->patch_jmp(c, label_patches[label_idx][j], label_offsets[label_idx]);
                    }
                    label_patch_counts[label_idx] = 0;
                }
                break;

            case OP_JMP:
            case OP_JE:
            case OP_JNE:
            case OP_JL:
            case OP_JLE:
            case OP_JG: {
                size_t patch_pos = 0;
                switch (instr->op) {
                    case OP_JMP: patch_pos = BE(c)->emit_jmp(c, ""); break;
                    case OP_JE:
                        BE(c)->emit_test(c, phys_src1, phys_src1);
                        patch_pos = BE(c)->emit_jeq(c, "");
                        break;
                    case OP_JNE:
                        BE(c)->emit_test(c, phys_src1, phys_src1);
                        patch_pos = BE(c)->emit_jne(c, "");
                        break;
                    case OP_JL:  patch_pos = BE(c)->emit_jlt(c, ""); break;
                    case OP_JLE: patch_pos = BE(c)->emit_jle(c, ""); break;
                    case OP_JG:  patch_pos = BE(c)->emit_jgt(c, ""); break;
                    default: break;
                }
                if (instr->label_idx < label_count) {
                    size_t label_idx = instr->label_idx;
                    if (label_defined[label_idx]) {
                        BE(c)->patch_jmp(c, patch_pos, label_offsets[label_idx]);
                    } else {
                        if (label_patch_counts[label_idx] >= label_patch_caps[label_idx]) {
                            size_t new_cap = label_patch_caps[label_idx] ? label_patch_caps[label_idx] * 2 : 4;
                            size_t *new_patches = (size_t *)realloc(label_patches[label_idx], new_cap * sizeof(size_t));
                            if (new_patches) {
                                label_patches[label_idx] = new_patches;
                                label_patch_caps[label_idx] = new_cap;
                            }
                        }
                        if (label_patches[label_idx] && label_patch_counts[label_idx] < label_patch_caps[label_idx]) {
                            label_patches[label_idx][label_patch_counts[label_idx]++] = patch_pos;
                        }
                    }
                }
                break;
            }
            
            default:
                break;
        }
    }

    for (size_t i = 0; i < label_count; i++) {
        free(label_patches ? label_patches[i] : NULL);
    }
    free(label_defined);
    free(label_offsets);
    free(label_patches);
    free(label_patch_counts);
    free(label_patch_caps);
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_H */