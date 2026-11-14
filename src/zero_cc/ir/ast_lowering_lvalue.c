/* AST Lowering - Lvalue Address Resolution & Aggregate Copy Helpers
 *
 * Extracted from ast_lowering_expr.c.
 */

#include <stdio.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/isa/isa_backend.h"

/* C99 6.3.1.5 integer -> floating conversion, u64-aware (see header). */
int zc_emit_int_to_double(ZeroCCCompiler *c, int src_temp, const TypeInfo *src_ti) {
    bool u64 = src_ti && type_size(src_ti) == 8 &&
               ((src_ti->kind == TYPE_KIND_LONG && (src_ti->modifiers & TYPE_MOD_UNSIGNED) != 0) ||
                src_ti->kind == TYPE_KIND_POINTER ||
                (src_ti->modifiers & TYPE_MOD_UNSIGNED) != 0);
    int fp_res = zc_alloc_temp(c);
    if (!u64) {
        ir_emit_cvtsi2sd(c, (uint32_t)fp_res, (uint32_t)src_temp);
        c->last_expr_is_float = 1;
        return fp_res;
    }
    int zero_temp = zc_alloc_temp(c);
    ir_emit_mov_imm(c, zero_temp, 0);
    int is_ge = zc_alloc_temp(c);
    ir_emit_cmp(c, src_temp, zero_temp);
    ir_emit_cond_set(c, is_ge, CC_GE);
    uint16_t neg_label = c->ir_next_label++;
    uint16_t done_label = c->ir_next_label++;
    ir_emit_je(c, (uint32_t)is_ge, neg_label);
    ir_emit_cvtsi2sd(c, (uint32_t)fp_res, (uint32_t)src_temp);
    ir_emit_jmp(c, done_label);
    ir_emit_label(c, neg_label);
    int one_temp = zc_alloc_temp(c);
    ir_emit_mov_imm(c, one_temp, 1);
    int odd_temp = zc_alloc_temp(c);
    ir_emit_and(c, odd_temp, src_temp, one_temp);
    int shr_temp = zc_alloc_temp(c);
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = shr_temp,
        .temp_src1 = src_temp, .temp_src2 = one_temp});
    int adj_temp = zc_alloc_temp(c);
    ir_emit_or(c, adj_temp, shr_temp, odd_temp);
    int half_d = zc_alloc_temp(c);
    ir_emit_cvtsi2sd(c, (uint32_t)half_d, (uint32_t)adj_temp);
    ir_emit_fadd(c, (uint32_t)fp_res, (uint32_t)half_d, (uint32_t)half_d);
    ir_emit_label(c, done_label);
    c->last_expr_is_float = 1;
    return fp_res;
}

int zc_rodata_double_offset(ZeroCCCompiler *c, double val) {
    size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
    if (pad) {
        uint8_t z8[8] = {0};
        zero_cc_buf_put_bytes(&c->rodata_section, z8, pad);
    }
    size_t off = c->rodata_section.length;
    zero_cc_buf_put_bytes(&c->rodata_section, &val, sizeof(double));
    return (int)off;
}

