/* AST Lowering - wide (__int128) bitfield read-modify-write (Sprint 113/114:
 * extracted from ast_lowering_assign.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

/* Sprint 113: 128-bit / and % for compound assignment into a wide
 * (16-byte unit) bit-field - the zero_rt software routines (same pattern
 * as zc_lower_binop128's divmod path). a and b are GPR128 temps; the
 * result is a new GPR128 temp. */
int assign_emit_128_divmod(ZeroCCCompiler *c, int a, int b,
                            bool is_div, bool is_unsigned) {
    int alo = c->next_temp_id++; ir_emit_lo128(c, alo, a);
    int ahi = c->next_temp_id++; ir_emit_hi128(c, ahi, a);
    int blo = c->next_temp_id++; ir_emit_lo128(c, blo, b);
    int bhi = c->next_temp_id++; ir_emit_hi128(c, bhi, b);
    const char *fn = is_div ? (is_unsigned ? "zc_udiv128" : "zc_sdiv128")
                            : (is_unsigned ? "zc_umod128" : "zc_smod128");
    if (c->function_count < ZERO_CC_MAX_FUNCTIONS &&
        !zero_cc_find_function(c, fn)) {
        ZeroCCFunction *zfn = &c->functions[c->function_count++];
        memset(zfn, 0, sizeof(*zfn));
        snprintf(zfn->name, sizeof(zfn->name), "%s", fn);
        zfn->text_offset = 0;
        zfn->returns_struct = false;
        zfn->ret_struct_sz = 0;
        zfn->is_prototype = true;
        zfn->has_prototype = false;
        zfn->is_external = true;
        zfn->is_variadic = false;
    }
    ir_emit_call(c, fn);
    int at[4] = {alo, ahi, blo, bhi};
    ir_call_args_record(c, ir_get_count(c) - 1, 4, at, NULL, NULL);
    int rlo = c->next_temp_id++; ir_emit_mov_reg(c, rlo, VREG_0);
    int rhi = c->next_temp_id++; ir_emit_mov_reg(c, rhi, VREG_2);
    int res = c->next_temp_id++; ir_emit_mk128(c, res, rlo, rhi);
    return res;
}

/* Sprint 113: variable-count 128-bit shift for compound assignment into a
 * wide bit-field (`s.t <<= n` / `s.t >>= n`): the zero_rt zc_shl128_v /
 * zc_shr128_v / zc_sar128_v (>>= on a SIGNED field is arithmetic). a is a
 * GPR128 temp, cnt a 64-bit count temp; the result is a new GPR128 temp. */
int assign_emit_128_var_shift(ZeroCCCompiler *c, int a, int cnt,
                             bool is_shl, bool is_unsigned) {
    int alo = c->next_temp_id++; ir_emit_lo128(c, alo, a);
    int ahi = c->next_temp_id++; ir_emit_hi128(c, ahi, a);
    const char *fn = is_shl ? "zc_shl128_v" : (is_unsigned ? "zc_shr128_v" : "zc_sar128_v");
    if (c->function_count < ZERO_CC_MAX_FUNCTIONS &&
        !zero_cc_find_function(c, fn)) {
        ZeroCCFunction *zfn = &c->functions[c->function_count++];
        memset(zfn, 0, sizeof(*zfn));
        snprintf(zfn->name, sizeof(zfn->name), "%s", fn);
        zfn->text_offset = 0;
        zfn->returns_struct = false;
        zfn->ret_struct_sz = 0;
        zfn->is_prototype = true;
        zfn->has_prototype = false;
        zfn->is_external = true;
        zfn->is_variadic = false;
    }
    ir_emit_call(c, fn);
    int at[3] = {alo, ahi, cnt};
    ir_call_args_record(c, ir_get_count(c) - 1, 3, at, NULL, NULL);
    int rlo = c->next_temp_id++; ir_emit_mov_reg(c, rlo, VREG_0);
    int rhi = c->next_temp_id++; ir_emit_mov_reg(c, rhi, VREG_2);
    int res = c->next_temp_id++; ir_emit_mk128(c, res, rlo, rhi);
    return res;
}

