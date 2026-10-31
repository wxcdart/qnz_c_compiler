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

/* C99 6.7.2.2p4: a VLA's storage must be released when control jumps to a
 * point before its declaration (backward goto loop), or when a forward
 * goto lands inside the VLA's scope without passing through its
 * allocation. ZeroCC's hybrid VLA model allocates each VLA with its own
 * OP_VLA_ALLOC and unwinds them only at function exit, so any goto whose
 * control path crosses a VLA's allocation point is unsound (stack leak
 * per jump -> segfault). Detect such crossings at the top-level function
 * block using statement-position analysis and reject them cleanly (honest
 * rejection beats a segfault). Gotos nested in compound statements take
 * the position of their top-level containing statement, which is a sound
 * approximation for crossing analysis. */
#define ZC_VLA_GOTO_MAX 256

static void zc_stmt_collect_gotos(ZeroCCASTArena *arena, ASTRef ref, int pos,
                                  const char **gotos, int *gpos, int *ngoto);

static void zc_stmt_collect_labels(ZeroCCASTArena *arena, ASTRef ref, int pos,
                                   const char **names, int *pos_arr, int *nnames);

static void zc_list_collect_labels(ZeroCCASTArena *arena, ASTRef ref, int pos,
                                   const char **names, int *pos_arr, int *nnames) {
    ASTRef s = ref;
    while (s != AST_NULL && *nnames < ZC_VLA_GOTO_MAX) {
        zc_stmt_collect_labels(arena, s, pos, names, pos_arr, nnames);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, s);
        if (!n) break;
        s = n->next;
    }
}

static void zc_stmt_collect_labels(ZeroCCASTArena *arena, ASTRef ref, int pos,
                                   const char **names, int *pos_arr, int *nnames) {
    if (ref == AST_NULL || *nnames >= ZC_VLA_GOTO_MAX) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;
    switch (n->tag) {
        case NODE_LABEL:
            if (n->s.str) {
                names[*nnames] = n->s.str;
                pos_arr[*nnames] = pos;
                (*nnames)++;
            }
            /* Also scan the labeled statement (a label may directly precede
             * another label: `a: b:;`). */
            zc_stmt_collect_labels(arena, n->left, pos, names, pos_arr, nnames);
            break;
        case NODE_BLOCK:
            zc_list_collect_labels(arena, n->left, pos, names, pos_arr, nnames);
            break;
        case NODE_IF:
            zc_stmt_collect_labels(arena, n->right, pos, names, pos_arr, nnames);
            zc_stmt_collect_labels(arena, n->aux.alt, pos, names, pos_arr, nnames);
            break;
        case NODE_WHILE:
        case NODE_DO_WHILE:
            zc_stmt_collect_labels(arena, n->right, pos, names, pos_arr, nnames);
            break;
        case NODE_FOR:
            zc_stmt_collect_labels(arena, n->aux.alt, pos, names, pos_arr, nnames);
            break;
        case NODE_SWITCH:
            zc_stmt_collect_labels(arena, n->right, pos, names, pos_arr, nnames);
            break;
        default:
            break;
    }
}

static void zc_list_collect_gotos(ZeroCCASTArena *arena, ASTRef ref, int pos,
                                  const char **gotos, int *gpos, int *ngoto) {
    ASTRef s = ref;
    while (s != AST_NULL && *ngoto < ZC_VLA_GOTO_MAX) {
        zc_stmt_collect_gotos(arena, s, pos, gotos, gpos, ngoto);
        ZeroCCASTNode *n = zero_cc_ast_get(arena, s);
        if (!n) break;
        s = n->next;
    }
}

