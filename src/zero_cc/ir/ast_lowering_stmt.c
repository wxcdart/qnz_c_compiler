/* AST Lowering - Statement Handlers and Dispatcher */

#include "zero_cc/ir/ast_lowering_internal.h"

#define ZERO_CC_PARSER_AST_LOWERING_STMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/ir/ir_emit_ops.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/types/vregs.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/common/arena.h"
#include "zero_cc/ir/abi_classify.h"
#include "zero_cc/ir/label_patcher.h"

void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);

void lower_stmt_block(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    /* Track block depth for VLA hybrid rule enforcement */
    c->lowering.vla.block_depth++;

    ASTRef stmt = ref;
    while (stmt != AST_NULL) {
        lower_node(c, arena, stmt);
        ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
        stmt = snode ? snode->next : AST_NULL;
    }

    c->lowering.vla.block_depth--;
}

void lower_stmt_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        (void)lower_expr(c, arena, node->left);  /* Result not used */
    }
}

void lower_stmt_decl(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->flags & NODE_DECL_IS_VLA) {
        /* Variable Length Array (C99 VLA) - enforce hybrid rules */

        /* Rule 1: multiple VLAs per function are supported (Sprint 40):
         * each VLA emits its own OP_VLA_ALLOC (sub rsp, aligned-size;
         * mov ptr, rsp) and stores the pointer in its own local slot;
         * `leave` in the epilogue unwinds all of them. The legacy
         * one-VLA-per-function restriction is lifted. */
        c->lowering.vla.has_runtime_vla = true;
        c->lowering.vla.vla_count++;

        /* Rule 2: VLA must be at function scope (block_depth == 1) */
        if (c->lowering.vla.block_depth > 1) {
            diag_report_error_at_token(c, "VLA must be at function scope, not in nested blocks (hybrid VLA rule)");
            return;
        }

        /* Rule 3: VLA not allowed in loops */
        if (c->lowering.vla.loop_depth > 0) {
            diag_report_error_at_token(c, "VLA not allowed inside loops (hybrid VLA rule)");
            return;
        }

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
        /* C11 6.7.9: a string literal initializing a local char array.
         * Zero-fill the whole slot, then store the literal's bytes (all
         * compile-time constants) as byte immediates; the generic path
         * below would lower the literal to its rodata pointer and store
         * the pointer value into the array.  Mirrors the NODE_COMPOUND_LIT
         * zero-fill / width-chunk pattern. */
        ZeroCCASTNode *str_init_node = zero_cc_ast_get(arena, node->left);
        ZeroCCSym *str_sym = lowering_local_sym(c, node, node->s.str);
        if (str_init_node && str_init_node->tag == NODE_STRING && str_init_node->s.str &&
            str_sym && str_sym->type.tag == TYPE_ARRAY && str_sym->type.base &&
            (str_sym->type.base->tag == TYPE_INT8 || str_sym->type.base->tag == TYPE_UINT8)) {
            const char *lit = str_init_node->s.str;
            size_t total_size = str_sym->type.size;
            size_t copy_len = strlen(lit) + 1;
            if (copy_len > total_size) copy_len = total_size; /* C11 6.7.9p12: truncate */
            int zero_temp = c->next_temp_id++;
            ir_emit_mov_imm(zero_temp, 0);
            size_t zoff = 0;
            while (zoff < total_size) {
                size_t chunk = total_size - zoff;
                if (chunk >= 8) chunk = 8;
                else if (chunk >= 4) chunk = 4;
                else if (chunk >= 2) chunk = 2;
                else chunk = 1;
                int32_t store_offset = -(str_sym->stack_offset - (int32_t)zoff);
                if (chunk == 8) {
                    ir_emit_store(zero_temp, BE(c)->fp_reg, store_offset);
                } else {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, store_offset);
                    if (chunk == 4) {
                        ir_emit_store_32(zero_temp, addr_temp);
                    } else if (chunk == 2) {
                        ir_emit_store_16(zero_temp, addr_temp);
                    } else {
                        ir_emit_store_byte(zero_temp, addr_temp);
                    }
                }
                zoff += chunk;
            }
            for (size_t i = 0; i < copy_len; i++) {
                int val_temp = c->next_temp_id++;
                ir_emit_mov_imm(val_temp, (int32_t)(unsigned char)lit[i]);
                int addr_temp = c->next_temp_id++;
                ir_emit_lea(addr_temp, TEMP_NONE, -(int32_t)(str_sym->stack_offset - (int32_t)i));
                ir_emit_store_byte(val_temp, addr_temp);
            }
            c->last_expr_is_float = 0;
            return;
        }
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
                    ir_emit_cvtsi2sd((uint16_t)float_temp, (uint16_t)result_temp);
                }
                /* Float assignment: store via IR */
                ir_emit_fstore((uint16_t)float_temp, TEMP_NONE, -sym->stack_offset);
                c->last_expr_is_float = 1;
                return;
            } else {
                if (c->last_expr_is_float) {
                    /* Convert float to int via IR */
                    int int_temp = c->next_temp_id++;
                    ir_emit_cvttsd2si((uint16_t)int_temp, (uint16_t)result_temp);
                    ir_emit_store((uint16_t)int_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return;
                } else {
                    /* If initializer was a brace initializer storing directly to sym in-place, don't overwrite */
                    ZeroCCASTNode *init_node = zero_cc_ast_get(arena, node->left);
                    bool is_inplace_brace_init = (init_node && init_node->tag == NODE_COMPOUND_LIT &&
                                                  init_node->s.str && strcmp(init_node->s.str, sym->name) == 0);
                    if (!(sym->type.tag == TYPE_ARRAY && init_node && init_node->tag == NODE_COMPOUND_LIT)) {
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 16) {
                            if (!is_inplace_brace_init) {
                                /* System V AMD64 ABI: sret buffer pointer in result_temp.
                                 * Spill result_temp to a stack slot so offset additions don't clobber it.
                                 * Copy chunks from [result_temp + i*8] to local stack -(sym->stack_offset - i*8) */
                                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                c->current_stack_frame += 8;
                                int32_t buf_ptr_off = c->current_stack_frame;
                                ir_emit_store(result_temp, BE(c)->fp_reg, -buf_ptr_off);

                                int num_chunks = (sym->type.size + 7) / 8;
                                for (int i = 0; i < num_chunks; i++) {
                                    size_t rem = sym->type.size - (size_t)(i * 8);
                                    int chunk_temp = c->next_temp_id++;
                                    int addr_temp;
                                    if (i == 0) {
                                        addr_temp = c->next_temp_id++;
                                        ir_emit_load(addr_temp, BE(c)->fp_reg, -buf_ptr_off);
                                    } else {
                                        int base_temp = c->next_temp_id++;
                                        ir_emit_load(base_temp, BE(c)->fp_reg, -buf_ptr_off);
                                        int off_temp = c->next_temp_id++;
                                        ir_emit_mov_imm(off_temp, i * 8);
                                        addr_temp = c->next_temp_id++;
                                        ir_emit_add(addr_temp, base_temp, off_temp);
                                    }
                                    int32_t dst_off = -(sym->stack_offset - i * 8);
                                    if (rem >= 8) {
                                        ir_emit_deref(chunk_temp, addr_temp);
                                        ir_emit_store(chunk_temp, BE(c)->fp_reg, dst_off);
                                    } else if (rem >= 4) {
                                        ir_emit_load_32(chunk_temp, addr_temp);
                                        int dst_addr = c->next_temp_id++;
                                        ir_emit_lea(dst_addr, TEMP_NONE, dst_off);
                                        ir_emit_store_32(chunk_temp, dst_addr);
                                    } else if (rem >= 2) {
                                        ir_emit_load_16(chunk_temp, addr_temp);
                                        int dst_addr = c->next_temp_id++;
                                        ir_emit_lea(dst_addr, TEMP_NONE, dst_off);
                                        ir_emit_store_16(chunk_temp, dst_addr);
                                    } else {
                                        ir_emit_load_byte(chunk_temp, addr_temp);
                                        int dst_addr = c->next_temp_id++;
                                        ir_emit_lea(dst_addr, TEMP_NONE, dst_off);
                                        ir_emit_store_byte(chunk_temp, dst_addr);
                                    }
                                }
                            }
                        } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                   is_inplace_brace_init) {
                            /* Struct/union already initialized in-place by brace initializer */
                        } else {
                            /* Integer or small struct assignment: use IR store */
                            ir_emit_store(result_temp, BE(c)->fp_reg, -sym->stack_offset);
                            if (c->isa == ISA_RISCV32 && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 4 && sym->type.size <= 8) {
                                /* RV32 ILP32 ABI: second 4-byte chunk returned in a1 (VREG_2) */
                                ir_emit_store(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 4));
                            } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16) {
                                /* System V AMD64 ABI: second eightbyte returned in rdx (VREG_2) */
                                int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? VREG_1 : VREG_2;
                                ir_emit_store(reg2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                            }
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
    /* If currently expanding an inlined function call, redirect return to the target temp and jump to exit label */
    if (c->lowering.inlining.active_depth > 0) {
        if (node && node->left != AST_NULL) {
            int res = lower_expr(c, arena, node->left);
            if (c->lowering.inlining.return_target_temp >= 0 && res != c->lowering.inlining.return_target_temp) {
                ir_emit_mov_reg(c->lowering.inlining.return_target_temp, res);
            }
        }
        if (c->lowering.inlining.return_target_label != 0) {
            ir_emit_jmp(c->lowering.inlining.return_target_label);
        }
        return;
    }

    if (node && node->left != AST_NULL) {
        ZeroCCASTNode *ret_expr = zero_cc_ast_get(arena, node->left);
        ZeroCCSym *sym = (ret_expr && ret_expr->tag == NODE_IDENT) ? zero_cc_find_local(c, ret_expr->s.str) : NULL;
        if (sym && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION)) {
            int32_t struct_offset = -sym->stack_offset;
            if (c->isa == ISA_RISCV32 && sym->type.size > 4 && sym->type.size <= 8) {
                /* RV32 ILP32 ABI: 5-8 byte struct returned across a0 (VREG_0) and a1 (VREG_2) */
                ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);
                ir_emit_load(VREG_2, BE(c)->fp_reg, struct_offset + 4);
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            } else if (sym->type.size <= 8) {
                /* System V AMD64 ABI: <=8 byte struct returned in rax (VREG_0) */
                ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            } else if (sym->type.size <= 16) {
                /* System V AMD64 ABI: 9-16 byte struct returned across rax and rdx */
                // 1. Load lower 8 bytes into rax (VREG_0) via IR
                ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);

                // 2. Load upper 8 bytes into rdx (VREG_2) via IR (or a1/x1)
                int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? VREG_1 : VREG_2;
                ir_emit_load(reg2, BE(c)->fp_reg, struct_offset + 8);

                // 3. Jump to epilogue (which will emit leave; ret)
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }

            /* System V AMD64 ABI: >16 byte struct returned via hidden pointer (sret) */
            /* RDI contains the hidden pointer to caller's buffer */
            else {
                ZeroCCFunction *cur_fn = (c->function_count > 0) ? &c->functions[c->function_count - 1] : NULL;
                int32_t sret_off = cur_fn ? cur_fn->sret_stack_offset : 0;

                /* Load hidden pointer directly into VREG_0 (RAX) from stack slot (RDI saved at -sret_off) */
                ir_emit_load(VREG_0, BE(c)->fp_reg, -sret_off);

                /* Store each 8-byte chunk to [hidden_ptr + i*8] */
                int num_chunks = (sym->type.size + 7) / 8;
                for (int i = 0; i < num_chunks; i++) {
                    size_t rem = sym->type.size - (size_t)(i * 8);
                    int chunk_temp = c->next_temp_id++;
                    int addr_temp;
                    if (i == 0) {
                        addr_temp = c->next_temp_id++;
                        ir_emit_load(addr_temp, BE(c)->fp_reg, -sret_off);
                    } else {
                        int base_temp = c->next_temp_id++;
                        ir_emit_load(base_temp, BE(c)->fp_reg, -sret_off);
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(off_temp, i * 8);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(addr_temp, base_temp, off_temp);
                    }

                    /* Load chunk from local stack: local offset is -(sym->stack_offset - i*8) */
                    int32_t chunk_stack_off = -(sym->stack_offset - i * 8);
                    if (rem >= 8) {
                        ir_emit_load(chunk_temp, BE(c)->fp_reg, chunk_stack_off);
                        ir_emit_store_64(chunk_temp, addr_temp);
                    } else if (rem >= 4) {
                        int src_addr = c->next_temp_id++;
                        ir_emit_lea(src_addr, TEMP_NONE, chunk_stack_off);
                        ir_emit_load_32(chunk_temp, src_addr);
                        ir_emit_store_32(chunk_temp, addr_temp);
                    } else if (rem >= 2) {
                        int src_addr = c->next_temp_id++;
                        ir_emit_lea(src_addr, TEMP_NONE, chunk_stack_off);
                        ir_emit_load_16(chunk_temp, src_addr);
                        ir_emit_store_16(chunk_temp, addr_temp);
                    } else {
                        int src_addr = c->next_temp_id++;
                        ir_emit_lea(src_addr, TEMP_NONE, chunk_stack_off);
                        ir_emit_load_byte(chunk_temp, src_addr);
                        ir_emit_store_byte(chunk_temp, addr_temp);
                    }
                }

                /* Return hidden pointer in RAX (VREG_0) */
                ir_emit_load(VREG_0, BE(c)->fp_reg, -sret_off);
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }
        }

        int result_temp = lower_expr(c, arena, node->left);
        ZeroCCFunction *cur_fn = (c->function_count > 0) ? &c->functions[c->function_count - 1] : NULL;
        if (cur_fn && cur_fn->ret_struct_sz > 16) {
            /* Return expression (e.g., NODE_CALL or NODE_COMPOUND_LIT) evaluated to a pointer to a >16B struct buffer.
             * Copy chunks from [result_temp + i*8] to caller's sret buffer at -sret_off. */
            int32_t sret_off = cur_fn->sret_stack_offset;
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t src_ptr_off = c->current_stack_frame;
            ir_emit_store(result_temp, BE(c)->fp_reg, -src_ptr_off);

            size_t st_size = (size_t)cur_fn->ret_struct_sz;
            int num_chunks = (int)((st_size + 7) / 8);
            for (int i = 0; i < num_chunks; i++) {
                size_t rem = st_size - (size_t)(i * 8);
                int chunk_temp = c->next_temp_id++;
                int src_addr;
                int dst_addr;
                if (i == 0) {
                    src_addr = c->next_temp_id++;
                    ir_emit_load(src_addr, BE(c)->fp_reg, -src_ptr_off);
                    dst_addr = c->next_temp_id++;
                    ir_emit_load(dst_addr, BE(c)->fp_reg, -sret_off);
                } else {
                    int s_base = c->next_temp_id++;
                    ir_emit_load(s_base, BE(c)->fp_reg, -src_ptr_off);
                    int s_off = c->next_temp_id++;
                    ir_emit_mov_imm(s_off, i * 8);
                    src_addr = c->next_temp_id++;
                    ir_emit_add(src_addr, s_base, s_off);

                    int d_base = c->next_temp_id++;
                    ir_emit_load(d_base, BE(c)->fp_reg, -sret_off);
                    int d_off = c->next_temp_id++;
                    ir_emit_mov_imm(d_off, i * 8);
                    dst_addr = c->next_temp_id++;
                    ir_emit_add(dst_addr, d_base, d_off);
                }
                if (rem >= 8) {
                    ir_emit_deref(chunk_temp, src_addr);
                    ir_emit_store_64(chunk_temp, dst_addr);
                } else if (rem >= 4) {
                    ir_emit_load_32(chunk_temp, src_addr);
                    ir_emit_store_32(chunk_temp, dst_addr);
                } else if (rem >= 2) {
                    ir_emit_load_16(chunk_temp, src_addr);
                    ir_emit_store_16(chunk_temp, dst_addr);
                } else {
                    ir_emit_load_byte(chunk_temp, src_addr);
                    ir_emit_store_byte(chunk_temp, dst_addr);
                }
            }
            ir_emit_load(VREG_0, BE(c)->fp_reg, -sret_off);
            ir_emit_jmp(c->lowering.epilogue_label);
            return;
        }
        /* Ensure result is in rax (VREG_0) for return */
        int final_float_temp = result_temp;
        if (!c->last_expr_is_float) {
            ZeroCCType fn_ret_type = zero_cc_get_current_fn_ret_type(c);
            /* C99 width semantics: truncate/extend the result to the declared
             * return width (e.g. `return a + a;` from an `int` function must
             * wrap to 32 bits, not return the 64-bit residue). */
            if (fn_ret_type.tag != TYPE_FLOAT && fn_ret_type.tag != TYPE_DOUBLE) {
                zero_cc_emit_int_extend(c, result_temp, &fn_ret_type);
            }
            /* Move result to rax if not already there - emit IR */
            if (result_temp != VREG_0) {
                ir_emit_mov_reg(VREG_0, result_temp);
            }
            /* If function returns float/double, convert int to float */
            if (fn_ret_type.tag == TYPE_FLOAT || fn_ret_type.tag == TYPE_DOUBLE) {
                final_float_temp = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)final_float_temp, (uint16_t)result_temp);
                c->last_expr_is_float = 1;
            }
        }
        /* Float result must be in the FP return register (xmm0/d0/fa0).
         * VREG_F0 (temp 0) always maps to physical register 0, which the FP
         * emitters treat as the FP return register, so move the result there
         * when the computed value lives in another FP temp. */
        if (c->last_expr_is_float && final_float_temp != VREG_F0) {
            ir_emit_fmov((uint8_t)VREG_F0, (uint16_t)final_float_temp);
        }
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
    ir_emit_je((uint16_t)cond_temp, else_label);
    lower_node(c, arena, node->right);
    ir_emit_jmp(end_label);
    ir_emit_label(else_label);
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(end_label);
}