int assign_member_bitfield(ZeroCCCompiler *c,
                           ZeroCCASTNode *assign_node, ZeroCCASTNode *left_node,
                           int addr_temp, int right_temp, bool right_is_128) {
    uint8_t bit_offset = (uint8_t)(left_node->aux.step & 0xFF);
    uint8_t bit_width = (uint8_t)((left_node->aux.step >> 8) & 0xFF);
    bool is_signed = ((left_node->aux.step >> 16) & 1) != 0;
    bool sso_be = ((left_node->aux.step >> 17) & 1) != 0;
    int sso_unit = (int)((left_node->aux.step >> 18) & 0xF);
    if (sso_unit) sso_unit = 1 << sso_unit; /* log2 code -> bytes (2/4/8/16) */

    if ((int)bit_offset + (int)bit_width > 64) {
        /* Sprint 113: 16-byte allocation unit - the field value does not
         * fit in one 64-bit word; the whole RMW runs on a GPR128 (lo,hi).
         * Covers both >64-bit fields and narrow fields in the upper word
         * (bit_offset >= 64). */
        bool is_compound = (assign_node->aux.op != TK_ASSIGN && assign_node->aux.op != '=');
        int unit = c->next_temp_id++;
        ir_emit_load128(c, unit, addr_temp);
        if (sso_be) unit = zc_emit_unit_bswap128(c, unit);

        /* Extract the current field value (128-bit) for the compound op. */
        int cur = unit;
        if (bit_offset > 0) {
            int s_lo = c->next_temp_id++; ir_emit_lo128(c, s_lo, cur);
            int s_hi = c->next_temp_id++; ir_emit_hi128(c, s_hi, cur);
            int dlo = c->next_temp_id++, dhi = c->next_temp_id++;
            zc_emit_128_shift(c, dlo, dhi, s_lo, s_hi, bit_offset, TK_SHR, false);
            cur = c->next_temp_id++; ir_emit_mk128(c, cur, dlo, dhi);
        }
        /* bw < 64 keeps the value entirely in the low word (hi mask 0);
         * the shift count must never go negative (UB). */
        uint64_t vm_hi = (bit_width >= 128) ? ~0ULL :
                         (bit_width > 64 ? ((1ULL << (bit_width - 64)) - 1) : 0ULL);
        int vm_lo_t = c->next_temp_id++; ir_emit_mov_imm(c, vm_lo_t, (int64_t)~0ULL);
        int vm_hi_t = c->next_temp_id++; ir_emit_mov_imm(c, vm_hi_t, (int64_t)vm_hi);
        int vm = c->next_temp_id++; ir_emit_mk128(c, vm, vm_lo_t, vm_hi_t);
        int curm = c->next_temp_id++; ir_emit_and128(c, curm, cur, vm);
        cur = curm;
        if (is_signed && bit_width < 128) {
            int64_t sb = (int64_t)(1ULL << (bit_width - 65));
            int zlo = c->next_temp_id++; ir_emit_mov_imm(c, zlo, 0);
            int shi = c->next_temp_id++; ir_emit_mov_imm(c, shi, sb);
            int sbt = c->next_temp_id++; ir_emit_mk128(c, sbt, zlo, shi);
            int xt = c->next_temp_id++; ir_emit_xor128(c, xt, cur, sbt);
            cur = c->next_temp_id++; ir_emit_sub128(c, cur, xt, sbt);
        }

        /* Widen the RHS to 128 bits when it is not already. */
        if (!right_is_128) {
            int r128 = c->next_temp_id++;
            if (is_signed) ir_emit_sext64_128(c, r128, right_temp);
            else ir_emit_zext64_128(c, r128, right_temp);
            right_temp = r128;
        }

        int newval = right_temp;
        if (is_compound) {
            newval = c->next_temp_id++;
            switch (assign_node->aux.op) {
                case TK_ADD_ASSIGN: ir_emit_add128(c, newval, cur, right_temp); break;
                case TK_SUB_ASSIGN: ir_emit_sub128(c, newval, cur, right_temp); break;
                case TK_MUL_ASSIGN: ir_emit_mul128(c, newval, cur, right_temp); break;
                case TK_AND_ASSIGN: ir_emit_and128(c, newval, cur, right_temp); break;
                case TK_OR_ASSIGN:  ir_emit_or128(c, newval, cur, right_temp); break;
                case TK_XOR_ASSIGN: ir_emit_xor128(c, newval, cur, right_temp); break;
                case TK_DIV_ASSIGN:
                case TK_MOD_ASSIGN:
                    newval = assign_emit_128_divmod(c, cur, right_temp,
                                                     assign_node->aux.op == TK_DIV_ASSIGN,
                                                     !is_signed);
                    break;
                case TK_SHL_ASSIGN:
                case TK_SHR_ASSIGN:
                    newval = assign_emit_128_var_shift(c, cur, right_temp,
                                                        assign_node->aux.op == TK_SHL_ASSIGN,
                                                        !is_signed);
                    break;
                default: ir_emit_mov128(c, newval, right_temp); break;
            }
        }

        /* Value of the assignment expression: the field value (C11 6.5.16.1)
         * - masked to the field width, sign-extended for signed fields. */
        int field_val = c->next_temp_id++;
        ir_emit_and128(c, field_val, newval, vm);
        if (is_signed && bit_width < 128) {
            int64_t sb = (int64_t)(1ULL << (bit_width - 65));
            int zlo = c->next_temp_id++; ir_emit_mov_imm(c, zlo, 0);
            int shi = c->next_temp_id++; ir_emit_mov_imm(c, shi, sb);
            int sbt = c->next_temp_id++; ir_emit_mk128(c, sbt, zlo, shi);
            int xt = c->next_temp_id++; ir_emit_xor128(c, xt, field_val, sbt);
            field_val = c->next_temp_id++; ir_emit_sub128(c, field_val, xt, sbt);
        }

        /* Store back: clear the field bits, OR in the value shifted into
         * position. fmask = vmask << bit_offset (compile-time 128-bit const). */
        uint64_t f_lo, f_hi;
        if (bit_offset == 0) { f_lo = ~0ULL; f_hi = vm_hi; }
        else if (bit_offset < 64) {
            f_lo = ~0ULL << bit_offset;
            f_hi = (vm_hi << bit_offset) | (~0ULL >> (64 - bit_offset));
        } else { f_lo = 0; f_hi = ~0ULL << (bit_offset - 64); }
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
        int bits = field_val;
        if (bit_offset > 0) {
            int s_lo = c->next_temp_id++; ir_emit_lo128(c, s_lo, bits);
            int s_hi = c->next_temp_id++; ir_emit_hi128(c, s_hi, bits);
            int dlo = c->next_temp_id++, dhi = c->next_temp_id++;
            zc_emit_128_shift(c, dlo, dhi, s_lo, s_hi, bit_offset, TK_SHL, false);
            bits = c->next_temp_id++; ir_emit_mk128(c, bits, dlo, dhi);
        } else {
            /* field_val is sign-extended; the stored bits are its masked
             * two's-complement form. */
            bits = c->next_temp_id++; ir_emit_and128(c, bits, field_val, vm);
        }
        int combined = c->next_temp_id++;
        ir_emit_or128(c, combined, cleared, bits);
        if (sso_be) combined = zc_emit_unit_bswap128(c, combined);
        ir_emit_store128(c, combined, addr_temp);
        c->last_expr_is_float = 0;
        c->last_expr_is_128 = 1;
        return field_val;
    }

    uint64_t val_mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
    TypeInfo *ti = ast_node_get_typeinfo(c, left_node);
    size_t m_size = ti ? type_size(ti) : 4;
    bool is_64 = (m_size > 4 || (bit_offset + bit_width > 32));

    /* 1. Load current storage unit */
    int curr_temp = c->next_temp_id++;
    if (is_64) ir_emit_deref(c, curr_temp, addr_temp);
    else ir_emit_load_32(c, curr_temp, addr_temp);

    /* scalar_storage_order("big-endian"): convert the stored unit bytes
     * into the value domain before any bit manipulation. */
    if (sso_be) {
        curr_temp = zc_emit_unit_bswap(c, curr_temp, sso_unit);
    }

    bool is_compound = (assign_node->aux.op != TK_ASSIGN && assign_node->aux.op != '=');
    if (is_compound) {
        int curr_val = curr_temp;
        if (bit_offset > 0) {
            int shift_amt = c->next_temp_id++;
            ir_emit_mov_imm(c, shift_amt, (int32_t)bit_offset);
            int shifted = c->next_temp_id++;
            ir_emit_shr(c, shifted, curr_val, shift_amt);
            curr_val = shifted;
        }
        int mask_t = c->next_temp_id++;
        ir_emit_mov_imm(c, mask_t, (int32_t)val_mask);
        int masked = c->next_temp_id++;
        ir_emit_and(c, masked, curr_val, mask_t);
        curr_val = masked;
        if (is_signed && bit_width < 32) {
            int32_t sign_bit = (int32_t)(1ULL << (bit_width - 1));
            int s_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, s_temp, sign_bit);
            int x_temp = c->next_temp_id++;
            ir_emit_xor(c, x_temp, curr_val, s_temp);
            int e_temp = c->next_temp_id++;
            ir_emit_sub(c, e_temp, x_temp, s_temp);
            curr_val = e_temp;
        }
        int op_res = c->next_temp_id++;
        switch (assign_node->aux.op) {
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

    /* 2. Clear target bits */
    uint64_t field_mask = (bit_width >= 64) ? ~0ULL : (val_mask << bit_offset);
    uint64_t clear_mask = ~field_mask;
    int clear_mask_temp = c->next_temp_id++;
    ir_emit_mov_imm(c, clear_mask_temp, (int64_t)clear_mask);
    int cleared_temp = c->next_temp_id++;
    ir_emit_and(c, cleared_temp, curr_temp, clear_mask_temp);

    /* 3. Mask incoming value to bit_width */
    int val_mask_temp = c->next_temp_id++;
    ir_emit_mov_imm(c, val_mask_temp, (int64_t)val_mask);
    int masked_val = c->next_temp_id++;
    ir_emit_and(c, masked_val, right_temp, val_mask_temp);

    /* Value of the assignment expression: the RHS converted to the
     * bit-field type (C99 6.5.16.1) - masked to the field width and
     * sign-extended for signed fields (c-torture execute/921016-1:
     * `(l.m = 1081)` on a signed 11-bit field is -967, not 1081). */
    int field_val = masked_val;
    if (is_signed && bit_width > 0 && bit_width < 32) {
        int32_t sb = (int32_t)(1ULL << (bit_width - 1));
        int sft = c->next_temp_id++;
        ir_emit_mov_imm(c, sft, sb);
        int xft = c->next_temp_id++;
        ir_emit_xor(c, xft, field_val, sft);
        int eft = c->next_temp_id++;
        ir_emit_sub(c, eft, xft, sft);
        field_val = eft;
    }

    /* 4. Shift into position */
    if (bit_offset > 0) {
        int shl_amt = c->next_temp_id++;
        ir_emit_mov_imm(c, shl_amt, (int32_t)bit_offset);
        int shifted_val = c->next_temp_id++;
        ir_emit_shl(c, shifted_val, masked_val, shl_amt);
        masked_val = shifted_val;
    }

    /* 5. Combine and store (back to the storage domain for SSO-BE
     * units before the write). */
    int combined_temp = c->next_temp_id++;
    ir_emit_or(c, combined_temp, cleared_temp, masked_val);
    if (sso_be) {
        combined_temp = zc_emit_unit_bswap(c, combined_temp, sso_unit);
    }
    if (is_64) ir_emit_store_64(c, combined_temp, addr_temp);
    else ir_emit_store_32(c, combined_temp, addr_temp);

    c->last_expr_is_float = 0;
    return field_val;
}
