/* AST Lowering - Assignment Expression Lowering (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
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
        int la = zc_alloc_temp(c);
        if (i == 0) { ir_emit_mov_reg(c, la, base_t); }
        else { int o = zc_alloc_temp(c); ir_emit_mov_imm(c, o, (int32_t)(i * esz));
               ir_emit_add(c, la, base_t, o); }
        int lv = zc_alloc_temp(c);
        int rv = zc_alloc_temp(c);
        if (el_flt) {
            if (esz == 4) ir_emit_vloadss(c, (uint32_t)lv, (uint32_t)la);
            else ir_emit_vloadsd(c, (uint32_t)lv, (uint32_t)la);
            if (rhs_is_vec) {
                int ra = zc_alloc_temp(c);
                if (i == 0) { ir_emit_mov_reg(c, ra, rhs_t); }
                else { int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, (int32_t)(i * esz));
                         ir_emit_add(c, ra, rhs_t, o2); }
                if (esz == 4) ir_emit_vloadss(c, (uint32_t)rv, (uint32_t)ra);
                else ir_emit_vloadsd(c, (uint32_t)rv, (uint32_t)ra);
            } else if (rhs_is_flt) {
                ir_emit_mov_reg(c, rv, rhs_t);
            } else {
                ir_emit_cvtsi2sd(c, (uint32_t)rv, (uint32_t)rhs_t);
            }
            int ov = zc_alloc_temp(c);
            switch (op) {
                case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)ov, (uint32_t)lv, (uint32_t)rv); break;
                case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)ov, (uint32_t)lv, (uint32_t)rv); break;
                case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)ov, (uint32_t)lv, (uint32_t)rv); break;
                default:            ir_emit_fdiv(c, (uint32_t)ov, (uint32_t)lv, (uint32_t)rv); break;
            }
            if (esz == 4) ir_emit_vstoress(c, la, (uint32_t)ov);
            else ir_emit_vstoresd(c, la, (uint32_t)ov);
            continue;
        }
        if (esz == 1) { ir_emit_load_byte(c, lv, la); if (el_uns) ir_emit_zext(c, lv, 1); else ir_emit_sext(c, lv, 1); }
        else if (esz == 2) { ir_emit_load_16(c, lv, la); if (el_uns) ir_emit_zext(c, lv, 2); else ir_emit_sext(c, lv, 2); }
        else if (esz == 4) { ir_emit_load_32(c, lv, la); if (el_uns) ir_emit_zext(c, lv, 4); else ir_emit_sext(c, lv, 4); }
        else ir_emit_deref(c, lv, la);
        if (rhs_is_vec) {
            int ra = zc_alloc_temp(c);
            if (i == 0) { ir_emit_mov_reg(c, ra, rhs_t); }
            else { int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, (int32_t)(i * esz));
                    ir_emit_add(c, ra, rhs_t, o2); }
            if (esz == 1) { ir_emit_load_byte(c, rv, ra); if (el_uns) ir_emit_zext(c, rv, 1); else ir_emit_sext(c, rv, 1); }
            else if (esz == 2) { ir_emit_load_16(c, rv, ra); if (el_uns) ir_emit_zext(c, rv, 2); else ir_emit_sext(c, rv, 2); }
            else if (esz == 4) { ir_emit_load_32(c, rv, ra); if (el_uns) ir_emit_zext(c, rv, 4); else ir_emit_sext(c, rv, 4); }
            else ir_emit_deref(c, rv, ra);
        } else {
            ir_emit_mov_reg(c, rv, rhs_t);
        }
        int ov = zc_alloc_temp(c);
        switch (op) {
            case TK_ADD_ASSIGN: ir_emit_add(c, ov, lv, rv); break;
            case TK_SUB_ASSIGN: ir_emit_sub(c, ov, lv, rv); break;
            case TK_MUL_ASSIGN: ir_emit_mul(c, ov, lv, rv); break;
            case TK_DIV_ASSIGN: ir_emit_div(c, ov, lv, rv); if (el_uns) ir_mark_last_unsigned(c, 1); break;
            case TK_MOD_ASSIGN: ir_emit_mod(c, ov, lv, rv); if (el_uns) ir_mark_last_unsigned(c, 1); break;
            case TK_AND_ASSIGN: ir_emit_and(c, ov, lv, rv); break;
            case TK_OR_ASSIGN:  ir_emit_or(c, ov, lv, rv); break;
            case TK_XOR_ASSIGN: ir_emit_xor(c, ov, lv, rv); break;
            case TK_SHL_ASSIGN: ir_emit_shl(c, ov, lv, rv); break;
            case TK_SHR_ASSIGN: ir_emit_shr(c, ov, lv, rv); break;
            default: ir_emit_mov_reg(c, ov, lv); break;
        }
        if (esz == 1) ir_emit_store_byte(c, ov, la);
        else if (esz == 2) ir_emit_store_16(c, ov, la);
        else if (esz == 4) ir_emit_store_32(c, ov, la);
        else ir_emit_store_64(c, ov, la);
    }
}

static void member_apply_compound(ZeroCCCompiler *c, int op_tok, int addr,
                                  int *right_temp, size_t m_size, bool is_flt,
                                  const TypeInfo *member_ti) {
    /* C99 6.5.16.1: an integer member with a FLOATING-POINT RHS
     * (m.b -= post): the math is done in DOUBLE (the usual arithmetic
     * conversions promote the member) and the result is converted back
     * to the member's integer type on the assignment. The pre-fix
     * is_flt = (member FP) || (RHS FP) test took the FP path and ran
     * fsub on the member's raw integer bit pattern (test/abi_interop
     * s07: `m.b -= post` stored a double into the long long field). */
    if (!is_flt && c->last_expr_is_float && member_ti &&
        !type_is_floating(member_ti) &&
        (op_tok == TK_ADD_ASSIGN || op_tok == TK_SUB_ASSIGN ||
         op_tok == TK_MUL_ASSIGN || op_tok == TK_DIV_ASSIGN)) {
        int curr = zc_alloc_temp(c);
        if (m_size == 1) ir_emit_load_byte(c, curr, addr);
        else if (m_size == 2) ir_emit_load_16(c, curr, addr);
        else if (m_size == 4) ir_emit_load_32(c, curr, addr);
        else ir_emit_deref(c, curr, addr);
        int cf = zc_alloc_temp(c);
        ir_emit_cvtsi2sd(c, (uint32_t)cf, (uint32_t)curr);
        int resf = zc_alloc_temp(c);
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)resf, (uint32_t)cf, (uint32_t)*right_temp); break;
            case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)resf, (uint32_t)cf, (uint32_t)*right_temp); break;
            case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)resf, (uint32_t)cf, (uint32_t)*right_temp); break;
            default:            ir_emit_fdiv(c, (uint32_t)resf, (uint32_t)cf, (uint32_t)*right_temp); break;
        }
        int it = zc_alloc_temp(c);
        ir_emit_cvttsd2si(c, (uint32_t)it, (uint32_t)resf);
        *right_temp = it;
        c->last_expr_is_float = 0;
        return;
    }

    if (is_flt) {
        int lhs_val = zc_alloc_temp(c);
        if (m_size == 4) ir_emit_vloadss(c, (uint32_t)lhs_val, (uint32_t)addr);
        else ir_emit_vloadsd(c, (uint32_t)lhs_val, (uint32_t)addr);
        int float_rhs = *right_temp;
        if (!c->last_expr_is_float) {
            float_rhs = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)float_rhs, (uint32_t)*right_temp);
        }
        int op_res = zc_alloc_temp(c);
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
            case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
            case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
            case TK_DIV_ASSIGN: ir_emit_fdiv(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
            default: op_res = float_rhs; break;
        }
        *right_temp = op_res;
        c->last_expr_is_float = 1;
    } else {
        int curr_val = zc_alloc_temp(c);
        if (m_size == 1) ir_emit_load_byte(c, curr_val, addr);
        else if (m_size == 2) ir_emit_load_16(c, curr_val, addr);
        else if (m_size == 4) ir_emit_load_32(c, curr_val, addr);
        else ir_emit_deref(c, curr_val, addr);
        int op_res = zc_alloc_temp(c);
        switch (op_tok) {
            case TK_ADD_ASSIGN: ir_emit_add(c, op_res, curr_val, *right_temp); break;
            case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, curr_val, *right_temp); break;
            case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, curr_val, *right_temp); break;
            case TK_DIV_ASSIGN: ir_emit_div(c, op_res, curr_val, *right_temp); break;
            case TK_MOD_ASSIGN: ir_emit_mod(c, op_res, curr_val, *right_temp); break;
            case TK_AND_ASSIGN: ir_emit_and(c, op_res, curr_val, *right_temp); break;
            case TK_OR_ASSIGN:  ir_emit_or(c, op_res, curr_val, *right_temp); break;
            case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, curr_val, *right_temp); break;
            case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, curr_val, *right_temp); break;
            case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, curr_val, *right_temp); break;
            default: ir_emit_mov_reg(c, op_res, *right_temp); break;
        }
        *right_temp = op_res;
    }
}

/* Wide __int128 bitfield RMW (assign_member_bitfield + 128-bit div/mod/shift
 * helpers) -> ast_lowering_bitfield.c (Sprint 114). */



