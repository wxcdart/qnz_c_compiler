/* AST Lowering - Assignment Expression Lowering (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

/* Apply a compound-assignment op (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
 * to the member value at address temp `addr` (m_size bytes; float members
 * use the double-width FP model). Loads the current value with the
 * width-correct load, applies the op, and updates *right_temp. Mirrors the
 * multi-segment chain handling (C5). */
/* Per-lane in-place compound op (v OP= rhs) on a GNU vector object whose
 * base address is in base_t. The RHS is either a scalar value in rhs_t
 * (broadcast to every lane) or a vector in address form (each lane reads
 * its own amount from rhs_t + i*esz). Element sizes 1-8; float elements
 * run in the double-width FP model (VLOADSS/VSTORESS widen/narrow).
 * Shared by the plain-local, global, and pointer-deref compound paths so
 * all three keep identical lane semantics (c-torture execute/pr108292:
 * `x >>= ((V){} != x) & 31` on a 64-byte global vector). */
static void zc_emit_vector_compound_inplace(ZeroCCCompiler *c, int base_t, size_t vsize,
                                            const ZeroCCType *vel, int op,
                                            int rhs_t, bool rhs_is_vec, bool rhs_is_flt) {
    size_t esz = (vel && vel->size > 0) ? vel->size : 4;
    size_t lanes = (esz > 0) ? (vsize / esz) : 0;
    if (lanes == 0 || lanes > 64) return;
    bool el_flt = vel && (vel->tag == TYPE_FLOAT || vel->tag == TYPE_DOUBLE);
    bool el_uns = vel && (vel->tag == TYPE_UINT8 || vel->tag == TYPE_UINT16 ||
                          vel->tag == TYPE_UINT32 || vel->tag == TYPE_UINT64);
    for (size_t i = 0; i < lanes; i++) {
        int la = c->next_temp_id++;
        if (i == 0) { ir_emit_mov_reg(la, base_t); }
        else { int o = c->next_temp_id++; ir_emit_mov_imm(o, (int32_t)(i * esz));
               ir_emit_add(la, base_t, o); }
        int lv = c->next_temp_id++;
        int rv = c->next_temp_id++;
        if (el_flt) {
            if (esz == 4) ir_emit_vloadss((uint16_t)lv, (uint16_t)la);
            else ir_emit_vloadsd((uint16_t)lv, (uint16_t)la);
            if (rhs_is_vec) {
                int ra = c->next_temp_id++;
                if (i == 0) { ir_emit_mov_reg(ra, rhs_t); }
                else { int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int32_t)(i * esz));
                         ir_emit_add(ra, rhs_t, o2); }
                if (esz == 4) ir_emit_vloadss((uint16_t)rv, (uint16_t)ra);
                else ir_emit_vloadsd((uint16_t)rv, (uint16_t)ra);
            } else if (rhs_is_flt) {
                ir_emit_mov_reg(rv, rhs_t);
            } else {
                ir_emit_cvtsi2sd((uint16_t)rv, (uint16_t)rhs_t);
            }
            int ov = c->next_temp_id++;
            switch (op) {
                case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)ov, (uint16_t)lv, (uint16_t)rv); break;
                case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)ov, (uint16_t)lv, (uint16_t)rv); break;
                case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)ov, (uint16_t)lv, (uint16_t)rv); break;
                default:            ir_emit_fdiv((uint16_t)ov, (uint16_t)lv, (uint16_t)rv); break;
            }
            if (esz == 4) ir_emit_vstoress(la, (uint16_t)ov);
            else ir_emit_vstoresd(la, (uint16_t)ov);
            continue;
        }
        if (esz == 1) { ir_emit_load_byte(lv, la); if (el_uns) ir_emit_zext(lv, 1); else ir_emit_sext(lv, 1); }
        else if (esz == 2) { ir_emit_load_16(lv, la); if (el_uns) ir_emit_zext(lv, 2); else ir_emit_sext(lv, 2); }
        else if (esz == 4) { ir_emit_load_32(lv, la); if (el_uns) ir_emit_zext(lv, 4); else ir_emit_sext(lv, 4); }
        else ir_emit_deref(lv, la);
        if (rhs_is_vec) {
            int ra = c->next_temp_id++;
            if (i == 0) { ir_emit_mov_reg(ra, rhs_t); }
            else { int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int32_t)(i * esz));
                    ir_emit_add(ra, rhs_t, o2); }
            if (esz == 1) { ir_emit_load_byte(rv, ra); if (el_uns) ir_emit_zext(rv, 1); else ir_emit_sext(rv, 1); }
            else if (esz == 2) { ir_emit_load_16(rv, ra); if (el_uns) ir_emit_zext(rv, 2); else ir_emit_sext(rv, 2); }
            else if (esz == 4) { ir_emit_load_32(rv, ra); if (el_uns) ir_emit_zext(rv, 4); else ir_emit_sext(rv, 4); }
            else ir_emit_deref(rv, ra);
        } else {
            ir_emit_mov_reg(rv, rhs_t);
        }
        int ov = c->next_temp_id++;
        switch (op) {
            case TK_ADD_ASSIGN: ir_emit_add(ov, lv, rv); break;
            case TK_SUB_ASSIGN: ir_emit_sub(ov, lv, rv); break;
            case TK_MUL_ASSIGN: ir_emit_mul(ov, lv, rv); break;
            case TK_DIV_ASSIGN: ir_emit_div(ov, lv, rv); if (el_uns) ir_mark_last_unsigned(1); break;
            case TK_MOD_ASSIGN: ir_emit_mod(ov, lv, rv); if (el_uns) ir_mark_last_unsigned(1); break;
            case TK_AND_ASSIGN: ir_emit_and(ov, lv, rv); break;
            case TK_OR_ASSIGN:  ir_emit_or(ov, lv, rv); break;
            case TK_XOR_ASSIGN: ir_emit_xor(ov, lv, rv); break;
            case TK_SHL_ASSIGN: ir_emit_shl(ov, lv, rv); break;
            case TK_SHR_ASSIGN: ir_emit_shr(ov, lv, rv); break;
            default: ir_emit_mov_reg(ov, lv); break;
        }
        if (esz == 1) ir_emit_store_byte(ov, la);
        else if (esz == 2) ir_emit_store_16(ov, la);
        else if (esz == 4) ir_emit_store_32(ov, la);
        else ir_emit_store_64(ov, la);
    }
}

static void member_apply_compound(ZeroCCCompiler *c, int op_tok, int addr,
                                  int *right_temp, size_t m_size, bool is_flt) {
    if (is_flt) {
        int lhs_val = c->next_temp_id++;
        if (m_size == 4) ir_emit_vloadss((uint16_t)lhs_val, (uint16_t)addr);
        else ir_emit_vloadsd((uint16_t)lhs_val, (uint16_t)addr);
        int float_rhs = *right_temp;
        if (!c->last_expr_is_float) {
            float_rhs = c->next_temp_id++;
            ir_emit_cvtsi2sd((uint16_t)float_rhs, (uint16_t)*right_temp);
        }
        int op_res = c->next_temp_id++;
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
            default: op_res = float_rhs; break;
        }
        *right_temp = op_res;
        c->last_expr_is_float = 1;
    } else {
        int curr_val = c->next_temp_id++;
        if (m_size == 1) ir_emit_load_byte(curr_val, addr);
        else if (m_size == 2) ir_emit_load_16(curr_val, addr);
        else if (m_size == 4) ir_emit_load_32(curr_val, addr);
        else ir_emit_deref(curr_val, addr);
        int op_res = c->next_temp_id++;
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_add(op_res, curr_val, *right_temp); break;
            case TK_SUB_ASSIGN: ir_emit_sub(op_res, curr_val, *right_temp); break;
            case TK_MUL_ASSIGN: ir_emit_mul(op_res, curr_val, *right_temp); break;
            case TK_DIV_ASSIGN: ir_emit_div(op_res, curr_val, *right_temp); break;
            case TK_MOD_ASSIGN: ir_emit_mod(op_res, curr_val, *right_temp); break;
            case TK_AND_ASSIGN: ir_emit_and(op_res, curr_val, *right_temp); break;
            case TK_OR_ASSIGN:  ir_emit_or(op_res, curr_val, *right_temp); break;
            case TK_XOR_ASSIGN: ir_emit_xor(op_res, curr_val, *right_temp); break;
            case TK_SHL_ASSIGN: ir_emit_shl(op_res, curr_val, *right_temp); break;
            case TK_SHR_ASSIGN: ir_emit_shr(op_res, curr_val, *right_temp); break;
            default: ir_emit_mov_reg(op_res, *right_temp); break;
        }
        *right_temp = op_res;
    }
}

