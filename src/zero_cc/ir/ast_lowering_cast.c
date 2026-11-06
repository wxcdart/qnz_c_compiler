/* AST Lowering - Cast Lowering (AST to IR) (Sprint 114: extracted from ast_lowering_binop.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

int lower_expr_cast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        TypeInfo *cast_ti = ast_node_get_typeinfo(c, node);
        bool target_is_float = type_is_floating(cast_ti);
        ZeroCCASTNode *cast_src = zero_cc_ast_get(arena, node->left);
        if (cast_src && cast_src->tag == NODE_FLOAT && !target_is_float &&
            !(cast_ti && cast_ti->kind == TYPE_KIND_COMPLEX)) {
            double f = cast_src->float_val;
            int64_t ival;
            size_t sz = cast_ti ? type_size(cast_ti) : 4;
            bool is_u = cast_ti && (cast_ti->modifiers & TYPE_MOD_UNSIGNED);
            if (sz == 4 && !is_u) {
                if (f >= 2147483647.0) ival = 2147483647;
                else if (f <= -2147483648.0) ival = -2147483648LL;
                else ival = (int32_t)f;
            } else if (sz == 4 && is_u) {
                if (f >= 4294967295.0) ival = 4294967295ULL;
                else if (f <= 0.0) ival = 0;
                else ival = (uint32_t)f;
            } else if (sz == 8 && !is_u) {
                if (f >= 9223372036854775807.0) ival = 9223372036854775807LL;
                else if (f <= -9223372036854775808.0) ival = (-9223372036854775807LL - 1);
                else ival = (int64_t)f;
            } else if (sz == 8 && is_u) {
                if (f >= 18446744073709551615.0) ival = (int64_t)18446744073709551615ULL;
                else if (f <= 0.0) ival = 0;
                else ival = (int64_t)(uint64_t)f;
            } else if (sz == 1 && !is_u) {
                if (f >= 127.0) ival = 127;
                else if (f <= -128.0) ival = -128;
                else ival = (int8_t)f;
            } else if (sz == 1 && is_u) {
                if (f >= 255.0) ival = 255;
                else if (f <= 0.0) ival = 0;
                else ival = (uint8_t)f;
            } else if (sz == 2 && !is_u) {
                if (f >= 32767.0) ival = 32767;
                else if (f <= -32768.0) ival = -32768;
                else ival = (int16_t)f;
            } else if (sz == 2 && is_u) {
                if (f >= 65535.0) ival = 65535;
                else if (f <= 0.0) ival = 0;
                else ival = (uint16_t)f;
            } else {
                ival = (int64_t)f;
            }
            int int_res_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, int_res_temp, ival);
            c->last_expr_is_float = 0;
            return int_res_temp;
        }
        int result = lower_expr(c, arena, node->left);
        /* Scalar -> vector cast (GCC requires matching sizes): the vector's bit
         * pattern is the low sizeof(vector) bytes of the scalar value.
         * The result must be a fresh vector object in ADDRESS form
         * (vector expressions are always address form) - e.g.
         * `(V2HI)0` / `(V2SI)(long long)x` in c-torture
         * execute/20050316-1/-3. */
        {
            ZeroCCASTNode *csrc2 = zero_cc_ast_get(arena, node->left);
            TypeInfo *cst2 = csrc2 ? ast_node_get_typeinfo(c, csrc2) : NULL;
            /* C99 6.5.4: cast to a complex type.  A scalar operand
             * becomes (value, 0); a complex operand is copied.  The
             * result is a fresh address-form complex object. */
            if (cast_ti && cast_ti->kind == TYPE_KIND_COMPLEX) {
                const TypeInfo *cel = cast_ti->details.array.base;
                size_t cesz = cel ? type_size(cel) : 8;
                bool is_target_fp = cel ? type_is_floating(cel) : true;
                if (is_target_fp && cesz != 4 && cesz != 8) cesz = 8;
                c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                c->current_stack_frame = (c->current_stack_frame + (int32_t)(cesz * 2) + 7) & ~7;
                int32_t coff = c->current_stack_frame;
                int ca = c->next_temp_id++;
                ir_emit_lea(c, ca, BE(c)->fp_reg, -coff);
                int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, (int)cesz);
                int a2 = c->next_temp_id++; ir_emit_add(c, a2, ca, o2);
                if (cst2 && cst2->kind == TYPE_KIND_COMPLEX) {
                    const TypeInfo *src_el = cst2->details.array.base;
                    size_t sesz = src_el ? type_size(src_el) : 8;
                    bool is_src_fp = src_el ? type_is_floating(src_el) : true;
                    if (is_src_fp && sesz != 4 && sesz != 8) sesz = 8;
                    int sr = c->next_temp_id++, si = c->next_temp_id++;
                    int so2 = c->next_temp_id++; ir_emit_mov_imm(c, so2, (int)sesz);
                    int sa2 = c->next_temp_id++; ir_emit_add(c, sa2, result, so2);
                    if (is_src_fp) {
                        if (sesz == 4) { ir_emit_vloadss(c, (uint16_t)sr, (uint16_t)result); ir_emit_vloadss(c, (uint16_t)si, (uint16_t)sa2); }
                        else { ir_emit_vloadsd(c, (uint16_t)sr, (uint16_t)result); ir_emit_vloadsd(c, (uint16_t)si, (uint16_t)sa2); }
                    } else {
                        if (sesz == 1) { ir_emit_load_byte(c, sr, result); ir_emit_load_byte(c, si, sa2); }
                        else if (sesz == 2) { ir_emit_load_16(c, sr, result); ir_emit_load_16(c, si, sa2); }
                        else if (sesz == 4) { ir_emit_load_32(c, sr, result); ir_emit_load_32(c, si, sa2); }
                        else { ir_emit_deref(c, sr, result); ir_emit_deref(c, si, sa2); }
                    }
                    int tr = sr, ti = si;
                    if (is_target_fp && !is_src_fp) {
                        tr = c->next_temp_id++; ir_emit_cvtsi2sd(c, (uint16_t)tr, (uint16_t)sr);
                        ti = c->next_temp_id++; ir_emit_cvtsi2sd(c, (uint16_t)ti, (uint16_t)si);
                    } else if (!is_target_fp && is_src_fp) {
                        tr = c->next_temp_id++; ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)tr, .temp_src1 = (uint16_t)sr, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                        ti = c->next_temp_id++; ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)ti, .temp_src1 = (uint16_t)si, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    }
                    if (is_target_fp) {
                        if (cesz == 4) { ir_emit_vstoress(c, (uint16_t)ca, (uint16_t)tr); ir_emit_vstoress(c, (uint16_t)a2, (uint16_t)ti); }
                        else { ir_emit_vstoresd(c, (uint16_t)ca, (uint16_t)tr); ir_emit_vstoresd(c, (uint16_t)a2, (uint16_t)ti); }
                    } else {
                        if (cesz == 1) { ir_emit_store_byte(c, tr, ca); ir_emit_store_byte(c, ti, a2); }
                        else if (cesz == 2) { ir_emit_store_16(c, tr, ca); ir_emit_store_16(c, ti, a2); }
                        else if (cesz == 4) { ir_emit_store_32(c, tr, ca); ir_emit_store_32(c, ti, a2); }
                        else { ir_emit_store_64(c, tr, ca); ir_emit_store_64(c, ti, a2); }
                    }
                } else {
                    int tr = result;
                    if (is_target_fp && !c->last_expr_is_float) {
                        tr = zc_emit_int_to_double(c, result, cst2);
                    } else if (!is_target_fp && c->last_expr_is_float) {
                        tr = c->next_temp_id++;
                        ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)tr, .temp_src1 = (uint16_t)result, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    }
                    if (is_target_fp) {
                        int zr = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)zr, (int64_t)zc_rodata_double_offset(c, 0.0));
                        if (cesz == 4) { ir_emit_vstoress(c, (uint16_t)ca, (uint16_t)tr); ir_emit_vstoress(c, (uint16_t)a2, (uint16_t)zr); }
                        else { ir_emit_vstoresd(c, (uint16_t)ca, (uint16_t)tr); ir_emit_vstoresd(c, (uint16_t)a2, (uint16_t)zr); }
                    } else {
                        int zi = c->next_temp_id++; ir_emit_mov_imm(c, zi, 0);
                        if (cesz == 1) { ir_emit_store_byte(c, tr, ca); ir_emit_store_byte(c, zi, a2); }
                        else if (cesz == 2) { ir_emit_store_16(c, tr, ca); ir_emit_store_16(c, zi, a2); }
                        else if (cesz == 4) { ir_emit_store_32(c, tr, ca); ir_emit_store_32(c, zi, a2); }
                        else { ir_emit_store_64(c, tr, ca); ir_emit_store_64(c, zi, a2); }
                    }
                }
                int caddr = c->next_temp_id++;
                ir_emit_lea(c, caddr, BE(c)->fp_reg, -coff);
                c->last_expr_is_float = 0;
                return caddr;
            }
            if (cast_ti && type_is_vector(cast_ti) && (!cst2 || !type_is_vector(cst2))) {
                size_t vsz = type_size(cast_ti);
                if (vsz > 0 && vsz <= 128) {
                    c->current_stack_frame = (c->current_stack_frame + (int32_t)vsz + 7) & ~7;
                    int32_t voff = c->current_stack_frame;
                    const TypeInfo *el2 = cast_ti->details.array.base;
                    bool efl = el2 && type_is_floating(el2);
                    if (efl && !c->last_expr_is_float) {
                        int convf = c->next_temp_id++;
                        ir_emit_cvtsi2sd(c, (uint16_t)convf, (uint16_t)result);
                        result = convf;
                        c->last_expr_is_float = 1;
                    }
                    size_t sz = (vsz <= 8) ? vsz : 8;
                    if (efl) {
                        if (sz == 4) {
                            int fa = c->next_temp_id++;
                            ir_emit_lea(c, fa, BE(c)->fp_reg, -voff);
                            ir_emit_vstoress(c, (uint16_t)fa, (uint16_t)result);
                        } else {
                            ir_emit_fstore(c, (uint16_t)result, BE(c)->fp_reg, -voff);
                        }
                    } else {
                        if (sz == 1) ir_emit_store_byte(c, (uint16_t)result, (uint16_t)voff);
                        else if (sz == 2) {
                            int a2 = c->next_temp_id++;
                            ir_emit_lea(c, a2, TEMP_NONE, -voff);
                            ir_emit_store_16(c, (uint16_t)result, (uint16_t)a2);
                        } else if (sz == 4) {
                            int a2 = c->next_temp_id++;
                            ir_emit_lea(c, a2, TEMP_NONE, -voff);
                            ir_emit_store_32(c, (uint16_t)result, (uint16_t)a2);
                        } else {
                            ir_emit_store(c, (uint16_t)result, BE(c)->fp_reg, -voff);
                        }
                    }
                    /* Any bytes beyond the scalar's width (only possible
                     * when the vector is wider than the source type) are
                     * zero. */
                    if (vsz > sz) {
                        int zt = c->next_temp_id++;
                        ir_emit_mov_imm(c, zt, 0);
                        for (size_t off = sz; off < vsz; off += 8) {
                            size_t rem = vsz - off;
                            if (rem >= 8) {
                                ir_emit_store(c, (uint16_t)zt, BE(c)->fp_reg, -(int32_t)(voff - (int32_t)off));
                            } else {
                                int a2 = c->next_temp_id++;
                                ir_emit_lea(c, a2, TEMP_NONE, -(int32_t)(voff - (int32_t)off));
                                if (rem >= 4) ir_emit_store_32(c, (uint16_t)zt, (uint16_t)a2);
                                else if (rem >= 2) ir_emit_store_16(c, (uint16_t)zt, (uint16_t)a2);
                                else ir_emit_store_byte(c, (uint16_t)zt, (uint16_t)a2);
                            }
                        }
                    }
                    int vaddr2 = c->next_temp_id++;
                    ir_emit_lea(c, vaddr2, BE(c)->fp_reg, -voff);
                    c->last_expr_is_float = 0;
                    return vaddr2;
                }
            }
        }
        /* Vector -> same-size scalar cast (GCC requires matching sizes):
         * the vector lowers to the ADDRESS of the vector object; read
         * the whole vector as one scalar value of the target width.
         * Float targets use the FP load (widening in the double model). */
        {
            ZeroCCASTNode *csrc = zero_cc_ast_get(arena, node->left);
            TypeInfo *cst = csrc ? ast_node_get_typeinfo(c, csrc) : NULL;
            if (cst && type_is_vector(cst) && cast_ti && !type_is_vector(cast_ti)) {
                size_t wsz = type_size(cast_ti);
                int wv = c->next_temp_id++;
                if (target_is_float) {
                    if (wsz == 8) { ir_emit_vloadsd(c, (uint16_t)wv, (uint16_t)result); c->last_expr_is_float = 1; }
                    else { ir_emit_vloadss(c, (uint16_t)wv, (uint16_t)result); c->last_expr_is_float = 1; }
                } else {
                    if (wsz == 1) ir_emit_load_byte(c, wv, result);
                    else if (wsz == 2) ir_emit_load_16(c, wv, result);
                    else if (wsz == 4) ir_emit_load_32(c, wv, result);
                    else ir_emit_deref(c, wv, result);
                    c->last_expr_is_float = 0;
                }
                result = wv;
            }
        }
        if (target_is_float) {
            if (!c->last_expr_is_float) {
                /* int/pointer -> float/double: convert (C9). Previously
                 * this case fell through and returned the raw integer
                 * bits as if they were a double bit pattern (int 3 ->
                 * 3.0e-323), with last_expr_is_float left 0 so callers
                 * (e.g. the call-argument path) took the integer route.
                 *
                 * 64-bit *unsigned* sources (unsigned long / size_t /
                 * pointers on LP64) need the two-term form: cvtsi2sd
                 * reads the bits as *signed*, so values with the high
                 * bit set come out as (v - 2^64); add back 2^64 scaled
                 * by the top bit:  r = cvtsi2sd(v) + (double)(v >> 63) * 2^64. */
                ZeroCCASTNode *cast_src = zero_cc_ast_get(arena, node->left);
                TypeInfo *src_ti = cast_src ? ast_node_get_typeinfo(c, cast_src) : NULL;
                bool src_u64 = src_ti && type_size(src_ti) == 8 &&
                    ((src_ti->kind == TYPE_KIND_LONG && (src_ti->modifiers & TYPE_MOD_UNSIGNED) != 0) ||
                     src_ti->kind == TYPE_KIND_POINTER);
                /* 64-bit unsigned int literal (ULL/UL suffix): semantic
                 * analysis types every int literal plain 'int', so fall
                 * back to the stored suffix (bit0=u, bit1=l, bit2=ll). */
                if (!src_u64 && cast_src && cast_src->tag == NODE_INT &&
                    (cast_src->flags & 0x1) && (cast_src->flags & 0x6)) {
                    src_u64 = true;
                }
                if (src_u64) {
                    /* When casting to single-precision float (type_size 4), collapse sticky
                     * bits below bit 31 into bit 31 to prevent double-rounding when converting
                     * to float via intermediate double. */
                    if (cast_ti && type_size(cast_ti) == 4) {
                        int mask_lo = c->next_temp_id++;
                        ir_emit_mov_imm(c, mask_lo, 0x7FFFFFFFLL);
                        int has_sticky = c->next_temp_id++;
                        ir_emit_and(c, has_sticky, result, mask_lo);
                        int zero_sticky = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_sticky, 0);
                        int cond_sticky = c->next_temp_id++;
                        ir_emit_cmp(c, has_sticky, zero_sticky);
                        ir_emit_cond_set(c, cond_sticky, CC_NE);
                        /* Seed the collapsed result with the identity so
                         * the join has a defined value on BOTH paths -
                         * defining it only inside the taken branch left an
                         * undefined temp when the branch was skipped
                         * (c-torture execute/conversion: (float)2^63). */
                        int clean_res = c->next_temp_id++;
                        ir_emit_mov_reg(c, clean_res, result);
                        uint16_t skip_sticky = c->ir_next_label++;
                        ir_emit_je(c, (uint16_t)cond_sticky, skip_sticky);
                        int bit31 = c->next_temp_id++;
                        ir_emit_mov_imm(c, bit31, 0x80000000LL);
                        int or_res = c->next_temp_id++;
                        ir_emit_or(c, or_res, result, bit31);
                        int mask_hi = c->next_temp_id++;
                        ir_emit_mov_imm(c, mask_hi, ~0x7FFFFFFFLL);
                        ir_emit_and(c, clean_res, or_res, mask_hi);
                        result = clean_res;
                        ir_emit_label(c, skip_sticky);
                    }

                    /* Standard uint64 -> double IEEE-754 conversion:
                     * If high bit is not set (result >= 0), cvtsi2sd directly.
                     * If high bit is set (< 0), compute ((result >> 1) | (result & 1)) * 2.0
                     * to preserve the sticky bit and avoid double-rounding. */
                    int zero_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, zero_temp, 0);
                    int is_ge = c->next_temp_id++;
                    ir_emit_cmp(c, result, zero_temp);
                    ir_emit_cond_set(c, is_ge, CC_GE);

                    int fp_res = c->next_temp_id++;
                    uint16_t neg_label = c->ir_next_label++;
                    uint16_t done_label = c->ir_next_label++;

                    ir_emit_je(c, (uint16_t)is_ge, neg_label);

                    /* Positive (high bit clear): */
                    int pos_d = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)pos_d, (uint16_t)result);
                    ir_emit_fmov(c, (uint16_t)fp_res, (uint16_t)pos_d);
                    ir_emit_jmp(c, done_label);

                    /* Negative (high bit set): */
                    ir_emit_label(c, neg_label);
                    int one_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, one_temp, 1);
                    int odd_temp = c->next_temp_id++;
                    ir_emit_and(c, odd_temp, result, one_temp);

                    int shr_temp = c->next_temp_id++;
                    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)shr_temp,
                        .temp_src1 = (uint16_t)result, .temp_src2 = (uint16_t)one_temp});

                    int adj_temp = c->next_temp_id++;
                    ir_emit_or(c, adj_temp, shr_temp, odd_temp);

                    int half_d = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)half_d, (uint16_t)adj_temp);

                    int double_d = c->next_temp_id++;
                    ir_emit_fadd(c, (uint16_t)double_d, (uint16_t)half_d, (uint16_t)half_d);
                    ir_emit_fmov(c, (uint16_t)fp_res, (uint16_t)double_d);

                    ir_emit_label(c, done_label);
                    c->last_expr_is_float = 1;
                    result = fp_res;
                } else {
                    int fp_res = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)fp_res, (uint16_t)result);
                    c->last_expr_is_float = 1;
                    result = fp_res;
                }
            }
            if (cast_ti && type_size(cast_ti) == 4) {
                /* Explicit cast to float: round double to single-precision float */
                int addr = c->next_temp_id++;
                c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                c->current_stack_frame += 8;
                int32_t off = c->current_stack_frame;
                ir_emit_lea(c, addr, BE(c)->fp_reg, -off);
                ir_emit_vstoress(c, addr, result);
                int rounded = c->next_temp_id++;
                ir_emit_vloadss(c, rounded, addr);
                result = rounded;
            }
            return result;
        }
        /* Float to int conversion: emit via IR for correct sequencing */
        if (c->last_expr_is_float) {
            bool is_u64 = cast_ti && type_size(cast_ti) == 8 &&
                          (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
            if (is_u64) {
                double two63 = 9223372036854775808.0;
                size_t ro63 = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, &two63, sizeof(double));
                int two63_f = c->next_temp_id++;
                ir_emit_fmov_imm(c, (uint16_t)two63_f, (int64_t)ro63);

                int cond_ge = c->next_temp_id++;
                ir_emit_cmp(c, result, two63_f);
                ir_emit_cond_set(c, cond_ge, CC_GE);

                int sub_f = c->next_temp_id++;
                ir_emit_fsub(c, (uint16_t)sub_f, (uint16_t)result, (uint16_t)two63_f);

                int hi_i = c->next_temp_id++;
                ir_emit_cvttsd2si(c, (uint16_t)hi_i, (uint16_t)sub_f);

                int bit63 = c->next_temp_id++;
                ir_emit_mov_imm(c, bit63, (int64_t)0x8000000000000000ULL);
                int hi_adj = c->next_temp_id++;
                ir_emit_xor(c, hi_adj, hi_i, bit63);

                int lo_i = c->next_temp_id++;
                ir_emit_cvttsd2si(c, (uint16_t)lo_i, (uint16_t)result);

                uint16_t label_lo = c->ir_next_label++;
                uint16_t label_done = c->ir_next_label++;
                ir_emit_cmp_zero(c, cond_ge);
                ir_emit_je(c, (uint16_t)cond_ge, label_lo);

                int int_res_temp = c->next_temp_id++;
                ir_emit_mov_reg(c, int_res_temp, hi_adj);
                ir_emit_jmp(c, label_done);

                ir_emit_label(c, label_lo);
                ir_emit_mov_reg(c, int_res_temp, lo_i);

                ir_emit_label(c, label_done);
                c->last_expr_is_float = 0;
                return int_res_temp;
            }
            int int_res_temp = c->next_temp_id++;
            /* Out-of-range float -> int: clamp to the target's range
             * (GCC -fno-trapping-math semantics; C99 6.3.1.4 leaves it
             * undefined): (int)2147483648.0f must be INT_MAX,
             * (unsigned)(-1.0) must be 0. Two FP compares and
             * conditional FP replaces keep the value in the FP model. */
            if (cast_ti) {
                size_t w = type_size(cast_ti);
                bool uns = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                if (w == 1 || w == 2 || w == 4 || w == 8) {
                    double upper_b, lower_b, upper_c, lower_c;
                    if (uns) {
                        upper_b = (w == 1) ? 256.0 : (w == 2) ? 65536.0 : (w == 4) ? 4294967296.0 : 18446744073709551616.0;
                        upper_c = (w == 1) ? 255.0 : (w == 2) ? 65535.0 : (w == 4) ? 4294967295.0 : 18446744073709551615.0;
                        lower_b = 0.0;
                        lower_c = 0.0;
                    } else {
                        upper_b = (w == 1) ? 128.0 : (w == 2) ? 32768.0 : (w == 4) ? 2147483648.0 : 9223372036854775808.0;
                        upper_c = (w == 1) ? 127.0 : (w == 2) ? 32767.0 : (w == 4) ? 2147483647.0 : 9223372036854775807.0;
                        lower_b = (w == 1) ? -128.0 : (w == 2) ? -32768.0 : (w == 4) ? -2147483648.0 : -9223372036854775808.0;
                        lower_c = (w == 1) ? -128.0 : (w == 2) ? -32768.0 : (w == 4) ? -2147483648.0 : -9223372036854775808.0;
                    }
                    int n_bounds = (w == 8) ? 1 : 2;
                    double bnd[2] = { upper_b, lower_b };
                    double clp[2] = { upper_c, lower_c };
                    size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
                    if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
                    size_t ro = c->rodata_section.length;
                    for (int k = 0; k < n_bounds; k++) {
                        zero_cc_buf_put_bytes(&c->rodata_section, &bnd[k], sizeof(double));
                        zero_cc_buf_put_bytes(&c->rodata_section, &clp[k], sizeof(double));
                    }
                    for (int k = 0; k < n_bounds; k++) {
                        int bt = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)bt, (int64_t)(ro + 16 * k));
                        int ct = c->next_temp_id++;
                        ir_emit_cmp(c, result, bt);
                        ir_emit_cond_set(c, ct, k == 0 ? CC_GE : CC_LT);
                        uint16_t skip = c->ir_next_label++;
                        ir_emit_je(c, (uint16_t)ct, skip);
                        int cv = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)cv, (int64_t)(ro + 16 * k + 8));
                        ir_emit_fmov(c, (uint16_t)result, (uint16_t)cv);
                        ir_emit_label(c, skip);
                    }
                }
            }
            ir_emit_cvttsd2si(c, (uint16_t)int_res_temp, (uint16_t)result);
            c->last_expr_is_float = 0;
            /* cvttsd2si yields a 64-bit int; extend to the cast target
             * width when it is a narrow int ((int)d, (short)d). */
            if (cast_ti) {
                size_t f2isz = type_size(cast_ti);
                if (f2isz == 1 || f2isz == 2 || f2isz == 4) {
                    bool f2isu = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                    if (f2isu) ir_emit_zext(c, int_res_temp, (int)f2isz);
                    else ir_emit_sext(c, int_res_temp, (int)f2isz);
                }
            }
            return int_res_temp;
        }
        /* Vector->vector cast: same-size bit reinterpret (GCC allows
         * casts between vectors of different element types, e.g.
         * `(vector float)(vector int)x`). The source lowers to the
         * address of the vector object; copy its raw bytes into a
         * fresh temp object of the target vector type. */
        if (cast_ti && type_is_vector(cast_ti)) {
            ZeroCCASTNode *cs = zero_cc_ast_get(arena, node->left);
            TypeInfo *cst = cs ? ast_node_get_typeinfo(c, cs) : NULL;
            if (cst && type_is_vector(cst)) {
            size_t ctsz = type_size(cast_ti);
            if (ctsz >= 1 && ctsz <= 128) {
                c->current_stack_frame = (c->current_stack_frame + (int32_t)ctsz + 7) & ~7;
                int32_t vres_off = c->current_stack_frame;
                int saddr = result; /* source vector in address form */
                for (size_t off = 0; off < ctsz; off += 8) {
                    size_t rem = ctsz - off;
                    int ca = c->next_temp_id++;
                    if (off == 0) { ir_emit_mov_reg(c, ca, saddr); }
                    else {
                        int o = c->next_temp_id++;
                        ir_emit_mov_imm(c, o, (int32_t)off);
                        ir_emit_add(c, ca, saddr, o);
                    }
                    int cv = c->next_temp_id++;
                    if (rem >= 8) ir_emit_deref(c, cv, ca);
                    else if (rem >= 4) ir_emit_load_32(c, cv, ca);
                    else if (rem >= 2) ir_emit_load_16(c, cv, ca);
                    else ir_emit_load_byte(c, cv, ca);
                    int da = c->next_temp_id++;
                    if (off == 0) { ir_emit_lea(c, da, BE(c)->fp_reg, -vres_off); }
                    else { ir_emit_lea(c, da, BE(c)->fp_reg, -(int32_t)(vres_off - (int32_t)off)); }
                    if (rem >= 8) ir_emit_store_64(c, cv, da);
                    else if (rem >= 4) ir_emit_store_32(c, cv, da);
                    else if (rem >= 2) ir_emit_store_16(c, cv, da);
                    else ir_emit_store_byte(c, cv, da);
                }
                int vaddr = c->next_temp_id++;
                ir_emit_lea(c, vaddr, BE(c)->fp_reg, -vres_off);
                c->last_expr_is_float = 0;
                return vaddr;
            }
            }
        }
        /* int->int narrowing cast ((int)longv, (short)x): extend the
         * 64-bit value to the target width so it carries true N-bit
         * semantics (a plain cast previously did nothing). The
         * truncation must go to a FRESH temp - in-place zext would
         * destroy a source operand that is used again after the cast
         * (`if (((int)x | (int)y) != 0) ...; return x + y;` -
         * c-torture execute/pr59014-2). */
        if (cast_ti) {
            size_t ctsz = type_size(cast_ti);
            if (c->last_expr_is_128 && (ctsz <= 8 && cast_ti->kind != TYPE_KIND_INT128)) {
                int trunc_t = c->next_temp_id++;
                ir_emit_trunc128_64(c, trunc_t, result);
                result = trunc_t;
                c->last_expr_is_128 = 0;
            }
            if (ctsz == 1 || ctsz == 2 || ctsz == 4) {
                bool ctsu = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                int cast_t = c->next_temp_id++;
                ir_emit_mov_reg(c, cast_t, result);
                if (ctsu) ir_emit_zext(c, cast_t, (int)ctsz);
                else ir_emit_sext(c, cast_t, (int)ctsz);
                result = cast_t;
            } else if (ctsz == 16 && cast_ti->kind == TYPE_KIND_INT128 &&
                       !c->last_expr_is_128) {
                /* Sprint 113: int -> __int128 cast: widen the 64-bit value
                 * to a real GPR128 (lo,hi). Without this the 64-bit temp is
                 * consumed as a 128-bit pair whose hi register holds garbage
                 * (`(~((__uint128_t)0))` read random hi bits). */
                bool ctsu = (cast_ti->modifiers & TYPE_MOD_UNSIGNED) != 0;
                int cast_t = c->next_temp_id++;
                if (ctsu) ir_emit_zext64_128(c, cast_t, result);
                else ir_emit_sext64_128(c, cast_t, result);
                result = cast_t;
                c->last_expr_is_128 = 1;
            }
        }
        return result;
}