static void zc_stmt_collect_gotos(ZeroCCASTArena *arena, ASTRef ref, int pos,
                                  const char **gotos, int *gpos, int *ngoto) {
    if (ref == AST_NULL || *ngoto >= ZC_VLA_GOTO_MAX) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return;
    switch (n->tag) {
        case NODE_GOTO:
            if (n->s.str) {
                gotos[*ngoto] = n->s.str;
                gpos[*ngoto] = pos;
                (*ngoto)++;
            }
            break;
        case NODE_BLOCK:
            zc_list_collect_gotos(arena, n->left, pos, gotos, gpos, ngoto);
            break;
        case NODE_IF:
            zc_stmt_collect_gotos(arena, n->right, pos, gotos, gpos, ngoto);
            zc_stmt_collect_gotos(arena, n->aux.alt, pos, gotos, gpos, ngoto);
            break;
        case NODE_WHILE:
        case NODE_DO_WHILE:
            zc_stmt_collect_gotos(arena, n->right, pos, gotos, gpos, ngoto);
            break;
        case NODE_FOR:
            zc_stmt_collect_gotos(arena, n->aux.alt, pos, gotos, gpos, ngoto);
            break;
        case NODE_SWITCH:
            zc_stmt_collect_gotos(arena, n->right, pos, gotos, gpos, ngoto);
            break;
        case NODE_LABEL:
            zc_stmt_collect_gotos(arena, n->left, pos, gotos, gpos, ngoto);
            break;
        default:
            break;
    }
}

