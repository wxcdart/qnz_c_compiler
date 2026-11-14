/* AST Lowering - Control Flow Statement Lowering (if/while/for/switch/goto)
 *
 * Extracted from ast_lowering_stmt.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"

static int lower_condition_as_int(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef expr_ref) {
    TypeInfo *cond_ti = (expr_ref != AST_NULL) ? ast_node_get_typeinfo(c, zero_cc_ast_get(arena, expr_ref)) : NULL;
    int cond_temp = lower_expr(c, arena, expr_ref);
    if (cond_ti && cond_ti->kind == TYPE_KIND_COMPLEX) {
        size_t esz = (cond_ti->details.array.base) ? type_size(cond_ti->details.array.base) : 8;
        if (esz != 4 && esz != 8) esz = 8;
        int rh = zc_alloc_temp(c);
        int li = zc_alloc_temp(c);
        if (esz == 4) {
            ir_emit_vloadss(c, rh, (uint32_t)cond_temp);
            int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, 4);
            int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, cond_temp, o2);
            ir_emit_vloadss(c, li, (uint32_t)a2);
        } else {
            ir_emit_vloadsd(c, rh, (uint32_t)cond_temp);
            int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, 8);
            int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, cond_temp, o2);
            ir_emit_vloadsd(c, li, (uint32_t)a2);
        }
        int zf = zc_alloc_temp(c);
        ir_emit_fmov_imm(c, (uint32_t)zf, (int64_t)zc_rodata_double_offset(c, 0.0));
        int cr = zc_alloc_temp(c);
        ir_emit_cmp(c, rh, zf);
        ir_emit_cond_set(c, cr, CC_NE);
        int ci = zc_alloc_temp(c);
        ir_emit_cmp(c, li, zf);
        ir_emit_cond_set(c, ci, CC_NE);
        int res = zc_alloc_temp(c);
        ir_emit_or(c, res, cr, ci);
        c->last_expr_is_float = 0;
        return res;
    }
    if (c->last_expr_is_float) {
        int zero_i = zc_alloc_temp(c);
        ir_emit_mov_imm(c, zero_i, 0);
        int zero_f = zc_alloc_temp(c);
        ir_emit_cvtsi2sd(c, (uint32_t)zero_f, (uint32_t)zero_i);
        int cmp_i = zc_alloc_temp(c);
        ir_emit_cmp(c, cond_temp, zero_f);
        ir_emit_cond_set(c, cmp_i, CC_NE);
        cond_temp = cmp_i;
        c->last_expr_is_float = 0;
    } else if (c->isa == ISA_RISCV32 && c->last_expr_is_64) {
        int zero64 = zc_alloc_temp(c);
        ir_emit_mov64_imm(c, zero64, 0);
        ir_emit_cmp64(c, cond_temp, zero64);
        int cmp_i = zc_alloc_temp(c);
        ir_emit_cond_set(c, cmp_i, CC_NE);
        cond_temp = cmp_i;
        c->last_expr_is_64 = 0;
    }
    return cond_temp;
}

void lower_stmt_if(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    int cond_temp = lower_condition_as_int(c, arena, node->left);
    uint16_t else_label = c->ir_next_label++;
    uint16_t end_label = c->ir_next_label++;
    ir_emit_je(c, (uint32_t)cond_temp, else_label);
    lower_node(c, arena, node->right);
    ir_emit_jmp(c, end_label);
    ir_emit_label(c, else_label);
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(c, end_label);
}

void lower_stmt_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    c->lowering.vla.loop_depth++;
    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_start);
    ir_emit_label(c, loop_start);
    int cond_temp = lower_condition_as_int(c, arena, node->left);
    ir_emit_je(c, (uint32_t)cond_temp, loop_exit);
    lower_node(c, arena, node->right);
    ir_emit_jmp(c, loop_start);
    ir_emit_label(c, loop_exit);
    label_patcher_exit_loop(&c->label_patcher);
    c->lowering.vla.loop_depth--;
}

void lower_stmt_do_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    c->lowering.vla.loop_depth++;
    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_cond = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_cond);
    ir_emit_label(c, loop_start);
    lower_node(c, arena, node->right);
    ir_emit_label(c, loop_cond);
    int cond_temp = lower_condition_as_int(c, arena, node->left);
    ir_emit_jne(c, (uint32_t)cond_temp, loop_start);
    ir_emit_label(c, loop_exit);
    label_patcher_exit_loop(&c->label_patcher);
    c->lowering.vla.loop_depth--;
}

void lower_stmt_for(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    c->lowering.vla.loop_depth++;
    if (!node) return;
    int32_t saved_for_bound = c->local_lookup_bound;
    if (node->aux.op != 0) c->local_lookup_bound = (int32_t)(node->aux.op - 1);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_step = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_step);
    ir_emit_label(c, loop_start);
    if (node->right != AST_NULL) {
        int cond_temp = lower_condition_as_int(c, arena, node->right);
        ir_emit_je(c, (uint32_t)cond_temp, loop_exit);
    }
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(c, loop_step);
    if (node->aux.step != AST_NULL) {
        lower_expr(c, arena, node->aux.step);
    }
    ir_emit_jmp(c, loop_start);
    ir_emit_label(c, loop_exit);
    label_patcher_exit_loop(&c->label_patcher);
    c->local_lookup_bound = saved_for_bound;
    c->lowering.vla.loop_depth--;
}

static void collect_switch_cases(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref, int cond_temp, uint16_t *default_label) {
    while (ref != AST_NULL) {
        ZeroCCASTNode *sn = zero_cc_ast_get(arena, ref);
        if (!sn) break;
        if (sn->tag == NODE_SWITCH) {
            ref = sn->next;
            continue;
        }
        if (sn->tag == NODE_CASE) {
            sn->aux.alt = c->ir_next_label++;
            uint16_t case_label = (uint32_t)sn->aux.alt;
            if (sn->aux.step != 0) {
                ZeroCCASTNode *low_n = zero_cc_ast_get(arena, sn->left);
                ZeroCCASTNode *high_n = zero_cc_ast_get(arena, (ASTRef)sn->aux.step);
                bool range_unsigned = zero_cc_binop_is_unsigned(c, arena, sn->left, (ASTRef)sn->aux.step) ||
                                      (low_n && high_n && low_n->tag == NODE_INT &&
                                       high_n->tag == NODE_INT &&
                                       low_n->int_val >= 0 && high_n->int_val < 0);
                int low_temp = lower_expr(c, arena, sn->left);
                int high_temp = lower_expr(c, arena, (ASTRef)sn->aux.step);
                int flag_low = zc_alloc_temp(c);
                ir_emit_cmp(c, cond_temp, low_temp);
                ir_emit_cond_set(c, flag_low, range_unsigned ? CC_FLT_GE : CC_GE);
                int flag_high = zc_alloc_temp(c);
                ir_emit_cmp(c, cond_temp, high_temp);
                ir_emit_cond_set(c, flag_high, range_unsigned ? CC_FLT_LE : CC_LE);
                int in_range = zc_alloc_temp(c);
                ir_emit_and(c, in_range, flag_low, flag_high);
                ir_emit_jne(c, (uint32_t)in_range, case_label);
            } else {
                int val_temp = lower_expr(c, arena, sn->left);
                int flag_temp = zc_alloc_temp(c);
                ir_emit_cmp(c, cond_temp, val_temp);
                ir_emit_cond_set(c, flag_temp, CC_EQ);
                ir_emit_jne(c, (uint32_t)flag_temp, case_label);
            }
            if (sn->right != AST_NULL) {
                collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_DEFAULT) {
            sn->aux.alt = c->ir_next_label++;
            *default_label = (uint32_t)sn->aux.alt;
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
    label_patcher_enter_loop(&c->label_patcher, switch_exit, 0);
    int cond_temp = lower_expr(c, arena, node->left);
    uint16_t default_label = 0;
    if (node->right != AST_NULL) {
        collect_switch_cases(c, arena, node->right, cond_temp, &default_label);
    }
    if (default_label) {
        ir_emit_jmp(c, default_label);
    } else {
        ir_emit_jmp(c, switch_exit);
    }
    lower_node(c, arena, node->right);
    ir_emit_label(c, switch_exit);
    label_patcher_exit_loop(&c->label_patcher);
}

void lower_stmt_break(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    uint16_t lbl = label_patcher_get_break_label(&c->label_patcher);
    if (lbl) {
        ir_emit_jmp(c, lbl);
    }
}

void lower_stmt_continue(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    uint16_t lbl = label_patcher_get_continue_label(&c->label_patcher);
    if (lbl) {
        ir_emit_jmp(c, lbl);
    }
}

uint16_t get_or_create_user_ir_label(ZeroCCCompiler *c, const char *name) {
    uint16_t lbl = label_patcher_find_or_create_user_label(&c->label_patcher, name);
    if (lbl == 0) {
        lbl = c->ir_next_label++;
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
    ir_emit_label(c, lbl);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
}

void lower_stmt_goto(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->left != AST_NULL) {
        int target_temp = lower_expr(c, arena, node->left);
        ir_emit_jmp_ind(c, target_temp);
        return;
    }
    if (!node->s.str) return;

    ZeroCCFunction *cur = &c->functions[c->lowering.current_func_idx];
    if (cur->is_nested && cur->goto_pair_count > 0) {
        const char *nm = node->s.str;
        bool own = false;
        for (int li = 0; li < cur->nl_label_count; li++) {
            if (cur->nl_labels[li] && strcmp(cur->nl_labels[li], nm) == 0) own = true;
        }
        if (!own) {
            size_t chain[64];
            int chain_n = 0;
            for (size_t ai = cur->nested_enclosing_fn_idx;
                 ai != (size_t)-1 && chain_n < 64; ) {
                chain[chain_n++] = ai;
                if (!c->functions[ai].is_nested) break;
                ai = c->functions[ai].nested_enclosing_fn_idx;
            }
            for (int ci = 0; ci < chain_n; ci++) {
                size_t ai = chain[ci];
                ZeroCCFunction *af = &c->functions[ai];
                for (int li = 0; li < af->nl_label_count; li++) {
                    if (!af->nl_labels[li] || strcmp(af->nl_labels[li], nm) != 0) continue;
                    int pair = chain_n - 1 - ci;
                    char rn[48], sn[48];
                    snprintf(rn, sizeof(rn), "__nlgbp_%d", pair);
                    snprintf(sn, sizeof(sn), "__nlbsp_%d", pair);
                    ZeroCCSym *rb = zero_cc_find_local(c, rn);
                    ZeroCCSym *rs = zero_cc_find_local(c, sn);
                    if (rb && rs) {
                        int t1 = zc_alloc_temp(c);
                        zc_sym_load(c, rb, 0, t1);
                        int t2 = zc_alloc_temp(c);
                        zc_sym_load(c, rs, 0, t2);
                        ir_emit_nonlocal_goto(c, t1, t2, (int)ai,
                                              af->nl_label_ir[li]);
                        return;
                    }
                    break;
                }
            }
        }
    }

    uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
    ir_emit_jmp(c, lbl);
}
