/* IR-Based Expression Lowering (Minimal Demo)
 *
 * This file contains IR-based versions of lower_expr for a minimal subset
 * of expression types. Used to validate the IR pipeline end-to-end.
 *
 * Expression types supported:
 * - NODE_INT: Integer literals
 * - NODE_IDENT: Local variable references
 * - NODE_BINOP (ADD, SUB): Binary arithmetic
 *
 * Usage:
 *   Include this after ast_lowering.h
 *   Call lower_expr_ir() instead of lower_expr() for demo mode
 */

#ifndef ZERO_CC_PARSER_AST_LOWERING_IR_H
#define ZERO_CC_PARSER_AST_LOWERING_IR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emit_ops.h"
#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/types/vregs.h"

/* Forward declaration */
static int lower_expr_ir(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);

/*
 * IR-based expression lowering
 * Returns: temp ID containing the result
 */
static int lower_expr_ir(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return TEMP_NONE;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return TEMP_NONE;

    switch (node->tag) {
        case NODE_INT: {
            /* Emit: mov_imm temp, value */
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, node->int_val);
            return temp;
        }

        case NODE_BOOL: {
            /* Emit: mov_imm temp, value */
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, node->int_val);
            return temp;
        }

        case NODE_NULLPTR: {
            /* Emit: xor temp, temp, temp */
            int temp = c->next_temp_id++;
            ir_emit_mov_imm(temp, 0);  /* Simplified: use mov_imm for zero */
            return temp;
        }

        case NODE_IDENT: {
            const char *name = node->s.str;
            ZeroCCSym *sym = zero_cc_find_local(c, name);
            if (sym) {
                /* Emit: load temp, fp, offset */
                int temp = c->next_temp_id++;
                ir_emit_load(temp, BE(c)->fp_reg, -sym->stack_offset);
                return temp;
            } else {
                /* Global variable: load address */
                int temp = c->next_temp_id++;
                ir_emit_mov_imm(temp, 0);  /* Placeholder: actual addr from reloc */
                return temp;
            }
        }

        case NODE_BINOP: {
            /* Recursively lower left and right operands */
            int left_temp = lower_expr_ir(c, arena, node->left);
            int right_temp = lower_expr_ir(c, arena, node->right);

            int result_temp = c->next_temp_id++;

            switch (node->aux.op) {
                case '+':
                    ir_emit_add(result_temp, left_temp, right_temp);
                    break;
                case '-':
                    ir_emit_sub(result_temp, left_temp, right_temp);
                    break;
                case '*':
                    ir_emit_mul(result_temp, left_temp, right_temp);
                    break;
                case '/':
                    ir_emit_div(result_temp, left_temp, right_temp);
                    break;
                case '%':
                    ir_emit_mod(result_temp, left_temp, right_temp);
                    break;
                case '&':
                    ir_emit_and(result_temp, left_temp, right_temp);
                    break;
                case '|':
                    ir_emit_or(result_temp, left_temp, right_temp);
                    break;
                case '^':
                    ir_emit_xor(result_temp, left_temp, right_temp);
                    break;
                case TK_SHL:
                    ir_emit_shl(result_temp, left_temp, right_temp);
                    break;
                case TK_SHR:
                    ir_emit_shr(result_temp, left_temp, right_temp);
                    break;
                default:
                    /* Unknown operator, emit mov to preserve value */
                    ir_emit_mov_reg(result_temp, left_temp);
                    break;
            }
            return result_temp;
        }

        case NODE_UNOP: {
            int operand_temp = lower_expr_ir(c, arena, node->left);
            int result_temp = c->next_temp_id++;

            switch (node->aux.op) {
                case '-':  /* Negation */
                    ir_emit_neg(result_temp, operand_temp);
                    break;
                case '!':  /* Logical NOT */
                    ir_emit_lnot(result_temp, operand_temp);
                    break;
                case '~':  /* Bitwise NOT */
                    ir_emit_not(result_temp, operand_temp);
                    break;
                default:
                    ir_emit_mov_reg(result_temp, operand_temp);
                    break;
            }
            return result_temp;
        }

        case NODE_CAST: {
            /* For now, just pass through the value */
            return lower_expr_ir(c, arena, node->left);
        }

        default:
            /* Unsupported node type, return invalid temp */
            return TEMP_NONE;
    }
}

/*
 * Lower expression and move result to VREG_0
 * This is the main entry point for IR-based lowering
 */
static void lower_expr_to_vreg0(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    int result_temp = lower_expr_ir(c, arena, ref);
    if (result_temp != TEMP_NONE) {
        /* Move result temp to VREG_0 for return */
        BE(c)->emit_mov_reg(c, VREG_0, result_temp);
    }
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_IR_H */
