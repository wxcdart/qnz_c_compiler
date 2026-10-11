/* AST Lowering - Statement Handlers and Dispatcher */

#include "ast_lowering_internal.h"

#define ZERO_CC_PARSER_AST_LOWERING_STMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types/types.h"
#include "parser.h"
#include "ir_emitter.h"
#include "ir_emit_ops.h"
#include "machine_emitter.h"
#include "../isa/isa_backend.h"
#include "../types/vregs.h"
#include "../types/global_vars.h"
#include "../semantic/type.h"
#include "../common/arena.h"
#include "abi_classify.h"
#include "label_patcher.h"

void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);

void lower_stmt_block(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    ASTRef stmt = ref;
    while (stmt != AST_NULL) {
        lower_node(c, arena, stmt);
        ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
        stmt = snode ? snode->next : AST_NULL;
    }
}

void lower_stmt_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        (void)lower_expr(c, arena, node->left);  /* Result not used */
    }
}

void lower_stmt_decl(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
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
                int float_temp = result_temp;
                if (!c->last_expr_is_float) {
                    /* Convert int to float via IR */
                    float_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint8_t)float_temp, (uint8_t)result_temp);
                }
                /* Float assignment: store via IR */
                ir_emit_fstore((uint8_t)float_temp, 0xFF, -sym->stack_offset);
                c->last_expr_is_float = 1;
                return;
            } else {
                if (c->last_expr_is_float) {
                    /* Convert float to int via IR */
                    int int_temp = c->next_temp_id++;
                    ir_emit_cvttsd2si((uint8_t)int_temp, (uint8_t)result_temp);
                    ir_emit_store((uint8_t)int_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return;
                } else {
                    /* If initializer was a compound literal storing directly to an array variable, don't overwrite */
                    ZeroCCASTNode *init_node = zero_cc_ast_get(arena, node->left);
                    if (!(sym->type.tag == TYPE_ARRAY && init_node && init_node->tag == NODE_COMPOUND_LIT)) {
                        /* Integer or struct assignment: use IR store */
                        ir_emit_store(result_temp, BE(c)->fp_reg, -sym->stack_offset);
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16) {
                            /* System V AMD64 ABI: second eightbyte returned in rdx (VREG_2) */
                            ir_emit_store(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                        }
                    }
                }
            }
            c->last_expr_is_float = 0;
        }
    }
}

void lower_stmt_static_assert(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        lower_expr(c, arena, node->left);
    }
}

void lower_stmt_return(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        ZeroCCASTNode *ret_expr = zero_cc_ast_get(arena, node->left);
        ZeroCCSym *sym = (ret_expr && ret_expr->tag == NODE_IDENT) ? zero_cc_find_local(c, ret_expr->s.str) : NULL;
        if (sym && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION)) {
            int32_t struct_offset = -sym->stack_offset;
            if (sym->type.size <= 8) {
                /* System V AMD64 ABI: <=8 byte struct returned in rax (VREG_0) */
                ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            } else if (sym->type.size <= 16) {
                /* System V AMD64 ABI: 9-16 byte struct returned across rax and rdx */
                // 1. Load lower 8 bytes into rax (VREG_0) via IR
                ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);
                
                // 2. Load upper 8 bytes into rdx (VREG_2) via IR
                ir_emit_load(VREG_2, BE(c)->fp_reg, struct_offset + 8);
                
                // 3. Jump to epilogue (which will emit leave; ret)
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }
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
    ir_emit_jmp(c->lowering.epilogue_label);
}

/* Emit return jumps after IR-to-MC */
void emit_return_jumps(ZeroCCCompiler *c) {
    (void)c;
}

void lower_stmt_if(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
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

void lower_stmt_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_start);
    ir_emit_label(loop_start);
    int cond_temp = lower_expr(c, arena, node->left);
    ir_emit_je((uint8_t)cond_temp, loop_exit);
    lower_node(c, arena, node->right);
    ir_emit_jmp(loop_start);
    ir_emit_label(loop_exit);
    label_patcher_exit_loop(&c->label_patcher);
}

void lower_stmt_do_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_cond = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_cond);
    ir_emit_label(loop_start);
    lower_node(c, arena, node->right);
    ir_emit_label(loop_cond);
    int cond_temp = lower_expr(c, arena, node->left);
    ir_emit_jne((uint8_t)cond_temp, loop_start);
    ir_emit_label(loop_exit);
    label_patcher_exit_loop(&c->label_patcher);
}

void lower_stmt_for(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_step = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_step);
    ir_emit_label(loop_start);
    if (node->right != AST_NULL) {
        int cond_temp = lower_expr(c, arena, node->right);
        ir_emit_je((uint8_t)cond_temp, loop_exit);
    }
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(loop_step);
    if (node->aux.step != AST_NULL) {
        lower_expr(c, arena, node->aux.step);
    }
    ir_emit_jmp(loop_start);
    ir_emit_label(loop_exit);
    label_patcher_exit_loop(&c->label_patcher);
}

void lower_stmt_switch(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        lower_expr(c, arena, node->left);
    }
    /* TODO: Implement switch statement */
}

void lower_stmt_break(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    uint16_t lbl = label_patcher_get_break_label(&c->label_patcher);
    if (lbl) {
        ir_emit_jmp(lbl);
    }
}

void lower_stmt_continue(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    uint16_t lbl = label_patcher_get_continue_label(&c->label_patcher);
    if (lbl) {
        ir_emit_jmp(lbl);
    }
}

uint16_t get_or_create_user_ir_label(ZeroCCCompiler *c, const char *name) {
    uint16_t lbl = label_patcher_find_or_create_user_label(&c->label_patcher, name);
    if (lbl == 0) {
        lbl = c->ir_next_label++;
        /* Update the label_patcher with the new label */
        for (int i = 0; i < c->label_patcher.user_label_count; i++) {
            if (strcmp(c->label_patcher.user_labels[i].name, name) == 0) {
                c->label_patcher.user_labels[i].ir_label = lbl;
                break;
            }
        }
    }
    return lbl;
}

void lower_stmt_label(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node || !node->s.str) return;
    uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
    ir_emit_label(lbl);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
}

void lower_stmt_goto(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused)), ZeroCCASTNode *node) {
    if (!node || !node->s.str) return;
    uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
    ir_emit_jmp(lbl);
}

/* =========================================================================
 * Main Statement Dispatcher
 * ========================================================================= */

void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
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

