/* AST Lowering - Unary Operator Lowering (AST to IR) (Sprint 114: extracted from ast_lowering_binop.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

int lower_expr_unop(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        if (node->aux.op == TK_INC || node->aux.op == TK_DEC) {
            ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
            if (operand && operand->tag == NODE_IDENT) {
                const char *var = operand->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, var);
                if (sym) {
                    if (sym->is_const || sym->type.is_const) {
                        diag_report_error_at_token(c, "cannot modify const-qualified variable '%s'", var);
                        return TEMP_NONE;
                    }
                    if (sym->is_atomic || sym->type.is_atomic) {
                        int step = lvalue_inc_step(&sym->type);
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
                        int delta_temp = c->next_temp_id++;
                        int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                        ir_emit_mov_imm(c, delta_temp, (node->aux.op == TK_INC) ? step : -step);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xadd(c, old_temp, addr_temp, delta_temp, width);
                        c->last_expr_is_float = 0;
                        if (node->flags & 1) {
                            /* Postfix x++ / x--: return old value */
                            return old_temp;
                        } else {
                            /* Prefix ++x / --x: return new value (old ± step) */
                            int new_temp = c->next_temp_id++;
                            int step_temp = c->next_temp_id++;
                            ir_emit_mov_imm(c, step_temp, (node->aux.op == TK_INC) ? step : -step);
                            ir_emit_add(c, new_temp, old_temp, step_temp);
                            return new_temp;
                        }
                    }

                    if (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE) {
                        int val_temp = c->next_temp_id++;
                        int one_i = c->next_temp_id++;
                        int one_f = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)addr);
                        } else {
                            int a = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)a);
                        }
                        ir_emit_mov_imm(c, one_i, 1);
                        ir_emit_cvtsi2sd(c, (uint16_t)one_f, (uint16_t)one_i);
                        if (node->aux.op == TK_INC) {
                            ir_emit_fadd(c, (uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        } else {
                            ir_emit_fsub(c, (uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        }
                        if (sym->type.tag == TYPE_FLOAT) {
                            int addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vstoress(c, (uint16_t)addr, (uint16_t)res_temp);
                        } else {
                            int a = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                            ir_emit_vstoresd(c, (uint16_t)a, (uint16_t)res_temp);
                        }
                        c->last_expr_is_float = 1;
                        return (node->flags & 1) ? val_temp : res_temp;
                    }

                    int step = lvalue_inc_step(&sym->type);
                    int val_temp = c->next_temp_id++;
                    int one_temp = c->next_temp_id++;
                    int res_temp = c->next_temp_id++;

                    /* Load current variable value from stack */
                    zc_sym_load(c, sym, 0, val_temp);  /* Sprint 72 */
                    /* Step: sizeof(*x) for pointer locals (C11 6.5.6), else 1 */
                    ir_emit_mov_imm(c, one_temp, (node->aux.op == TK_INC) ? step : -step);
                    /* res = val ± step */
                    ir_emit_add(c, res_temp, val_temp, one_temp);
                    zero_cc_emit_int_extend(c, res_temp, &sym->type);
                    /* Store new value back to stack */
                    zc_sym_store(c, sym, 0, res_temp);  /* Sprint 72 */

                    c->last_expr_is_float = 0;
                    /* Postfix x++ / x-- yields the OLD value; prefix
                     * ++x / --x yields the new value ((node->flags & 1)
                     * marks postfix, matching the global/atomic paths
                     * below). Pre-fix the local path always returned the
                     * new value, miscompiling `return x++;` etc. */
                    return (node->flags & 1) ? val_temp : res_temp;
                }
                ZeroCCGlobalVar *gv = find_global_var(c, var);
                if (gv) {
                    int gaddr_temp = c->next_temp_id++;
                    ir_emit_global_addr(c, gaddr_temp, gv);
                    if (gv->type.tag == TYPE_FLOAT || gv->type.tag == TYPE_DOUBLE) {
                        int val_temp = c->next_temp_id++;
                        int one_i = c->next_temp_id++;
                        int one_f = c->next_temp_id++;
                        int res_temp = c->next_temp_id++;
                        if (gv->type.tag == TYPE_FLOAT) ir_emit_vloadss(c, (uint16_t)val_temp, (uint16_t)gaddr_temp);
                        else ir_emit_vloadsd(c, (uint16_t)val_temp, (uint16_t)gaddr_temp);
                        ir_emit_mov_imm(c, one_i, 1);
                        ir_emit_cvtsi2sd(c, (uint16_t)one_f, (uint16_t)one_i);
                        if (node->aux.op == TK_INC) {
                            ir_emit_fadd(c, (uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        } else {
                            ir_emit_fsub(c, (uint16_t)res_temp, (uint16_t)val_temp, (uint16_t)one_f);
                        }
                        if (gv->type.tag == TYPE_FLOAT) ir_emit_vstoress(c, (uint16_t)gaddr_temp, (uint16_t)res_temp);
                        else ir_emit_vstoresd(c, (uint16_t)gaddr_temp, (uint16_t)res_temp);
                        c->last_expr_is_float = 1;
                        return (node->flags & 1) ? val_temp : res_temp;
                    }
                    int val_temp = c->next_temp_id++;
                    int one_temp = c->next_temp_id++;
                    int res_temp = c->next_temp_id++;
                    size_t gv_sz = gv->size > 0 ? gv->size : 4;
                    if (gv_sz == 1) ir_emit_load_byte(c, val_temp, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_load_16(c, val_temp, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_load_32(c, val_temp, gaddr_temp);
                    else ir_emit_deref(c, val_temp, gaddr_temp);
                    zero_cc_emit_int_extend(c, val_temp, &gv->type);

                    ir_emit_mov_imm(c, one_temp, (node->aux.op == TK_INC) ?
                        lvalue_inc_step(&gv->type) : -lvalue_inc_step(&gv->type));
                    ir_emit_add(c, res_temp, val_temp, one_temp);
                    zero_cc_emit_int_extend(c, res_temp, &gv->type);

                    if (gv_sz == 1) ir_emit_store_byte(c, res_temp, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_store_16(c, res_temp, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_store_32(c, res_temp, gaddr_temp);
                    else ir_emit_store_64(c, res_temp, gaddr_temp);

                    c->last_expr_is_float = 0;
                    return (node->flags & 1) ? val_temp : res_temp;
                }
            }
            /* Non-ident lvalue (C11 6.5.2.4): a[i]++, (*p)++, s.f++,
             * p->f++.  Pre-fix these lowered to nothing (silent no-op).
             * Compute the lvalue address the same way the NODE_ASSIGN
             * branches do, then load, modify by the pointer-scaled
             * step, store; postfix returns the OLD value. */
            if (operand) {
                TypeInfo *lval_ti = ast_node_get_typeinfo(c, operand);
                size_t lval_size = lval_ti ? type_size(lval_ti) : 4;
                bool lval_float = (lval_ti && type_is_floating(lval_ti)) || c->last_expr_is_float;
                if (lval_size == 0) lval_size = 4;
                int addr = TEMP_NONE;

                if (operand->tag == NODE_INDEX) {
                    int array_temp = lower_expr(c, arena, operand->left);
                    int index_temp = lower_expr(c, arena, operand->right);
                    int scaled_index;
                    if (lval_size == 1) {
                        scaled_index = index_temp;
                    } else if (lval_size == 2 || lval_size == 4 || lval_size == 8) {
                        int shift = (lval_size == 2) ? 1 : ((lval_size == 4) ? 2 : 3);
                        int sh_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, sh_temp, shift);
                        scaled_index = c->next_temp_id++;
                        ir_emit_shl(c, scaled_index, index_temp, sh_temp);
                    } else {
                        int sz_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, sz_temp, (int32_t)lval_size);
                        scaled_index = c->next_temp_id++;
                        ir_emit_mul(c, scaled_index, index_temp, sz_temp);
                    }
                    addr = c->next_temp_id++;
                    ir_emit_add(c, addr, array_temp, scaled_index);
                } else if (operand->tag == NODE_UNOP &&
                           (operand->aux.op == '*' || operand->aux.op == TK_STAR)) {
                    addr = lower_expr(c, arena, operand->left); /* the pointer value */
                } else if (operand->tag == NODE_MEMBER) {
                    /* Single level: base address + member offset.
                     * Multi-segment chains (o.p->v) use the generic walker. */
                    ZeroCCASTNode *m = operand;
                    int depth = 0;
                    while (m && m->tag == NODE_MEMBER && depth < 128) {
                        depth++;
                        m = zero_cc_ast_get(arena, m->left);
                    }
                    if (depth >= 2) {
                        addr = lower_member_chain_addr(c, arena, operand);
                    } else {
                        ZeroCCASTNode *base_node = zero_cc_ast_get(arena, operand->left);
                        int32_t member_offset = operand->aux.alt;
                        bool is_arrow = (operand->flags & NODE_MEMBER_IS_ARROW) != 0;
                        int base_addr = TEMP_NONE;
                        if (base_node && base_node->tag == NODE_IDENT) {
                            ZeroCCSym *bsym = zero_cc_find_local(c, base_node->s.str);
                            ZeroCCGlobalVar *bgv = bsym ? NULL : find_global_var(c, base_node->s.str);
                            if (bsym) {
                                base_addr = c->next_temp_id++;
                                if (is_arrow) {
                                    /* p->m: load the pointer value */
                                    ir_emit_load(c, base_addr, BE(c)->fp_reg, -bsym->stack_offset);
                                } else {
                                    /* s.m: address of the struct object */
                                    ir_emit_lea(c, base_addr, TEMP_NONE, -(int32_t)bsym->stack_offset);
                                }
                            } else if (bgv) {
                                base_addr = c->next_temp_id++;
                                ir_emit_global_addr(c, base_addr, bgv);
                                if (is_arrow) {
                                    /* global pointer var: deref to get value */
                                    base_addr = c->next_temp_id++;
                                    ir_emit_deref(c, base_addr, base_addr);
                                }
                            }
                        } else if (base_node && base_node->tag == NODE_INDEX) {
                            /* arr[i].m: element address + member offset
                             * (c-torture execute/pr55750: `arr[i].n++`). */
                            int array_temp = lower_expr(c, arena, base_node->left);
                            int index_temp = lower_expr(c, arena, base_node->right);
                            TypeInfo *elti = ast_node_get_typeinfo(c, base_node);
                            size_t esz = elti ? type_size(elti) : 0;
                            if (esz == 0) esz = 4;
                            int scaled_index;
                            if (esz == 1) {
                                scaled_index = index_temp;
                            } else if (esz == 2 || esz == 4 || esz == 8) {
                                int shift = (esz == 2) ? 1 : ((esz == 4) ? 2 : 3);
                                int sh_temp = c->next_temp_id++;
                                ir_emit_mov_imm(c, sh_temp, shift);
                                scaled_index = c->next_temp_id++;
                                ir_emit_shl(c, scaled_index, index_temp, sh_temp);
                            } else {
                                int sz_temp = c->next_temp_id++;
                                ir_emit_mov_imm(c, sz_temp, (int32_t)esz);
                                scaled_index = c->next_temp_id++;
                                ir_emit_mul(c, scaled_index, index_temp, sz_temp);
                            }
                            base_addr = c->next_temp_id++;
                            ir_emit_add(c, base_addr, array_temp, scaled_index);
                        }
                        if (base_addr == TEMP_NONE && is_arrow) {
                            base_addr = lower_expr(c, arena, operand->left);
                        }
                        if (base_addr != TEMP_NONE) {
                            addr = base_addr;
                            if (member_offset != 0) {
                                int off_temp = c->next_temp_id++;
                                ir_emit_mov_imm(c, off_temp, member_offset);
                                addr = c->next_temp_id++;
                                ir_emit_add(c, addr, base_addr, off_temp);
                            }
                        }
                    }
                }
                if (addr == TEMP_NONE) return TEMP_NONE;

                /* Bit-field member (`++c.u33`, `--c.u40`): the lvalue
                 * address is the storage UNIT, not the field's bit
                 * position - do the field RMW with read-modify-write,
                 * and convert the result to the bit-field type (C11
                 * 6.5.6 / 6.5.7.3p2; c-torture bitfld-3). */
                if (operand->tag == NODE_MEMBER &&
                    (operand->flags & NODE_MEMBER_IS_BITFIELD)) {
                    uint8_t bf_off = (uint8_t)(operand->aux.step & 0xFF);
                    uint8_t bf_w = (uint8_t)((operand->aux.step >> 8) & 0xFF);
                    bool bf_signed = ((operand->aux.step >> 16) & 1) != 0;
                    bool bf_sso_be = ((operand->aux.step >> 17) & 1) != 0;
                    TypeInfo *bti = ast_node_get_typeinfo(c, operand);
                    size_t bm_size = bti ? type_size(bti) : 4;
                    bool bf64 = (bm_size > 4 || (bf_off + bf_w > 32));

                    if ((int)bf_off + (int)bf_w > 64) {
                        /* Sprint 113: 16-byte allocation unit (GPR128 RMW). */
                        int unit = c->next_temp_id++;
                        ir_emit_load128(c, unit, addr);
                        if (bf_sso_be) unit = zc_emit_unit_bswap128(c, unit);
                        int cur = unit;
                        if (bf_off > 0) {
                            int s_lo = c->next_temp_id++; ir_emit_lo128(c, s_lo, cur);
                            int s_hi = c->next_temp_id++; ir_emit_hi128(c, s_hi, cur);
                            int dlo = c->next_temp_id++, dhi = c->next_temp_id++;
                            zc_emit_128_shift(c, dlo, dhi, s_lo, s_hi, bf_off, TK_SHR, false);
                            cur = c->next_temp_id++; ir_emit_mk128(c, cur, dlo, dhi);
                        }
                        uint64_t vm_hi = (bf_w >= 128) ? ~0ULL : ((1ULL << (bf_w - 64)) - 1);
                        int vm_lo_t = c->next_temp_id++; ir_emit_mov_imm(c, vm_lo_t, (int64_t)~0ULL);
                        int vm_hi_t = c->next_temp_id++; ir_emit_mov_imm(c, vm_hi_t, (int64_t)vm_hi);
                        int vm = c->next_temp_id++; ir_emit_mk128(c, vm, vm_lo_t, vm_hi_t);
                        int curm = c->next_temp_id++; ir_emit_and128(c, curm, cur, vm);
                        int old_val = curm;
                        if (bf_signed && bf_w < 128) {
                            int64_t sb = (int64_t)(1ULL << (bf_w - 65));
                            int zlo = c->next_temp_id++; ir_emit_mov_imm(c, zlo, 0);
                            int shi = c->next_temp_id++; ir_emit_mov_imm(c, shi, sb);
                            int sbt = c->next_temp_id++; ir_emit_mk128(c, sbt, zlo, shi);
                            int xt = c->next_temp_id++; ir_emit_xor128(c, xt, old_val, sbt);
                            old_val = c->next_temp_id++; ir_emit_sub128(c, old_val, xt, sbt);
                        }
                        int one_t = c->next_temp_id++;
                        ir_emit_mov_imm(c, one_t, (node->aux.op == TK_INC) ? 1 : -1);
                        int one128 = c->next_temp_id++;
                        if (bf_signed) ir_emit_sext64_128(c, one128, one_t);
                        else ir_emit_zext64_128(c, one128, one_t);
                        int newv = c->next_temp_id++;
                        ir_emit_add128(c, newv, old_val, one128);
                        int bits = c->next_temp_id++; ir_emit_and128(c, bits, newv, vm);
                        uint64_t f_lo, f_hi;
                        if (bf_off == 0) { f_lo = ~0ULL; f_hi = vm_hi; }
                        else if (bf_off < 64) {
                            f_lo = ~0ULL << bf_off;
                            f_hi = (vm_hi << bf_off) | (~0ULL >> (64 - bf_off));
                        } else { f_lo = 0; f_hi = ~0ULL << (bf_off - 64); }
                        int fmask = c->next_temp_id++;
                        {
                            int fml = c->next_temp_id++; ir_emit_mov_imm(c, fml, (int64_t)f_lo);
                            int fmh = c->next_temp_id++; ir_emit_mov_imm(c, fmh, (int64_t)f_hi);
                            ir_emit_mk128(c, fmask, fml, fmh);
                        }
                        int cmask = c->next_temp_id++;
                        {
                            int cml = c->next_temp_id++; ir_emit_mov_imm(c, cml, (int64_t)~f_lo);
                            int cmh = c->next_temp_id++; ir_emit_mov_imm(c, cmh, (int64_t)~f_hi);
                            ir_emit_mk128(c, cmask, cml, cmh);
                        }
                        int cleared = c->next_temp_id++;
                        ir_emit_and128(c, cleared, unit, cmask);
                        if (bf_off > 0) {
                            int s_lo = c->next_temp_id++; ir_emit_lo128(c, s_lo, bits);
                            int s_hi = c->next_temp_id++; ir_emit_hi128(c, s_hi, bits);
                            int dlo = c->next_temp_id++, dhi = c->next_temp_id++;
                            zc_emit_128_shift(c, dlo, dhi, s_lo, s_hi, bf_off, TK_SHL, false);
                            bits = c->next_temp_id++; ir_emit_mk128(c, bits, dlo, dhi);
                        }
                        int comb = c->next_temp_id++;
                        ir_emit_or128(c, comb, cleared, bits);
                        if (bf_sso_be) comb = zc_emit_unit_bswap128(c, comb);
                        ir_emit_store128(c, comb, addr);
                        int new_val = c->next_temp_id++; ir_emit_and128(c, new_val, newv, vm);
                        if (bf_signed && bf_w < 128) {
                            int64_t sb = (int64_t)(1ULL << (bf_w - 65));
                            int zlo = c->next_temp_id++; ir_emit_mov_imm(c, zlo, 0);
                            int shi = c->next_temp_id++; ir_emit_mov_imm(c, shi, sb);
                            int sbt = c->next_temp_id++; ir_emit_mk128(c, sbt, zlo, shi);
                            int xt = c->next_temp_id++; ir_emit_xor128(c, xt, new_val, sbt);
                            new_val = c->next_temp_id++; ir_emit_sub128(c, new_val, xt, sbt);
                        }
                        c->last_expr_is_float = 0;
                        c->last_expr_is_128 = 1;
                        return (node->flags & 1) ? old_val : new_val;
                    }

                    uint64_t wmask = (bf_w >= 64) ? ~0ULL : ((1ULL << bf_w) - 1);
                    int word_t = c->next_temp_id++;
                    if (bf64) ir_emit_deref(c, word_t, addr);
                    else ir_emit_load_32(c, word_t, addr);
                    int fld = word_t;
                    if (bf_off > 0) {
                        int sh = c->next_temp_id++;
                        ir_emit_mov_imm(c, sh, (int32_t)bf_off);
                        int s2 = c->next_temp_id++;
                        ir_emit_shr(c, s2, fld, sh);
                        fld = s2;
                    }
                    int m1 = c->next_temp_id++;
                    ir_emit_mov_imm(c, m1, (int64_t)wmask);
                    int fm = c->next_temp_id++;
                    ir_emit_and(c, fm, fld, m1);
                    int old_val = fm;
                    if (bf_signed && bf_w > 0 && bf_w < 64) {
                        int64_t sb = (int64_t)(1ULL << (bf_w - 1));
                        int st = c->next_temp_id++;
                        ir_emit_mov_imm(c, st, sb);
                        int x1 = c->next_temp_id++;
                        ir_emit_xor(c, x1, old_val, st);
                        int e1 = c->next_temp_id++;
                        ir_emit_sub(c, e1, x1, st);
                        old_val = e1;
                    }
                    int one_t = c->next_temp_id++;
                    ir_emit_mov_imm(c, one_t, (node->aux.op == TK_INC) ? 1 : -1);
                    int new_val = c->next_temp_id++;
                    ir_emit_add(c, new_val, old_val, one_t);
                    int m2 = c->next_temp_id++;
                    ir_emit_mov_imm(c, m2, (int64_t)wmask);
                    int red = c->next_temp_id++;
                    ir_emit_and(c, red, new_val, m2);
                    if (bf_signed && bf_w > 0 && bf_w < 64) {
                        int64_t sb2 = (int64_t)(1ULL << (bf_w - 1));
                        int st2 = c->next_temp_id++;
                        ir_emit_mov_imm(c, st2, sb2);
                        int x2 = c->next_temp_id++;
                        ir_emit_xor(c, x2, red, st2);
                        int e2 = c->next_temp_id++;
                        ir_emit_sub(c, e2, x2, st2);
                        red = e2;
                    }
                    uint64_t fmask = (bf_w >= 64) ? ~0ULL : (wmask << bf_off);
                    int cm = c->next_temp_id++;
                    ir_emit_mov_imm(c, cm, (int64_t)(~fmask));
                    int cleared = c->next_temp_id++;
                    ir_emit_and(c, cleared, word_t, cm);
                    int mv = red;
                    if (bf_off > 0) {
                        int sh2 = c->next_temp_id++;
                        ir_emit_mov_imm(c, sh2, (int32_t)bf_off);
                        int s3 = c->next_temp_id++;
                        ir_emit_shl(c, s3, mv, sh2);
                        mv = s3;
                    }
                    int comb = c->next_temp_id++;
                    ir_emit_or(c, comb, cleared, mv);
                    if (bf64) ir_emit_store_64(c, comb, addr);
                    else ir_emit_store_32(c, comb, addr);
                    c->last_expr_is_float = 0;
                    return (node->flags & 1) ? old_val : red;
                }

                int step = 1;
                if (lval_ti && type_is_pointer(lval_ti)) {
                    step = (int)type_size(lval_ti->details.pointer.base);
                    if (step <= 0) step = 8;
                }
                int old_t = c->next_temp_id++;
                int new_t = c->next_temp_id++;
                if (lval_float) {
                    if (lval_size == 4) ir_emit_vloadss(c, (uint16_t)old_t, (uint16_t)addr);
                    else ir_emit_vloadsd(c, (uint16_t)old_t, (uint16_t)addr);
                    int one_i = c->next_temp_id++;
                    ir_emit_mov_imm(c, one_i, 1);
                    int one_f = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)one_f, (uint16_t)one_i);
                    if (node->aux.op == TK_INC) {
                        ir_emit_fadd(c, (uint16_t)new_t, (uint16_t)old_t, (uint16_t)one_f);
                    } else {
                        ir_emit_fsub(c, (uint16_t)new_t, (uint16_t)old_t, (uint16_t)one_f);
                    }
                    if (lval_size == 4) ir_emit_vstoress(c, (uint16_t)addr, (uint16_t)new_t);
                    else ir_emit_vstoresd(c, (uint16_t)addr, (uint16_t)new_t);
                    c->last_expr_is_float = 1;
                    return (node->flags & 1) ? old_t : new_t;
                }
                if (lval_size == 1) ir_emit_load_byte(c, old_t, addr);
                else if (lval_size == 2) ir_emit_load_16(c, old_t, addr);
                else if (lval_size == 4) ir_emit_load_32(c, old_t, addr);
                else ir_emit_deref(c, old_t, addr);
                lower_extend_narrow(c, old_t, lval_ti);
                int step_t = c->next_temp_id++;
                ir_emit_mov_imm(c, step_t, (node->aux.op == TK_INC) ? step : -step);
                ir_emit_add(c, new_t, old_t, step_t);
                if (lval_size == 1) ir_emit_store_byte(c, new_t, addr);
                else if (lval_size == 2) ir_emit_store_16(c, new_t, addr);
                else if (lval_size == 4) ir_emit_store_32(c, new_t, addr);
                else ir_emit_store_64(c, new_t, addr);
                c->last_expr_is_float = 0;
                return (node->flags & 1) ? old_t : new_t;
            }
            return TEMP_NONE;
        }

        if (node->aux.op == '&' || node->aux.op == TK_AMP) {
            int addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
            if (addr != TEMP_NONE) {
                c->last_expr_is_float = 0;
                return addr;
            }
        }

        /* GNU vector: per-lane unary +/-/~. The operand lowers to the
         * address of the vector object; the result is a fresh stack
         * temp returned as its address (like the vector binops). */
        {
            TypeInfo *un_ti = ast_node_get_typeinfo(c, node);
            if (un_ti && type_is_vector(un_ti) &&
                (node->aux.op == TK_MINUS || node->aux.op == TK_TILDE ||
                 node->aux.op == TK_PLUS || node->aux.op == '+')) {
                const TypeInfo *el_ti = un_ti->details.array.base;
                size_t esz = el_ti ? type_size(el_ti) : 0;
                size_t lanes = (esz > 0 && type_size(un_ti) > 0) ? type_size(un_ti) / esz : 0;
                /* Sprint 113: 16-byte (128-bit) integer lanes (v2ti). */
                if (esz < 1 || esz > 16 || lanes < 1 || lanes > 32) {
                    diag_report_error_at_token(c, "unsupported vector operand size");
                    int zt = c->next_temp_id++;
                    ir_emit_mov_imm(c, zt, 0);
                    c->last_expr_is_float = 0;
                    return zt;
                }
                bool elem_float = type_is_floating(el_ti);
                bool elem_unsigned = type_is_unsigned(el_ti);
                size_t vsize = lanes * esz;
                if (vsize > 16) c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)vsize + 7) & ~7;
                int32_t res_off = c->current_stack_frame;
                int op_addr = lower_expr(c, arena, node->left);
                for (size_t i = 0; i < lanes; i++) {
                    int a = op_addr;
                    if (i > 0) {
                        int o = c->next_temp_id++;
                        ir_emit_mov_imm(c, o, (int32_t)(i * esz));
                        a = c->next_temp_id++;
                        ir_emit_add(c, a, op_addr, o);
                    }
                    int lv = c->next_temp_id++;
                    if (elem_float) {
                        if (esz == 4) ir_emit_vloadss(c, (uint16_t)lv, (uint16_t)a);
                        else ir_emit_vloadsd(c, (uint16_t)lv, (uint16_t)a);
                    } else if (esz == 1) {
                        ir_emit_load_byte(c, lv, a);
                        if (elem_unsigned) ir_emit_zext(c, lv, 1); else ir_emit_sext(c, lv, 1);
                    } else if (esz == 2) {
                        ir_emit_load_16(c, lv, a);
                        if (elem_unsigned) ir_emit_zext(c, lv, 2); else ir_emit_sext(c, lv, 2);
                    } else if (esz == 4) {
                        ir_emit_load_32(c, lv, a);
                        if (elem_unsigned) ir_emit_zext(c, lv, 4); else ir_emit_sext(c, lv, 4);
                    } else if (esz == 16) {
                        ir_emit_load128(c, lv, a);
                    } else {
                        ir_emit_deref(c, lv, a);
                    }
                    int ov = c->next_temp_id++;
                    bool ov_flt = false;
                    if (elem_float) {
                        int ls = lv;
                        if (esz == 4) { int t = c->next_temp_id++; ir_emit_cvtss2sd(c, (uint16_t)t, (uint16_t)lv); ls = t; }
                        if (node->aux.op == TK_MINUS || node->aux.op == '-') {
                            int zero_i = c->next_temp_id++;
                            ir_emit_mov_imm(c, zero_i, 0);
                            int zero_f = c->next_temp_id++;
                            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
                            ir_emit_fsub(c, (uint16_t)ov, (uint16_t)zero_f, (uint16_t)ls);
                        } else {
                            /* Unary + (and ~, undefined for floats): identity */
                            ir_emit_fmov(c, (uint16_t)ov, (uint16_t)ls);
                        }
                        ov_flt = 1;
                    } else if (esz == 16) {
                        /* 128-bit lane: NOT128; negation is NOT + add 1. */
                        if (node->aux.op == TK_TILDE || node->aux.op == '~') {
                            ir_emit_not128(c, ov, lv);
                        } else if (node->aux.op == TK_MINUS || node->aux.op == '-') {
                            int one_lo = c->next_temp_id++; ir_emit_mov_imm(c, one_lo, 1);
                            int one_hi = c->next_temp_id++; ir_emit_mov_imm(c, one_hi, 0);
                            int one = c->next_temp_id++; ir_emit_mk128(c, one, one_lo, one_hi);
                            ir_emit_not128(c, ov, lv);
                            ir_emit_add128(c, ov, ov, one);
                        } else {
                            ir_emit_mov128(c, ov, lv);
                        }
                    } else if (node->aux.op == TK_TILDE || node->aux.op == '~') {
                        ir_emit_not(c, ov, lv);
                    } else if (node->aux.op == TK_MINUS || node->aux.op == '-') {
                        ir_emit_neg(c, ov, lv);
                    } else {
                        ir_emit_mov_reg(c, ov, lv);
                    }
                    int32_t lane_off = -(res_off - (int32_t)(i * esz));
                    int lane_addr = c->next_temp_id++;
                    ir_emit_lea(c, lane_addr, BE(c)->fp_reg, lane_off);
                    if (elem_float) {
                        if (esz == 4) {
                            if (ov_flt) { int t = c->next_temp_id++; ir_emit_cvtsd2ss(c, (uint16_t)t, (uint16_t)ov); ov = t; }
                            ir_emit_vstoress(c, (uint16_t)lane_addr, (uint16_t)ov);
                        } else {
                            ir_emit_vstoresd(c, (uint16_t)lane_addr, (uint16_t)ov);
                        }
                    } else if (esz == 16) {
                        ir_emit_store128(c, ov, lane_addr);
                    } else if (esz == 1) {
                        ir_emit_store_byte(c, ov, lane_addr);
                    } else if (esz == 2) {
                        ir_emit_store_16(c, ov, lane_addr);
                    } else if (esz == 4) {
                        ir_emit_store_32(c, ov, lane_addr);
                    } else {
                        ir_emit_store_64(c, ov, lane_addr);
                    }
                }
                int res_addr = c->next_temp_id++;
                ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
                c->last_expr_is_float = 0;
                return res_addr;
            }
        }

        int operand_temp = lower_expr(c, arena, node->left);
        int result_temp = c->next_temp_id++;

        switch (node->aux.op) {
            case TK_STAR: { /* Dereference */
                /* Dereferencing a pointer to function yields a function
                 * lvalue (C99 6.5.3.2): its value is the pointer itself,
                 * and [fp] holds the function's machine code, not data.
                 * So `*fp` (and e.g. `(*fp)(args)`) evaluates to the
                 * pointer value with no load. */
                TypeInfo *op_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->left));
                TypeInfo *ti = ast_node_get_typeinfo(c, node);

                if (ti && type_is_vector(ti)) {
                    /* Dereference of a pointer-to-vector: the vector value
                     * is in address form - the pointer itself is the value
                     * (consumers read lanes through it). */
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                if (type_is_array(ti) ||
                    (type_is_pointer(op_ti) && op_ti->details.pointer.base &&
                     (type_is_array(op_ti->details.pointer.base) ||
                      type_is_function(op_ti->details.pointer.base)))) {
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                size_t p_size = 0;
                if (ti) p_size = type_size(ti);
                else {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->size > 0) {
                            p_size = ptr_sym->type.base->size;
                        }
                        ZeroCCGlobalVar *gv = find_global_var(c, inner->s.str);
                        if (gv && gv->type.base && gv->type.base->size > 0) {
                            p_size = gv->type.base->size;
                        }
                    }
                }
                bool is_struct = type_is_struct_or_union(ti);
                if (!is_struct) {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base && zero_cc_type_is_struct_or_union(ptr_sym->type.base)) {
                            is_struct = true;
                        }
                        ZeroCCGlobalVar *gv = find_global_var(c, inner->s.str);
                        if (gv && gv->type.base && zero_cc_type_is_struct_or_union(gv->type.base)) {
                            is_struct = true;
                        }
                    }
                }
                if (is_struct && p_size > 16) {
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                /* Zero-size struct (C11 6.7.2.1p17: `struct g {}`) -
                 * the dereference has no bytes to load; yielding the
                 * pointer itself keeps the lvalue usable and, crucially,
                 * must NOT emit an 8-byte read past the empty object
                 * (c-torture execute/zero-struct-1: the phantom load
                 * read 8 bytes of a zero-size object and the matching
                 * store scribbled on whatever follows it). */
                if (is_struct && p_size == 0) {
                    c->last_expr_is_float = 0;
                    return operand_temp;
                }
                bool is_fp = (ti && type_is_floating(ti));
                if (!is_fp) {
                    ZeroCCASTNode *inner = zero_cc_ast_get(arena, node->left);
                    if (inner && inner->tag == NODE_IDENT) {
                        ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                        if (ptr_sym && ptr_sym->type.base &&
                            (ptr_sym->type.base->tag == TYPE_FLOAT || ptr_sym->type.base->tag == TYPE_DOUBLE)) {
                            is_fp = true;
                            if (p_size == 0) p_size = (ptr_sym->type.base->tag == TYPE_FLOAT) ? 4 : 8;
                        }
                        ZeroCCGlobalVar *gv = find_global_var(c, inner->s.str);
                        if (gv && gv->type.base &&
                            (gv->type.base->tag == TYPE_FLOAT || gv->type.base->tag == TYPE_DOUBLE)) {
                            is_fp = true;
                            if (p_size == 0) p_size = (gv->type.base->tag == TYPE_FLOAT) ? 4 : 8;
                        }
                    }
                }
                if (is_fp) {
                    if (p_size == 4) ir_emit_vloadss(c, (uint16_t)result_temp, (uint16_t)operand_temp);
                    else ir_emit_vloadsd(c, (uint16_t)result_temp, (uint16_t)operand_temp);
                    c->last_expr_is_float = 1;
                    return result_temp;
                }
                if (p_size == 1) ir_emit_load_byte(c, result_temp, operand_temp);
                else if (p_size == 2) ir_emit_load_16(c, result_temp, operand_temp);
                else if (p_size == 4) ir_emit_load_32(c, result_temp, operand_temp);
                else if (!is_struct && (p_size == 16 || (ti && ti->kind == TYPE_KIND_INT128))) {
                    ir_emit_load128(c, result_temp, operand_temp);
                    c->last_expr_is_float = 0;
                    c->last_expr_is_128 = 1;
                    return result_temp;
                }
                else ir_emit_deref(c, result_temp, operand_temp);
                lower_extend_narrow(c, result_temp, ti);
                break;
            }
            case TK_MINUS:  /* Negation */
                if (c->last_expr_is_float) {
                    /* IEEE 754 float negation: flip the sign bit (bit 63).
                     * (0.0 - x) does not preserve -0.0 because 0.0 - 0.0 = +0.0
                     * per IEEE 754 §6.3. */
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    int32_t slot = c->current_stack_frame;
                    ir_emit_fstore(c, (uint16_t)operand_temp, TEMP_NONE, -slot);
                    int int_temp = c->next_temp_id++;
                    ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
                    int sign_mask = c->next_temp_id++;
                    ir_emit_mov_imm(c, sign_mask, (int64_t)0x8000000000000000ULL);
                    int flipped = c->next_temp_id++;
                    ir_emit_xor(c, flipped, int_temp, sign_mask);
                    ir_emit_store(c, flipped, BE(c)->fp_reg, -slot);
                    ir_emit_fload(c, (uint16_t)result_temp, TEMP_NONE, -slot);
                    c->last_expr_is_float = 1;
                    return result_temp;
                }
                {
                    /* 128-bit negation: operand is a GPR128 (lo,hi) pair, so use the
                     * dedicated two's-complement neg128, not the 64-bit ir_emit_neg
                     * (which only touches the low half and leaves hi = copy of lo). */
                    TypeInfo *neg128_ti = ast_node_get_typeinfo(c, node);
                    if (neg128_ti && neg128_ti->kind == TYPE_KIND_INT128) {
                        ir_emit_neg128(c, result_temp, operand_temp);
                        c->last_expr_is_128 = 1;
                        break;
                    }
                }
                ir_emit_neg(c, result_temp, operand_temp);
                /* C99 6.5.3.3: the result type is the promoted operand
                 * type, so a 32-bit unsigned negation wraps at 32 bits
                 * (-1U == 0xFFFFFFFF, not 0xFFFFFFFFFFFFFFFF) and a
                 * 1/2-byte operand promotes to signed int.  Without the
                 * mark the full-width negation poisons later 32-bit
                 * compares/divides (c-torture pr49039, pr56250). */
                {
                    TypeInfo *neg_ti = ast_node_get_typeinfo(c, node);
                    if (neg_ti) {
                        size_t nwsz = type_size(neg_ti);
                        if (nwsz == 4) {
                            bool wu = (neg_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                            ir_mark_last_width(c, wu ? (uint8_t)(8 + 4) : (uint8_t)4);
                        } else if (nwsz == 1 || nwsz == 2) {
                            ir_mark_last_width(c, 4); /* integer promotion */
                        }
                    }
                }
                break;
            case TK_BANG: { /* Logical NOT: !operand */
                TypeInfo *op_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->left));
                if (op_ti && op_ti->kind == TYPE_KIND_COMPLEX) {
                    const TypeInfo *cel = op_ti->details.array.base;
                    size_t esz = (cel) ? type_size(cel) : 8;
                    bool is_elem_fp = cel ? type_is_floating(cel) : true;
                    if (is_elem_fp && esz != 4 && esz != 8) esz = 8;
                    int rh = c->next_temp_id++, li = c->next_temp_id++;
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, (int)esz);
                    int a2 = c->next_temp_id++; ir_emit_add(c, a2, operand_temp, o2);
                    if (is_elem_fp) {
                        if (esz == 4) {
                            ir_emit_vloadss(c, (uint16_t)rh, (uint16_t)operand_temp);
                            ir_emit_vloadss(c, (uint16_t)li, (uint16_t)a2);
                        } else {
                            ir_emit_vloadsd(c, (uint16_t)rh, (uint16_t)operand_temp);
                            ir_emit_vloadsd(c, (uint16_t)li, (uint16_t)a2);
                        }
                        int zf = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)zf, (int64_t)zc_rodata_double_offset(c, 0.0));
                        int er = c->next_temp_id++;
                        ir_emit_cmp(c, rh, zf);
                        ir_emit_cond_set(c, er, CC_EQ);
                        int ei = c->next_temp_id++;
                        ir_emit_cmp(c, li, zf);
                        ir_emit_cond_set(c, ei, CC_EQ);
                        ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)result_temp,
                                        .temp_src1 = (uint16_t)er, .temp_src2 = (uint16_t)ei});
                    } else {
                        if (esz == 1) { ir_emit_load_byte(c, rh, operand_temp); ir_emit_load_byte(c, li, a2); }
                        else if (esz == 2) { ir_emit_load_16(c, rh, operand_temp); ir_emit_load_16(c, li, a2); }
                        else if (esz == 4) { ir_emit_load_32(c, rh, operand_temp); ir_emit_load_32(c, li, a2); }
                        else { ir_emit_deref(c, rh, operand_temp); ir_emit_deref(c, li, a2); }
                        int er = c->next_temp_id++;
                        ir_emit_cmp_zero(c, rh);
                        ir_emit_cond_set(c, er, CC_EQ);
                        int ei = c->next_temp_id++;
                        ir_emit_cmp_zero(c, li);
                        ir_emit_cond_set(c, ei, CC_EQ);
                        ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)result_temp,
                                        .temp_src1 = (uint16_t)er, .temp_src2 = (uint16_t)ei});
                    }
                    c->last_expr_is_float = 0;
                } else if (c->last_expr_is_float) {
                    int zero_i = c->next_temp_id++;
                    ir_emit_mov_imm(c, zero_i, 0);
                    int zero_f = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
                    ir_emit_cmp(c, operand_temp, zero_f);
                    ir_emit_cond_set(c, result_temp, CC_EQ);
                    c->last_expr_is_float = 0;
                } else {
                    ir_emit_cmp_zero(c, operand_temp);
                    ir_emit_cond_set(c, result_temp, CC_EQ);
                }
                break;
            }
            case TK_TILDE: { /* Bitwise NOT / Complex conjugate (~x) */
                TypeInfo *op_ti = ast_node_get_typeinfo(c, zero_cc_ast_get(arena, node->left));
                const ZeroCCType *op_zt = NULL;
                ZeroCCASTNode *op_node = zero_cc_ast_get(arena, node->left);
                if (op_node && op_node->tag == NODE_IDENT) {
                    ZeroCCSym *sym = zero_cc_find_local(c, op_node->s.str);
                    if (sym && zero_cc_type_is_complex(&sym->type)) op_zt = &sym->type;
                    else {
                        ZeroCCGlobalVar *gv = find_global_var(c, op_node->s.str);
                        if (gv && zero_cc_type_is_complex(&gv->type)) op_zt = &gv->type;
                    }
                }
                if ((op_ti && op_ti->kind == TYPE_KIND_COMPLEX) || (op_zt && op_zt->is_complex)) {
                    const TypeInfo *cel = (op_ti && op_ti->kind == TYPE_KIND_COMPLEX) ? op_ti->details.array.base : NULL;
                    size_t cesz = cel ? type_size(cel) : (op_zt && op_zt->base ? op_zt->base->size : 8);
                    if (cesz == 0) cesz = 8;
                    bool cflt = cel ? type_is_floating(cel) :
                                (op_zt && op_zt->base ? (op_zt->base->tag == TYPE_FLOAT || op_zt->base->tag == TYPE_DOUBLE) : true);
                    if (cflt && cesz != 4 && cesz != 8) cesz = 8;
                    int rh = c->next_temp_id++, li = c->next_temp_id++;
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, (int)cesz);
                    int a2 = c->next_temp_id++; ir_emit_add(c, a2, operand_temp, o2);
                    int neg_li = c->next_temp_id++;
                    if (cflt) {
                        if (cesz == 4) {
                            ir_emit_vloadss(c, (uint16_t)rh, (uint16_t)operand_temp);
                            ir_emit_vloadss(c, (uint16_t)li, (uint16_t)a2);
                        } else {
                            ir_emit_vloadsd(c, (uint16_t)rh, (uint16_t)operand_temp);
                            ir_emit_vloadsd(c, (uint16_t)li, (uint16_t)a2);
                        }
                        int zero_i = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_i, 0);
                        int zero_f = c->next_temp_id++;
                        ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
                        ir_emit_fsub(c, (uint16_t)neg_li, (uint16_t)zero_f, (uint16_t)li);
                    } else {
                        if (cesz == 1) { ir_emit_load_byte(c, rh, operand_temp); ir_emit_load_byte(c, li, a2); }
                        else if (cesz == 2) { ir_emit_load_16(c, rh, operand_temp); ir_emit_load_16(c, li, a2); }
                        else if (cesz == 4) { ir_emit_load_32(c, rh, operand_temp); ir_emit_load_32(c, li, a2); }
                        else { ir_emit_deref(c, rh, operand_temp); ir_emit_deref(c, li, a2); }
                        ir_emit_neg(c, neg_li, li);
                    }
                    size_t csz = cesz * 2;
                    c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                    c->current_stack_frame += (int32_t)csz;
                    int32_t res_off = c->current_stack_frame;
                    int res_addr = c->next_temp_id++;
                    ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
                    int res_a2 = c->next_temp_id++;
                    ir_emit_add(c, res_a2, res_addr, o2);
                    if (cflt) {
                        if (cesz == 4) {
                            ir_emit_vstoress(c, (uint16_t)res_addr, (uint16_t)rh);
                            ir_emit_vstoress(c, (uint16_t)res_a2, (uint16_t)neg_li);
                        } else {
                            ir_emit_vstoresd(c, (uint16_t)res_addr, (uint16_t)rh);
                            ir_emit_vstoresd(c, (uint16_t)res_a2, (uint16_t)neg_li);
                        }
                    } else {
                        if (cesz == 1) { ir_emit_store_byte(c, rh, res_addr); ir_emit_store_byte(c, neg_li, res_a2); }
                        else if (cesz == 2) { ir_emit_store_16(c, rh, res_addr); ir_emit_store_16(c, neg_li, res_a2); }
                        else if (cesz == 4) { ir_emit_store_32(c, rh, res_addr); ir_emit_store_32(c, neg_li, res_a2); }
                        else { ir_emit_store_64(c, rh, res_addr); ir_emit_store_64(c, neg_li, res_a2); }
                    }
                    c->last_expr_is_float = 0;
                    return res_addr;
                }
                /* 128-bit bitwise NOT: operand is a GPR128 (lo,hi) pair, so use
                 * NOT128, not the 64-bit ir_emit_not (which only inverts the low
                 * half and leaves hi untouched). */
                {
                    TypeInfo *tilde128_ti = ast_node_get_typeinfo(c, node);
                    if (tilde128_ti && tilde128_ti->kind == TYPE_KIND_INT128) {
                        ir_emit_not128(c, result_temp, operand_temp);
                        c->last_expr_is_128 = 1;
                        break;
                    }
                }
                ir_emit_not(c, result_temp, operand_temp);
                break;
            }
            default:
                /* Unary + (identity): keep the operand's float-ness so a
                 * following mixed int/FP op does not cvtsi2sd the bits
                 * (c-torture execute/ieee/mzero2: `+1.00 / pzero`). An FP
                 * operand must move through an FP-class temp (fmov), not
                 * a GP mov_reg. */
                if (c->last_expr_is_float) {
                    ir_emit_fmov(c, (uint16_t)result_temp, (uint16_t)operand_temp);
                    return result_temp;
                }
                ir_emit_mov_reg(c, result_temp, operand_temp);
                break;
        }
        /* C99 width semantics: bitwise NOT produces a full-width bit
         * pattern that must be re-extended to the operand's int width.
         * (Negation of an already-extended operand stays extended; the
         * INT_MIN case is UB in C.) */
        if (node->aux.op == TK_TILDE) {
            TypeInfo *u_ti = ast_node_get_typeinfo(c, node);
            if (u_ti) {
                size_t wsz = type_size(u_ti);
                if (wsz == 1 || wsz == 2 || wsz == 4) {
                    bool wu = (u_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                    ir_mark_last_width(c, wu ? (uint8_t)(8 + wsz) : (uint8_t)wsz);
                }
            }
        }
        if (node->aux.op != TK_PLUS && node->aux.op != '+')
            c->last_expr_is_float = 0;
        return result_temp;
}