static int assign_member_bitfield(ZeroCCCompiler *c,
                                  ZeroCCASTNode *assign_node, ZeroCCASTNode *left_node,
                                  int addr_temp, int right_temp) {
    uint8_t bit_offset = (uint8_t)(left_node->aux.step & 0xFF);
    uint8_t bit_width = (uint8_t)((left_node->aux.step >> 8) & 0xFF);
    bool is_signed = ((left_node->aux.step >> 16) & 1) != 0;
    uint64_t val_mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
    TypeInfo *ti = ast_node_get_typeinfo(c, left_node);
    size_t m_size = ti ? type_size(ti) : 4;
    bool is_64 = (m_size > 4 || (bit_offset + bit_width > 32));

    /* 1. Load current storage unit */
    int curr_temp = c->next_temp_id++;
    if (is_64) ir_emit_deref(curr_temp, addr_temp);
    else ir_emit_load_32(curr_temp, addr_temp);

    bool is_compound = (assign_node->aux.op != TK_ASSIGN && assign_node->aux.op != '=');
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
        switch (assign_node->aux.op) {
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

    /* 2. Clear target bits */
    uint64_t field_mask = (bit_width >= 64) ? ~0ULL : (val_mask << bit_offset);
    uint64_t clear_mask = ~field_mask;
    int clear_mask_temp = c->next_temp_id++;
    ir_emit_mov_imm(clear_mask_temp, (int64_t)clear_mask);
    int cleared_temp = c->next_temp_id++;
    ir_emit_and(cleared_temp, curr_temp, clear_mask_temp);

    /* 3. Mask incoming value to bit_width */
    int val_mask_temp = c->next_temp_id++;
    ir_emit_mov_imm(val_mask_temp, (int64_t)val_mask);
    int masked_val = c->next_temp_id++;
    ir_emit_and(masked_val, right_temp, val_mask_temp);

    /* Value of the assignment expression: the RHS converted to the
     * bit-field type (C99 6.5.16.1) - masked to the field width and
     * sign-extended for signed fields (c-torture execute/921016-1:
     * `(l.m = 1081)` on a signed 11-bit field is -967, not 1081). */
    int field_val = masked_val;
    if (is_signed && bit_width > 0 && bit_width < 32) {
        int32_t sb = (int32_t)(1ULL << (bit_width - 1));
        int sft = c->next_temp_id++;
        ir_emit_mov_imm(sft, sb);
        int xft = c->next_temp_id++;
        ir_emit_xor(xft, field_val, sft);
        int eft = c->next_temp_id++;
        ir_emit_sub(eft, xft, sft);
        field_val = eft;
    }

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
    if (is_64) ir_emit_store_64(combined_temp, addr_temp);
    else ir_emit_store_32(combined_temp, addr_temp);

    c->last_expr_is_float = 0;
    return field_val;
}


int lower_expr_assign(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* Check if this is a compound assignment (+=, -=, etc.) */
        bool is_compound = (node->aux.op != TK_ASSIGN && node->aux.op != '=');
        ZeroCCASTNode *left_node = zero_cc_ast_get(arena, node->left);

        /* C99 complex compound assignment (x += y, x -= y, x *= y,
         * x /= y): desugar to x = x OP y via a synthetic complex
         * binop (the full binop path handles the part math, scalar
         * operand conversion, and the __divdc3 delegation for double
         * division), then store the two result halves into the LHS. */
        if (is_compound) {
            TypeInfo *lhs_ti_c = ast_node_get_typeinfo(c, left_node);
            ZeroCCSym *cx_lsym_c = (left_node && left_node->tag == NODE_IDENT &&
                                    zero_cc_find_local(c, left_node->s.str))
                                       ? zero_cc_find_local(c, left_node->s.str) : NULL;
            bool lhs_cx_c = (lhs_ti_c && lhs_ti_c->kind == TYPE_KIND_COMPLEX);
            if (!lhs_cx_c && cx_lsym_c && zero_cc_type_is_complex(&cx_lsym_c->type)) {
                lhs_cx_c = true;
            }
            uint32_t cop_c = node->aux.op == TK_ADD_ASSIGN ? TK_PLUS :
                             node->aux.op == TK_SUB_ASSIGN ? TK_MINUS :
                             node->aux.op == TK_MUL_ASSIGN ? TK_STAR :
                             node->aux.op == TK_DIV_ASSIGN ? TK_SLASH : 0;
            if (lhs_cx_c && cop_c != 0 && node->right != AST_NULL) {
                size_t esz_c = (lhs_ti_c && lhs_ti_c->details.array.base)
                                   ? type_size(lhs_ti_c->details.array.base) : 8;
                if (esz_c != 4 && esz_c != 8) esz_c = 8;
                int target_off = TEMP_NONE, target_addr = TEMP_NONE;
                if (cx_lsym_c && left_node->tag == NODE_IDENT) {
                    target_off = cx_lsym_c->stack_offset;
                } else if (left_node->tag == NODE_UNOP &&
                           (left_node->aux.op == '*' || left_node->aux.op == TK_STAR)) {
                    target_addr = lower_expr(c, arena, left_node->left);
                } else if (left_node->tag == NODE_MEMBER) {
                    target_addr = zero_cc_lower_lvalue_addr(c, arena, left_node);
                }
                if (target_off != TEMP_NONE || target_addr != TEMP_NONE) {
                    ASTRef syn_ref = zero_cc_ast_alloc_node(arena, NODE_BINOP);
                    ZeroCCASTNode *syn = zero_cc_ast_get(arena, syn_ref);
                    syn->aux.op = cop_c;
                    syn->line = node->line;
                    syn->col = node->col;
                    syn->left = node->left;
                    syn->right = node->right;
                    ast_node_set_typeinfo(c, syn, lhs_ti_c);
                    int res_addr = lower_expr(c, arena, syn_ref);

                    int rhc, lic;
                    if (esz_c == 4) {
                        rhc = c->next_temp_id++; ir_emit_vloadss(rhc, (uint16_t)res_addr);
                        int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 4);
                        int a2 = c->next_temp_id++; ir_emit_add(a2, res_addr, o2);
                        lic = c->next_temp_id++; ir_emit_vloadss(lic, (uint16_t)a2);
                    } else {
                        rhc = c->next_temp_id++; ir_emit_vloadsd(rhc, (uint16_t)res_addr);
                        int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 8);
                        int a2 = c->next_temp_id++; ir_emit_add(a2, res_addr, o2);
                        lic = c->next_temp_id++; ir_emit_vloadsd(lic, (uint16_t)a2);
                    }
                    if (target_off != TEMP_NONE) {
                        if (esz_c == 4) {
                            int sa = c->next_temp_id++;
                            ir_emit_lea(sa, BE(c)->fp_reg, -target_off);
                            ir_emit_vstoress(sa, (uint16_t)rhc);
                            int sa2 = c->next_temp_id++; ir_emit_mov_imm(sa2, 4);
                            int sa3 = c->next_temp_id++; ir_emit_add(sa3, sa, sa2);
                            ir_emit_vstoress(sa3, (uint16_t)lic);
                        } else {
                            ir_emit_fstore((uint16_t)rhc, TEMP_NONE, -target_off);
                            ir_emit_fstore((uint16_t)lic, TEMP_NONE, -target_off + (int)esz_c);
                        }
                    } else {
                        if (esz_c == 4) { ir_emit_vstoress((uint16_t)target_addr, (uint16_t)rhc); int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 4); int a2 = c->next_temp_id++; ir_emit_add(a2, target_addr, o2); ir_emit_vstoress((uint16_t)a2, (uint16_t)lic); }
                        else { ir_emit_vstoresd((uint16_t)target_addr, (uint16_t)rhc); int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 8); int a2 = c->next_temp_id++; ir_emit_add(a2, target_addr, o2); ir_emit_vstoresd((uint16_t)a2, (uint16_t)lic); }
                    }
                    c->last_expr_is_float = 0;
                    int zt = c->next_temp_id++; ir_emit_mov_imm(zt, 0);
                    return zt;
                }
            }
        }

        /* C99 complex assignment:
         *  - LHS complex local, RHS complex: copy the two halves
         *  - LHS complex local, RHS scalar: real = rhs, imag = 0
         *  - LHS scalar, RHS complex: lhs = real half of rhs
         *  - LHS complex via *ptr: store the halves at the target
         * Complex values are address-form (16 bytes, real first). */
        if (!is_compound) {
            TypeInfo *lhs_ti = ast_node_get_typeinfo(c, left_node);
            TypeInfo *rhs_ti_a = ast_node_get_typeinfo(c,
                (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL);
            bool lhs_cx = (lhs_ti && lhs_ti->kind == TYPE_KIND_COMPLEX);
            bool rhs_cx = (rhs_ti_a && rhs_ti_a->kind == TYPE_KIND_COMPLEX);
            /* GNU part-extractor lvalue: `__real__ a = 9;` / `__imag__ a = 42;`
             * (desugared to `__real__(a) = 9`). Store the RHS scalar into
             * the corresponding half of the complex object. */
            {
                bool lhs_is_cxpart = false;
                bool cxpart_imag = false;
                ZeroCCASTNode *lcal = (left_node && left_node->tag == NODE_CALL)
                                          ? zero_cc_ast_get(arena, left_node->left) : NULL;
                if (lcal && lcal->tag == NODE_IDENT && lcal->s.str) {
                    const char *cs = lcal->s.str;
                    if (strcmp(cs, "__real") == 0 || strcmp(cs, "__real__") == 0) lhs_is_cxpart = true;
                    else if (strcmp(cs, "__imag") == 0 || strcmp(cs, "__imag__") == 0) { lhs_is_cxpart = true; cxpart_imag = true; }
                }
                if (lhs_is_cxpart && node->right != AST_NULL) {
                    ZeroCCASTNode *cxan = zero_cc_ast_get(arena, left_node->right);
                    TypeInfo *cxati = cxan ? ast_node_get_typeinfo(c, cxan) : NULL;
                    if (cxati && cxati->kind == TYPE_KIND_COMPLEX) {
                        size_t pesz = (cxati->details.array.base) ? type_size(cxati->details.array.base) : 8;
                        if (pesz != 4 && pesz != 8) pesz = 8;
                        int paddr = lower_expr(c, arena, left_node->right);
                        if (cxpart_imag) {
                            int po = c->next_temp_id++;
                            ir_emit_mov_imm(po, (int)pesz);
                            int pa = c->next_temp_id++;
                            ir_emit_add(pa, paddr, po);
                            paddr = pa;
                        }
                        int prv = lower_expr(c, arena, node->right);
                        if (c->last_expr_is_float) {
                            if (pesz == 4) ir_emit_vstoress((uint16_t)paddr, (uint16_t)prv);
                            else ir_emit_vstoresd((uint16_t)paddr, (uint16_t)prv);
                        } else {
                            int pdv = zc_emit_int_to_double(c, prv, rhs_ti_a);
                            if (pesz == 4) ir_emit_vstoress((uint16_t)paddr, (uint16_t)pdv);
                            else ir_emit_vstoresd((uint16_t)paddr, (uint16_t)pdv);
                        }
                        c->last_expr_is_float = 0;
                        int pz = c->next_temp_id++;
                        ir_emit_mov_imm(pz, 0);
                        return pz;
                    }
                }
            }
            ZeroCCSym *cx_lsym = (left_node && left_node->tag == NODE_IDENT &&
                                  zero_cc_find_local(c, left_node->s.str))
                                     ? zero_cc_find_local(c, left_node->s.str) : NULL;
            if (!lhs_cx && !rhs_cx && cx_lsym && zero_cc_type_is_complex(&cx_lsym->type)) {
                lhs_cx = true;
            }
            if (lhs_cx || rhs_cx) {
                size_t lhs_esz = (lhs_cx && lhs_ti && lhs_ti->details.array.base)
                                 ? type_size(lhs_ti->details.array.base) : 8;
                size_t rhs_esz = (rhs_cx && rhs_ti_a && rhs_ti_a->details.array.base)
                                 ? type_size(rhs_ti_a->details.array.base) : 8;
                if (lhs_esz != 4 && lhs_esz != 8) lhs_esz = 8; /* long double: double model */
                if (rhs_esz != 4 && rhs_esz != 8) rhs_esz = 8;

                int target_off = TEMP_NONE; /* local slot offset if LHS ident */
                int target_addr = TEMP_NONE;
                if (cx_lsym && left_node->tag == NODE_IDENT) {
                    target_off = cx_lsym->stack_offset;
                } else if (left_node && left_node->tag == NODE_UNOP &&
                           (left_node->aux.op == '*' || left_node->aux.op == TK_STAR)) {
                    target_addr = lower_expr(c, arena, left_node->left);
                } else {
                    target_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
                    if (target_addr == TEMP_NONE) {
                        goto zc_complex_assign_generic;
                    }
                }

                int rv = lower_expr(c, arena, node->right);
                bool rv_is_float = c->last_expr_is_float;
                int rh = c->next_temp_id++, li = c->next_temp_id++;
                if (rhs_cx) {
                    if (rhs_esz == 4) { ir_emit_vloadss(rh, (uint16_t)rv); int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 4); int a2 = c->next_temp_id++; ir_emit_add(a2, rv, o2); ir_emit_vloadss(li, (uint16_t)a2); }
                    else { ir_emit_vloadsd(rh, (uint16_t)rv); int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 8); int a2 = c->next_temp_id++; ir_emit_add(a2, rv, o2); ir_emit_vloadsd(li, (uint16_t)a2); }
                } else {
                    if (rv_is_float) {
                        ir_emit_fmov(rh, (uint16_t)rv);
                    } else {
                        rh = zc_emit_int_to_double(c, rv, rhs_ti_a);
                    }
                    ir_emit_fmov_imm((uint16_t)li, (int64_t)zc_rodata_double_offset(c, 0.0));
                }
                if (lhs_cx) {
                    if (target_off != TEMP_NONE) {
                        int sa = c->next_temp_id++;
                        ir_emit_lea(sa, BE(c)->fp_reg, -target_off);
                        target_addr = sa;
                        if (lhs_esz == 4) {
                            ir_emit_vstoress(sa, (uint16_t)rh);
                            int sa2 = c->next_temp_id++;
                            ir_emit_mov_imm(sa2, 4);
                            int sa3 = c->next_temp_id++;
                            ir_emit_add(sa3, sa, sa2);
                            ir_emit_vstoress(sa3, (uint16_t)li);
                        } else {
                            ir_emit_fstore((uint16_t)rh, TEMP_NONE, -target_off);
                            ir_emit_fstore((uint16_t)li, TEMP_NONE, -target_off + (int)lhs_esz);
                        }
                    } else {
                        if (lhs_esz == 4) { ir_emit_vstoress((uint16_t)target_addr, (uint16_t)rh); int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 4); int a2 = c->next_temp_id++; ir_emit_add(a2, target_addr, o2); ir_emit_vstoress((uint16_t)a2, (uint16_t)li); }
                        else { ir_emit_vstoresd((uint16_t)target_addr, (uint16_t)rh); int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, 8); int a2 = c->next_temp_id++; ir_emit_add(a2, target_addr, o2); ir_emit_vstoresd((uint16_t)a2, (uint16_t)li); }
                    }
                    c->last_expr_is_float = 0;
                    return target_addr;
                } else {
                    /* scalar LHS = complex RHS: real half */
                    (void)li;
                    if (lhs_ti && (lhs_ti->kind == TYPE_KIND_FLOAT || lhs_ti->kind == TYPE_KIND_DOUBLE)) {
                        c->last_expr_is_float = 1;
                        return rh;
                    }
                    /* integral LHS: double -> integer conversion */
                    int zt2 = c->next_temp_id++;
                    ir_emit((Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)zt2,
                        .temp_src1 = (uint16_t)rh, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    c->last_expr_is_float = 0;
                    return zt2;
                }
            }
            zc_complex_assign_generic:
        ;}

        /* Dereference assignment (*ptr = expr): evaluate LHS target address
         * BEFORE RHS value expression so that RHS calling-convention return
         * registers (VREG_0 / VREG_2 / VREG_F0) are not clobbered by any
         * function calls or statement expressions inside the LHS address
         * computation (e.g. GCC c-torture execute/20000917-1.c:
         * `*({ ({ one(); &a; }); }) = zero();`). Also supports full by-value
         * struct copies for 9-16B (register pair) and >16B (memory buffer). */
        if (left_node && left_node->tag == NODE_UNOP && (left_node->aux.op == '*' || left_node->aux.op == TK_STAR)) {
            ZeroCCASTNode *inner = zero_cc_ast_get(arena, left_node->left);
            if (inner && inner->tag == NODE_IDENT) {
                ZeroCCSym *ptr_sym = zero_cc_find_local(c, inner->s.str);
                if (ptr_sym && ptr_sym->type.base && ptr_sym->type.base->is_const) {
                    diag_report_error_at_token(c, "cannot assign to const-qualified pointer target '%s'", inner->s.str);
                    return TEMP_NONE;
                }
            }
            int ptr_temp = lower_expr(c, arena, left_node->left);

            /* Target size up front (it decides the RHS lowering form).
             * The rhs-type fallback below is filled in after the RHS is
             * lowered. */
            size_t p_size = 4;
            TypeInfo *target_ti = ast_node_get_typeinfo(c, left_node);
            if (target_ti) {
                p_size = type_size(target_ti);
            } else if (inner && inner->tag == NODE_IDENT) {
                ZeroCCSym *ps = zero_cc_find_local(c, inner->s.str);
                if (ps && ps->type.base && ps->type.base->size > 0) {
                    p_size = ps->type.base->size;
                }
            }

            ZeroCCASTNode *rhs_node = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
            int right_temp;
            if (p_size > 16 && rhs_node && rhs_node->tag == NODE_IDENT) {
                ZeroCCSym *rhs_sym = zero_cc_find_local(c, rhs_node->s.str);
                if (rhs_sym && (rhs_sym->type.tag == TYPE_STRUCT || rhs_sym->type.tag == TYPE_UNION) &&
                    rhs_sym->type.size > 16) {
                    /* >16B by-value struct RHS must be a buffer address
                     * for the chunked copy below. A local struct ident
                     * normally lowers to an 8-byte value load (the form
                     * by-value argument marshaling needs), so take its
                     * address instead (c-torture pr35472.c: `*p = a;`
                     * with a 64-byte local struct). */
                    right_temp = c->next_temp_id++;
                    ir_emit_lea(right_temp, BE(c)->fp_reg, -rhs_sym->stack_offset);
                    c->last_expr_is_float = 0;
                } else {
                    right_temp = lower_expr(c, arena, node->right);
                }
            } else {
                right_temp = lower_expr(c, arena, node->right);
            }
            if (p_size == 4 && !target_ti) {
                TypeInfo *rhs_ti = (rhs_node) ? ast_node_get_typeinfo(c, rhs_node) : NULL;
                if (rhs_ti && type_size(rhs_ti) > 0) p_size = type_size(rhs_ti);
            }

            /* Zero-size struct target (`*((struct g*)p) = ...` with
             * `struct g {}`): the store has no bytes to write. The LHS
             * address (with any side effects, e.g. p++) and the RHS were
             * lowered above for their side effects; emit no store - an
             * 8-byte store here scribbles past the empty object
             * (c-torture execute/zero-struct-1). */
            if (p_size == 0 && target_ti &&
                (target_ti->kind == TYPE_KIND_STRUCT || target_ti->kind == TYPE_KIND_UNION)) {
                c->last_expr_is_float = 0;
                return right_temp;
            }

            /* Pointer compound assignment scaling (p += n / p -= n) */
            if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
                TypeInfo *lvti = ast_node_get_typeinfo(c, left_node);
                if (lvti && (type_is_pointer(lvti) || lvti->kind == TYPE_KIND_ARRAY)) {
                    size_t psz = type_size(lvti->kind == TYPE_KIND_ARRAY ?
                        lvti->details.array.base : lvti->details.pointer.base);
                    if (psz > 1) {
                        int si = c->next_temp_id++;
                        ir_emit_mov_imm(si, (int32_t)psz);
                        int s = c->next_temp_id++;
                        ir_emit_mul(s, right_temp, si);
                        right_temp = s;
                    }
                }
            }

            int final_val = right_temp;
            if (is_compound && target_ti && type_is_vector(target_ti)) {
                /* Per-lane compound assignment through a pointer-to-vector
                 * (*w += v, *w %= n, ...): the target is in address form
                 * (ptr_temp); the RHS is either a vector in address form
                 * (per-lane read) or a scalar broadcast to every lane. */
                TypeInfo *lt = target_ti->details.array.base;
                size_t elsz = lt ? type_size(lt) : 1;
                size_t lanes = p_size / (elsz ? elsz : 1);
                bool lt_float = lt && type_is_floating(lt);
                ZeroCCASTNode *rhs_node_c = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                TypeInfo *rhs_ti_c = rhs_node_c ? ast_node_get_typeinfo(c, rhs_node_c) : NULL;
                bool rhs_is_scalar_c = !(rhs_ti_c && type_is_vector(rhs_ti_c));
                for (size_t i = 0; i < lanes; i++) {
                    int laddr = c->next_temp_id++;
                    if (i == 0) { ir_emit_mov_reg(laddr, ptr_temp); }
                    else {
                        int off = c->next_temp_id++;
                        ir_emit_mov_imm(off, (int32_t)(i * elsz));
                        ir_emit_add(laddr, ptr_temp, off);
                    }
                    int cur = c->next_temp_id++;
                    int rvl = c->next_temp_id++;
                    if (lt_float) {
                        /* Float element: double-width FP model */
                        if (elsz == 4) ir_emit_vloadss((uint16_t)cur, (uint16_t)laddr);
                        else ir_emit_vloadsd((uint16_t)cur, (uint16_t)laddr);
                        if (!rhs_is_scalar_c) {
                            int raddr = c->next_temp_id++;
                            if (i == 0) { ir_emit_mov_reg(raddr, right_temp); }
                            else { int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int32_t)(i * elsz));
                                   ir_emit_add(raddr, right_temp, o2); }
                            if (elsz == 4) ir_emit_vloadss((uint16_t)rvl, (uint16_t)raddr);
                            else ir_emit_vloadsd((uint16_t)rvl, (uint16_t)raddr);
                        } else if (c->last_expr_is_float) {
                            ir_emit_mov_reg(rvl, right_temp);
                        } else {
                            ir_emit_cvtsi2sd((uint16_t)rvl, (uint16_t)right_temp);
                        }
                        int res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)res, (uint16_t)cur, (uint16_t)rvl); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)res, (uint16_t)cur, (uint16_t)rvl); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)res, (uint16_t)cur, (uint16_t)rvl); break;
                            default:            ir_emit_fdiv((uint16_t)res, (uint16_t)cur, (uint16_t)rvl); break;
                        }
                        if (elsz == 4) ir_emit_vstoress(laddr, (uint16_t)res);
                        else ir_emit_vstoresd(laddr, (uint16_t)res);
                        continue;
                    }
                    if (elsz == 1) { ir_emit_load_byte(cur, laddr); ir_emit_load_byte(rvl, laddr); }
                    else if (elsz == 2) { ir_emit_load_16(cur, laddr); ir_emit_load_16(rvl, laddr); }
                    else if (elsz == 4) { ir_emit_load_32(cur, laddr); ir_emit_load_32(rvl, laddr); }
                    else { ir_emit_deref(cur, laddr); ir_emit_deref(rvl, laddr); }
                    if (!rhs_is_scalar_c) {
                        int raddr = c->next_temp_id++;
                        if (i == 0) { ir_emit_mov_reg(raddr, right_temp); }
                        else { int o2 = c->next_temp_id++; ir_emit_mov_imm(o2, (int32_t)(i * elsz));
                               ir_emit_add(raddr, right_temp, o2); }
                        if (elsz == 1) ir_emit_load_byte(rvl, raddr);
                        else if (elsz == 2) ir_emit_load_16(rvl, raddr);
                        else if (elsz == 4) ir_emit_load_32(rvl, raddr);
                        else ir_emit_deref(rvl, raddr);
                    } else {
                        ir_emit_mov_reg(rvl, right_temp);
                    }
                    int res = c->next_temp_id++;
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(res, cur, rvl); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(res, cur, rvl); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(res, cur, rvl); break;
                        case TK_DIV_ASSIGN: ir_emit_div(res, cur, rvl); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(res, cur, rvl); break;
                        case TK_AND_ASSIGN: ir_emit_and(res, cur, rvl); break;
                        case TK_OR_ASSIGN:  ir_emit_or(res, cur, rvl); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(res, cur, rvl); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(res, cur, rvl); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(res, cur, rvl); break;
                        default: res = rvl; break;
                    }
                    if (elsz == 1) ir_emit_store_byte(res, laddr);
                    else if (elsz == 2) ir_emit_store_16(res, laddr);
                    else if (elsz == 4) ir_emit_store_32(res, laddr);
                    else ir_emit_store_64(res, laddr);
                }
                final_val = c->next_temp_id++;
                ir_emit_mov_imm(final_val, 0);
                c->last_expr_is_float = 0;
                return final_val;
            }
            if (is_compound) {
                int curr_val = c->next_temp_id++;
                if (p_size == 1) ir_emit_load_byte(curr_val, ptr_temp);
                else if (p_size == 2) ir_emit_load_16(curr_val, ptr_temp);
                else if (p_size == 4) ir_emit_load_32(curr_val, ptr_temp);
                else ir_emit_deref(curr_val, ptr_temp);
                lower_extend_narrow(c, curr_val, target_ti);
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
                    default: op_res = right_temp; break;
                }
                final_val = op_res;
            }
            if (!c->last_expr_is_float && !is_compound &&
                target_ti && type_is_floating(target_ti)) {
                /* C11 6.5.16.1: `*p = <int>` into a float/double pointee
                 * must convert before the FP store (c-torture pr47538). */
                int cvt_t = c->next_temp_id++;
                ir_emit_cvtsi2sd((uint16_t)cvt_t, (uint16_t)final_val);
                final_val = cvt_t;
                c->last_expr_is_float = 1;
            }
            if (c->last_expr_is_float) {
                if (p_size == 4) ir_emit_vstoress((uint16_t)ptr_temp, (uint16_t)final_val);
                else ir_emit_vstoresd((uint16_t)ptr_temp, (uint16_t)final_val);
                c->last_expr_is_float = 1;
            } else if (target_ti && type_is_vector(target_ti)) {
                /* Vector target: the RHS is in address form (binop temp,
                 * compound literal, local decay, call result); copy the
                 * 8-byte chunk(s) through the address. */
                zero_cc_emit_struct_copy_to_addr(c, arena, ptr_temp, p_size, node->right, right_temp);
                c->last_expr_is_float = 0;
            } else if (p_size <= 8 && !is_compound &&
                       target_ti && (target_ti->kind == TYPE_KIND_STRUCT || target_ti->kind == TYPE_KIND_UNION) &&
                       ast_expr_is_struct_addr(c, arena, node->right)) {
                /* Struct member RHS (`*tp = q->d`, d <= 8B) lowers to the
                 * member's address - copy through it, don't store the
                 * pointer value. */
                zero_cc_emit_struct_copy_to_addr(c, arena, ptr_temp, p_size, node->right, right_temp);
                c->last_expr_is_float = 0;
            } else if (p_size <= 8) {
                if (p_size == 1) ir_emit_store_byte(final_val, ptr_temp);
                else if (p_size == 2) ir_emit_store_16(final_val, ptr_temp);
                else if (p_size == 4) ir_emit_store_32(final_val, ptr_temp);
                else ir_emit_store_64(final_val, ptr_temp);
                c->last_expr_is_float = 0;
            } else if (p_size > 8 && !is_compound) {
                zero_cc_emit_struct_copy_to_addr(c, arena, ptr_temp, p_size, node->right, right_temp);
                c->last_expr_is_float = 0;
            }
            if (!c->last_expr_is_float) {
                lower_extend_narrow(c, final_val, target_ti);
            }
            return final_val;
        }

        int right_temp = lower_expr(c, arena, node->right);
        int right_is_float = c->last_expr_is_float;

        /* C11 6.5.16: pointer compound assignment (p += n / p -= n)
         * scales by sizeof(pointee), like pointer +/- arithmetic.  The
         * plain-assignment forms (p = e) get their scaling from the BINOP
         * lowering of the RHS; compound forms have a bare RHS. */
        if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
            TypeInfo *lvti = ast_node_get_typeinfo(c, left_node);
            if (lvti && (type_is_pointer(lvti) || lvti->kind == TYPE_KIND_ARRAY)) {
                size_t psz = type_size(lvti->kind == TYPE_KIND_ARRAY ?
                    lvti->details.array.base : lvti->details.pointer.base);
                if (psz > 1) {
                    int si = c->next_temp_id++;
                    ir_emit_mov_imm(si, (int32_t)psz);
                    int s = c->next_temp_id++;
                    ir_emit_mul(s, right_temp, si);
                    right_temp = s;
                }
            }
        }

        if (left_node && left_node->tag == NODE_MEMBER) {
            /* C11 6.5.16.1: the RHS of an assignment is converted to the
             * lvalue's type.  A float/double member with an integer RHS
             * must be converted BEFORE the store - a raw integer store
             * writes the integer bit pattern into the IEEE-754 slot
             * (c-torture pr47538: `x->a = y->a` style `y.a = 10`). */
            {
                TypeInfo *lhs_ti = ast_node_get_typeinfo(c, left_node);
                if (!is_compound && !c->last_expr_is_float &&
                    lhs_ti && type_is_floating(lhs_ti)) {
                    int cvt_t = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)cvt_t, (uint16_t)right_temp);
                    right_temp = cvt_t;
                    c->last_expr_is_float = 1;
                    right_is_float = 1;
                }
            }
            /* Struct member assignment: traverse chain of member accesses */
            int32_t member_offset = 0;
            ZeroCCASTNode *curr = left_node;
            ZeroCCASTNode *innermost = left_node;
            int chain_depth = 0;
            ASTRef base_ref = node->left;
            while (curr && curr->tag == NODE_MEMBER) {
                member_offset += (int32_t)curr->aux.alt;
                chain_depth++;
                innermost = curr;
                base_ref = curr->left;
                curr = zero_cc_ast_get(arena, curr->left);
            }
            ZeroCCASTNode *base_node = curr;
            /* The member node directly above the base decides whether the
             * base is a pointer (->) or a struct object (.). */
            bool base_is_ptr = (innermost && (innermost->flags & NODE_MEMBER_IS_ARROW)) != 0;
            if (base_is_ptr && zc_is_array_base(c, arena, innermost, base_node)) base_is_ptr = false;
            int32_t base_stack_offset = 0;

            /* Multi-segment chains (o.p->v = x): compute the address
             * generically - the flat paths below cannot dereference
             * intermediate pointer members (C5). */
            if (chain_depth >= 2) {
                int addr = lower_member_chain_addr(c, arena, left_node);
                if (addr == TEMP_NONE) return TEMP_NONE;
                if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                    return assign_member_bitfield(c, node, left_node, addr, right_temp);
                }
                size_t m_size = 4;
                TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                if (member_ti) m_size = type_size(member_ti);
                bool is_flt = (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float;
                if (is_compound) {
                    if (is_flt) {
                        int lhs_val = c->next_temp_id++;
                        if (m_size == 4) ir_emit_vloadss((uint16_t)lhs_val, (uint16_t)addr);
                        else ir_emit_vloadsd((uint16_t)lhs_val, (uint16_t)addr);
                        int float_rhs = right_temp;
                        if (!c->last_expr_is_float) {
                            float_rhs = c->next_temp_id++;
                            ir_emit_cvtsi2sd((uint16_t)float_rhs, (uint16_t)right_temp);
                        }
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_val, (uint16_t)float_rhs); break;
                            default: op_res = float_rhs; break;
                        }
                        right_temp = op_res;
                        c->last_expr_is_float = 1;
                    } else {
                        int curr_val = c->next_temp_id++;
                        if (m_size == 1) ir_emit_load_byte(curr_val, addr);
                        else if (m_size == 2) ir_emit_load_16(curr_val, addr);
                        else if (m_size == 4) ir_emit_load_32(curr_val, addr);
                        else ir_emit_deref(curr_val, addr);
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
                }
                bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                    (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                      ast_expr_is_struct_addr(c, arena, node->right)) ||
                     (type_is_vector(member_ti) &&
                      ast_expr_is_vector_addr(c, arena, node->right))));
                if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, addr, m_size, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
                if (c->last_expr_is_float) {
                    if (m_size == 4) ir_emit_vstoress(addr, (uint16_t)right_temp);
                    else ir_emit_vstoresd(addr, (uint16_t)right_temp);
                    c->last_expr_is_float = 1;
                } else {
                    if (m_size == 1) ir_emit_store_byte(right_temp, addr);
                    else if (m_size == 2) ir_emit_store_16(right_temp, addr);
                    else if (m_size == 4) ir_emit_store_32(right_temp, addr);
                    else ir_emit_store_64(right_temp, addr);
                    c->last_expr_is_float = 0;
                }
                return right_temp;
            }

            // Find the base variable's stack offset if it's a local identifier
            if (base_node && base_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, base_node->s.str);
                ZeroCCGlobalVar *ggv = sym ? NULL : find_global_var(c, base_node->s.str);
                if (ggv) {
                    /* Global struct/union or pointer base: `g.member = val` or `p->member = val`. */
                    int gaddr = c->next_temp_id++;
                    ir_emit_global_addr(c, gaddr, ggv);
                    if (base_is_ptr) {
                        int ptr_temp = c->next_temp_id++;
                        ir_emit_deref(ptr_temp, gaddr);
                        gaddr = ptr_temp;
                    }
                    int addr_temp = gaddr;
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(off_temp, member_offset);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(addr_temp, gaddr, off_temp);
                    }
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                    }
                    size_t m_size = 4;
                    TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                    if (member_ti) m_size = type_size(member_ti);
                    if (is_compound) {
                        if (member_ti && type_is_vector(member_ti)) {
                            /* Vector member compound (u.v += w / u.v += 2): per-lane in-place op;
                             * the RHS is either a vector in address form (per-lane amount) or a
                             * scalar broadcast to every lane. */
                            ZeroCCASTNode *rhs_n = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                            TypeInfo *rhs_t = rhs_n ? ast_node_get_typeinfo(c, rhs_n) : NULL;
                            bool rhs_v = !!(rhs_t && type_is_vector(rhs_t));
                            ZeroCCType vel;
                            memset(&vel, 0, sizeof(vel));
                            const TypeInfo *el = member_ti->details.array.base;
                            if (el) {
                                vel.size = type_size(el);
                                if (el->kind == TYPE_KIND_FLOAT) vel.tag = TYPE_FLOAT;
                                else if (el->kind == TYPE_KIND_DOUBLE) vel.tag = TYPE_DOUBLE;
                                else if (el->kind == TYPE_KIND_INT || el->kind == TYPE_KIND_LONG ||
                                         el->kind == TYPE_KIND_SHORT || el->kind == TYPE_KIND_CHAR)
                                    vel.tag = (el->modifiers & TYPE_MOD_UNSIGNED) ? TYPE_UINT32 : TYPE_INT32;
                                else vel.tag = TYPE_INT64;
                            }
                            zc_emit_vector_compound_inplace(c, addr_temp, m_size, &vel, node->aux.op,
                                                            right_temp, rhs_v, c->last_expr_is_float);
                            c->last_expr_is_float = 0;
                            return addr_temp;
                        }
                        member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                               m_size,
                                               (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                    }
                    bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                        (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                          ast_expr_is_struct_addr(c, arena, node->right)) ||
                         (type_is_vector(member_ti) &&
                          ast_expr_is_vector_addr(c, arena, node->right))));
                    if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (m_size == 4) {
                            ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                        } else {
                            ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                        }
                        c->last_expr_is_float = 1;
                    } else {
                        if (m_size == 1) {
                            ir_emit_store_byte(right_temp, addr_temp);
                        } else if (m_size == 2) {
                            ir_emit_store_16(right_temp, addr_temp);
                        } else if (m_size == 4) {
                            ir_emit_store_32(right_temp, addr_temp);
                        } else {
                            ir_emit_store_64(right_temp, addr_temp);
                        }
                        c->last_expr_is_float = 0;
                    }
                    lower_extend_narrow(c, right_temp, member_ti);
                    return right_temp;
                }
                if (!sym) {
                    /* Generic base not in the local or global-var tables (e.g. an
                     * extern object registered only in the function table):
                     * resolve the base address via the lvalue resolver (same
                     * fallback the read path uses, which also hits the function
                     * table for extern objects), then store.
                     * (c-torture 20000606-1: `extern struct S bar; bar.a = 0;`.)
                     * This path was previously a hard error. */
                    int gen_base = zero_cc_lower_lvalue_addr(c, arena, base_ref);
                    if (gen_base == TEMP_NONE) gen_base = lower_expr(c, arena, base_ref);
                    if (base_is_ptr && gen_base != TEMP_NONE) {
                        int gptr = c->next_temp_id++;
                        ir_emit_deref(gptr, gen_base);
                        gen_base = gptr;
                    }
                    if (gen_base == TEMP_NONE) {
                        diag_report_error_at_token(c, "undefined local variable '%s' in struct assignment", base_node->s.str);
                        return TEMP_NONE;
                    }
                    int gen_addr = gen_base;
                    if (member_offset != 0) {
                        int goff = c->next_temp_id++;
                        ir_emit_mov_imm(goff, member_offset);
                        gen_addr = c->next_temp_id++;
                        ir_emit_add(gen_addr, gen_base, goff);
                    }
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, gen_addr, right_temp);
                    }
                    size_t gm_size = 4;
                    TypeInfo *gm_ti = ast_node_get_typeinfo(c, left_node);
                    if (gm_ti) gm_size = type_size(gm_ti);
                    if (is_compound) {
                        if (gm_ti && type_is_vector(gm_ti)) {
                            ZeroCCASTNode *rhs_n = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                            TypeInfo *rhs_t = rhs_n ? ast_node_get_typeinfo(c, rhs_n) : NULL;
                            bool rhs_v = !!(rhs_t && type_is_vector(rhs_t));
                            ZeroCCType vel;
                            memset(&vel, 0, sizeof(vel));
                            const TypeInfo *el = gm_ti->details.array.base;
                            if (el) {
                                vel.size = type_size(el);
                                if (el->kind == TYPE_KIND_FLOAT) vel.tag = TYPE_FLOAT;
                                else if (el->kind == TYPE_KIND_DOUBLE) vel.tag = TYPE_DOUBLE;
                                else if (el->kind == TYPE_KIND_INT || el->kind == TYPE_KIND_LONG ||
                                         el->kind == TYPE_KIND_SHORT || el->kind == TYPE_KIND_CHAR)
                                    vel.tag = (el->modifiers & TYPE_MOD_UNSIGNED) ? TYPE_UINT32 : TYPE_INT32;
                                else vel.tag = TYPE_INT64;
                            }
                            zc_emit_vector_compound_inplace(c, gen_addr, gm_size, &vel, node->aux.op,
                                                            right_temp, rhs_v, c->last_expr_is_float);
                            c->last_expr_is_float = 0;
                            return gen_addr;
                        }
                        member_apply_compound(c, node->aux.op, gen_addr, &right_temp,
                                               gm_size,
                                               (gm_ti && type_is_floating(gm_ti)) || c->last_expr_is_float);
                    }
                    bool gm_rhs_struct_addr = (gm_size <= 8 && gm_ti &&
                        (((gm_ti->kind == TYPE_KIND_STRUCT || gm_ti->kind == TYPE_KIND_UNION) &&
                          ast_expr_is_struct_addr(c, arena, node->right)) ||
                         (type_is_vector(gm_ti) &&
                          ast_expr_is_vector_addr(c, arena, node->right))));
                    if ((gm_size > 8 || gm_rhs_struct_addr) && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, gen_addr, gm_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (gm_size == 4) ir_emit_vstoress(gen_addr, (uint16_t)right_temp);
                        else ir_emit_vstoresd(gen_addr, (uint16_t)right_temp);
                        c->last_expr_is_float = 1;
                    } else {
                        if (gm_size == 1) ir_emit_store_byte(right_temp, gen_addr);
                        else if (gm_size == 2) ir_emit_store_16(right_temp, gen_addr);
                        else if (gm_size == 4) ir_emit_store_32(right_temp, gen_addr);
                        else ir_emit_store_64(right_temp, gen_addr);
                        c->last_expr_is_float = 0;
                    }
                    lower_extend_narrow(c, right_temp, gm_ti);
                    return right_temp;
                }
                if (base_is_ptr) {
                    /* p->member = val: store through the pointer */
                    int ptr_temp = c->next_temp_id++;
                    ir_emit_load(ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    int addr_temp = ptr_temp;
                    if (member_offset != 0) {
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(off_temp, member_offset);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(addr_temp, ptr_temp, off_temp);
                    }
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                    }
                    size_t m_size = 4;
                    TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                    if (member_ti) m_size = type_size(member_ti);
                    if (is_compound) {
                        if (member_ti && type_is_vector(member_ti)) {
                            /* Vector member compound (u.v += w / u.v += 2): per-lane in-place op;
                             * the RHS is either a vector in address form (per-lane amount) or a
                             * scalar broadcast to every lane. */
                            ZeroCCASTNode *rhs_n = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                            TypeInfo *rhs_t = rhs_n ? ast_node_get_typeinfo(c, rhs_n) : NULL;
                            bool rhs_v = !!(rhs_t && type_is_vector(rhs_t));
                            ZeroCCType vel;
                            memset(&vel, 0, sizeof(vel));
                            const TypeInfo *el = member_ti->details.array.base;
                            if (el) {
                                vel.size = type_size(el);
                                if (el->kind == TYPE_KIND_FLOAT) vel.tag = TYPE_FLOAT;
                                else if (el->kind == TYPE_KIND_DOUBLE) vel.tag = TYPE_DOUBLE;
                                else if (el->kind == TYPE_KIND_INT || el->kind == TYPE_KIND_LONG ||
                                         el->kind == TYPE_KIND_SHORT || el->kind == TYPE_KIND_CHAR)
                                    vel.tag = (el->modifiers & TYPE_MOD_UNSIGNED) ? TYPE_UINT32 : TYPE_INT32;
                                else vel.tag = TYPE_INT64;
                            }
                            zc_emit_vector_compound_inplace(c, addr_temp, m_size, &vel, node->aux.op,
                                                            right_temp, rhs_v, c->last_expr_is_float);
                            c->last_expr_is_float = 0;
                            return addr_temp;
                        }
                        member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                               m_size,
                                               (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                    }
                    bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                        (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                          ast_expr_is_struct_addr(c, arena, node->right)) ||
                         (type_is_vector(member_ti) &&
                          ast_expr_is_vector_addr(c, arena, node->right))));
                    if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (m_size == 4) {
                            ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                        } else {
                            ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                        }
                        c->last_expr_is_float = 1;
                    } else {
                        if (m_size == 1) {
                            ir_emit_store_byte(right_temp, addr_temp);
                        } else if (m_size == 2) {
                            ir_emit_store_16(right_temp, addr_temp);
                        } else if (m_size == 4) {
                            ir_emit_store_32(right_temp, addr_temp);
                        } else {
                            ir_emit_store_64(right_temp, addr_temp);
                        }
                        c->last_expr_is_float = 0;
                    }
                    return right_temp;
                }
                if (sym->is_const || sym->type.is_const) {
                    diag_report_error_at_token(c, "cannot assign to member of const-qualified struct '%s'", base_node->s.str);
                    return TEMP_NONE;
                }
                TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                if (member_ti && (member_ti->modifiers & TYPE_MOD_CONST)) {
                    diag_report_error_at_token(c, "cannot assign to const-qualified member");
                    return TEMP_NONE;
                }
                for (size_t m_i = 0; m_i < sym->type.member_count; m_i++) {
                    if (sym->type.members[m_i].offset == (size_t)member_offset) {
                        if (sym->type.members[m_i].is_const) {
                            diag_report_error_at_token(c, "cannot assign to const-qualified member '%s'", sym->type.members[m_i].name);
                            return TEMP_NONE;
                        }
                        break;
                    }
                }
                base_stack_offset = -sym->stack_offset;
            } else if (base_node && base_node->tag == NODE_COMPOUND_LIT) {
                (void)lower_expr(c, arena, base_ref);
                ZeroCCSym *sym = lowering_local_sym(c, base_node, base_node->s.str); /* index-based: name lookup misses post-bound __clit syms */
                if (sym) {
                    base_stack_offset = -sym->stack_offset;
                }
            } else {
                /* General base expression evaluating to struct pointer/address (e.g. p[i][j]) */
                int base_ptr = TEMP_NONE;
    if (!base_is_ptr) {
        base_ptr = zero_cc_lower_lvalue_addr(c, arena, base_ref);
    }
    if (base_ptr == TEMP_NONE) {
        base_ptr = lower_expr(c, arena, base_ref);
    }
                int addr_temp = base_ptr;
                if (member_offset != 0) {
                    int off_temp = c->next_temp_id++;
                    ir_emit_mov_imm(off_temp, member_offset);
                    addr_temp = c->next_temp_id++;
                    ir_emit_add(addr_temp, base_ptr, off_temp);
                }
                size_t m_size = 4;
                TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                if (member_ti) m_size = type_size(member_ti);
                if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                    return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
                }
                if (is_compound) {
                    if (member_ti && type_is_vector(member_ti)) {
                        /* Vector member compound (u.v += w / u.v += 2): per-lane in-place op;
                         * the RHS is either a vector in address form (per-lane amount) or a
                         * scalar broadcast to every lane. */
                        ZeroCCASTNode *rhs_n = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                        TypeInfo *rhs_t = rhs_n ? ast_node_get_typeinfo(c, rhs_n) : NULL;
                        bool rhs_v = !!(rhs_t && type_is_vector(rhs_t));
                        ZeroCCType vel;
                        memset(&vel, 0, sizeof(vel));
                        const TypeInfo *el = member_ti->details.array.base;
                        if (el) {
                            vel.size = type_size(el);
                            if (el->kind == TYPE_KIND_FLOAT) vel.tag = TYPE_FLOAT;
                            else if (el->kind == TYPE_KIND_DOUBLE) vel.tag = TYPE_DOUBLE;
                            else if (el->kind == TYPE_KIND_INT || el->kind == TYPE_KIND_LONG ||
                                     el->kind == TYPE_KIND_SHORT || el->kind == TYPE_KIND_CHAR)
                                vel.tag = (el->modifiers & TYPE_MOD_UNSIGNED) ? TYPE_UINT32 : TYPE_INT32;
                            else vel.tag = TYPE_INT64;
                        }
                        zc_emit_vector_compound_inplace(c, addr_temp, m_size, &vel, node->aux.op,
                                                        right_temp, rhs_v, c->last_expr_is_float);
                        c->last_expr_is_float = 0;
                        return addr_temp;
                    }
                    member_apply_compound(c, node->aux.op, addr_temp, &right_temp,
                                           m_size,
                                           (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float);
                }
                bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                    (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                      ast_expr_is_struct_addr(c, arena, node->right)) ||
                     (type_is_vector(member_ti) &&
                      ast_expr_is_vector_addr(c, arena, node->right))));
                if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
                if (right_is_float || c->last_expr_is_float) {
                    if (m_size == 4) {
                        ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                    } else {
                        ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                    }
                    c->last_expr_is_float = 1;
                } else {
                    if (m_size == 1) {
                        ir_emit_store_byte(right_temp, addr_temp);
                    } else if (m_size == 2) {
                        ir_emit_store_16(right_temp, addr_temp);
                    } else if (m_size == 4) {
                        ir_emit_store_32(right_temp, addr_temp);
                    } else {
                        ir_emit_store_64(right_temp, addr_temp);
                    }
                    c->last_expr_is_float = 0;
                }
                return right_temp;
            }

            int32_t total_offset = base_stack_offset + member_offset;

            if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                int addr_temp = c->next_temp_id++;
                ir_emit_lea(addr_temp, TEMP_NONE, total_offset);
                return assign_member_bitfield(c, node, left_node, addr_temp, right_temp);
            }

            /* Compound assignment on a plain (non-bitfield) single-segment
             * member: load the current member value width-correct, apply
             * the op, then the store below writes the combined result.
             * (C7 - previously only the RHS was stored, so `s.w += 5`
             * stored 5.) */
            if (is_compound) {
                int mc_addr = c->next_temp_id++;
                ir_emit_lea(mc_addr, TEMP_NONE, total_offset);
                TypeInfo *mc_ti = ast_node_get_typeinfo(c, left_node);
                size_t mc_size = mc_ti ? type_size(mc_ti) : 4;
                if (mc_ti && type_is_vector(mc_ti)) {
                    /* Vector member compound (u.v += w / u.v += 2): per-lane in-place op;
                     * the RHS is either a vector in address form (per-lane amount) or a
                     * scalar broadcast to every lane. */
                    ZeroCCASTNode *rhs_n = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                    TypeInfo *rhs_t = rhs_n ? ast_node_get_typeinfo(c, rhs_n) : NULL;
                    bool rhs_v = !!(rhs_t && type_is_vector(rhs_t));
                    ZeroCCType vel;
                    memset(&vel, 0, sizeof(vel));
                    const TypeInfo *el = mc_ti->details.array.base;
                    if (el) {
                        vel.size = type_size(el);
                        if (el->kind == TYPE_KIND_FLOAT) vel.tag = TYPE_FLOAT;
                        else if (el->kind == TYPE_KIND_DOUBLE) vel.tag = TYPE_DOUBLE;
                        else if (el->kind == TYPE_KIND_INT || el->kind == TYPE_KIND_LONG ||
                                 el->kind == TYPE_KIND_SHORT || el->kind == TYPE_KIND_CHAR)
                            vel.tag = (el->modifiers & TYPE_MOD_UNSIGNED) ? TYPE_UINT32 : TYPE_INT32;
                        else vel.tag = TYPE_INT64;
                    }
                    zc_emit_vector_compound_inplace(c, mc_addr, mc_size, &vel, node->aux.op,
                                                    right_temp, rhs_v, c->last_expr_is_float);
                    c->last_expr_is_float = 0;
                    return mc_addr;
                }
                member_apply_compound(c, node->aux.op, mc_addr, &right_temp, mc_size,
                                       (mc_ti && type_is_floating(mc_ti)) || c->last_expr_is_float);
            }

            if (right_is_float || c->last_expr_is_float) {
                /* Float value: store via IR. A 4-byte float member must
                 * use the width-correct VSTORESS - the 8-byte FSTORE
                 * writes the raw double bits and clobbers the bytes
                 * following the member (adjacent-member destruction). */
                size_t m_size_f = 4;
                TypeInfo *member_ti_f = ast_node_get_typeinfo(c, left_node);
                if (member_ti_f) m_size_f = type_size(member_ti_f);
                if (m_size_f == 4) {
                    int a4 = c->next_temp_id++;
                    ir_emit_lea(a4, TEMP_NONE, total_offset);
                    ir_emit_vstoress((uint16_t)a4, (uint16_t)right_temp);
                } else {
                    ir_emit_fstore((uint16_t)right_temp, TEMP_NONE, total_offset); /* TEMP_NONE = frame-pointer base */
                }
                c->last_expr_is_float = 1;
                return right_temp;
            } else {
                /* Integer/pointer value in GPR - width-correct store
                 * (a 64-bit store into a 1/2/4-byte member clobbers the
                 * adjacent member). */
                TypeInfo *mc_ti2 = ast_node_get_typeinfo(c, left_node);
                size_t mc_size2 = mc_ti2 ? type_size(mc_ti2) : 8;
                int mc_addr2 = c->next_temp_id++;
                ir_emit_lea(mc_addr2, TEMP_NONE, total_offset);
                bool mc_rhs_struct_addr = (mc_size2 <= 8 && mc_ti2 &&
                    (((mc_ti2->kind == TYPE_KIND_STRUCT || mc_ti2->kind == TYPE_KIND_UNION) &&
                      ast_expr_is_struct_addr(c, arena, node->right)) ||
                     (type_is_vector(mc_ti2) &&
                      ast_expr_is_vector_addr(c, arena, node->right))));
                if ((mc_size2 > 8 || mc_rhs_struct_addr) && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, mc_addr2, mc_size2, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
                if (mc_size2 == 1) ir_emit_store_byte((uint16_t)right_temp, (uint16_t)mc_addr2);
                else if (mc_size2 == 2) ir_emit_store_16((uint16_t)right_temp, (uint16_t)mc_addr2);
                else if (mc_size2 == 4) ir_emit_store_32((uint16_t)right_temp, (uint16_t)mc_addr2);
                else ir_emit_store_64((uint16_t)right_temp, (uint16_t)mc_addr2);
                c->last_expr_is_float = 0;
                lower_extend_narrow(c, right_temp, mc_ti2);
                return right_temp;
            }
        }

        if (left_node && left_node->tag == NODE_INDEX) {
            /* Array element assignment: arr[idx] = expr or arr[idx] op= expr */
            int array_temp = lower_expr(c, arena, left_node->left);
            int index_temp = lower_expr(c, arena, left_node->right);

            size_t elem_size = 4;
            TypeInfo *elem_ti = ast_node_get_typeinfo(c, left_node);
            if (elem_ti) {
                elem_size = type_size(elem_ti);
            } else {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                    if (zt && zt->base && zt->base->size > 0) {
                        elem_size = zt->base->size;
                    }
                } else if (ln && ln->tag == NODE_COMPOUND_LIT) {
                    ZeroCCSym *sym = lowering_local_sym(c, ln, ln->s.str); /* index-based: name lookup misses post-bound __clit syms */
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

            /* float/double RHS into a non-float element: truncate to the
             * element's integer type (C99 6.3.1.3) BEFORE the compound-op
             * math or the store - the raw IEEE-754 bit pattern in the FP
             * temp is not the value (c-torture pr66233: `v[i] = (float)i;`
             * into `unsigned int v[]`). right_is_float was captured right
             * after the RHS lowered; the LHS lowering below clobbers
             * c->last_expr_is_float. */
            if (right_is_float && !(elem_ti && type_is_floating(elem_ti))) {
                int it = c->next_temp_id++;
                ir_emit_cvttsd2si((uint16_t)it, (uint16_t)right_temp);
                if (elem_size == 1 || elem_size == 2 || elem_size == 4) {
                    if (elem_ti && type_is_unsigned(elem_ti)) ir_emit_zext(it, (int)elem_size);
                    else ir_emit_sext(it, (int)elem_size);
                }
                right_temp = it;
                c->last_expr_is_float = 0;
            }
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

            bool is_elem_float = false;
            if (elem_ti && type_is_floating(elem_ti)) {
                is_elem_float = true;
            } else {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                    if (zt && zt->base && (zt->base->tag == TYPE_FLOAT || zt->base->tag == TYPE_DOUBLE)) {
                        is_elem_float = true;
                    }
                }
            }

            if (is_elem_float) {
                if (!right_is_float) {
                    int flt_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)flt_temp, (uint16_t)right_temp);
                    right_temp = flt_temp;
                }
                if (elem_size == 4) {
                    ir_emit_vstoress(addr_temp, (uint16_t)right_temp);
                } else {
                    ir_emit_vstoresd(addr_temp, (uint16_t)right_temp);
                }
                if (elem_size == 16) {
                    /* long double element: 16-byte storage, 8-byte value -
                     * zero the top half (c-torture ieee/copysign2). */
                    int z8 = c->next_temp_id++;
                    ir_emit_mov_imm(z8, 0);
                    int o8 = c->next_temp_id++;
                    ir_emit_mov_imm(o8, 8);
                    int zaddr = c->next_temp_id++;
                    ir_emit_add(zaddr, addr_temp, o8);
                    ir_emit_store_64((uint16_t)z8, (uint16_t)zaddr);
                }
                c->last_expr_is_float = 1;
            } else {
                bool is_elem_struct = (elem_ti && (elem_ti->kind == TYPE_KIND_STRUCT || elem_ti->kind == TYPE_KIND_UNION));
                if (!is_elem_struct) {
                    ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                    if (ln && ln->tag == NODE_IDENT) {
                        const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                        if (zt && zt->base && (zt->base->tag == TYPE_STRUCT || zt->base->tag == TYPE_UNION)) {
                            is_elem_struct = true;
                        }
                    }
                }
                if (is_elem_struct && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, elem_size, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return right_temp;
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
            }
            lower_extend_narrow(c, right_temp, elem_ti);
            return right_temp;
        }

        const char *var = left_node ? left_node->s.str : "";
        ZeroCCSym *sym = zero_cc_find_local(c, var);
        if (sym && !sym->is_extern) {
            if (sym->is_const || sym->type.is_const) {
                diag_report_error_at_token(c, "cannot assign to const-qualified variable '%s'", var);
                return TEMP_NONE;
            }
            bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            if (sym->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&sym->type)) {
                /* GNU vector target: the RHS is the address of a vector
                 * object (per-lane binop/unop temp, decaying ident,
                 * compound literal, sret call buffer) or - for a <=16B
                 * by-value call result - the VREG_0/VREG_2 pair. */
                size_t vsize = sym->type.size ? sym->type.size : 16;
                const ZeroCCType *vel = sym->type.base;
                size_t vesz = (vel && vel->size > 0) ? vel->size : 4;
                size_t vlanes = (vesz > 0) ? (vsize / vesz) : 0;
                if (is_compound && vsize >= 8 && vlanes > 0 && vlanes <= 32) {
                    /* Element-wise in-place compound (v %= 3, v += w,
                     * v >>= c, ...): scalar RHS broadcasts to every lane,
                     * vector RHS reads a per-lane amount. */
                    ZeroCCASTNode *rhs_node_l = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                    TypeInfo *rhs_ti_l = rhs_node_l ? ast_node_get_typeinfo(c, rhs_node_l) : NULL;
                    bool rhs_vec_l = !!(rhs_ti_l && type_is_vector(rhs_ti_l));
                    int base_t = c->next_temp_id++;
                    ir_emit_lea(base_t, BE(c)->fp_reg, -sym->stack_offset);
                    zc_emit_vector_compound_inplace(c, base_t, vsize, vel, node->aux.op,
                                                    right_temp, rhs_vec_l, c->last_expr_is_float);
                    c->last_expr_is_float = 0;
                    return base_t;
                }
                if (vsize > 8 && !is_compound) {
                    int dst_addr = c->next_temp_id++;
                    ir_emit_lea(dst_addr, TEMP_NONE, -sym->stack_offset);
                    zero_cc_emit_struct_copy_to_addr(c, arena, dst_addr, vsize, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
                /* <=8B vector: the RHS vector expression lowers to the
                 * ADDRESS of the vector object (decayed ident, per-lane
                 * temp, call buffer) - load width-correct, then store. */
                if (vsize == 8) {
                    int vv = c->next_temp_id++;
                    ir_emit_deref(vv, right_temp);
                    ir_emit_store(vv, BE(c)->fp_reg, -sym->stack_offset);
                } else {
                    int va = c->next_temp_id++;
                    ir_emit_lea(va, BE(c)->fp_reg, -sym->stack_offset);
                    int vv = c->next_temp_id++;
                    if (vsize == 1) { ir_emit_load_byte(vv, right_temp); ir_emit_store_byte(vv, va); }
                    else if (vsize == 2) { ir_emit_load_16(vv, right_temp); ir_emit_store_16(vv, va); }
                    else { ir_emit_load_32(vv, right_temp); ir_emit_store_32(vv, va); }
                }
                c->last_expr_is_float = 0;
                return right_temp;
            }
            if (target_is_float) {
                int float_temp = right_temp;
                if (!right_is_float) {
                    /* Convert int to float via IR */
                    float_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)float_temp, (uint16_t)right_temp);
                }
                /* For compound assignment, load current value, operate, then store */
                if (is_compound) {
                    int lhs_temp = c->next_temp_id++;
                    if (sym->type.tag == TYPE_FLOAT) {
                        int addr = c->next_temp_id++;
                        ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_vloadss((uint16_t)lhs_temp, (uint16_t)addr);
                    } else {
                        ir_emit_fload((uint16_t)lhs_temp, TEMP_NONE, -sym->stack_offset);
                    }
                    int op_res = c->next_temp_id++;
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_fadd((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_fsub((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_fmul((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_fdiv((uint16_t)op_res, (uint16_t)lhs_temp, (uint16_t)float_temp); break;
                        default: op_res = float_temp; break;
                    }
                    if (sym->type.tag == TYPE_FLOAT) {
                        int addr = c->next_temp_id++;
                        ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_vstoress((uint16_t)addr, (uint16_t)op_res);
                    } else {
                        ir_emit_fstore((uint16_t)op_res, TEMP_NONE, -sym->stack_offset);
                    }
                    if (sym->type.is_long_double) {
                        /* Zero the top 8 bytes of the 16-byte slot: the
                         * value is an 8-byte double and the top half is
                         * otherwise stale stack (c-torture ieee/copysign*
                         * memcmp the full sizeof(long double)). */
                        int z8 = c->next_temp_id++;
                        ir_emit_mov_imm(z8, 0);
                        ir_emit_store(z8, BE(c)->fp_reg, -sym->stack_offset + 8);
                    }
                    c->last_expr_is_float = 1;
                    return op_res;
                } else {
                    /* Simple assignment: just store */
                    if (sym->type.tag == TYPE_FLOAT) {
                        int addr = c->next_temp_id++;
                        ir_emit_lea(addr, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_vstoress((uint16_t)addr, (uint16_t)float_temp);
                    } else {
                        ir_emit_fstore((uint16_t)float_temp, TEMP_NONE, -sym->stack_offset);
                    }
                    if (sym->type.is_long_double) {
                        int z8 = c->next_temp_id++;
                        ir_emit_mov_imm(z8, 0);
                        ir_emit_store(z8, BE(c)->fp_reg, -sym->stack_offset + 8);
                    }
                    c->last_expr_is_float = 1;
                    return float_temp;
                }
            } else {
                if (c->last_expr_is_float) {
                    /* Convert float to int via IR */
                    int int_temp = c->next_temp_id++;
                    ir_emit_cvttsd2si((uint16_t)int_temp, (uint16_t)right_temp);
                    if (is_compound) {
                        /* Load current value and perform operation */
                        int left_temp = c->next_temp_id++;
                        ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(int_temp, left_temp, int_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(int_temp, left_temp, int_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(int_temp, left_temp, int_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(int_temp, left_temp, int_temp); break;
                            case TK_MOD_ASSIGN: ir_emit_mod(int_temp, left_temp, int_temp); break;
                            case TK_AND_ASSIGN: ir_emit_and(int_temp, left_temp, int_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(int_temp, left_temp, int_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(int_temp, left_temp, int_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(int_temp, left_temp, int_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(int_temp, left_temp, int_temp); break;
                        }
                    }
                    ir_emit_store((uint16_t)int_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return int_temp;
                } else {
                    if (sym && (sym->is_atomic || sym->type.is_atomic)) {
                        int addr_temp = c->next_temp_id++;
                        ir_emit_lea(addr_temp, TEMP_NONE, -sym->stack_offset);
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
                        /* Load current value and perform operation.
                         * The result goes into a fresh temp: right_temp may
                         * hold a loop-invariant constant that must survive
                         * repeated uses, so it must not be used as dest. */
                        int left_temp = c->next_temp_id++;
                        ir_emit_load(left_temp, BE(c)->fp_reg, -sym->stack_offset);
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(op_res, left_temp, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(op_res, left_temp, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(op_res, left_temp, right_temp); break;
                            case TK_DIV_ASSIGN:
                                ir_emit_div(op_res, left_temp, right_temp);
                                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) ir_mark_last_unsigned(1);
                                break;
                            case TK_MOD_ASSIGN:
                                ir_emit_mod(op_res, left_temp, right_temp);
                                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) ir_mark_last_unsigned(1);
                                break;
                            case TK_AND_ASSIGN: ir_emit_and(op_res, left_temp, right_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(op_res, left_temp, right_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(op_res, left_temp, right_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(op_res, left_temp, right_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(op_res, left_temp, right_temp); break;
                        }
                        right_temp = op_res;
                    }
                    if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 16 && !is_compound) {
                        int dst_addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                        zero_cc_emit_struct_copy_to_addr(c, arena, dst_addr, sym->type.size, node->right, right_temp);
                    } else {
                        bool rhs_is_struct_addr = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                                  !is_compound && ast_expr_is_struct_addr(c, arena, node->right);
                        if (rhs_is_struct_addr) {
                            if (sym->type.size <= 8) {
                                int val_temp = c->next_temp_id++;
                                if (sym->type.size == 1) ir_emit_load_byte(val_temp, right_temp);
                                else if (sym->type.size == 2) ir_emit_load_16(val_temp, right_temp);
                                else if (sym->type.size == 4) ir_emit_load_32(val_temp, right_temp);
                                else ir_emit_deref(val_temp, right_temp);
                                zc_sym_store(c, sym, 0, val_temp);  /* Sprint 72 */
                            } else {
                                int val1 = c->next_temp_id++;
                                ir_emit_deref(val1, right_temp);
                                zc_sym_store(c, sym, 0, val1);  /* Sprint 72 */

                                int off8 = c->next_temp_id++;
                                ir_emit_mov_imm(off8, 8);
                                int addr2 = c->next_temp_id++;
                                ir_emit_add(addr2, right_temp, off8);

                                int val2 = c->next_temp_id++;
                                size_t rem = sym->type.size - 8;
                                if (rem == 8) {
                                    ir_emit_deref(val2, addr2);
                                    zc_sym_store(c, sym, 8, val2);  /* Sprint 72 */
                                } else {
                                    if (rem == 1) ir_emit_load_byte(val2, addr2);
                                    else if (rem == 2) ir_emit_load_16(val2, addr2);
                                    else if (rem == 4) ir_emit_load_32(val2, addr2);
                                    else ir_emit_deref(val2, addr2);
                                    /* Store exactly rem bytes - a full
                                     * 8-byte store scribbles the bytes
                                     * adjacent to the destination slot
                                     * (c-torture execute/va-arg-22). */
                                    int dst_a;
                                    if (zc_sym_uses_data_section(sym)) {
                                        dst_a = zc_sym_addr(c, sym, 8);  /* Sprint 72 */
                                    } else {
                                        dst_a = c->next_temp_id++;
                                        ir_emit_lea(dst_a, TEMP_NONE, -(sym->stack_offset - 8));
                                    }
                                    zc_store_low_bytes(c, val2, dst_a, rem);
                                }
                            }
                        } else {
                            lower_assign_conv_zt(c, right_temp, &sym->type);
                            zc_sym_store(c, sym, 0, right_temp);  /* Sprint 72 */
                            if (c->isa == ISA_RISCV32 && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 4 && sym->type.size <= 8 && !is_compound) {
                                /* RV32 ILP32 ABI: second 4-byte chunk returned in a1 (VREG_2) */
                                zc_sym_store(c, sym, 4, VREG_2);  /* Sprint 72 */
                            } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16 && !is_compound) {
                                int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? 1 : 2;
                                size_t rem = sym->type.size - 8;
                                int32_t tail_off = -(sym->stack_offset - 8);
                                if (rem >= 8) {
                                    zc_sym_store(c, sym, 8, reg2);  /* Sprint 72 */
                                } else {
                                    /* Copy exactly the remaining rem
                                     * (1..7) bytes of the secondary
                                     * register - a single narrow store
                                     * drops the extra bytes (c-torture
                                     * execute/va-arg-22: an 11-byte
                                     * struct lost its 11th byte). */
                                    int dst_addr;
                                    if (zc_sym_uses_data_section(sym)) {
                                        dst_addr = zc_sym_addr(c, sym, 8);  /* Sprint 72 */
                                    } else {
                                        dst_addr = c->next_temp_id++;
                                        ir_emit_lea(dst_addr, TEMP_NONE, tail_off);
                                    }
                                    zc_store_low_bytes(c, reg2, dst_addr, rem);
                                }
                            }
                        }
                    }
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
            }
        } else {
            ZeroCCGlobalVar *gv = find_global_var(c, var);
            if (gv) {
                int gaddr_temp = c->next_temp_id++;
                ir_emit_global_addr(c, gaddr_temp, gv);
                int final_val = right_temp;
                size_t gv_sz = gv->size > 0 ? gv->size : 4;
                bool gv_is_float = (gv->type.tag == TYPE_FLOAT);
                bool gv_is_double = (gv->type.tag == TYPE_DOUBLE);
                bool gv_is_flt = gv_is_float || gv_is_double;
                if (is_compound && gv->type.tag == TYPE_USER_0 &&
                    zero_cc_type_is_vector(&gv->type)) {
                    /* Per-lane in-place compound on a global vector
                     * (v += 2, v >>= w, ...): scalar RHS broadcasts to
                     * every lane, vector RHS reads a per-lane amount.
                     * Must run before the float-RHS truncation below -
                     * a float scalar into a float-element vector is a
                     * broadcast, not an integer conversion. */
                    ZeroCCASTNode *rhs_node_g = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                    TypeInfo *rhs_ti_g = rhs_node_g ? ast_node_get_typeinfo(c, rhs_node_g) : NULL;
                    bool rhs_vec_g = !!(rhs_ti_g && type_is_vector(rhs_ti_g));
                    zc_emit_vector_compound_inplace(c, gaddr_temp, gv_sz, gv->type.base,
                                                    node->aux.op, right_temp,
                                                    rhs_vec_g, c->last_expr_is_float);
                    c->last_expr_is_float = 0;
                    return gaddr_temp;
                }
                if (gv_is_flt && c->last_expr_is_float) {
                    /* Float/double global: keep the value in the FP pipeline
                     * (an integer load/add/store on the raw bits would be
                     * arithmetic on the IEEE-754 pattern, not the value). */
                    if (is_compound) {
                        int curr_val = c->next_temp_id++;
                        if (gv_is_float) ir_emit_vloadss((uint16_t)curr_val, (uint16_t)gaddr_temp);
                        else ir_emit_vloadsd((uint16_t)curr_val, (uint16_t)gaddr_temp);
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv(op_res, curr_val, right_temp); break;
                            default: ir_emit_fmov(op_res, right_temp); break; /* bit-ops are invalid on floats */
                        }
                        final_val = op_res;
                    }
                    if (gv_is_float) ir_emit_vstoress((uint16_t)gaddr_temp, (uint16_t)final_val);
                    else ir_emit_vstoresd((uint16_t)gaddr_temp, (uint16_t)final_val);
                    c->last_expr_is_float = 1;
                    return final_val;
                }
                if (gv_is_flt) {
                    /* int RHS -> float global: convert, then store */
                    int flt_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)flt_temp, (uint16_t)right_temp);
                    if (gv_is_float) ir_emit_vstoress(gaddr_temp, (uint16_t)flt_temp);
                    else ir_emit_vstoresd(gaddr_temp, (uint16_t)flt_temp);
                    c->last_expr_is_float = 1;
                    return flt_temp;
                }
                if (c->last_expr_is_float) {
                    /* float/double RHS -> integer global: truncate to the
                     * global's integer type (C99 6.3.1.3) BEFORE the
                     * integer store - the raw IEEE-754 bit pattern is not
                     * the value. Mirrors the local-variable path. */
                    int int_temp = c->next_temp_id++;
                    ir_emit_cvttsd2si((uint16_t)int_temp, (uint16_t)right_temp);
                    if (gv_sz == 1 || gv_sz == 2 || gv_sz == 4) {
                        bool gu = (gv->type.tag == TYPE_UINT8 || gv->type.tag == TYPE_UINT16 ||
                                   gv->type.tag == TYPE_UINT32);
                        if (gu) ir_emit_zext(int_temp, (int)gv_sz);
                        else ir_emit_sext(int_temp, (int)gv_sz);
                    }
                    right_temp = int_temp;
                    final_val = int_temp;
                    c->last_expr_is_float = 0;
                }
                if (is_compound) {
                    int curr_val = c->next_temp_id++;
                    if (gv_sz == 1) ir_emit_load_byte(curr_val, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_load_16(curr_val, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_load_32(curr_val, gaddr_temp);
                    else ir_emit_deref(curr_val, gaddr_temp);

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
                        default: op_res = right_temp; break;
                    }
                    final_val = op_res;
                }
                if (((gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION) ||
                     (gv->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&gv->type))) &&
                    gv_sz > 8 && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, gaddr_temp, gv_sz, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return right_temp;
                }
                bool rhs_is_struct_addr = (gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION) &&
                                          !is_compound && ast_expr_is_struct_addr(c, arena, node->right);
                /* A GNU vector global target: the RHS vector expression
                 * lowers to the object's ADDRESS (decayed ident, compound
                 * literal, per-lane temp) - copy the bytes, don't store
                 * the pointer (c-torture execute/20050604-1). */
                bool rhs_is_vec_addr = gv->type.tag == TYPE_USER_0 &&
                                       zero_cc_type_is_vector(&gv->type) &&
                                       !is_compound && ast_expr_is_vector_addr(c, arena, node->right);
                if (rhs_is_struct_addr || rhs_is_vec_addr) {
                    int val_temp = c->next_temp_id++;
                    if (gv_sz == 1) ir_emit_load_byte(val_temp, final_val);
                    else if (gv_sz == 2) ir_emit_load_16(val_temp, final_val);
                    else if (gv_sz == 4) ir_emit_load_32(val_temp, final_val);
                    else ir_emit_deref(val_temp, final_val);
                    final_val = val_temp;
                }
                lower_assign_conv_zt(c, final_val, &gv->type);
                if (gv_sz == 1) ir_emit_store_byte(final_val, gaddr_temp);
                else if (gv_sz == 2) ir_emit_store_16(final_val, gaddr_temp);
                else if (gv_sz == 4) ir_emit_store_32(final_val, gaddr_temp);
                else ir_emit_store_64(final_val, gaddr_temp);

                c->last_expr_is_float = 0;
                return final_val;
            }
            /* Extern object not defined in this TU (multi-TU / extern
             * declaration): the parser registered it in the function
             * table (is_external) - store through its address, mirroring
             * the NODE_IDENT load path above. Previously this fell
             * through and the store was silently dropped, so `g += 1`
             * on a cross-TU global compiled to a no-op. */
            ZeroCCFunction *xfn = zero_cc_find_function(c, var);
            if (xfn && xfn->is_external) {
                int xaddr = c->next_temp_id++;
                ir_emit_mov_global(xaddr, xfn->name);
                int xfinal = right_temp;
                /* The extern registration stores the declared scalar
                 * type in return_type (same source the NODE_IDENT load
                 * path uses). */
                const ZeroCCType *xt = &xfn->return_type;
                size_t xsz = (xt->size > 0) ? xt->size : 8;
                bool xflt = (xt->tag == TYPE_FLOAT || xt->tag == TYPE_DOUBLE);
                if (xflt && c->last_expr_is_float) {
                    if (is_compound) {
                        int curr_val = c->next_temp_id++;
                        if (xsz == 4) ir_emit_vloadss((uint16_t)curr_val, (uint16_t)xaddr);
                        else ir_emit_vloadsd((uint16_t)curr_val, (uint16_t)xaddr);
                        int op_res = c->next_temp_id++;
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv(op_res, curr_val, right_temp); break;
                            default: ir_emit_fmov(op_res, right_temp); break;
                        }
                        xfinal = op_res;
                    }
                    if (xsz == 4) ir_emit_vstoress((uint16_t)xaddr, (uint16_t)xfinal);
                    else ir_emit_vstoresd((uint16_t)xaddr, (uint16_t)xfinal);
                    c->last_expr_is_float = 1;
                    return xfinal;
                }
                if (xflt) {
                    int flt_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd((uint16_t)flt_temp, (uint16_t)right_temp);
                    if (xsz == 4) ir_emit_vstoress(xaddr, (uint16_t)flt_temp);
                    else ir_emit_vstoresd(xaddr, (uint16_t)flt_temp);
                    c->last_expr_is_float = 1;
                    return flt_temp;
                }
                if (is_compound) {
                    int curr_val = c->next_temp_id++;
                    if (xsz == 1) ir_emit_load_byte(curr_val, xaddr);
                    else if (xsz == 2) ir_emit_load_16(curr_val, xaddr);
                    else if (xsz == 4) ir_emit_load_32(curr_val, xaddr);
                    else ir_emit_deref(curr_val, xaddr);
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
                        default: op_res = right_temp; break;
                    }
                    xfinal = op_res;
                }
                if (xsz == 1) ir_emit_store_byte(xfinal, xaddr);
                else if (xsz == 2) ir_emit_store_16(xfinal, xaddr);
                else if (xsz == 4) ir_emit_store_32(xfinal, xaddr);
                else ir_emit_store_64(xfinal, xaddr);
                c->last_expr_is_float = 0;
                return xfinal;
            }
        }
        c->last_expr_is_float = 0;
        return right_temp;
}