void lower_stmt_block(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    /* Track block depth for VLA hybrid rule enforcement */
    c->lowering.vla.block_depth++;

    if (c->lowering.vla.block_depth == 1) {
        const char *gotos[ZC_VLA_GOTO_MAX];
        int gpos[ZC_VLA_GOTO_MAX];
        int ngoto = 0;
        const char *label_names[ZC_VLA_GOTO_MAX];
        int label_idx[ZC_VLA_GOTO_MAX];
        int nlabels = 0;
        int vla_idx[ZC_VLA_GOTO_MAX];
        int nvla = 0;

        int total = 0;
        int i = 0;
        ASTRef s = ref;
        while (s != AST_NULL) {
            ZeroCCASTNode *n = zero_cc_ast_get(arena, s);
            if (!n) break;
            if (n->tag == NODE_DECL && (n->flags & NODE_DECL_IS_VLA) &&
                nvla < ZC_VLA_GOTO_MAX) {
                vla_idx[nvla++] = i;
            }
            zc_stmt_collect_gotos(arena, s, i, gotos, gpos, &ngoto);
            zc_stmt_collect_labels(arena, s, i, label_names, label_idx, &nlabels);
            s = n->next;
            i++;
        }
        total = i;

        for (int g = 0; g < ngoto && g < ZC_VLA_GOTO_MAX; g++) {
            int t = -1;
            for (int l = 0; l < nlabels; l++) {
                if (strcmp(label_names[l], gotos[g]) == 0) { t = label_idx[l]; break; }
            }
            if (t < 0) {
                /* Label not found in the top-level list (should not happen
                 * for valid C); be conservative. */
                t = total; /* past the end: only forward-crossings possible */
            }
            int gi = gpos[g];
            for (int v = 0; v < nvla; v++) {
                if ((t < vla_idx[v] && gi >= vla_idx[v]) ||
                    (gi < vla_idx[v] && t >= vla_idx[v])) {
                    ZeroCCASTNode *bn = zero_cc_ast_get(arena, ref);
                    diag_report_error(&c->diag_buf,
                                      bn ? bn->line : 1, bn ? bn->col : 1,
                                      NULL,
                                      "goto '%s' crosses a VLA declaration: stack rebalancing on jumps is not supported (VLA must be in a scope without crossing gotos)",
                                      gotos[g]);
                }
            }
        }
    }

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
        /* Use the declaration's own stashed symbol (aux.step) rather than a
         * by-name lookup: after a preceding nested block, local_lookup_bound
         * is capped at the block-entry symbol count, which excludes this
         * declaration's own (later) symbol and silently drops the VLA
         * allocation (vt5: `{ int k = 1; } int x[n];` -> x never allocated,
         * segfault on first use). */
        ZeroCCSym *sym = lowering_local_sym(c, node, var);
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
            ((str_sym->type.base->tag == TYPE_INT8 || str_sym->type.base->tag == TYPE_UINT8) ||
             ((str_sym->type.base->tag == TYPE_INT32 || str_sym->type.base->tag == TYPE_UINT32) && (str_init_node->flags & NODE_FLAG_WIDE)))) {
            bool is_wide = (str_init_node->flags & NODE_FLAG_WIDE) != 0;
            size_t term_sz = is_wide ? 4 : 1;
            const char *lit = str_init_node->s.str;
            size_t total_size = str_sym->type.size;
            size_t copy_len = str_init_node->s_len + term_sz; /* may contain embedded NULs */
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
            if (is_wide) {
                size_t n_words = str_init_node->s_len / 4;
                const uint32_t *wlit = (const uint32_t *)lit;
                for (size_t i = 0; i < n_words && (i + 1) * 4 <= total_size; i++) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_mov_imm(val_temp, (int32_t)wlit[i]);
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, -(int32_t)(str_sym->stack_offset - (int32_t)(i * 4)));
                    ir_emit_store_32(val_temp, addr_temp);
                }
            } else {
                for (size_t i = 0; i < copy_len; i++) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_mov_imm(val_temp, (int32_t)(unsigned char)lit[i]);
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(addr_temp, TEMP_NONE, -(int32_t)(str_sym->stack_offset - (int32_t)i));
                    ir_emit_store_byte(val_temp, addr_temp);
                }
            }
            c->last_expr_is_float = 0;
            return;
        }
        int result_temp = lower_expr(c, arena, node->left);
        const char *var = node->s.str;
        ZeroCCSym *sym = lowering_local_sym(c, node, var);
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
                if (sym->type.tag == TYPE_FLOAT) {
                    int addr = c->next_temp_id++;
                    ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                    ir_emit_vstoress((uint16_t)addr, (uint16_t)float_temp);
                } else {
                    ir_emit_fstore((uint16_t)float_temp, TEMP_NONE, -sym->stack_offset);
                }
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
                        ZeroCCSym *src_sym = (init_node && init_node->tag == NODE_IDENT) ? zero_cc_find_local(c, init_node->s.str) : NULL;
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 16) {
                            if (!is_inplace_brace_init) {
                                int num_chunks = (sym->type.size + 7) / 8;
                                if (src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION)) {
                                    for (int i = 0; i < num_chunks; i++) {
                                        int chunk_temp = c->next_temp_id++;
                                        ir_emit_load(chunk_temp, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
                                        ir_emit_store(chunk_temp, BE(c)->fp_reg, -(sym->stack_offset - i * 8));
                                    }
                                } else {
                                    /* System V AMD64 ABI: sret buffer pointer in result_temp.
                                     * Spill result_temp to a stack slot so offset additions don't clobber it.
                                     * Copy chunks from [result_temp + i*8] to local stack -(sym->stack_offset - i*8) */
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    int32_t buf_ptr_off = c->current_stack_frame;
                                    ir_emit_store(result_temp, BE(c)->fp_reg, -buf_ptr_off);

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
                            }
                        } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                   is_inplace_brace_init) {
                            /* Struct/union already initialized in-place by brace initializer */
                        } else if (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type)) {
                            /* GNU vector declaration initializer. The RHS form:
                             *  - NODE_CALL: value in the VREG_0/VREG_2 pair;
                             *  - vector local ident: value in its frame slot;
                             *  - anything else (in-place brace init, binop
                             *    temp, compound literal, deref): ADDRESS of
                             *    a vector object. Copy 8-byte chunks into
                             *    the new slot. */
                            if (is_inplace_brace_init) {
                                /* Already initialized in place. */
                            } else {
                                int num_chunks = (int)((sym->type.size + 7) / 8);
                                ZeroCCASTNode *lnode = zero_cc_ast_get(arena, node->left);
                                ZeroCCSym *src_sym = (lnode && lnode->tag == NODE_IDENT) ? zero_cc_find_local(c, lnode->s.str) : NULL;
                                bool src_is_vec_ident = src_sym && src_sym->type.tag == TYPE_USER_0 &&
                                                        zero_cc_type_is_vector(&src_sym->type);
                                /* A by-value call result is already
                                 * materialized into a stack object; its
                                 * result_temp is the object's ADDRESS, so
                                 * it falls into the generic address form. */
                                for (int i = 0; i < num_chunks; i++) {
                                    int ch_val = c->next_temp_id++;
                                    if (src_is_vec_ident) {
                                        if (src_sym->is_promoted) {
                                            ch_val = src_sym->promoted_vreg;  /* 8B single-reg */
                                        } else {
                                            ir_emit_load(ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
                                        }
                                    } else {
                                        if (i == 0) {
                                            ir_emit_deref(ch_val, result_temp);
                                        } else {
                                            int o8 = c->next_temp_id++;
                                            ir_emit_mov_imm(o8, i * 8);
                                            int a2 = c->next_temp_id++;
                                            ir_emit_add(a2, result_temp, o8);
                                            ir_emit_deref(ch_val, a2);
                                        }
                                    }
                                    ir_emit_store(ch_val, BE(c)->fp_reg, -(sym->stack_offset - i * 8));
                                }
                            }
                        } else {
                            /* Integer or small struct assignment: use IR store */
                            bool rhs_is_struct_addr = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                                      ast_expr_is_struct_addr(c, arena, node->left);
                            if (rhs_is_struct_addr) {
                                if (sym->type.size <= 8) {
                                    int val_temp = c->next_temp_id++;
                                    if (sym->type.size == 1) ir_emit_load_byte(val_temp, result_temp);
                                    else if (sym->type.size == 2) ir_emit_load_16(val_temp, result_temp);
                                    else if (sym->type.size == 4) ir_emit_load_32(val_temp, result_temp);
                                    else ir_emit_deref(val_temp, result_temp);
                                    ir_emit_store(val_temp, BE(c)->fp_reg, -sym->stack_offset);
                                } else {
                                    int val1 = c->next_temp_id++;
                                    ir_emit_deref(val1, result_temp);
                                    ir_emit_store(val1, BE(c)->fp_reg, -sym->stack_offset);

                                    int off8 = c->next_temp_id++;
                                    ir_emit_mov_imm(off8, 8);
                                    int addr2 = c->next_temp_id++;
                                    ir_emit_add(addr2, result_temp, off8);

                                    int val2 = c->next_temp_id++;
                                    size_t rem = sym->type.size - 8;
                                    if (rem == 1) ir_emit_load_byte(val2, addr2);
                                    else if (rem == 2) ir_emit_load_16(val2, addr2);
                                    else if (rem == 4) ir_emit_load_32(val2, addr2);
                                    else ir_emit_deref(val2, addr2);
                                    ir_emit_store(val2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                                }
                            } else {
                                ZeroCCASTNode *lnode = zero_cc_ast_get(arena, node->left);
                                ZeroCCSym *src_sym = (lnode && lnode->tag == NODE_IDENT) ? zero_cc_find_local(c, lnode->s.str) : NULL;
                                if (src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION) && sym->type.size > 16) {
                                    int num_chunks = (int)((sym->type.size + 7) / 8);
                                    for (int ch = 0; ch < num_chunks; ch++) {
                                        int ch_val = c->next_temp_id++;
                                        ir_emit_load(ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - ch * 8));
                                        ir_emit_store(ch_val, BE(c)->fp_reg, -(sym->stack_offset - ch * 8));
                                    }
                                } else {
                                    ir_emit_store(result_temp, BE(c)->fp_reg, -sym->stack_offset);
                                    if (c->isa == ISA_RISCV32 && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 4 && sym->type.size <= 8) {
                                        /* RV32 ILP32 ABI: second 4-byte chunk returned in a1 (VREG_2) */
                                        ir_emit_store(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 4));
                                    } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16) {
                                        /* System V AMD64 ABI: second eightbyte returned in rdx (VREG_2)
                                         * Canonical secondary value register: VREG_2 maps to rdx/x1/a1
                                         * via regalloc_get_reg_for_temp. */
                                        ir_emit_store(VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                                    }
                                }
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
            if (!c->last_expr_is_float && c->lowering.inlining.return_type.size > 0) {
                zero_cc_emit_int_extend(c, res, &c->lowering.inlining.return_type);
            }
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
        if (sym && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION ||
                    (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type)))) {
            if (sym->is_promoted) {
                ir_emit_mov_reg(VREG_0, sym->promoted_vreg);
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }
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
                /* System V AMD64 ABI: 9-16 byte struct returned across rax and rdx
                 * Canonical secondary value register: VREG_2 maps to rdx/x1/a1
                 * via regalloc_get_reg_for_temp. */
                // 1. Load lower 8 bytes into rax (VREG_0) via IR
                ir_emit_load(VREG_0, BE(c)->fp_reg, struct_offset);

                // 2. Load upper 8 bytes into rdx (VREG_2) via IR
                ir_emit_load(VREG_2, BE(c)->fp_reg, struct_offset + 8);

                // 3. Jump to epilogue (which will emit leave; ret)
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }

            /* System V AMD64 ABI: >16 byte struct returned via hidden pointer (sret) */
            /* RDI contains the hidden pointer to caller's buffer */
            else {
                ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
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

        /* Global struct/union return (`return x;` where x is a file-scope
         * struct): the local-ident branch above only sees function-scope
         * symbols, so a 9-16B global struct would fall through to the
         * generic path and return only its first 8 bytes in VREG_0
         * (c-torture pr60017). Load the full register pair / sret chunks
         * directly from the global. */
        ZeroCCGlobalVar *gsym = (ret_expr && ret_expr->tag == NODE_IDENT) ? find_global_var(ret_expr->s.str) : NULL;
        if (gsym && (gsym->type.tag == TYPE_STRUCT || gsym->type.tag == TYPE_UNION) &&
            gsym->type.size > 0) {
            int gaddr = c->next_temp_id++;
            if (gsym->is_tls && !c->jit_mode) ir_emit_tls_lea(gaddr, gsym->name);
            else ir_emit_mov_global(gaddr, gsym->name);
            if (gsym->type.size <= 16) {
                ir_emit_deref(VREG_0, gaddr);
                if (gsym->type.size > 8) {
                    int off_t = c->next_temp_id++;
                    ir_emit_mov_imm(off_t, 8);
                    int a2 = c->next_temp_id++;
                    ir_emit_add(a2, gaddr, off_t);
                    ir_emit_deref(VREG_2, a2);
                }
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }
            /* >16B: sret chunk copy into the caller's hidden-pointer buffer */
            ZeroCCFunction *gf = zero_cc_get_current_fn(c);
            int32_t gs_off = gf ? gf->sret_stack_offset : 0;
            int g_chunks = (int)((gsym->type.size + 7) / 8);
            for (int i = 0; i < g_chunks; i++) {
                int s_base = gaddr;
                if (i > 0) {
                    int s_off = c->next_temp_id++;
                    ir_emit_mov_imm(s_off, i * 8);
                    s_base = c->next_temp_id++;
                    ir_emit_add(s_base, gaddr, s_off);
                }
                int d_base = c->next_temp_id++;
                ir_emit_load(d_base, BE(c)->fp_reg, -gs_off);
                if (i > 0) {
                    int d_off = c->next_temp_id++;
                    ir_emit_mov_imm(d_off, i * 8);
                    int d_addr = c->next_temp_id++;
                    ir_emit_add(d_addr, d_base, d_off);
                    int gchunk = c->next_temp_id++;
                    ir_emit_deref(gchunk, s_base);
                    ir_emit_store_64(gchunk, d_addr);
                } else {
                    int gchunk = c->next_temp_id++;
                    ir_emit_deref(gchunk, s_base);
                    ir_emit_store_64(gchunk, d_base);
                }
            }
            ir_emit_load(VREG_0, BE(c)->fp_reg, -gs_off);
            ir_emit_jmp(c->lowering.epilogue_label);
            return;
        }

        int result_temp = lower_expr(c, arena, node->left);
        ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
        {
            ZeroCCType fn_ret = zero_cc_get_current_fn_ret_type(c);
            if ((fn_ret.tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn_ret)) &&
                fn_ret.size <= 16 && !c->last_expr_is_float) {
                if (ret_expr->tag == NODE_CALL) {
                    /* Nested call returning a vector: the result is already
                     * in the VREG_0/VREG_2 register pair. */
                    ir_emit_jmp(c->lowering.epilogue_label);
                    return;
                }
                /* Vector return value (cast / binop / compound literal): the
                 * expression lowered to the address of a 16/8B stack object;
                 * load the 8-byte chunks into the register pair. */
                ir_emit_deref(VREG_0, result_temp);
                if (fn_ret.size > 8) {
                    int off = c->next_temp_id++;
                    ir_emit_mov_imm(off, 8);
                    int a2 = c->next_temp_id++;
                    ir_emit_add(a2, result_temp, off);
                    int ch2 = c->next_temp_id++;
                    ir_emit_deref(ch2, a2);
                    ir_emit_mov_reg(VREG_2, ch2);
                }
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }
        }
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
        /* Struct/union return from a non-ident expression (array element,
         * deref, member, compound literal): the expression lowers to the
         * ADDRESS of the struct object - storing result_temp would put the
         * pointer in rax, and a 9-16B return also needs VREG_2. A nested
         * call already placed the value in the ABI registers/sret buffer.
         * (c-torture execute/pr82388: `return h[29];` on a 12B struct.) */
        {
            ZeroCCType fn_ret_s = zero_cc_get_current_fn_ret_type(c);
            if ((fn_ret_s.tag == TYPE_STRUCT || fn_ret_s.tag == TYPE_UNION) && fn_ret_s.size > 0) {
                if (ret_expr->tag == NODE_CALL) {
                    ir_emit_jmp(c->lowering.epilogue_label);
                    return;
                }
                if (ret_expr->tag == NODE_UNOP &&
                    (ret_expr->aux.op == '*' || ret_expr->aux.op == TK_STAR) &&
                    fn_ret_s.size > 8 && fn_ret_s.size <= 16) {
                    /* `return *p;` on a 9-16B struct: the deref lowering
                     * only loads the first 8 bytes into result_temp; load
                     * the full ABI pair straight from the pointee. */
                    int ptr_t = lower_expr(c, arena, ret_expr->left);
                    ir_emit_deref(VREG_0, ptr_t);
                    int roff = c->next_temp_id++;
                    ir_emit_mov_imm(roff, 8);
                    int ra2 = c->next_temp_id++;
                    ir_emit_add(ra2, ptr_t, roff);
                    int rch2 = c->next_temp_id++;
                    ir_emit_deref(rch2, ra2);
                    ir_emit_mov_reg(VREG_2, rch2);
                    ir_emit_jmp(c->lowering.epilogue_label);
                    return;
                }
                if (ast_expr_is_struct_addr(c, arena, node->left)) {
                    if (fn_ret_s.size <= 8) {
                        ir_emit_deref(VREG_0, result_temp);
                    } else if (fn_ret_s.size <= 16) {
                        ir_emit_deref(VREG_0, result_temp);
                        int roff = c->next_temp_id++;
                        ir_emit_mov_imm(roff, 8);
                        int ra2 = c->next_temp_id++;
                        ir_emit_add(ra2, result_temp, roff);
                        int rch2 = c->next_temp_id++;
                        ir_emit_deref(rch2, ra2);
                        ir_emit_mov_reg(VREG_2, rch2);
                    }
                } else if (result_temp != VREG_0) {
                    ir_emit_mov_reg(VREG_0, result_temp);
                }
                ir_emit_jmp(c->lowering.epilogue_label);
                return;
            }
        }
        ZeroCCType fn_ret_type = zero_cc_get_current_fn_ret_type(c);
        int final_float_temp = result_temp;
        if (!c->last_expr_is_float) {
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
        } else {
            /* Expression is float/double. If function returns integer, convert float to int */
            if (fn_ret_type.tag != TYPE_FLOAT && fn_ret_type.tag != TYPE_DOUBLE) {
                int int_res = c->next_temp_id++;
                ir_emit_cvttsd2si((uint16_t)int_res, (uint16_t)result_temp);
                zero_cc_emit_int_extend(c, int_res, &fn_ret_type);
                ir_emit_mov_reg(VREG_0, int_res);
                c->last_expr_is_float = 0;
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

static int lower_condition_as_int(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef expr_ref) {
    int cond_temp = lower_expr(c, arena, expr_ref);
    if (c->last_expr_is_float) {
        int zero_i = c->next_temp_id++;
        ir_emit_mov_imm(zero_i, 0);
        int zero_f = c->next_temp_id++;
        ir_emit_cvtsi2sd((uint16_t)zero_f, (uint16_t)zero_i);
        int cmp_i = c->next_temp_id++;
        ir_emit_cmp(cond_temp, zero_f);
        ir_emit_cond_set(cmp_i, CC_NE);
        cond_temp = cmp_i;
        c->last_expr_is_float = 0;
    }
    return cond_temp;
}

void lower_stmt_if(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    int cond_temp = lower_condition_as_int(c, arena, node->left);
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
    int cond_temp = lower_condition_as_int(c, arena, node->left);
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
    int cond_temp = lower_condition_as_int(c, arena, node->left);
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
    int32_t saved_for_bound = c->local_lookup_bound;
    if (node->aux.op != 0) c->local_lookup_bound = (int32_t)(node->aux.op - 1);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_step = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_step);
    ir_emit_label(loop_start);
    if (node->right != AST_NULL) {
        int cond_temp = lower_condition_as_int(c, arena, node->right);
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

static void collect_switch_cases(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref, int cond_temp, uint16_t *default_label) {
    while (ref != AST_NULL) {
        ZeroCCASTNode *sn = zero_cc_ast_get(arena, ref);
        if (!sn) break;
        if (sn->tag == NODE_SWITCH) {
            /* Do not recurse into inner switch statements (their cases belong to the inner switch) */
            ref = sn->next;
            continue;
        }
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
            if (sn->right != AST_NULL) {
                collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_DEFAULT) {
            sn->aux.alt = c->ir_next_label++;
            *default_label = (uint16_t)sn->aux.alt;
            if (sn->left != AST_NULL) {
                collect_switch_cases(c, arena, sn->left, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_BLOCK) {
            collect_switch_cases(c, arena, sn->left, cond_temp, default_label);
        } else if (sn->tag == NODE_IF) {
            collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
            if (sn->aux.alt != AST_NULL) {
                collect_switch_cases(c, arena, (ASTRef)sn->aux.alt, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_WHILE || sn->tag == NODE_DO_WHILE || sn->tag == NODE_FOR) {
            collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
        } else if (sn->tag == NODE_LABEL) {
            collect_switch_cases(c, arena, sn->left, cond_temp, default_label);
        }
        ref = sn->next;
    }
}

void lower_stmt_switch(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;

    uint16_t switch_exit = c->ir_next_label++;

    /* break inside a switch jumps to the switch exit */
    label_patcher_enter_loop(&c->label_patcher, switch_exit, 0);

    int cond_temp = lower_expr(c, arena, node->left);

    /* Dispatch: recursively scan the switch body for all case and default labels
     * (supporting Duff's device, cases nested in if/while/for/blocks).
     * C3 fix: no case cap - dispatch is O(n) CMP/JNE and the per-function
     * temp guard (C2b, 65534 temps) bounds the case count (~2 temps per
     * case -> ~32k cases), which stays under the 16-bit label space. */
    uint16_t default_label = 0;
    if (node->right != AST_NULL) {
        collect_switch_cases(c, arena, node->right, cond_temp, &default_label);
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
            if (c->lowering.inlining.active_depth > 0) {
                lower_stmt_block(c, arena, node->left);
                break;
            }
            /* Block scope (C11 6.2.1): a statement in this list sees the
             * declarations made before it in this block plus everything in
             * enclosing scope - not the block's own declarations made after
             * it (a name redeclared later in a nested sibling block would
             * otherwise shadow earlier references in the flat latest-wins
             * table).  Start the list at the entry bound (aux.alt - 1, or
             * the enclosing bound when larger) and let each lowered
             * declaration raise it; on exit restore the entry bound - the
             * block's own declarations were marked scope-ended below, so
             * later outer statements skip them. */
            int32_t saved_bound = c->local_lookup_bound;
            int32_t entry_bound = (node->aux.alt != 0) ? (int32_t)(node->aux.alt - 1) : saved_bound;
            int32_t list_bound = (saved_bound < 0) ? entry_bound : ((entry_bound > saved_bound) ? entry_bound : saved_bound);
            c->local_lookup_bound = list_bound;
            lower_stmt_block(c, arena, node->left);
            c->local_lookup_bound = list_bound;
            /* C11 6.2.1 scope end: this block's own declarations (indices
             * [aux.alt-1, aux.step-2]) go out of scope at the closing brace.
             * The flat latest-wins table + single numeric bound cannot
             * express that for shadowed names (an outer-scope reference
             * before or after the block would still resolve to the inner
             * declaration), so mark them and let zero_cc_find_local skip
             * them.  Index-based access (lowering_local_sym, the prologue /
             * epilogue static-local mirrors) is unaffected. */
            if (node->aux.alt != 0 && node->aux.step != 0 && node->aux.step > node->aux.alt) {
                for (size_t si = node->aux.alt - 1; si + 1 < node->aux.step && si < c->local_count; si++) {
                    c->locals[si].scope_ended = true;
                }
            }
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
                if (c->local_lookup_bound >= 0 && node->aux.step != 0) {
                    if ((int32_t)node->aux.step > c->local_lookup_bound) {
                        c->local_lookup_bound = (int32_t)node->aux.step;
                    }
                }
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