int lower_member_chain_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *head) {
    ZeroCCASTNode *stack[128];
    int n = 0;
    ASTRef base_ref = AST_NULL;
    ZeroCCASTNode *curr = head;
    while (curr && curr->tag == NODE_MEMBER && n < 128) {
        stack[n++] = curr;
        base_ref = curr->left;
        curr = zero_cc_ast_get(arena, curr->left);
    }
    ZeroCCASTNode *base_node = curr;
    if (!base_node || n < 2) return TEMP_NONE;

    int addr = TEMP_NONE;
    bool generic_base = false;
    if (base_node->tag == NODE_IDENT) {
        ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
        ZeroCCGlobalVar *ggv = (sym && !sym->is_extern) ? NULL : find_global_var(c, base_node->s.str);
        if (sym && !sym->is_extern) {
            addr = zc_sym_addr(c, sym, 0);
        } else if (ggv) {
            addr = zc_alloc_temp(c);
            ir_emit_global_addr(c, addr, ggv);
        } else {
            generic_base = true;
        }
    } else if (base_node->tag == NODE_COMPOUND_LIT) {
        (void)lower_expr(c, arena, base_ref);
        ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str);
        if (sym) {
            addr = zc_alloc_temp(c);
            ir_emit_lea(c, addr, BE(c)->fp_reg, -sym->stack_offset);
        } else {
            generic_base = true;
        }
    } else {
        generic_base = true;
    }

    if (generic_base) {
        bool innermost_is_ptr = (stack[n - 1]->flags & NODE_MEMBER_IS_ARROW) != 0;
        if (!innermost_is_ptr) {
            addr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
        }
        if (addr == TEMP_NONE) {
            addr = lower_expr(c, arena, base_ref);
        }
        if (addr == TEMP_NONE) return TEMP_NONE;
    }

    for (int i = n - 1; i >= 0 && addr != TEMP_NONE; i--) {
        ZeroCCASTNode *m = stack[i];
        int32_t off = (int32_t)m->aux.alt;
        bool skip_load = (generic_base && i == n - 1);
        bool is_arr_base = zc_is_array_base(c, arena, m, (i == n - 1) ? base_node : NULL);
        if ((m->flags & NODE_MEMBER_IS_ARROW) && !skip_load && !is_arr_base) {
            int ptr = zc_alloc_temp(c);
            ir_emit_deref(c, ptr, addr);
            addr = ptr;
        }
        if (off != 0) {
            int o = zc_alloc_temp(c);
            ir_emit_mov_imm(c, o, off);
            int na = zc_alloc_temp(c);
            ir_emit_add(c, na, addr, o);
            addr = na;
        }
    }
    return addr;
}