void lower_stmt_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* Track loop depth for VLA hybrid rule enforcement */
    c->lowering.vla.loop_depth++;

    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_start);
    ir_emit_label(loop_start);
    int cond_temp = lower_expr(c, arena, node->left);
    ir_emit_je((uint16_t)cond_temp, loop_exit);
    lower_node(c, arena, node->right);
    ir_emit_jmp(loop_start);
    ir_emit_label(loop_exit);
    label_patcher_exit_loop(&c->label_patcher);

    c->lowering.vla.loop_depth--;
}

void lower_stmt_do_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* Track loop depth for VLA hybrid rule enforcement */
    c->lowering.vla.loop_depth++;

    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_cond = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_cond);
    ir_emit_label(loop_start);
    lower_node(c, arena, node->right);
    ir_emit_label(loop_cond);
    int cond_temp = lower_expr(c, arena, node->left);
    ir_emit_jne((uint16_t)cond_temp, loop_start);
    ir_emit_label(loop_exit);
    label_patcher_exit_loop(&c->label_patcher);

    c->lowering.vla.loop_depth--;
}

void lower_stmt_for(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* Track loop depth for VLA hybrid rule enforcement */
    c->lowering.vla.loop_depth++;

    if (!node) return;
    /* For-scope (C11 6.8.5): the init-declared loop variable is visible for
     * the whole for statement (init/cond/step/body).  Cap name lookup at the
     * symbol count recorded by the parser after the init, so a later sibling
     * for redeclaring the same name doesn't shadow this loop's header. */
    uint32_t saved_for_bound = c->local_lookup_bound;
    if (node->aux.op != 0) c->local_lookup_bound = node->aux.op - 1;
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
        ir_emit_je((uint16_t)cond_temp, loop_exit);
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

    c->local_lookup_bound = saved_for_bound;
    c->lowering.vla.loop_depth--;
}