int lower_expr_assign(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* Check if this is a compound assignment (+=, -=, etc.) */
        bool is_compound = (node->aux.op != TK_ASSIGN && node->aux.op != '=');
        ZeroCCASTNode *left_node = zero_cc_ast_get(arena, node->left);

        /* GNU __int128 (Sprint 109): assignment to a 128-bit scalar lvalue
         * (local, global, or element). Store the full 16 bytes. Compound ops
         * (+=, -=, *=, &=, |=, ^=) desugar to a 128-bit binop on the loaded
         * LHS and the RHS. */
        if (left_node && node->right != AST_NULL) {
            TypeInfo *lhs_ti = ast_node_get_typeinfo(c, left_node);
            ZeroCCSym *lhs_sym = (left_node->tag == NODE_IDENT) ? zero_cc_find_local(c, left_node->s.str) : NULL;
            ZeroCCGlobalVar *lhs_gv = (left_node->tag == NODE_IDENT && left_node->s.str) ? find_global_var(c, left_node->s.str) : NULL;
            bool lhs_is128 = (lhs_ti && lhs_ti->kind == TYPE_KIND_INT128 && type_size(lhs_ti) == 16) ||
                             (lhs_sym && (lhs_sym->type.tag == TYPE_INT128 || lhs_sym->type.tag == TYPE_UINT128)) ||
                             (lhs_gv && (lhs_gv->type.tag == TYPE_INT128 || lhs_gv->type.tag == TYPE_UINT128));
            /* Bit-field members (Sprint 113): a 128-bit-typed field is a
             * sub-unit value - the raw 16-byte store would skip the
             * read-modify-write and clobber the field's neighbours. The
             * member-assignment path below does the field RMW. */
            if (lhs_is128 && left_node->flags & NODE_MEMBER_IS_BITFIELD) lhs_is128 = false;
            if (lhs_is128) {
                bool unsigned_ = lhs_ti ? type_is_unsigned(lhs_ti) :
                              (lhs_sym ? zero_cc_type_is_unsigned(&lhs_sym->type) :
                               (lhs_gv ? zero_cc_type_is_unsigned(&lhs_gv->type) : false));
                /* Decide whether the 128-bit path can handle this assignment:
                 * plain `=`, arithmetic/bitwise compounds, and constant-count
                 * shift compounds.  div/mod and non-constant shifts are deferred
                 * to the generic path below (128-bit division is not implemented). */
                uint32_t cop = 0;
                if (is_compound) {
                    cop = node->aux.op == TK_ADD_ASSIGN ? TK_PLUS :
                          node->aux.op == TK_SUB_ASSIGN ? TK_MINUS :
                          node->aux.op == TK_MUL_ASSIGN ? TK_STAR :
                          node->aux.op == TK_AND_ASSIGN ? TK_AMP :
                          node->aux.op == TK_OR_ASSIGN ? TK_PIPE :
                          node->aux.op == TK_XOR_ASSIGN ? TK_CARET :
                          node->aux.op == TK_SHL_ASSIGN ? TK_SHL :
                          node->aux.op == TK_SHR_ASSIGN ? TK_SHR : 0;
                }
                bool shift_op = (cop == TK_SHL || cop == TK_SHR);
                bool shift_const = !shift_op;
                if (shift_op) {
                    ZeroCCASTNode *rc = zero_cc_ast_get(arena, node->right);
                    shift_const = (rc && rc->tag == NODE_INT);
                }
                bool supported = !is_compound || (!shift_op && cop != 0) || (shift_op && shift_const);
                if (supported) {
                    int target = zero_cc_lower_lvalue_addr(c, arena, node->left);
                    if (target != TEMP_NONE) {
                        int val;
                        int val_is128 = 0;
                        if (!is_compound) {
                            val = lower_expr(c, arena, node->right);
                            val_is128 = c->last_expr_is_128;
                        } else if (shift_op) {
                            int64_t N = zero_cc_ast_get(arena, node->right)->int_val;
                            int lv = lower_expr(c, arena, node->left);
                            int lv128 = c->last_expr_is_128;
                            if (!lv128) { int d = zc_alloc_temp(c); if (unsigned_) ir_emit_zext64_128(c, d, lv); else ir_emit_sext64_128(c, d, lv); lv = d; }
                            int s_lo = zc_alloc_temp(c); ir_emit_lo128(c, s_lo, lv);
                            int s_hi = zc_alloc_temp(c); ir_emit_hi128(c, s_hi, lv);
                            int dlo = zc_alloc_temp(c), dhi = zc_alloc_temp(c);
                            if (N <= 0) { ir_emit_mov_reg(c, dlo, s_lo); ir_emit_mov_reg(c, dhi, s_hi); }
                            else zc_emit_128_shift(c, dlo, dhi, s_lo, s_hi, N, cop, !unsigned_);
                            val = zc_alloc_temp(c); ir_emit_mk128(c, val, dlo, dhi);
                            val_is128 = 1;
                        } else {
                            int lv = lower_expr(c, arena, node->left);
                            int lv128 = c->last_expr_is_128;
                            int rv = lower_expr(c, arena, node->right);
                            int rv128 = c->last_expr_is_128;
                            if (!lv128) { int d = zc_alloc_temp(c); if (unsigned_) ir_emit_zext64_128(c, d, lv); else ir_emit_sext64_128(c, d, lv); lv = d; }
                            if (!rv128) { int d = zc_alloc_temp(c); if (unsigned_) ir_emit_zext64_128(c, d, rv); else ir_emit_sext64_128(c, d, rv); rv = d; }
                            val = zc_alloc_temp(c);
                            switch (cop) {
                                case TK_PLUS:  ir_emit_add128(c, val, lv, rv); break;
                                case TK_MINUS: ir_emit_sub128(c, val, lv, rv); break;
                                case TK_STAR:  ir_emit_mul128(c, val, lv, rv); break;
                                case TK_AMP:   ir_emit_and128(c, val, lv, rv); break;
                                case TK_PIPE:  ir_emit_or128(c, val, lv, rv);  break;
                                case TK_CARET: ir_emit_xor128(c, val, lv, rv); break;
                            }
                            val_is128 = 1;  /* 128-bit binop result is GPR128 */
                        }
                        if (!val_is128) {
                            int d = zc_alloc_temp(c);
                            if (unsigned_) ir_emit_zext64_128(c, d, val); else ir_emit_sext64_128(c, d, val);
                            val = d;
                        }
                        ir_emit_store128(c, val, target);
                        c->last_expr_is_float = 0;
                        c->last_expr_is_128 = 1;  /* the stored value is 128-bit */
                        return val;
                    }
                }
            }
        }

        /* GNU variably-modified struct/union target: the object is
         * runtime-sized (the static layout size is a 2-element
         * placeholder), so the value copy must use the runtime size.
         * Lower as a runtime-sized memcpy; the assignment's value is the
         * source address (struct values are address-form). */
        if (!is_compound && left_node && node->right != AST_NULL) {
            ZeroCCSym *lhs_sym_v = (left_node->tag == NODE_IDENT)
                ? zero_cc_find_local(c, left_node->s.str) : NULL;
            TypeInfo *lhs_ti_v = ast_node_get_typeinfo(c, left_node);
            const ZeroCCType *vla_v = NULL;
            if (lhs_sym_v) {
                vla_v = zc_find_vla_struct(&lhs_sym_v->type);
            } else if (lhs_ti_v && (lhs_ti_v->kind == TYPE_KIND_STRUCT || lhs_ti_v->kind == TYPE_KIND_UNION) &&
                       lhs_ti_v->details.record.is_vla_struct && lhs_ti_v->details.record.tag) {
                ZeroCCType *reg = zero_cc_find_struct(c, lhs_ti_v->details.record.tag);
                if (reg) vla_v = zc_find_vla_struct(reg);
            }
            if (vla_v && !vla_v->members && vla_v->struct_name[0]) {
                ZeroCCType *reg = zero_cc_find_struct(c, vla_v->struct_name);
                if (reg) vla_v = reg;
            }
            if (vla_v) {
                int dst_v = zero_cc_lower_lvalue_addr(c, arena, node->left);
                int src_v = zero_cc_lower_lvalue_addr(c, arena, node->right);
                if (src_v == TEMP_NONE) src_v = lower_expr(c, arena, node->right);
                if (dst_v == TEMP_NONE) {
                    diag_report_error_at_token(c, "cannot take address of assignment target for variable-length struct copy");
                    return TEMP_NONE;
                }
                int n_v = TEMP_NONE;
                ASTRef sz_v = zc_vla_struct_size_ast(arena, (ZeroCCASTArena *)c->tu_ast_arena, vla_v, vla_v->struct_name);
                if (sz_v != AST_NULL) n_v = lower_expr(c, arena, sz_v);
                if (n_v == TEMP_NONE) {
                    n_v = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, n_v, (int32_t)vla_v->size);
                }
                zero_cc_emit_vla_struct_copy(c, dst_v, src_v, n_v);
                c->last_expr_is_float = 0;
                return src_v;
            }
        }

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
                const TypeInfo *el_ti_c = (lhs_ti_c && lhs_ti_c->kind == TYPE_KIND_COMPLEX) ? lhs_ti_c->details.array.base : NULL;
                const ZeroCCType *el_zt_c = (cx_lsym_c && zero_cc_type_is_complex(&cx_lsym_c->type)) ? cx_lsym_c->type.base : NULL;
                size_t esz_c = el_ti_c ? type_size(el_ti_c) : (el_zt_c ? el_zt_c->size : 8);
                if (esz_c == 0) esz_c = 8;
                bool is_fp_c = el_ti_c ? type_is_floating(el_ti_c) :
                               (el_zt_c ? (el_zt_c->tag == TYPE_FLOAT || el_zt_c->tag == TYPE_DOUBLE) : true);
                if (is_fp_c && esz_c != 4 && esz_c != 8) esz_c = 8;
                int target_off = TEMP_NONE, target_addr = TEMP_NONE;
                if (cx_lsym_c && left_node->tag == NODE_IDENT) {
                    target_off = cx_lsym_c->stack_offset;
                } else if (left_node->tag == NODE_UNOP &&
                           (left_node->aux.op == '*' || left_node->aux.op == TK_STAR)) {
                    target_addr = lower_expr(c, arena, left_node->left);
                } else if (left_node->tag == NODE_MEMBER) {
                    target_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
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

                    int rhc = zc_alloc_temp(c), lic = zc_alloc_temp(c);
                    int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, (int)esz_c);
                    int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, res_addr, o2);
                    if (is_fp_c) {
                        if (esz_c == 4) {
                            ir_emit_vloadss(c, rhc, (uint32_t)res_addr);
                            ir_emit_vloadss(c, lic, (uint32_t)a2);
                        } else {
                            ir_emit_vloadsd(c, rhc, (uint32_t)res_addr);
                            ir_emit_vloadsd(c, lic, (uint32_t)a2);
                        }
                    } else {
                        if (esz_c == 1) {
                            ir_emit_load_byte(c, rhc, res_addr);
                            ir_emit_load_byte(c, lic, a2);
                        } else if (esz_c == 2) {
                            ir_emit_load_16(c, rhc, res_addr);
                            ir_emit_load_16(c, lic, a2);
                        } else if (esz_c == 4) {
                            ir_emit_load_32(c, rhc, res_addr);
                            ir_emit_load_32(c, lic, a2);
                        } else {
                            ir_emit_deref(c, rhc, res_addr);
                            ir_emit_deref(c, lic, a2);
                        }
                    }
                    if (target_off != TEMP_NONE) {
                        int sa = zc_alloc_temp(c);
                        ir_emit_lea(c, sa, BE(c)->fp_reg, -target_off);
                        target_addr = sa;
                    }
                    if (is_fp_c) {
                        if (esz_c == 4) {
                            ir_emit_vstoress(c, target_addr, (uint32_t)rhc);
                            int sa2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sa2, 4);
                            int sa3 = zc_alloc_temp(c); ir_emit_add(c, sa3, target_addr, sa2);
                            ir_emit_vstoress(c, sa3, (uint32_t)lic);
                        } else {
                            ir_emit_vstoresd(c, target_addr, (uint32_t)rhc);
                            int sa2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sa2, 8);
                            int sa3 = zc_alloc_temp(c); ir_emit_add(c, sa3, target_addr, sa2);
                            ir_emit_vstoresd(c, sa3, (uint32_t)lic);
                        }
                    } else {
                        int sa2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sa2, (int)esz_c);
                        int sa3 = zc_alloc_temp(c); ir_emit_add(c, sa3, target_addr, sa2);
                        if (esz_c == 1) {
                            ir_emit_store_byte(c, rhc, target_addr);
                            ir_emit_store_byte(c, lic, sa3);
                        } else if (esz_c == 2) {
                            ir_emit_store_16(c, rhc, target_addr);
                            ir_emit_store_16(c, lic, sa3);
                        } else if (esz_c == 4) {
                            ir_emit_store_32(c, rhc, target_addr);
                            ir_emit_store_32(c, lic, sa3);
                        } else {
                            ir_emit_store_64(c, rhc, target_addr);
                            ir_emit_store_64(c, lic, sa3);
                        }
                    }
                    c->last_expr_is_float = 0;
                    int zt = zc_alloc_temp(c); ir_emit_mov_imm(c, zt, 0);
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
                    const TypeInfo *pel_ti = (cxati && cxati->kind == TYPE_KIND_COMPLEX) ? cxati->details.array.base : NULL;
                    const ZeroCCType *pel_zt = NULL;
                    if (!pel_ti && cxan && cxan->tag == NODE_IDENT) {
                        ZeroCCSym *sym = zero_cc_find_local(c, cxan->s.str);
                        if (sym && zero_cc_type_is_complex(&sym->type)) pel_zt = sym->type.base;
                        else {
                            ZeroCCGlobalVar *gv = find_global_var(c, cxan->s.str);
                            if (gv && zero_cc_type_is_complex(&gv->type)) pel_zt = gv->type.base;
                        }
                    }
                    if (cxati || pel_zt) {
                        size_t pesz = pel_ti ? type_size(pel_ti) : (pel_zt ? pel_zt->size : 8);
                        if (pesz == 0) pesz = 8;
                        bool pel_is_fp = pel_ti ? type_is_floating(pel_ti) :
                                         (pel_zt ? (pel_zt->tag == TYPE_FLOAT || pel_zt->tag == TYPE_DOUBLE) : true);
                        if (pel_is_fp && pesz != 4 && pesz != 8) pesz = 8;
                        int paddr = lower_expr(c, arena, left_node->right);
                        if (cxpart_imag) {
                            int po = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, po, (int)pesz);
                            int pa = zc_alloc_temp(c);
                            ir_emit_add(c, pa, paddr, po);
                            paddr = pa;
                        }
                        int prv = lower_expr(c, arena, node->right);
                        if (pel_is_fp) {
                            if (c->last_expr_is_float) {
                                if (pesz == 4) ir_emit_vstoress(c, (uint32_t)paddr, (uint32_t)prv);
                                else ir_emit_vstoresd(c, (uint32_t)paddr, (uint32_t)prv);
                                return prv;
                            } else {
                                int pdv = zc_emit_int_to_double(c, prv, rhs_ti_a);
                                if (pesz == 4) ir_emit_vstoress(c, (uint32_t)paddr, (uint32_t)pdv);
                                else ir_emit_vstoresd(c, (uint32_t)paddr, (uint32_t)pdv);
                                c->last_expr_is_float = 1;
                                return pdv;
                            }
                        } else {
                            int piv = prv;
                            if (c->last_expr_is_float) {
                                piv = zc_alloc_temp(c);
                                ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = piv,
                                    .temp_src1 = prv, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                            }
                            if (pesz == 1) ir_emit_store_byte(c, piv, paddr);
                            else if (pesz == 2) ir_emit_store_16(c, piv, paddr);
                            else if (pesz == 4) ir_emit_store_32(c, piv, paddr);
                            else ir_emit_store_64(c, piv, paddr);
                            c->last_expr_is_float = 0;
                            return piv;
                        }
                    }
                }
            }
            ZeroCCSym *cx_lsym = (left_node && left_node->tag == NODE_IDENT &&
                                  zero_cc_find_local(c, left_node->s.str))
                                     ? zero_cc_find_local(c, left_node->s.str) : NULL;
            const ZeroCCType *lhs_zt = cx_lsym ? &cx_lsym->type : NULL;
            if (!lhs_zt && left_node && left_node->tag == NODE_IDENT) {
                ZeroCCGlobalVar *gv = find_global_var(c, left_node->s.str);
                if (gv && zero_cc_type_is_complex(&gv->type)) {
                    lhs_zt = &gv->type;
                    lhs_cx = true;
                }
            }
            if (!lhs_ti && left_node && left_node->tag == NODE_INDEX) {
                ZeroCCASTNode *an = zero_cc_ast_get(arena, left_node->left);
                if (an && an->tag == NODE_IDENT) {
                    ZeroCCSym *asym = zero_cc_find_local(c, an->s.str);
                    if (asym && asym->type.base && zero_cc_type_is_complex(asym->type.base)) {
                        lhs_zt = asym->type.base;
                        lhs_cx = true;
                    } else {
                        ZeroCCGlobalVar *agv = find_global_var(c, an->s.str);
                        if (agv && agv->type.base && zero_cc_type_is_complex(agv->type.base)) {
                            lhs_zt = agv->type.base;
                            lhs_cx = true;
                        }
                    }
                }
            }
            if (!lhs_cx && !rhs_cx && cx_lsym && zero_cc_type_is_complex(&cx_lsym->type)) {
                lhs_cx = true;
            }
            if (lhs_cx || rhs_cx) {
                const TypeInfo *lhs_el = (lhs_cx && lhs_ti && lhs_ti->details.array.base) ? lhs_ti->details.array.base : NULL;
                const TypeInfo *rhs_el = (rhs_cx && rhs_ti_a && rhs_ti_a->details.array.base) ? rhs_ti_a->details.array.base : NULL;
                bool lhs_is_fp = lhs_el ? type_is_floating(lhs_el) :
                                 (lhs_zt ? ((lhs_zt->base ? lhs_zt->base->tag : lhs_zt->tag) == TYPE_FLOAT ||
                                            (lhs_zt->base ? lhs_zt->base->tag : lhs_zt->tag) == TYPE_DOUBLE) : true);
                bool rhs_is_fp = rhs_el ? type_is_floating(rhs_el) : true;
                size_t lhs_esz = lhs_el ? type_size(lhs_el) : (lhs_zt ? (lhs_zt->base ? lhs_zt->base->size : lhs_zt->size) : 8);
                if (lhs_esz == 0) lhs_esz = 8;
                if (lhs_is_fp && lhs_esz != 4 && lhs_esz != 8) lhs_esz = 8;
                size_t rhs_esz = rhs_el ? type_size(rhs_el) : 8;
                if (rhs_esz == 0) rhs_esz = 8;
                if (rhs_is_fp && rhs_esz != 4 && rhs_esz != 8) rhs_esz = 8;

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
                int rh = zc_alloc_temp(c), li = zc_alloc_temp(c);
                if (rhs_cx) {
                    if (rhs_is_fp) {
                        if (rhs_esz == 4) { ir_emit_vloadss(c, rh, (uint32_t)rv); int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, 4); int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, rv, o2); ir_emit_vloadss(c, li, (uint32_t)a2); }
                        else { ir_emit_vloadsd(c, rh, (uint32_t)rv); int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, 8); int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, rv, o2); ir_emit_vloadsd(c, li, (uint32_t)a2); }
                    } else {
                        int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, (int)rhs_esz);
                        int a2 = zc_alloc_temp(c); ir_emit_add(c, a2, rv, o2);
                        if (rhs_esz == 1) { ir_emit_load_byte(c, rh, rv); ir_emit_load_byte(c, li, a2); }
                        else if (rhs_esz == 2) { ir_emit_load_16(c, rh, rv); ir_emit_load_16(c, li, a2); }
                        else if (rhs_esz == 4) { ir_emit_load_32(c, rh, rv); ir_emit_load_32(c, li, a2); }
                        else { ir_emit_deref(c, rh, rv); ir_emit_deref(c, li, a2); }
                        if (rhs_el) { lower_extend_narrow(c, rh, rhs_el); lower_extend_narrow(c, li, rhs_el); }
                        else if (rhs_esz < 8) { ir_emit_sext(c, rh, (int)rhs_esz); ir_emit_sext(c, li, (int)rhs_esz); }
                    }
                } else {
                    if (lhs_is_fp) {
                        if (rv_is_float) {
                            ir_emit_fmov(c, rh, (uint32_t)rv);
                        } else {
                            rh = zc_emit_int_to_double(c, rv, rhs_ti_a);
                        }
                        ir_emit_fmov_imm(c, (uint32_t)li, (int64_t)zc_rodata_double_offset(c, 0.0));
                    } else {
                        if (rv_is_float) {
                            rh = zc_alloc_temp(c);
                            ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = rh,
                                .temp_src1 = rv, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                        } else {
                            rh = rv;
                        }
                        ir_emit_mov_imm(c, li, 0);
                    }
                }

                /* Convert (rh, li) from (rhs_is_fp, rhs_esz) to (lhs_is_fp, lhs_esz) */
                if (rhs_cx && rhs_is_fp && !lhs_is_fp) {
                    int irh = zc_alloc_temp(c);
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = irh,
                                       .temp_src1 = rh, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    int ili = zc_alloc_temp(c);
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = ili,
                                       .temp_src1 = li, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    rh = irh;
                    li = ili;
                } else if (rhs_cx && !rhs_is_fp && lhs_is_fp) {
                    int frh = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)frh, (uint32_t)rh);
                    int fli = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)fli, (uint32_t)li);
                    rh = frh;
                    li = fli;
                } else if (rhs_cx && !rhs_is_fp && !lhs_is_fp) {
                    if (lhs_el) { lower_extend_narrow(c, rh, lhs_el); lower_extend_narrow(c, li, lhs_el); }
                    else if (lhs_zt && lhs_zt->base) { lower_assign_conv_zt(c, rh, lhs_zt->base); lower_assign_conv_zt(c, li, lhs_zt->base); }
                    else if (lhs_esz < 8) { ir_emit_sext(c, rh, (int)lhs_esz); ir_emit_sext(c, li, (int)lhs_esz); }
                }
                if (lhs_cx) {
                    if (target_off != TEMP_NONE) {
                        int sa = zc_alloc_temp(c);
                        ir_emit_lea(c, sa, BE(c)->fp_reg, -target_off);
                        target_addr = sa;
                    }
                    if (lhs_is_fp) {
                        if (lhs_esz == 4) {
                            ir_emit_vstoress(c, target_addr, (uint32_t)rh);
                            int sa2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sa2, 4);
                            int sa3 = zc_alloc_temp(c); ir_emit_add(c, sa3, target_addr, sa2);
                            ir_emit_vstoress(c, sa3, (uint32_t)li);
                        } else {
                            ir_emit_vstoresd(c, target_addr, (uint32_t)rh);
                            int sa2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sa2, 8);
                            int sa3 = zc_alloc_temp(c); ir_emit_add(c, sa3, target_addr, sa2);
                            ir_emit_vstoresd(c, sa3, (uint32_t)li);
                        }
                    } else {
                        int sa2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sa2, (int)lhs_esz);
                        int sa3 = zc_alloc_temp(c); ir_emit_add(c, sa3, target_addr, sa2);
                        if (lhs_esz == 1) {
                            ir_emit_store_byte(c, rh, target_addr);
                            ir_emit_store_byte(c, li, sa3);
                        } else if (lhs_esz == 2) {
                            ir_emit_store_16(c, rh, target_addr);
                            ir_emit_store_16(c, li, sa3);
                        } else if (lhs_esz == 4) {
                            ir_emit_store_32(c, rh, target_addr);
                            ir_emit_store_32(c, li, sa3);
                        } else {
                            ir_emit_store_64(c, rh, target_addr);
                            ir_emit_store_64(c, li, sa3);
                        }
                    }
                    c->last_expr_is_float = 0;
                    return target_addr;
                } else {
                    /* scalar LHS = complex RHS: real half */
                    (void)li;
                    if (lhs_ti && (lhs_ti->kind == TYPE_KIND_FLOAT || lhs_ti->kind == TYPE_KIND_DOUBLE)) {
                        if (rhs_is_fp) {
                            c->last_expr_is_float = 1;
                            return rh;
                        } else {
                            int fd = zc_alloc_temp(c);
                            ir_emit_cvtsi2sd(c, (uint32_t)fd, (uint32_t)rh);
                            c->last_expr_is_float = 1;
                            return fd;
                        }
                    }
                    /* integral LHS */
                    if (rhs_is_fp) {
                        int zt2 = zc_alloc_temp(c);
                        ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = zt2,
                            .temp_src1 = rh, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                        c->last_expr_is_float = 0;
                        return zt2;
                    } else {
                        c->last_expr_is_float = 0;
                        return rh;
                    }
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
                    right_temp = zc_alloc_temp(c);
                    ir_emit_lea(c, right_temp, BE(c)->fp_reg, -rhs_sym->stack_offset);
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
                        int si = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, si, (int32_t)psz);
                        int s = zc_alloc_temp(c);
                        ir_emit_mul(c, s, right_temp, si);
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
                    int laddr = zc_alloc_temp(c);
                    if (i == 0) { ir_emit_mov_reg(c, laddr, ptr_temp); }
                    else {
                        int off = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off, (int32_t)(i * elsz));
                        ir_emit_add(c, laddr, ptr_temp, off);
                    }
                    int cur = zc_alloc_temp(c);
                    int rvl = zc_alloc_temp(c);
                    if (lt_float) {
                        /* Float element: double-width FP model */
                        if (elsz == 4) ir_emit_vloadss(c, (uint32_t)cur, (uint32_t)laddr);
                        else ir_emit_vloadsd(c, (uint32_t)cur, (uint32_t)laddr);
                        if (!rhs_is_scalar_c) {
                            int raddr = zc_alloc_temp(c);
                            if (i == 0) { ir_emit_mov_reg(c, raddr, right_temp); }
                            else { int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, (int32_t)(i * elsz));
                                   ir_emit_add(c, raddr, right_temp, o2); }
                            if (elsz == 4) ir_emit_vloadss(c, (uint32_t)rvl, (uint32_t)raddr);
                            else ir_emit_vloadsd(c, (uint32_t)rvl, (uint32_t)raddr);
                        } else if (c->last_expr_is_float) {
                            ir_emit_mov_reg(c, rvl, right_temp);
                        } else {
                            ir_emit_cvtsi2sd(c, (uint32_t)rvl, (uint32_t)right_temp);
                        }
                        int res = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)res, (uint32_t)cur, (uint32_t)rvl); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)res, (uint32_t)cur, (uint32_t)rvl); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)res, (uint32_t)cur, (uint32_t)rvl); break;
                            default:            ir_emit_fdiv(c, (uint32_t)res, (uint32_t)cur, (uint32_t)rvl); break;
                        }
                        if (elsz == 4) ir_emit_vstoress(c, laddr, (uint32_t)res);
                        else ir_emit_vstoresd(c, laddr, (uint32_t)res);
                        continue;
                    }
                    if (elsz == 1) { ir_emit_load_byte(c, cur, laddr); ir_emit_load_byte(c, rvl, laddr); }
                    else if (elsz == 2) { ir_emit_load_16(c, cur, laddr); ir_emit_load_16(c, rvl, laddr); }
                    else if (elsz == 4) { ir_emit_load_32(c, cur, laddr); ir_emit_load_32(c, rvl, laddr); }
                    else { ir_emit_deref(c, cur, laddr); ir_emit_deref(c, rvl, laddr); }
                    if (!rhs_is_scalar_c) {
                        int raddr = zc_alloc_temp(c);
                        if (i == 0) { ir_emit_mov_reg(c, raddr, right_temp); }
                        else { int o2 = zc_alloc_temp(c); ir_emit_mov_imm(c, o2, (int32_t)(i * elsz));
                               ir_emit_add(c, raddr, right_temp, o2); }
                        if (elsz == 1) ir_emit_load_byte(c, rvl, raddr);
                        else if (elsz == 2) ir_emit_load_16(c, rvl, raddr);
                        else if (elsz == 4) ir_emit_load_32(c, rvl, raddr);
                        else ir_emit_deref(c, rvl, raddr);
                    } else {
                        ir_emit_mov_reg(c, rvl, right_temp);
                    }
                    int res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(c, res, cur, rvl); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(c, res, cur, rvl); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(c, res, cur, rvl); break;
                        case TK_DIV_ASSIGN: ir_emit_div(c, res, cur, rvl); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(c, res, cur, rvl); break;
                        case TK_AND_ASSIGN: ir_emit_and(c, res, cur, rvl); break;
                        case TK_OR_ASSIGN:  ir_emit_or(c, res, cur, rvl); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(c, res, cur, rvl); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(c, res, cur, rvl); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(c, res, cur, rvl); break;
                        default: res = rvl; break;
                    }
                    if (elsz == 1) ir_emit_store_byte(c, res, laddr);
                    else if (elsz == 2) ir_emit_store_16(c, res, laddr);
                    else if (elsz == 4) ir_emit_store_32(c, res, laddr);
                    else ir_emit_store_64(c, res, laddr);
                }
                final_val = zc_alloc_temp(c);
                ir_emit_mov_imm(c, final_val, 0);
                c->last_expr_is_float = 0;
                return final_val;
            }
            if (is_compound) {
                /* C11 6.5.16.1: floating-point compound assignment through a
                 * pointer/deref (*p += x, a[i] += x). The current value must
                 * be loaded as a float and the arithmetic done in floating
                 * point - an integer load (deref) + integer add operates on
                 * the raw IEEE-754 bit pattern, not the value (Sprint 126:
                 * sqlite3AtoF's dekkerMul2 `x[1] += cc` miscompiled to 2x).
                 * Mirrors the global-variable float compound path. */
                if (target_ti && type_is_floating(target_ti)) {
                    /* C11 6.5.16.1: floating-point compound through a pointer/
                     * deref (*p += x). Do the math in the pointee's native
                     * width: load (vloadss/vloadsd), convert the RHS to the
                     * same width, FADD (width from temp tags), store. C99
                     * 6.3.1.3: int RHS converts. Sprint 126: the old code did
                     * an integer deref + integer add on the raw IEEE-754 bits. */
                    int curr_val = zc_alloc_temp(c);
                    if (p_size == 4) ir_emit_vloadss(c, (uint32_t)curr_val, (uint32_t)ptr_temp);
                    else ir_emit_vloadsd(c, (uint32_t)curr_val, (uint32_t)ptr_temp);
                    int rhs_f = right_temp;
                    ZeroCCASTNode *rhs_node_u = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                    const TypeInfo *rhs_ti_u = rhs_node_u ? ast_node_get_typeinfo(c, rhs_node_u) : NULL;
                    int rhs_w = (rhs_ti_u && rhs_ti_u->kind == TYPE_KIND_FLOAT) ? 4 :
                                (rhs_ti_u && rhs_ti_u->kind == TYPE_KIND_DOUBLE) ? 8 : 0; /* 0 = int */
                    if (p_size == 4) {
                        if (rhs_w == 8) { int t = zc_alloc_temp(c); ir_emit_cvtsd2ss(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                        else if (rhs_w == 0) { int t = zc_alloc_temp(c); ir_emit_vcvtsi2ss(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                    } else {
                        if (rhs_w == 4) { int t = zc_alloc_temp(c); ir_emit_cvtss2sd(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                        else if (rhs_w == 0) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                    }
                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_fadd(c, op_res, curr_val, rhs_f); break;
                        case TK_SUB_ASSIGN: ir_emit_fsub(c, op_res, curr_val, rhs_f); break;
                        case TK_MUL_ASSIGN: ir_emit_fmul(c, op_res, curr_val, rhs_f); break;
                        case TK_DIV_ASSIGN: ir_emit_fdiv(c, op_res, curr_val, rhs_f); break;
                        default: ir_emit_mov_reg(c, op_res, rhs_f); break; /* bit/shift/mod invalid */
                    }
                    final_val = op_res;
                    c->last_expr_is_float = 1;
                } else {
                    int curr_val = zc_alloc_temp(c);
                    if (p_size == 1) ir_emit_load_byte(c, curr_val, ptr_temp);
                    else if (p_size == 2) ir_emit_load_16(c, curr_val, ptr_temp);
                    else if (p_size == 4) ir_emit_load_32(c, curr_val, ptr_temp);
                    else ir_emit_deref(c, curr_val, ptr_temp);
                    lower_extend_narrow(c, curr_val, target_ti);
                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(c, op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(c, op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(c, op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(c, op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(c, op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, curr_val, right_temp); break;
                        default: op_res = right_temp; break;
                    }
                    final_val = op_res;
                }
            }
            if (!c->last_expr_is_float && !is_compound &&
                target_ti && type_is_floating(target_ti)) {
                /* C11 6.5.16.1: `*p = <int>` into a float/double pointee
                 * must convert before the FP store (c-torture pr47538). */
                int cvt_t = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)cvt_t, (uint32_t)final_val);
                final_val = cvt_t;
                c->last_expr_is_float = 1;
            }
            if (c->last_expr_is_float) {
                if (p_size == 4) ir_emit_vstoress(c, (uint32_t)ptr_temp, (uint32_t)final_val);
                else ir_emit_vstoresd(c, (uint32_t)ptr_temp, (uint32_t)final_val);
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
                if (p_size == 1) ir_emit_store_byte(c, final_val, ptr_temp);
                else if (p_size == 2) ir_emit_store_16(c, final_val, ptr_temp);
                else if (p_size == 4) ir_emit_store_32(c, final_val, ptr_temp);
                else ir_emit_store_64(c, final_val, ptr_temp);
                c->last_expr_is_float = 0;
            } else if (p_size > 8 && !is_compound) {
                zero_cc_emit_struct_copy_to_addr(c, arena, ptr_temp, p_size, node->right, right_temp);
                c->last_expr_is_float = 0;
                /* The assignment expression's value is its destination:
                 * report the pointee's address (chained `*p = *q = r` etc.
                 * copy from there). */
                final_val = ptr_temp;
            }
            if (!c->last_expr_is_float) {
                lower_extend_narrow(c, final_val, target_ti);
            }
            return final_val;
        }

        int right_temp = lower_expr(c, arena, node->right);
        int right_is_float = c->last_expr_is_float;
        int right_is_128 = c->last_expr_is_128; /* Sprint 113 wide bit-field RMW */

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
                    int si = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, si, (int32_t)psz);
                    int s = zc_alloc_temp(c);
                    ir_emit_mul(c, s, right_temp, si);
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
                    int cvt_t = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)cvt_t, (uint32_t)right_temp);
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
                    return assign_member_bitfield(c, node, left_node, addr, right_temp, right_is_128);
                }
                size_t m_size = 4;
                TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                if (member_ti) m_size = type_size(member_ti);
                bool is_flt = (member_ti && type_is_floating(member_ti)) || c->last_expr_is_float;
                if (is_compound) {
                    if (is_flt) {
                        int lhs_val = zc_alloc_temp(c);
                        if (m_size == 4) ir_emit_vloadss(c, (uint32_t)lhs_val, (uint32_t)addr);
                        else ir_emit_vloadsd(c, (uint32_t)lhs_val, (uint32_t)addr);
                        int float_rhs = right_temp;
                        if (!c->last_expr_is_float) {
                            float_rhs = zc_alloc_temp(c);
                            ir_emit_cvtsi2sd(c, (uint32_t)float_rhs, (uint32_t)right_temp);
                        }
                        int op_res = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv(c, (uint32_t)op_res, (uint32_t)lhs_val, (uint32_t)float_rhs); break;
                            default: op_res = float_rhs; break;
                        }
                        right_temp = op_res;
                        c->last_expr_is_float = 1;
                    } else {
                        int curr_val = zc_alloc_temp(c);
                        if (m_size == 1) ir_emit_load_byte(c, curr_val, addr);
                        else if (m_size == 2) ir_emit_load_16(c, curr_val, addr);
                        else if (m_size == 4) ir_emit_load_32(c, curr_val, addr);
                        else ir_emit_deref(c, curr_val, addr);
                        int op_res = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(c, op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(c, op_res, curr_val, right_temp); break;
                            case TK_MOD_ASSIGN: ir_emit_mod(c, op_res, curr_val, right_temp); break;
                            case TK_AND_ASSIGN: ir_emit_and(c, op_res, curr_val, right_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(c, op_res, curr_val, right_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, curr_val, right_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, curr_val, right_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, curr_val, right_temp); break;
                            default: ir_emit_mov_reg(c, op_res, right_temp); break;
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
                    return (m_size > 8) ? addr : right_temp;
                }
                if (c->last_expr_is_float) {
                    if (m_size == 4) ir_emit_vstoress(c, addr, (uint32_t)right_temp);
                    else ir_emit_vstoresd(c, addr, (uint32_t)right_temp);
                    c->last_expr_is_float = 1;
                } else {
                    if (m_size == 1) ir_emit_store_byte(c, right_temp, addr);
                    else if (m_size == 2) ir_emit_store_16(c, right_temp, addr);
                    else if (m_size == 4) ir_emit_store_32(c, right_temp, addr);
                    else ir_emit_store_64(c, right_temp, addr);
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
                    int gaddr = zc_alloc_temp(c);
                    ir_emit_global_addr(c, gaddr, ggv);
                    if (base_is_ptr) {
                        int ptr_temp = zc_alloc_temp(c);
                        ir_emit_deref(c, ptr_temp, gaddr);
                        gaddr = ptr_temp;
                    }
                    int addr_temp = gaddr;
                    if (member_offset != 0) {
                        int off_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        addr_temp = zc_alloc_temp(c);
                        ir_emit_add(c, addr_temp, gaddr, off_temp);
                    }
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, addr_temp, right_temp, right_is_128);
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
                                               member_ti && type_is_floating(member_ti), member_ti);
                        right_is_float = c->last_expr_is_float;
                    }
                    bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                        (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                          ast_expr_is_struct_addr(c, arena, node->right)) ||
                         (type_is_vector(member_ti) &&
                          ast_expr_is_vector_addr(c, arena, node->right))));
                    if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return (m_size > 8) ? addr_temp : right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (m_size == 4) {
                            ir_emit_vstoress(c, addr_temp, (uint32_t)right_temp);
                        } else {
                            ir_emit_vstoresd(c, addr_temp, (uint32_t)right_temp);
                        }
                        c->last_expr_is_float = 1;
                    } else {
                        if (m_size == 1) {
                            ir_emit_store_byte(c, right_temp, addr_temp);
                        } else if (m_size == 2) {
                            ir_emit_store_16(c, right_temp, addr_temp);
                        } else if (m_size == 4) {
                            ir_emit_store_32(c, right_temp, addr_temp);
                        } else {
                            ir_emit_store_64(c, right_temp, addr_temp);
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
                        int gptr = zc_alloc_temp(c);
                        ir_emit_deref(c, gptr, gen_base);
                        gen_base = gptr;
                    }
                    if (gen_base == TEMP_NONE) {
                        diag_report_error_at_token(c, "undefined local variable '%s' in struct assignment", base_node->s.str);
                        return TEMP_NONE;
                    }
                    int gen_addr = gen_base;
                    if (member_offset != 0) {
                        int goff = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, goff, member_offset);
                        gen_addr = zc_alloc_temp(c);
                        ir_emit_add(c, gen_addr, gen_base, goff);
                    }
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, gen_addr, right_temp, right_is_128);
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
                                               gm_ti && type_is_floating(gm_ti), gm_ti);
                        right_is_float = c->last_expr_is_float;
                    }
                    bool gm_rhs_struct_addr = (gm_size <= 8 && gm_ti &&
                        (((gm_ti->kind == TYPE_KIND_STRUCT || gm_ti->kind == TYPE_KIND_UNION) &&
                          ast_expr_is_struct_addr(c, arena, node->right)) ||
                         (type_is_vector(gm_ti) &&
                          ast_expr_is_vector_addr(c, arena, node->right))));
                    if ((gm_size > 8 || gm_rhs_struct_addr) && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, gen_addr, gm_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return (gm_size > 8) ? gen_addr : right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (gm_size == 4) ir_emit_vstoress(c, gen_addr, (uint32_t)right_temp);
                        else ir_emit_vstoresd(c, gen_addr, (uint32_t)right_temp);
                        c->last_expr_is_float = 1;
                    } else {
                        if (gm_size == 1) ir_emit_store_byte(c, right_temp, gen_addr);
                        else if (gm_size == 2) ir_emit_store_16(c, right_temp, gen_addr);
                        else if (gm_size == 4) ir_emit_store_32(c, right_temp, gen_addr);
                        else ir_emit_store_64(c, right_temp, gen_addr);
                        c->last_expr_is_float = 0;
                    }
                    lower_extend_narrow(c, right_temp, gm_ti);
                    return right_temp;
                }
                /* GNU VLA struct local/param: the frame slot holds the
                 * runtime-allocated object POINTER (OP_VLA_ALLOC), so member
                 * stores must go through the pointer path (pointer-relative
                 * addressing) - a frame-relative store would clobber the
                 * saved pointer with member data. */
                if (sym->is_vla_struct) base_is_ptr = true;
                if (base_is_ptr) {
                    /* p->member = val: store through the pointer */
                    int ptr_temp = zc_alloc_temp(c);
                    ir_emit_load(c, ptr_temp, BE(c)->fp_reg, -sym->stack_offset);
                    int addr_temp = ptr_temp;
                    if (member_offset != 0) {
                        int off_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_temp, member_offset);
                        addr_temp = zc_alloc_temp(c);
                        ir_emit_add(c, addr_temp, ptr_temp, off_temp);
                    }
                    if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                        return assign_member_bitfield(c, node, left_node, addr_temp, right_temp, right_is_128);
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
                                               member_ti && type_is_floating(member_ti), member_ti);
                        right_is_float = c->last_expr_is_float;
                    }
                    bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                        (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                          ast_expr_is_struct_addr(c, arena, node->right)) ||
                         (type_is_vector(member_ti) &&
                          ast_expr_is_vector_addr(c, arena, node->right))));
                    if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                        zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        return (m_size > 8) ? addr_temp : right_temp;
                    }
                    if (right_is_float || c->last_expr_is_float) {
                        if (m_size == 4) {
                            ir_emit_vstoress(c, addr_temp, (uint32_t)right_temp);
                        } else {
                            ir_emit_vstoresd(c, addr_temp, (uint32_t)right_temp);
                        }
                        c->last_expr_is_float = 1;
                    } else {
                        if (m_size == 1) {
                            ir_emit_store_byte(c, right_temp, addr_temp);
                        } else if (m_size == 2) {
                            ir_emit_store_16(c, right_temp, addr_temp);
                        } else if (m_size == 4) {
                            ir_emit_store_32(c, right_temp, addr_temp);
                        } else {
                            ir_emit_store_64(c, right_temp, addr_temp);
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
                    int off_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, off_temp, member_offset);
                    addr_temp = zc_alloc_temp(c);
                    ir_emit_add(c, addr_temp, base_ptr, off_temp);
                }
                size_t m_size = 4;
                TypeInfo *member_ti = ast_node_get_typeinfo(c, left_node);
                if (member_ti) m_size = type_size(member_ti);
                if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                    return assign_member_bitfield(c, node, left_node, addr_temp, right_temp, right_is_128);
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
                                           member_ti && type_is_floating(member_ti), member_ti);
                    right_is_float = c->last_expr_is_float;
                }
                bool m_rhs_struct_addr = (m_size <= 8 && member_ti &&
                    (((member_ti->kind == TYPE_KIND_STRUCT || member_ti->kind == TYPE_KIND_UNION) &&
                      ast_expr_is_struct_addr(c, arena, node->right)) ||
                     (type_is_vector(member_ti) &&
                      ast_expr_is_vector_addr(c, arena, node->right))));
                if ((m_size > 8 || m_rhs_struct_addr) && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, m_size, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return (m_size > 8) ? addr_temp : right_temp;
                }
                if (right_is_float || c->last_expr_is_float) {
                    if (m_size == 4) {
                        ir_emit_vstoress(c, addr_temp, (uint32_t)right_temp);
                    } else {
                        ir_emit_vstoresd(c, addr_temp, (uint32_t)right_temp);
                    }
                    c->last_expr_is_float = 1;
                } else {
                    if (m_size == 1) {
                        ir_emit_store_byte(c, right_temp, addr_temp);
                    } else if (m_size == 2) {
                        ir_emit_store_16(c, right_temp, addr_temp);
                    } else if (m_size == 4) {
                        ir_emit_store_32(c, right_temp, addr_temp);
                    } else {
                        ir_emit_store_64(c, right_temp, addr_temp);
                    }
                    c->last_expr_is_float = 0;
                }
                return right_temp;
            }

            int32_t total_offset = base_stack_offset + member_offset;

            if (left_node->flags & NODE_MEMBER_IS_BITFIELD) {
                int addr_temp = zc_alloc_temp(c);
                ir_emit_lea(c, addr_temp, TEMP_NONE, total_offset);
                return assign_member_bitfield(c, node, left_node, addr_temp, right_temp, right_is_128);
            }

            /* Compound assignment on a plain (non-bitfield) single-segment
             * member: load the current member value width-correct, apply
             * the op, then the store below writes the combined result.
             * (C7 - previously only the RHS was stored, so `s.w += 5`
             * stored 5.) */
            if (is_compound) {
                int mc_addr = zc_alloc_temp(c);
                ir_emit_lea(c, mc_addr, TEMP_NONE, total_offset);
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
                                       mc_ti && type_is_floating(mc_ti), mc_ti);
                right_is_float = c->last_expr_is_float;
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
                    int a4 = zc_alloc_temp(c);
                    ir_emit_lea(c, a4, TEMP_NONE, total_offset);
                    ir_emit_vstoress(c, (uint32_t)a4, (uint32_t)right_temp);
                } else {
                    ir_emit_fstore(c, (uint32_t)right_temp, TEMP_NONE, total_offset); /* TEMP_NONE = frame-pointer base */
                }
                c->last_expr_is_float = 1;
                return right_temp;
            } else {
                /* Integer/pointer value in GPR - width-correct store
                 * (a 64-bit store into a 1/2/4-byte member clobbers the
                 * adjacent member). */
                TypeInfo *mc_ti2 = ast_node_get_typeinfo(c, left_node);
                size_t mc_size2 = mc_ti2 ? type_size(mc_ti2) : 8;
                int mc_addr2 = zc_alloc_temp(c);
                ir_emit_lea(c, mc_addr2, TEMP_NONE, total_offset);
                bool mc_rhs_struct_addr = (mc_size2 <= 8 && mc_ti2 &&
                    (((mc_ti2->kind == TYPE_KIND_STRUCT || mc_ti2->kind == TYPE_KIND_UNION) &&
                      ast_expr_is_struct_addr(c, arena, node->right)) ||
                     (type_is_vector(mc_ti2) &&
                      ast_expr_is_vector_addr(c, arena, node->right))));
                if ((mc_size2 > 8 || mc_rhs_struct_addr) && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, mc_addr2, mc_size2, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return (mc_size2 > 8) ? mc_addr2 : right_temp;
                }
                if (mc_size2 == 1) ir_emit_store_byte(c, (uint32_t)right_temp, (uint32_t)mc_addr2);
                else if (mc_size2 == 2) ir_emit_store_16(c, (uint32_t)right_temp, (uint32_t)mc_addr2);
                else if (mc_size2 == 4) ir_emit_store_32(c, (uint32_t)right_temp, (uint32_t)mc_addr2);
                else ir_emit_store_64(c, (uint32_t)right_temp, (uint32_t)mc_addr2);
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

            ZeroCCSym *vla_2d_sym = NULL;
            {
                ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                if (ln && ln->tag == NODE_IDENT) {
                    ZeroCCSym *ls = zero_cc_find_local(c, ln->s.str);
                    if (ls && ls->is_vla && ls->type.base && ls->type.base->tag == TYPE_ARRAY && ls->type.base->is_vla) {
                        vla_2d_sym = ls;
                    }
                }
            }

            int scaled_index;
            if (vla_2d_sym) {
                int stride_temp = zc_alloc_temp(c);
                ir_emit_load(c, stride_temp, BE(c)->fp_reg, -(vla_2d_sym->stack_offset - 16));
                scaled_index = zc_alloc_temp(c);
                ir_emit_mul(c, scaled_index, index_temp, stride_temp);
            } else if (elem_size == 1) {
                scaled_index = index_temp;
            } else if (elem_size == 2 || elem_size == 4 || elem_size == 8) {
                int shift = (elem_size == 2) ? 1 : ((elem_size == 4) ? 2 : 3);
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
            ir_emit_add(c, addr_temp, array_temp, scaled_index);

            /* float/double RHS into a non-float element: truncate to the
             * element's integer type (C99 6.3.1.3) BEFORE the compound-op
             * math or the store - the raw IEEE-754 bit pattern in the FP
             * temp is not the value (c-torture pr66233: `v[i] = (float)i;`
             * into `unsigned int v[]`). right_is_float was captured right
             * after the RHS lowered; the LHS lowering below clobbers
             * c->last_expr_is_float. */
            if (right_is_float && !(elem_ti && type_is_floating(elem_ti))) {
                int it = zc_alloc_temp(c);
                ir_emit_cvttsd2si(c, (uint32_t)it, (uint32_t)right_temp);
                if (elem_size == 1 || elem_size == 2 || elem_size == 4) {
                    if (elem_ti && type_is_unsigned(elem_ti)) ir_emit_zext(c, it, (int)elem_size);
                    else ir_emit_sext(c, it, (int)elem_size);
                }
                right_temp = it;
                c->last_expr_is_float = 0;
            }
            if (is_compound) {
                /* C11 6.5.16.1: floating-point compound on an array element
                 * (a[i] += x, a[i] *= x, ...): load the current value as a
                 * float and do the math in floating point. An integer deref +
                 * integer add operates on the raw IEEE-754 bit pattern, not
                 * the value (Sprint 126: sqlite3AtoF's dekkerMul2
                 * `x[1] += cc` miscompiled to 2x). right_temp becomes the
                 * float result and right_is_float is set so the store below
                 * uses vstore instead of re-converting. */
                if (elem_ti && type_is_floating(elem_ti)) {
                    /* C11 6.5.16.1: floating-point compound on an array element
                     * (a[i] += x, ...). Do the math in the element's native
                     * width: load the current value (vloadss/vloadsd), convert
                     * the RHS to the same width, FADD (width from temp tags -
                     * ir_emit_fadd, not the SIMD vaddss), store. Sprint 126:
                     * the old code did an integer deref + integer add on the
                     * raw IEEE-754 bits (sqlite3AtoF's dekkerMul2
                     * `x[1] += cc` miscompiled to 2x). */
                    int curr_val = zc_alloc_temp(c);
                    if (elem_size == 4) ir_emit_vloadss(c, (uint32_t)curr_val, (uint32_t)addr_temp);
                    else ir_emit_vloadsd(c, (uint32_t)curr_val, (uint32_t)addr_temp);
                    int rhs_f = right_temp;
                    ZeroCCASTNode *rhs_node = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
                    const TypeInfo *rhs_ti = rhs_node ? ast_node_get_typeinfo(c, rhs_node) : NULL;
                    int rhs_w = (rhs_ti && rhs_ti->kind == TYPE_KIND_FLOAT) ? 4 :
                                (rhs_ti && rhs_ti->kind == TYPE_KIND_DOUBLE) ? 8 : 0; /* 0 = int */
                    if (elem_size == 4) {
                        if (rhs_w == 8) { int t = zc_alloc_temp(c); ir_emit_cvtsd2ss(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                        else if (rhs_w == 0) { int t = zc_alloc_temp(c); ir_emit_vcvtsi2ss(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                    } else {
                        if (rhs_w == 4) { int t = zc_alloc_temp(c); ir_emit_cvtss2sd(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                        else if (rhs_w == 0) { int t = zc_alloc_temp(c); ir_emit_cvtsi2sd(c, (uint32_t)t, (uint32_t)right_temp); rhs_f = t; }
                    }
                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_fadd(c, op_res, curr_val, rhs_f); break;
                        case TK_SUB_ASSIGN: ir_emit_fsub(c, op_res, curr_val, rhs_f); break;
                        case TK_MUL_ASSIGN: ir_emit_fmul(c, op_res, curr_val, rhs_f); break;
                        case TK_DIV_ASSIGN: ir_emit_fdiv(c, op_res, curr_val, rhs_f); break;
                        default: ir_emit_mov_reg(c, op_res, rhs_f); break; /* bit/shift/mod invalid */
                    }
                    right_temp = op_res;
                    right_is_float = 1;
                } else {
                    int curr_val = zc_alloc_temp(c);
                    if (elem_size == 1) {
                        ir_emit_load_byte(c, curr_val, addr_temp);
                    } else if (elem_size == 2) {
                        ir_emit_load_16(c, curr_val, addr_temp);
                    } else if (elem_size == 4) {
                        ir_emit_load_32(c, curr_val, addr_temp);
                    } else {
                        ir_emit_deref(c, curr_val, addr_temp);
                    }
                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(c, op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(c, op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(c, op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(c, op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(c, op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, curr_val, right_temp); break;
                        default: ir_emit_mov_reg(c, op_res, right_temp); break;
                    }
                    right_temp = op_res;
                }
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
                    int flt_temp = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)flt_temp, (uint32_t)right_temp);
                    right_temp = flt_temp;
                }
                if (elem_size == 4) {
                    ir_emit_vstoress(c, addr_temp, (uint32_t)right_temp);
                } else {
                    ir_emit_vstoresd(c, addr_temp, (uint32_t)right_temp);
                }
                if (elem_size == 16) {
                    /* long double element: 16-byte storage, 8-byte value -
                     * zero the top half (c-torture ieee/copysign2). */
                    int z8 = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, z8, 0);
                    int o8 = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, o8, 8);
                    int zaddr = zc_alloc_temp(c);
                    ir_emit_add(c, zaddr, addr_temp, o8);
                    ir_emit_store_64(c, (uint32_t)z8, (uint32_t)zaddr);
                }
                c->last_expr_is_float = 1;
            } else {
                bool is_elem_struct = (elem_ti && (elem_ti->kind == TYPE_KIND_STRUCT || elem_ti->kind == TYPE_KIND_UNION || type_is_vector(elem_ti)));
                if (!is_elem_struct) {
                    ZeroCCASTNode *ln = zero_cc_ast_get(arena, left_node->left);
                    if (ln && ln->tag == NODE_IDENT) {
                        const ZeroCCType *zt = find_ident_zero_type(c, ln->s.str);
                        if (zt && zt->base && (zt->base->tag == TYPE_STRUCT || zt->base->tag == TYPE_UNION || zero_cc_type_is_vector(zt->base))) {
                            is_elem_struct = true;
                        }
                    }
                }
                if (is_elem_struct && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, addr_temp, elem_size, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return (elem_size > 8) ? addr_temp : right_temp;
                }
                if (elem_size == 1) {
                    ir_emit_store_byte(c, right_temp, addr_temp);
                } else if (elem_size == 2) {
                    ir_emit_store_16(c, right_temp, addr_temp);
                } else if (elem_size == 4) {
                    ir_emit_store_32(c, right_temp, addr_temp);
                } else {
                    ir_emit_store_64(c, right_temp, addr_temp);
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
                    int base_t = zc_alloc_temp(c);
                    ir_emit_lea(c, base_t, BE(c)->fp_reg, -sym->stack_offset);
                    zc_emit_vector_compound_inplace(c, base_t, vsize, vel, node->aux.op,
                                                    right_temp, rhs_vec_l, c->last_expr_is_float);
                    c->last_expr_is_float = 0;
                    return base_t;
                }
                if (vsize > 8 && !is_compound) {
                    int dst_addr = zc_alloc_temp(c);
                    ir_emit_lea(c, dst_addr, TEMP_NONE, -sym->stack_offset);
                    zero_cc_emit_struct_copy_to_addr(c, arena, dst_addr, vsize, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return dst_addr;
                }
                /* <=8B vector: the RHS vector expression lowers to the
                 * ADDRESS of the vector object (decayed ident, per-lane
                 * temp, call buffer) - load width-correct, then store. */
                if (vsize == 8) {
                    int vv = zc_alloc_temp(c);
                    ir_emit_deref(c, vv, right_temp);
                    ir_emit_store(c, vv, BE(c)->fp_reg, -sym->stack_offset);
                } else {
                    int va = zc_alloc_temp(c);
                    ir_emit_lea(c, va, BE(c)->fp_reg, -sym->stack_offset);
                    int vv = zc_alloc_temp(c);
                    if (vsize == 1) { ir_emit_load_byte(c, vv, right_temp); ir_emit_store_byte(c, vv, va); }
                    else if (vsize == 2) { ir_emit_load_16(c, vv, right_temp); ir_emit_store_16(c, vv, va); }
                    else { ir_emit_load_32(c, vv, right_temp); ir_emit_store_32(c, vv, va); }
                }
                c->last_expr_is_float = 0;
                return right_temp;
            }
            if (target_is_float) {
                int float_temp = right_temp;
                if (!right_is_float) {
                    /* Convert int to float via IR */
                    float_temp = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)float_temp, (uint32_t)right_temp);
                }
                /* For compound assignment, load current value, operate, then store */
                if (is_compound) {
                    int lhs_temp = zc_alloc_temp(c);
                    if (sym->is_capture_mirror) {
                        /* By-reference capture: the slot holds the storage
                         * address; load the value through the pointer. */
                        zc_sym_load(c, sym, 0, lhs_temp);
                    } else if (sym->type.tag == TYPE_FLOAT) {
                        int addr = zc_alloc_temp(c);
                        ir_emit_lea(c, addr, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_vloadss(c, (uint32_t)lhs_temp, (uint32_t)addr);
                    } else {
                        ir_emit_fload(c, (uint32_t)lhs_temp, TEMP_NONE, -sym->stack_offset);
                    }
                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)op_res, (uint32_t)lhs_temp, (uint32_t)float_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)op_res, (uint32_t)lhs_temp, (uint32_t)float_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)op_res, (uint32_t)lhs_temp, (uint32_t)float_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_fdiv(c, (uint32_t)op_res, (uint32_t)lhs_temp, (uint32_t)float_temp); break;
                        default: op_res = float_temp; break;
                    }
                    if (sym->is_capture_mirror) {
                        /* By-reference capture: store through the pointer
                         * (zc_sym_store is width-aware). */
                        zc_sym_store(c, sym, 0, op_res);
                    } else if (sym->type.tag == TYPE_FLOAT) {
                        int addr = zc_alloc_temp(c);
                        ir_emit_lea(c, addr, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_vstoress(c, (uint32_t)addr, (uint32_t)op_res);
                    } else {
                        ir_emit_fstore(c, (uint32_t)op_res, TEMP_NONE, -sym->stack_offset);
                    }
                    if (sym->type.is_long_double || sym->type.is_float128) {
                        /* Zero the top 8 bytes of the 16-byte slot: the
                         * value is an 8-byte double and the top half is
                         * otherwise stale stack (c-torture ieee/copysign*
                         * memcmp the full sizeof(long double); the same
                         * byte-level view applies to __float128). */
                        int z8 = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, z8, 0);
                        ir_emit_store(c, z8, BE(c)->fp_reg, -sym->stack_offset + 8);
                    }
                    c->last_expr_is_float = 1;
                    return op_res;
                } else {
                    /* Simple assignment: just store */
                    if (sym->is_capture_mirror) {
                        /* By-reference capture: store through the pointer. */
                        zc_sym_store(c, sym, 0, float_temp);
                        c->last_expr_is_float = 1;
                        return float_temp;
                    }
                    if (sym->type.tag == TYPE_FLOAT) {
                        int addr = zc_alloc_temp(c);
                        ir_emit_lea(c, addr, BE(c)->fp_reg, -sym->stack_offset);
                        ir_emit_vstoress(c, (uint32_t)addr, (uint32_t)float_temp);
                    } else {
                        ir_emit_fstore(c, (uint32_t)float_temp, TEMP_NONE, -sym->stack_offset);
                    }
                    if (sym->type.is_long_double || sym->type.is_float128) {
                        int z8 = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, z8, 0);
                        ir_emit_store(c, z8, BE(c)->fp_reg, -sym->stack_offset + 8);
                    }
                    c->last_expr_is_float = 1;
                    return float_temp;
                }
            } else {
                if (c->last_expr_is_float) {
                    /* C99 6.5.16.1: integer local with a FLOATING-POINT RHS
                     * (b -= post): arithmetic compound ops do the math in
                     * DOUBLE (the usual arithmetic conversions promote the
                     * LHS) and convert the result back to the integer type
                     * on the assignment. The pre-fix path converted the RHS
                     * first (b -= (ll)post) - wrong value (199 instead of
                     * (ll)(200.0 - 1.5) = 198). */
                    bool arith_mixed = is_compound && (
                        node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN ||
                        node->aux.op == TK_MUL_ASSIGN || node->aux.op == TK_DIV_ASSIGN);
                    if (arith_mixed) {
                        int left_temp = zc_alloc_temp(c);
                        if (sym->is_capture_mirror) {
                            zc_sym_load(c, sym, 0, left_temp);
                        } else {
                            ir_emit_load(c, left_temp, BE(c)->fp_reg, -sym->stack_offset);
                        }
                        int lf = zc_alloc_temp(c);
                        ir_emit_cvtsi2sd(c, (uint32_t)lf, (uint32_t)left_temp);
                        int resf = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(c, (uint32_t)resf, (uint32_t)lf, (uint32_t)right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(c, (uint32_t)resf, (uint32_t)lf, (uint32_t)right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(c, (uint32_t)resf, (uint32_t)lf, (uint32_t)right_temp); break;
                            default:            ir_emit_fdiv(c, (uint32_t)resf, (uint32_t)lf, (uint32_t)right_temp); break;
                        }
                        int int_temp = zc_alloc_temp(c);
                        ir_emit_cvttsd2si(c, (uint32_t)int_temp, (uint32_t)resf);
                        if (sym->is_capture_mirror) { zc_sym_store(c, sym, 0, int_temp); }
                        else { ir_emit_store(c, (uint32_t)int_temp, BE(c)->fp_reg, -sym->stack_offset); }
                        c->last_expr_is_float = 0;
                        return int_temp;
                    }
                    /* Convert float to int via IR */
                    int int_temp = zc_alloc_temp(c);
                    ir_emit_cvttsd2si(c, (uint32_t)int_temp, (uint32_t)right_temp);
                    if (is_compound) {
                        /* Load current value and perform operation */
                        int left_temp = zc_alloc_temp(c);
                        ir_emit_load(c, left_temp, BE(c)->fp_reg, -sym->stack_offset);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(c, int_temp, left_temp, int_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(c, int_temp, left_temp, int_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(c, int_temp, left_temp, int_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_div(c, int_temp, left_temp, int_temp); break;
                            case TK_MOD_ASSIGN: ir_emit_mod(c, int_temp, left_temp, int_temp); break;
                            case TK_AND_ASSIGN: ir_emit_and(c, int_temp, left_temp, int_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(c, int_temp, left_temp, int_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(c, int_temp, left_temp, int_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(c, int_temp, left_temp, int_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(c, int_temp, left_temp, int_temp); break;
                        }
                    }
                    if (sym->is_capture_mirror) { zc_sym_store(c, sym, 0, int_temp); }
                    else { ir_emit_store(c, (uint32_t)int_temp, BE(c)->fp_reg, -sym->stack_offset); }
                    c->last_expr_is_float = 0;
                    return int_temp;
                } else {
                    if (sym && (sym->is_atomic || sym->type.is_atomic)) {
                        int addr_temp = zc_alloc_temp(c);
                        ir_emit_lea(c, addr_temp, TEMP_NONE, -sym->stack_offset);
                        int width = (sym->type.size > 0) ? (int)sym->type.size : 4;
                        if (is_compound && (node->aux.op == TK_ADD_ASSIGN || node->aux.op == TK_SUB_ASSIGN)) {
                            int delta_temp = right_temp;
                            if (node->aux.op == TK_SUB_ASSIGN) {
                                delta_temp = zc_alloc_temp(c);
                                ir_emit_neg(c, delta_temp, right_temp);
                            }
                            int delta_copy = zc_alloc_temp(c);
                            ir_emit_mov_reg(c, delta_copy, delta_temp);
                            int old_temp = zc_alloc_temp(c);
                            ir_emit_atomic_xadd(c, old_temp, addr_temp, delta_temp, width);
                            int new_temp = zc_alloc_temp(c);
                            ir_emit_add(c, new_temp, old_temp, delta_copy);
                            c->last_expr_is_float = 0;
                            return new_temp;
                        } else if (!is_compound) {
                            int old_temp = zc_alloc_temp(c);
                            ir_emit_atomic_xchg(c, old_temp, addr_temp, right_temp, width);
                            c->last_expr_is_float = 0;
                            return right_temp;
                        }
                    }

                    if (is_compound) {
                        /* Load current value and perform operation.
                         * The result goes into a fresh temp: right_temp may
                         * hold a loop-invariant constant that must survive
                         * repeated uses, so it must not be used as dest.
                         * By-reference capture: route through the pointer
                         * (the slot holds the storage address, not the
                         * value - raw frame load would do pointer math). */
                        int left_temp = zc_alloc_temp(c);
                        if (sym->is_capture_mirror) {
                            zc_sym_load(c, sym, 0, left_temp);
                        } else {
                            ir_emit_load(c, left_temp, BE(c)->fp_reg, -sym->stack_offset);
                        }
                        int op_res = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_add(c, op_res, left_temp, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, left_temp, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, left_temp, right_temp); break;
                            case TK_DIV_ASSIGN:
                                ir_emit_div(c, op_res, left_temp, right_temp);
                                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) ir_mark_last_unsigned(c, 1);
                                break;
                            case TK_MOD_ASSIGN:
                                ir_emit_mod(c, op_res, left_temp, right_temp);
                                if (zero_cc_binop_is_unsigned(c, arena, node->left, node->right)) ir_mark_last_unsigned(c, 1);
                                break;
                            case TK_AND_ASSIGN: ir_emit_and(c, op_res, left_temp, right_temp); break;
                            case TK_OR_ASSIGN:  ir_emit_or(c, op_res, left_temp, right_temp); break;
                            case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, left_temp, right_temp); break;
                            case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, left_temp, right_temp); break;
                            case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, left_temp, right_temp); break;
                        }
                        right_temp = op_res;
                    }
                    if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 16 && !is_compound) {
                        int dst_addr = zc_sym_addr(c, sym, 0);  /* Sprint 72 */
                        zero_cc_emit_struct_copy_to_addr(c, arena, dst_addr, sym->type.size, node->right, right_temp);
                        c->last_expr_is_float = 0;
                        /* The assignment expression's value is its
                         * destination (chained `a = b = y` copies from
                         * there); right_temp may be undefined for a
                         * generic RHS (c-torture compile/pr71109). */
                        return dst_addr;
                    } else {
                        bool rhs_is_struct_addr = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                                  !is_compound && ast_expr_is_struct_addr(c, arena, node->right);
                        if (rhs_is_struct_addr) {
                            if (sym->type.size <= 8) {
                                int val_temp = zc_alloc_temp(c);
                                if (sym->type.size == 1) ir_emit_load_byte(c, val_temp, right_temp);
                                else if (sym->type.size == 2) ir_emit_load_16(c, val_temp, right_temp);
                                else if (sym->type.size == 4) ir_emit_load_32(c, val_temp, right_temp);
                                else ir_emit_deref(c, val_temp, right_temp);
                                zc_sym_store(c, sym, 0, val_temp);  /* Sprint 72 */
                            } else {
                                int val1 = zc_alloc_temp(c);
                                ir_emit_deref(c, val1, right_temp);
                                zc_sym_store(c, sym, 0, val1);  /* Sprint 72 */

                                int off8 = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, off8, 8);
                                int addr2 = zc_alloc_temp(c);
                                ir_emit_add(c, addr2, right_temp, off8);

                                int val2 = zc_alloc_temp(c);
                                size_t rem = sym->type.size - 8;
                                if (rem == 8) {
                                    ir_emit_deref(c, val2, addr2);
                                    zc_sym_store(c, sym, 8, val2);  /* Sprint 72 */
                                } else {
                                    if (rem == 1) ir_emit_load_byte(c, val2, addr2);
                                    else if (rem == 2) ir_emit_load_16(c, val2, addr2);
                                    else if (rem == 4) ir_emit_load_32(c, val2, addr2);
                                    else ir_emit_deref(c, val2, addr2);
                                    /* Store exactly rem bytes - a full
                                     * 8-byte store scribbles the bytes
                                     * adjacent to the destination slot
                                     * (c-torture execute/va-arg-22). */
                                    int dst_a;
                                    if (zc_sym_uses_data_section(sym)) {
                                        dst_a = zc_sym_addr(c, sym, 8);  /* Sprint 72 */
                                    } else {
                                        dst_a = zc_alloc_temp(c);
                                        ir_emit_lea(c, dst_a, TEMP_NONE, -(sym->stack_offset - 8));
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
                                int reg2 = (c->isa == ISA_AARCH64 || c->isa == ISA_RISCV64) ? VREG_1 : VREG_2;
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
                                        dst_addr = zc_alloc_temp(c);
                                        ir_emit_lea(c, dst_addr, TEMP_NONE, tail_off);
                                    }
                                    zc_store_low_bytes(c, reg2, dst_addr, rem);
                                }
                            }
                        }
                    }
                    c->last_expr_is_float = 0;
                    /* The assignment expression's value is its destination:
                     * a struct/union local of size > 8 reports the local's
                     * address (consistent with the NODE_ASSIGN classification
                     * in ast_expr_is_struct_addr, which chained copies depend
                     * on - returning right_temp there was a VALUE (or a
                     * source address) that consumers dereferenced as the
                     * destination (c-torture compile/pr71109, 9-16B case)).
                     * VLA struct locals hold a pointer in the frame slot, so
                     * keep the old value-return for them. */
                    if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                        sym->type.size > 8 && !sym->is_vla_struct) {
                        return zc_sym_addr(c, sym, 0);
                    }
                    return right_temp;
                }
            }
        } else {
            ZeroCCGlobalVar *gv = find_global_var(c, var);
            if (gv) {
                int gaddr_temp = zc_alloc_temp(c);
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
                        int curr_val = zc_alloc_temp(c);
                        if (gv_is_float) ir_emit_vloadss(c, (uint32_t)curr_val, (uint32_t)gaddr_temp);
                        else ir_emit_vloadsd(c, (uint32_t)curr_val, (uint32_t)gaddr_temp);
                        int op_res = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(c, op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(c, op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(c, op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv(c, op_res, curr_val, right_temp); break;
                            default: ir_emit_fmov(c, op_res, right_temp); break; /* bit-ops are invalid on floats */
                        }
                        final_val = op_res;
                    }
                    if (gv_is_float) ir_emit_vstoress(c, (uint32_t)gaddr_temp, (uint32_t)final_val);
                    else ir_emit_vstoresd(c, (uint32_t)gaddr_temp, (uint32_t)final_val);
                    c->last_expr_is_float = 1;
                    return final_val;
                }
                if (gv_is_flt) {
                    /* int RHS -> float global: convert, then store */
                    int flt_temp = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)flt_temp, (uint32_t)right_temp);
                    if (gv_is_float) ir_emit_vstoress(c, gaddr_temp, (uint32_t)flt_temp);
                    else ir_emit_vstoresd(c, gaddr_temp, (uint32_t)flt_temp);
                    c->last_expr_is_float = 1;
                    return flt_temp;
                }
                if (c->last_expr_is_float) {
                    /* float/double RHS -> integer global: truncate to the
                     * global's integer type (C99 6.3.1.3) BEFORE the
                     * integer store - the raw IEEE-754 bit pattern is not
                     * the value. Mirrors the local-variable path. */
                    int int_temp = zc_alloc_temp(c);
                    ir_emit_cvttsd2si(c, (uint32_t)int_temp, (uint32_t)right_temp);
                    if (gv_sz == 1 || gv_sz == 2 || gv_sz == 4) {
                        bool gu = (gv->type.tag == TYPE_UINT8 || gv->type.tag == TYPE_UINT16 ||
                                   gv->type.tag == TYPE_UINT32);
                        if (gu) ir_emit_zext(c, int_temp, (int)gv_sz);
                        else ir_emit_sext(c, int_temp, (int)gv_sz);
                    }
                    right_temp = int_temp;
                    final_val = int_temp;
                    c->last_expr_is_float = 0;
                }
                if (is_compound) {
                    int curr_val = zc_alloc_temp(c);
                    if (gv_sz == 1) ir_emit_load_byte(c, curr_val, gaddr_temp);
                    else if (gv_sz == 2) ir_emit_load_16(c, curr_val, gaddr_temp);
                    else if (gv_sz == 4) ir_emit_load_32(c, curr_val, gaddr_temp);
                    else ir_emit_deref(c, curr_val, gaddr_temp);

                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(c, op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(c, op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(c, op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(c, op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(c, op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, curr_val, right_temp); break;
                        default: op_res = right_temp; break;
                    }
                    final_val = op_res;
                }
                if (((gv->type.tag == TYPE_STRUCT || gv->type.tag == TYPE_UNION) ||
                     (gv->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&gv->type))) &&
                    gv_sz > 8 && !is_compound) {
                    zero_cc_emit_struct_copy_to_addr(c, arena, gaddr_temp, gv_sz, node->right, right_temp);
                    c->last_expr_is_float = 0;
                    return gaddr_temp;
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
                    int val_temp = zc_alloc_temp(c);
                    if (gv_sz == 1) ir_emit_load_byte(c, val_temp, final_val);
                    else if (gv_sz == 2) ir_emit_load_16(c, val_temp, final_val);
                    else if (gv_sz == 4) ir_emit_load_32(c, val_temp, final_val);
                    else ir_emit_deref(c, val_temp, final_val);
                    final_val = val_temp;
                }
                lower_assign_conv_zt(c, final_val, &gv->type);
                if (gv_sz == 1) ir_emit_store_byte(c, final_val, gaddr_temp);
                else if (gv_sz == 2) ir_emit_store_16(c, final_val, gaddr_temp);
                else if (gv_sz == 4) ir_emit_store_32(c, final_val, gaddr_temp);
                else ir_emit_store_64(c, final_val, gaddr_temp);

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
                int xaddr = zc_alloc_temp(c);
                ir_emit_mov_global(c, xaddr, xfn->name);
                int xfinal = right_temp;
                /* The extern registration stores the declared scalar
                 * type in return_type (same source the NODE_IDENT load
                 * path uses). */
                const ZeroCCType *xt = &xfn->return_type;
                size_t xsz = (xt->size > 0) ? xt->size : 8;
                bool xflt = (xt->tag == TYPE_FLOAT || xt->tag == TYPE_DOUBLE);
                if (xflt && c->last_expr_is_float) {
                    if (is_compound) {
                        int curr_val = zc_alloc_temp(c);
                        if (xsz == 4) ir_emit_vloadss(c, (uint32_t)curr_val, (uint32_t)xaddr);
                        else ir_emit_vloadsd(c, (uint32_t)curr_val, (uint32_t)xaddr);
                        int op_res = zc_alloc_temp(c);
                        switch (node->aux.op) {
                            case TK_ADD_ASSIGN: ir_emit_fadd(c, op_res, curr_val, right_temp); break;
                            case TK_SUB_ASSIGN: ir_emit_fsub(c, op_res, curr_val, right_temp); break;
                            case TK_MUL_ASSIGN: ir_emit_fmul(c, op_res, curr_val, right_temp); break;
                            case TK_DIV_ASSIGN: ir_emit_fdiv(c, op_res, curr_val, right_temp); break;
                            default: ir_emit_fmov(c, op_res, right_temp); break;
                        }
                        xfinal = op_res;
                    }
                    if (xsz == 4) ir_emit_vstoress(c, (uint32_t)xaddr, (uint32_t)xfinal);
                    else ir_emit_vstoresd(c, (uint32_t)xaddr, (uint32_t)xfinal);
                    c->last_expr_is_float = 1;
                    return xfinal;
                }
                if (xflt) {
                    int flt_temp = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)flt_temp, (uint32_t)right_temp);
                    if (xsz == 4) ir_emit_vstoress(c, xaddr, (uint32_t)flt_temp);
                    else ir_emit_vstoresd(c, xaddr, (uint32_t)flt_temp);
                    c->last_expr_is_float = 1;
                    return flt_temp;
                }
                if (is_compound) {
                    int curr_val = zc_alloc_temp(c);
                    if (xsz == 1) ir_emit_load_byte(c, curr_val, xaddr);
                    else if (xsz == 2) ir_emit_load_16(c, curr_val, xaddr);
                    else if (xsz == 4) ir_emit_load_32(c, curr_val, xaddr);
                    else ir_emit_deref(c, curr_val, xaddr);
                    int op_res = zc_alloc_temp(c);
                    switch (node->aux.op) {
                        case TK_ADD_ASSIGN: ir_emit_add(c, op_res, curr_val, right_temp); break;
                        case TK_SUB_ASSIGN: ir_emit_sub(c, op_res, curr_val, right_temp); break;
                        case TK_MUL_ASSIGN: ir_emit_mul(c, op_res, curr_val, right_temp); break;
                        case TK_DIV_ASSIGN: ir_emit_div(c, op_res, curr_val, right_temp); break;
                        case TK_MOD_ASSIGN: ir_emit_mod(c, op_res, curr_val, right_temp); break;
                        case TK_AND_ASSIGN: ir_emit_and(c, op_res, curr_val, right_temp); break;
                        case TK_OR_ASSIGN:  ir_emit_or(c, op_res, curr_val, right_temp); break;
                        case TK_XOR_ASSIGN: ir_emit_xor(c, op_res, curr_val, right_temp); break;
                        case TK_SHL_ASSIGN: ir_emit_shl(c, op_res, curr_val, right_temp); break;
                        case TK_SHR_ASSIGN: ir_emit_shr(c, op_res, curr_val, right_temp); break;
                        default: op_res = right_temp; break;
                    }
                    xfinal = op_res;
                }
                if (xsz == 1) ir_emit_store_byte(c, xfinal, xaddr);
                else if (xsz == 2) ir_emit_store_16(c, xfinal, xaddr);
                else if (xsz == 4) ir_emit_store_32(c, xfinal, xaddr);
                else ir_emit_store_64(c, xfinal, xaddr);
                c->last_expr_is_float = 0;
                return xfinal;
            }
        }
        c->last_expr_is_float = 0;
        return right_temp;
}