int zero_cc_lower_lvalue_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return TEMP_NONE;
    ZeroCCASTNode *operand = zero_cc_ast_get(arena, ref);
    if (!operand) return TEMP_NONE;

    if (operand->tag == NODE_IDENT) {
        ZeroCCSym *sym = zero_cc_find_local(c, operand->s.str);
        if (sym && !sym->is_extern) {
            if (sym->is_vla) {
                int addr_temp = zc_alloc_temp(c);
                ir_emit_load(c, addr_temp, BE(c)->fp_reg, -sym->stack_offset);
                return addr_temp;
            }
            return zc_sym_addr(c, sym, 0);
        }
        ZeroCCGlobalVar *gv = find_global_var(c, operand->s.str);
        if (gv) {
            int addr_temp = zc_alloc_temp(c);
            ir_emit_global_addr(c, addr_temp, gv);
            return addr_temp;
        }
        ZeroCCFunction *fn = zero_cc_find_function(c, operand->s.str);
        if (fn) {
            int addr_temp = zc_lower_fn_value(c, fn);
            if (addr_temp >= 0) return addr_temp;
            return -1;
        }
    } else if (operand->tag == NODE_COMPOUND_LIT) {
        (void)lower_expr(c, arena, ref);
        ZeroCCSym *sym = lowering_local_sym(c, operand, operand->s.str);
        if (sym) {
            int addr_temp = zc_alloc_temp(c);
            ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
            return addr_temp;
        }
    } else if (operand->tag == NODE_MEMBER) {
        int32_t member_offset = 0;
        int chain_depth = 0;
        ZeroCCASTNode *curr = operand;
        ZeroCCASTNode *innermost = operand;
        ASTRef base_ref = ref;
        while (curr && curr->tag == NODE_MEMBER) {
            member_offset += (int32_t)curr->aux.alt;
            chain_depth++;
            innermost = curr;
            base_ref = curr->left;
            curr = zero_cc_ast_get(arena, curr->left);
        }
        ZeroCCASTNode *base_node = curr;
        int addr_temp = zc_alloc_temp(c);
        if (chain_depth >= 2) {
            int ch = lower_member_chain_addr(c, arena, operand);
            if (ch != TEMP_NONE) return ch;
        }
        bool base_is_ptr = (innermost && (innermost->flags & NODE_MEMBER_IS_ARROW)) != 0;
        if (base_is_ptr && zc_is_array_base(c, arena, innermost, base_node)) base_is_ptr = false;
        if (!base_is_ptr && base_node && base_node->tag == NODE_IDENT) {
            ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
            if (sym && !sym->is_extern) {
                if (sym->is_vla_struct) {
                    ir_emit_load(c, addr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    if (member_offset != 0) {
                        int off_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        int sum_temp = zc_alloc_temp(c);
                        ir_emit_add(c, sum_temp, addr_temp, off_temp);
                        return sum_temp;
                    }
                    return addr_temp;
                }
                if (zc_sym_uses_data_section(sym)) {
                    ir_emit_mov_global(c, (uint32_t)addr_temp, sym->static_global_name);
                    if (member_offset != 0) {
                        int off_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        int sum_temp = zc_alloc_temp(c);
                        ir_emit_add(c, sum_temp, addr_temp, off_temp);
                        return sum_temp;
                    }
                    return addr_temp;
                }
                int32_t total_offset = -sym->stack_offset + member_offset;
                ir_emit_lea(c, addr_temp, TEMP_NONE, total_offset);
                return addr_temp;
            }
            ZeroCCGlobalVar *bgv = find_global_var(c, base_node->s.str);
            if (bgv) {
                ir_emit_global_addr(c, addr_temp, bgv);
                if (member_offset != 0) {
                    int off_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, off_temp, member_offset);
                    int sum_temp = zc_alloc_temp(c);
                    ir_emit_add(c, sum_temp, addr_temp, off_temp);
                    addr_temp = sum_temp;
                }
                return addr_temp;
            }
        } else if (!base_is_ptr && base_node && base_node->tag == NODE_COMPOUND_LIT) {
            (void)lower_expr(c, arena, base_ref);
            ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str);
            if (sym) {
                int32_t total_offset = -sym->stack_offset + member_offset;
                ir_emit_lea(c, addr_temp, TEMP_NONE, total_offset);
                return addr_temp;
            }
        }
        int base_ptr = TEMP_NONE;
        if (!base_is_ptr) {
            base_ptr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
        }
        if (base_ptr == TEMP_NONE) {
            base_ptr = lower_expr(c, arena, base_ref);
        }
        if (member_offset != 0) {
            int off_temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off_temp, member_offset);
            ir_emit_add(c, addr_temp, base_ptr, off_temp);
        } else {
            ir_emit_mov_reg(c, addr_temp, base_ptr);
        }
        return addr_temp;
    } else if (operand->tag == NODE_INDEX) {
        int array_temp = lower_expr(c, arena, operand->left);
        int index_temp = lower_expr(c, arena, operand->right);
        size_t elem_size = 4;
        TypeInfo *elem_ti = ast_node_get_typeinfo(c, operand);
        if (elem_ti) {
            elem_size = type_size(elem_ti);
        } else {
            ZeroCCASTNode *ln = zero_cc_ast_get(arena, operand->left);
            if (ln && ln->tag == NODE_IDENT) {
                const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                if (zt && zt->base && zt->base->size > 0) {
                    elem_size = zt->base->size;
                }
            }
        }
        if (elem_size == 0) elem_size = 4;
        bool mirror_form = false;
        size_t mirror_esz = 1;
        {
            TypeInfo *idx_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, operand->right));
            mirror_form = (idx_ti != NULL && idx_ti->kind == TYPE_KIND_POINTER);
            if (mirror_form && elem_ti &&
                elem_ti->kind != TYPE_KIND_VOID && type_size(elem_ti) > 0)
                mirror_esz = type_size(elem_ti);
        }
        const ZeroCCType *vla_et = NULL;
        if (!mirror_form && elem_ti &&
            (elem_ti->kind == TYPE_KIND_STRUCT || elem_ti->kind == TYPE_KIND_UNION) &&
            elem_ti->details.record.is_vla_struct && elem_ti->details.record.tag) {
            ZeroCCType *reg = zero_cc_find_struct(c, elem_ti->details.record.tag);
            if (reg) vla_et = zc_find_vla_struct(reg);
        }
        if (vla_et && !vla_et->members && vla_et->struct_name[0]) {
            ZeroCCType *reg = zero_cc_find_struct(c, vla_et->struct_name);
            if (reg) vla_et = reg;
        }
        ZeroCCSym *vla_2d_sym = NULL;
        {
            ZeroCCASTNode *ln = zero_cc_ast_get(arena, operand->left);
            if (ln && ln->tag == NODE_IDENT) {
                ZeroCCSym *ls = zero_cc_find_local(c, ln->s.str);
                if (ls && ls->is_vla && ls->type.base && ls->type.base->tag == TYPE_ARRAY && ls->type.base->is_vla) {
                    vla_2d_sym = ls;
                }
            }
        }
        int scaled_index;
        if (mirror_form) {
            scaled_index = TEMP_NONE;
        } else if (vla_2d_sym) {
            int stride_temp = zc_alloc_temp(c);
            if (vla_2d_sym->is_param && vla_2d_sym->vla_slot_off > 0) {
                ir_emit_load(c, stride_temp, BE(c)->fp_reg, -vla_2d_sym->vla_slot_off);
                const ZeroCCType *leaf = vla_2d_sym->type.base;
                while (leaf && leaf->tag == TYPE_ARRAY) leaf = leaf->base;
                size_t leaf_sz = (leaf && leaf->size > 0) ? leaf->size : 1;
                if (leaf_sz != 1) {
                    int ls_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, ls_temp, (int32_t)leaf_sz);
                    int s2 = zc_alloc_temp(c);
                    ir_emit_mul(c, s2, stride_temp, ls_temp);
                    stride_temp = s2;
                }
            } else {
                ir_emit_load(c, stride_temp, BE(c)->fp_reg, -(vla_2d_sym->stack_offset - 16));
            }
            scaled_index = zc_alloc_temp(c);
            ir_emit_mul(c, scaled_index, index_temp, stride_temp);
        } else if (vla_et) {
            int sz_temp = zc_alloc_temp(c);
            ASTRef esz = zc_vla_struct_size_ast(arena, (ZeroCCASTArena *)c->tu_ast_arena, vla_et, vla_et->struct_name);
            if (esz != AST_NULL) sz_temp = lower_expr(c, arena, esz);
            else ir_emit_mov_imm(c, sz_temp, (int32_t)elem_size);
            scaled_index = zc_alloc_temp(c);
            ir_emit_mul(c, scaled_index, index_temp, sz_temp);
        } else if (elem_size == 1) {
            scaled_index = index_temp;
        } else if (elem_size == 2 || elem_size == 4 || elem_size == 8 || elem_size == 16) {
            int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : ((elem_size == 8) ? 3 : 4));
            int sh_temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, sh_temp, shift);
            scaled_index = zc_alloc_temp(c);
            ir_emit_shl(c, scaled_index, index_temp, sh_temp);
        } else {
            int sz_temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, sz_temp, (int32_t)elem_size);
            scaled_index = zc_alloc_temp(c);
            ir_emit_mul(c, scaled_index, index_temp, sz_temp);
        }
        int addr_temp = zc_alloc_temp(c);
        if (mirror_form) {
            int base_scaled = array_temp;
            if (mirror_esz != 1) {
                if (mirror_esz == 2 || mirror_esz == 4 || mirror_esz == 8 || mirror_esz == 16) {
                    int shift = (mirror_esz == 2) ? 1 : ((mirror_esz == 4) ? 2 : ((mirror_esz == 8) ? 3 : 4));
                    int sh_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, sh_temp, shift);
                    base_scaled = zc_alloc_temp(c);
                    ir_emit_shl(c, base_scaled, array_temp, sh_temp);
                } else {
                    int sz_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, sz_temp, (int32_t)mirror_esz);
                    base_scaled = zc_alloc_temp(c);
                    ir_emit_mul(c, base_scaled, array_temp, sz_temp);
                }
            }
            ir_emit_add(c, addr_temp, index_temp, base_scaled);
        } else {
            ir_emit_add(c, addr_temp, array_temp, scaled_index);
        }
        return addr_temp;
    } else if (operand->tag == NODE_UNOP && (operand->aux.op == '*' || operand->aux.op == TK_STAR)) {
        return lower_expr(c, arena, operand->left);
    } else if (operand->tag == NODE_CALL) {
        ZeroCCASTNode *cal = zero_cc_ast_get(arena, operand->left);
        if (cal && cal->tag == NODE_IDENT && cal->s.str &&
            (strcmp(cal->s.str, "__real") == 0 ||
             strcmp(cal->s.str, "__real__") == 0 ||
             strcmp(cal->s.str, "__imag") == 0 ||
             strcmp(cal->s.str, "__imag__") == 0)) {
            bool is_imag = (cal->s.str[2] == 'i');
            int addr = lower_expr(c, arena, operand->right);
            if (!is_imag) return addr;
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, operand->right);
            TypeInfo *cx_ti = ast_node_get_typeinfo(c, cx_node);
            size_t esz = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX && cx_ti->details.array.base)
                         ? type_size(cx_ti->details.array.base) : 8;
            if (!cx_ti && cx_node && cx_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, cx_node->s.str);
                if (sym && zero_cc_type_is_complex(&sym->type) && sym->type.base)
                    esz = sym->type.base->size;
                else {
                    ZeroCCGlobalVar *gv = find_global_var(c, cx_node->s.str);
                    if (gv && zero_cc_type_is_complex(&gv->type) && gv->type.base)
                        esz = gv->type.base->size;
                }
            }
            if (esz != 1 && esz != 2 && esz != 4 && esz != 8) esz = 8;
            int off = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off, (int)esz);
            int la = zc_alloc_temp(c);
            ir_emit_add(c, la, addr, off);
            return la;
        }
    }
    return TEMP_NONE;
}