void lower_stmt_switch(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;

    uint16_t switch_exit = c->ir_next_label++;

    /* break inside a switch jumps to the switch exit */
    label_patcher_enter_loop(&c->label_patcher, switch_exit, 0);

    int cond_temp = lower_expr(c, arena, node->left);

    /* Body is normally a NODE_BLOCK; its top-level statement chain may
     * contain NODE_CASE / NODE_DEFAULT entries. */
    ZeroCCASTNode *body = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
    ASTRef first = AST_NULL;
    if (body) {
        first = (body->tag == NODE_BLOCK) ? body->left : node->right;
    }

    /* Dispatch: for each case, compare the switch value against the case
     * constant and jump to the case's label (emitted when the case node
     * itself is lowered below). The labels are stored in aux.alt.
     * C3 fix: no case cap - dispatch is O(n) CMP/JNE and the per-function
     * temp guard (C2b, 65534 temps) bounds the case count (~2 temps per
     * case -> ~32k cases), which stays under the 16-bit label space. */
    uint16_t default_label = 0;
    ASTRef s = first;
    while (s != AST_NULL) {
        ZeroCCASTNode *sn = zero_cc_ast_get(arena, s);
        if (!sn) break;
        if (sn->tag == NODE_CASE) {
            sn->aux.alt = c->ir_next_label++;
            uint16_t case_label = (uint16_t)sn->aux.alt;
            if (sn->aux.step != 0) {
                /* Range case: low in sn->left, high in sn->aux.step */
                int low_temp = lower_expr(c, arena, sn->left);
                int high_temp = lower_expr(c, arena, (ASTRef)sn->aux.step);
                int flag_low = c->next_temp_id++;
                ir_emit_cmp(cond_temp, low_temp);
                ir_emit_cond_set(flag_low, CC_GE);
                int flag_high = c->next_temp_id++;
                ir_emit_cmp(cond_temp, high_temp);
                ir_emit_cond_set(flag_high, CC_LE);
                int in_range = c->next_temp_id++;
                ir_emit_and(in_range, flag_low, flag_high);
                ir_emit_jne((uint16_t)in_range, case_label);
            } else {
                int val_temp = lower_expr(c, arena, sn->left);
                int flag_temp = c->next_temp_id++;
                ir_emit_cmp(cond_temp, val_temp);
                ir_emit_cond_set(flag_temp, CC_EQ);
                /* JE = jump-if-zero, so take the case when the flag is non-zero */
                ir_emit_jne((uint16_t)flag_temp, case_label);
            }
        } else if (sn->tag == NODE_DEFAULT) {
            sn->aux.alt = c->ir_next_label++;
            default_label = (uint16_t)sn->aux.alt;
        }
        s = sn->next;
    }

    /* No matching case: jump to default, or skip the whole body.
     * The unconditional jump makes the body's start unreachable from the
     * dispatch path, so a non-matching value executes no body statement
     * (per C semantics). */
    if (default_label) {
        ir_emit_jmp(default_label);
    } else {
        ir_emit_jmp(switch_exit);
    }
    /* Fallthrough between case labels is automatic: each case's body
     * continues into the next statement until break/return. */

    lower_node(c, arena, node->right);

    ir_emit_label(switch_exit);
    label_patcher_exit_loop(&c->label_patcher);
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
        case NODE_BLOCK: {
            /* Block scope (C11 6.2.1): cap local-name lookup at the symbol
             * count recorded by the parser when this block was parsed
             * (aux.step = local_count at the block's '}' + 1, so 0 means
             * unannotated/unbounded).  Without this, a name redeclared in a
             * later sibling block (latest-wins table) shadows this block's
             * references and miscompiles them to the newer symbol's slot. */
            uint32_t saved_bound = c->local_lookup_bound;
            if (node->aux.step != 0) c->local_lookup_bound = node->aux.step - 1;
            lower_stmt_block(c, arena, node->left);
            c->local_lookup_bound = saved_bound;
            break;
        }
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
            if (node->aux.alt) ir_emit_label((uint16_t)node->aux.alt);
            if (node->right != AST_NULL) lower_node(c, arena, node->right);
            break;
        case NODE_DEFAULT:
            if (node->aux.alt) ir_emit_label((uint16_t)node->aux.alt);
            if (node->left != AST_NULL) lower_node(c, arena, node->left);
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