void zero_cc_emit_struct_copy_to_addr(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                      int dst_addr, size_t struct_sz,
                                      ASTRef rhs_ref, int rhs_temp) {
    int num_chunks = (int)((struct_sz + 7) / 8);
    ZeroCCASTNode *rnode = zero_cc_ast_get(arena, rhs_ref);
    ZeroCCSym *src_sym = (rnode && rnode->tag == NODE_IDENT) ? zero_cc_find_local(c, rnode->s.str) : NULL;
    bool rhs_is_struct_addr = ast_expr_is_struct_addr(c, arena, rhs_ref);
    bool rhs_is_vec_addr = ast_expr_is_vector_addr(c, arena, rhs_ref);
    bool src_is_vec = src_sym && src_sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&src_sym->type);

    {
        TypeInfo *c_ti = ast_node_get_typeinfo(c, rnode);
        bool struct_ret_call = rnode && rnode->tag == NODE_CALL && c_ti &&
            (c_ti->kind == TYPE_KIND_STRUCT || c_ti->kind == TYPE_KIND_UNION);
        bool sse_class_ret = (c->isa == ISA_X86_64 &&
            (zc_typeinfo_unit_is_fp(c_ti, 0) || zc_typeinfo_unit_is_fp(c_ti, 1)));
        if (struct_ret_call && struct_sz > 8 && struct_sz <= 16 && !src_sym &&
            !sse_class_ret) {
            int pair0 = zc_alloc_temp(c);
            ir_emit_mov_reg(c, pair0, VREG_0);
            int pair1 = zc_alloc_temp(c);
            int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? VREG_1 : VREG_2;
            ir_emit_mov_reg(c, pair1, reg2);
            if (struct_sz >= 8) ir_emit_store_64(c, pair0, dst_addr);
            else if (struct_sz >= 4) ir_emit_store_32(c, pair0, dst_addr);
            else if (struct_sz >= 2) ir_emit_store_16(c, pair0, dst_addr);
            else ir_emit_store_byte(c, pair0, dst_addr);
            if (struct_sz > 8) {
                int off_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off_t, 8);
                int cur_dst = zc_alloc_temp(c);
                ir_emit_add(c, cur_dst, dst_addr, off_t);
                size_t rem = struct_sz - 8;
                if (rem >= 8) ir_emit_store_64(c, pair1, cur_dst);
                else zc_store_low_bytes(c, pair1, cur_dst, rem);
            }
            return;
        }
    }

    for (int i = 0; i < num_chunks; i++) {
        size_t rem = struct_sz - (size_t)(i * 8);
        int ch_val = zc_alloc_temp(c);
        int cur_dst = dst_addr;
        if (i > 0) {
            int off_t = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off_t, i * 8);
            cur_dst = zc_alloc_temp(c);
            ir_emit_add(c, cur_dst, dst_addr, off_t);
        }
        if (src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION)) {
            ir_emit_load(c, ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
        } else if (src_is_vec) {
            ir_emit_load(c, ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
        } else if (rhs_is_struct_addr || rhs_is_vec_addr) {
            int cur_src = rhs_temp;
            if (i > 0) {
                int off_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off_t, i * 8);
                cur_src = zc_alloc_temp(c);
                ir_emit_add(c, cur_src, rhs_temp, off_t);
            }
            if (rem >= 8) ir_emit_deref(c, ch_val, cur_src);
            else if (rem >= 4) ir_emit_load_32(c, ch_val, cur_src);
            else if (rem >= 2) ir_emit_load_16(c, ch_val, cur_src);
            else ir_emit_load_byte(c, ch_val, cur_src);
        } else if (i == 0) {
            ch_val = rhs_temp;
        } else if (i == 1 && struct_sz <= 16) {
            int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? VREG_1 : VREG_2;
            int ch2 = zc_alloc_temp(c);
            ir_emit_mov_reg(c, ch2, reg2);
            ch_val = ch2;
        }
        if (rem >= 8) {
            ir_emit_store_64(c, ch_val, cur_dst);
        } else if (rem == 4 || rem == 2 || rem == 1) {
            if (rem == 4) ir_emit_store_32(c, ch_val, cur_dst);
            else if (rem == 2) ir_emit_store_16(c, ch_val, cur_dst);
            else ir_emit_store_byte(c, ch_val, cur_dst);
        } else if ((src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION)) || src_is_vec) {
            int tail_a = zc_alloc_temp(c);
            ir_emit_lea(c, tail_a, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
            for (size_t b = 0; b < rem; b++) {
                int sv = zc_alloc_temp(c);
                int sa = tail_a;
                int da = cur_dst;
                if (b) {
                    int t = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t, (int)b);
                    sa = zc_alloc_temp(c);
                    ir_emit_add(c, sa, tail_a, t);
                    int t2 = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t2, (int)b);
                    da = zc_alloc_temp(c);
                    ir_emit_add(c, da, cur_dst, t2);
                }
                ir_emit_load_byte(c, sv, sa);
                ir_emit_store_byte(c, sv, da);
            }
        } else if (rhs_is_struct_addr || rhs_is_vec_addr) {
            for (size_t b = 0; b < rem; b++) {
                int sv = zc_alloc_temp(c);
                int sa = rhs_temp;
                size_t soff = (size_t)(i * 8 + b);
                if (soff) {
                    int t = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t, (int)soff);
                    sa = zc_alloc_temp(c);
                    ir_emit_add(c, sa, rhs_temp, t);
                }
                ir_emit_load_byte(c, sv, sa);
                int da = cur_dst;
                if (b) {
                    int t = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t, (int)b);
                    da = zc_alloc_temp(c);
                    ir_emit_add(c, da, cur_dst, t);
                }
                ir_emit_store_byte(c, sv, da);
            }
        } else {
            zc_store_low_bytes(c, ch_val, cur_dst, rem);
        }
    }
}

void zero_cc_emit_vla_struct_copy(ZeroCCCompiler *c, int dst_addr, int src_addr, int size_temp) {
    if (!zero_cc_find_function(c, "memcpy") && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
        ZeroCCFunction *fn = &c->functions[c->function_count++];
        memset(fn, 0, sizeof(*fn));
        snprintf(fn->name, sizeof(fn->name), "%s", "memcpy");
        fn->is_prototype = true;
    }
    int arg_temps[3] = { dst_addr, src_addr, size_temp };
    ir_emit_call(c, "memcpy");
    if (ir_get_count(c) > 0) {
        Instr *li = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
        li->temp_dest = 3;
        li->temp_src1 = (uint32_t)arg_temps[0];
        li->temp_src2 = (uint32_t)arg_temps[1];
        li->imm = IR_CALL_ARG01_REG;
        li->cond = 0;
        li->width = 0;
        ir_emit_call_arg(c, (uint32_t)arg_temps[2], 0, 255);
    }
}
