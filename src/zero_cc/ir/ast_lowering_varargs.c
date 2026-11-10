/* AST Lowering - variadic argument lowering (va_start / va_arg)
 * (Sprint 114: extracted from ast_lowering_builtins.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

int lower_expr_va_start(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* va_start(ap, last) - initialize the ABI-correct va_list.
         *
         * The callee prologue spilled ALL argument registers into a
         * 16-slot save area above the locals (see the variadic spill in
         * zero_cc_lower_ast): slots 0..5 = integer arg regs in order,
         * slots 6..13 = FP arg regs in order. Named parameters consume
         * the streams in order, so the streams start just past the
         * named count. Once a stream is exhausted (ptr == end) reads
         * continue from ovf - the shared stack-overflow area (the
         * caller's stack args, in argument order).
         *
         * va_list layout (5 x 8-byte pointers):
         *   +0 gp, +8 gp_end, +16 fp, +24 fp_end, +32 ovf. */
        int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
        ZeroCCFunction *fn = zero_cc_get_current_fn(c);

        if (ap_addr != TEMP_NONE && fn) {

            /* Save area base: above all named locals (mirrors the
             * prologue's base_spill_offset = locals-top + 8). */
            int32_t top_local = (int32_t)BE(c)->callee_save_frame_reserve;
            for (size_t i = 0; i < c->local_count; i++) {
                if (c->locals[i].stack_offset > top_local) top_local = c->locals[i].stack_offset;
            }
            int32_t save_base = top_local + 8;

            /* Named params consume the GP/FP streams in order (mirror
             * the prologue's param-save loop, incl. the hidden sret
             * pointer and multi-chunk struct params). */
            int gp_regs = BE(c)->conv->int_arg_reg_count;
            int fp_regs = BE(c)->conv->fp_arg_reg_count;
            int gp_idx = (fn->ret_struct_sz > 16 && c->isa != ISA_AARCH64) ? 1 : 0;
            int fp_idx = 0;
            int stack_named = 0;  /* stack-passed named params (ovf base) */
            for (size_t p = 0; p < fn->param_count && p < c->local_count; p++) {
                ZeroCCSym *ps = &c->locals[p];
                if (!ps->is_param) continue;
                bool pfp = (ps->type.tag == TYPE_FLOAT || ps->type.tag == TYPE_DOUBLE);
                /* >16B struct/union params take ONE hidden-pointer slot
                 * (SysV ABI), not one slot per 8-byte chunk. */
                bool phidden = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 16;
                bool pmulti = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 8 && !phidden;
                if (phidden) {
                    if (gp_idx < gp_regs) gp_idx++; else stack_named++;
                } else if (pmulti) {
                    int chunks = (int)((ps->type.size + 7) / 8);
                    for (int ch = 0; ch < chunks; ch++) { if (gp_idx < gp_regs) gp_idx++; else stack_named++; }
                } else if (pfp) {
                    if (fp_idx < fp_regs) fp_idx++; else stack_named++;
                } else {
                    if (gp_idx < gp_regs) gp_idx++; else stack_named++;
                }
            }

            int t;
            t = zc_alloc_temp(c);
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + gp_idx * 8));
            ir_emit_store_64(c, t, ap_addr);             /* +0 gp */

            t = zc_alloc_temp(c);
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + gp_regs * 8));
            int off8 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off8, 8);
            int ap_8 = zc_alloc_temp(c);
            ir_emit_add(c, ap_8, ap_addr, off8);
            ir_emit_store_64(c, t, ap_8);                /* +8 gp_end */

            t = zc_alloc_temp(c);
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + (gp_regs + fp_idx) * 8));
            int off16 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off16, 16);
            int ap_16 = zc_alloc_temp(c);
            ir_emit_add(c, ap_16, ap_addr, off16);
            ir_emit_store_64(c, t, ap_16);               /* +16 fp */

            t = zc_alloc_temp(c);
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + (gp_regs + fp_regs) * 8));
            int off24 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off24, 24);
            int ap_24 = zc_alloc_temp(c);
            ir_emit_add(c, ap_24, ap_addr, off24);
            ir_emit_store_64(c, t, ap_24);               /* +24 fp_end */

            t = zc_alloc_temp(c);
            ir_emit_lea(c, t, TEMP_NONE, 16 + 8 * stack_named);
            int off32 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off32, 32);
            int ap_32 = zc_alloc_temp(c);
            ir_emit_add(c, ap_32, ap_addr, off32);
            ir_emit_store_64(c, t, ap_32);               /* +32 ovf */
        }

        c->last_expr_is_float = 0;
        return VREG_0;
}

int lower_expr_va_arg(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* va_arg(ap, type) - extract the next variadic argument.
         * Integer/pointer targets read the GP stream (va_list +0/+8),
         * float/double targets the FP stream (+16/+24); when a stream
         * is exhausted (ptr == end) both continue from the shared
         * overflow pointer (+32). Values are read through the stream
         * pointer (frame-independent dereference). */
        TypeInfo *t_t = ast_node_get_typeinfo(c, node);

        /* GNU variably-modified struct: passed as two consecutive GP
         * units - the object pointer, then the total runtime size.
         * Read both (with the stream-exhaustion overflow path) and
         * return the pointer; the enclosing assignment copies the
         * runtime number of bytes (zero_cc_emit_vla_struct_copy). */
        if (t_t && (t_t->kind == TYPE_KIND_STRUCT || t_t->kind == TYPE_KIND_UNION) &&
            t_t->details.record.is_vla_struct && t_t->details.record.tag) {
            ZeroCCType *reg_v = zero_cc_find_struct(c, t_t->details.record.tag);
            if (reg_v && zc_find_vla_struct(reg_v)) {
                int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
                int result_v = zc_alloc_temp(c);
                if (ap_addr != TEMP_NONE) {
                    for (int unit = 0; unit < 2; unit++) {
                        int off_pf = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_pf, 0);
                        int addr_ptr_field = zc_alloc_temp(c);
                        ir_emit_add(c, addr_ptr_field, ap_addr, off_pf);
                        int off_ef = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_ef, 8);
                        int addr_end_field = zc_alloc_temp(c);
                        ir_emit_add(c, addr_end_field, ap_addr, off_ef);
                        int off_ovf = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off_ovf, 32);
                        int addr_ovf_field = zc_alloc_temp(c);
                        ir_emit_add(c, addr_ovf_field, ap_addr, off_ovf);
                        int hp = zc_alloc_temp(c);
                        ir_emit_deref(c, hp, addr_ptr_field);
                        int he = zc_alloc_temp(c);
                        ir_emit_deref(c, he, addr_end_field);
                        int hd = zc_alloc_temp(c);
                        ir_emit_sub(c, hd, hp, he);
                        uint16_t ov_lbl = c->ir_next_label++;
                        uint16_t ok_lbl = c->ir_next_label++;
                        ir_emit_je(c, (uint32_t)hd, ov_lbl);
                        int val = (unit == 0) ? result_v : zc_alloc_temp(c);
                        ir_emit_deref(c, val, hp);
                        int hn8 = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, hn8, -8);
                        int hp2 = zc_alloc_temp(c);
                        ir_emit_add(c, hp2, hp, hn8);
                        ir_emit_store_64(c, hp2, addr_ptr_field);
                        ir_emit_jmp(c, ok_lbl);
                        ir_emit_label(c, ov_lbl);
                        int ho = zc_alloc_temp(c);
                        ir_emit_deref(c, ho, addr_ovf_field);
                        if (unit == 0) {
                            ir_emit_deref(c, result_v, ho);
                        } else {
                            int sv = zc_alloc_temp(c);
                            ir_emit_deref(c, sv, ho);
                            (void)sv;
                        }
                        int ho8 = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, ho8, 8);
                        int ho2 = zc_alloc_temp(c);
                        ir_emit_add(c, ho2, ho, ho8);
                        ir_emit_store_64(c, ho2, addr_ovf_field);
                        ir_emit_label(c, ok_lbl);
                    }
                }
                c->last_expr_is_float = 0;
                return result_v;
            }
        }

        bool is_fp = t_t && (t_t->kind == TYPE_KIND_FLOAT || t_t->kind == TYPE_KIND_DOUBLE);
        bool use_fp_stream = is_fp && !(c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32);

        int result_temp = zc_alloc_temp(c);
        int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
        if (ap_addr != TEMP_NONE) {
            int32_t ptr_field = use_fp_stream ? 16 : 0;    /* va_list member offset of the stream ptr */
            int32_t end_field = use_fp_stream ? 24 : 8;   /* ...and of its end sentinel */
            uint16_t ovf_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            int addr_ptr_field = ap_addr;
            if (ptr_field != 0) {
                int off_pf = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off_pf, ptr_field);
                addr_ptr_field = zc_alloc_temp(c);
                ir_emit_add(c, addr_ptr_field, ap_addr, off_pf);
            }
            int off_ef = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off_ef, end_field);
            int addr_end_field = zc_alloc_temp(c);
            ir_emit_add(c, addr_end_field, ap_addr, off_ef);

            int off_ovf = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off_ovf, 32);
            int addr_ovf_field = zc_alloc_temp(c);
            ir_emit_add(c, addr_ovf_field, ap_addr, off_ovf);

            size_t struct_sz = (t_t && (t_t->kind == TYPE_KIND_STRUCT || t_t->kind == TYPE_KIND_UNION)) ? type_size(t_t) : 0;
            bool is_struct_hidden = (!is_fp && struct_sz > 16);
            bool is_struct2 = (!is_fp && struct_sz > 8 && struct_sz <= 16);
            if (is_struct_hidden) {
                /* >16B struct va_arg: the caller passed ONE hidden
                 * pointer slot (SysV ABI) - read it like a pointer;
                 * the result is the address of the caller's copy. */
                int hp = zc_alloc_temp(c);
                ir_emit_deref(c, hp, addr_ptr_field);    /* fresh stream ptr */
                int he = zc_alloc_temp(c);
                ir_emit_deref(c, he, addr_end_field);    /* fresh end sentinel */
                int hd = zc_alloc_temp(c);
                ir_emit_sub(c, hd, hp, he);
                uint16_t ho_lbl = c->ir_next_label++;
                uint16_t ha_lbl = c->ir_next_label++;
                ir_emit_je(c, (uint32_t)hd, ho_lbl);
                /* hp = ap[0] = the ADDRESS of the save slot; the slot
                 * holds the hidden POINTER to the caller's copy. The
                 * va_arg value for a >16B struct is that hidden pointer
                 * (an address the consumers copy from; see
                 * ast_expr_is_struct_addr) - one dereference of the slot. */
                ir_emit_deref(c, result_temp, hp);
                int hn8 = zc_alloc_temp(c);
                ir_emit_mov_imm(c, hn8, -8);
                int hp2 = zc_alloc_temp(c);
                ir_emit_add(c, hp2, hp, hn8);
                ir_emit_store_64(c, hp2, addr_ptr_field);
                ir_emit_jmp(c, ha_lbl);
                ir_emit_label(c, ho_lbl);
                int ho = zc_alloc_temp(c);
                ir_emit_deref(c, ho, addr_ovf_field);
                ir_emit_deref(c, result_temp, ho);
                int hp8 = zc_alloc_temp(c);
                ir_emit_mov_imm(c, hp8, 8);
                int ho2 = zc_alloc_temp(c);
                ir_emit_add(c, ho2, ho, hp8);
                ir_emit_store_64(c, ho2, addr_ovf_field);
                ir_emit_label(c, ha_lbl);
            } else if (is_struct2) {
                /* 16B (or 9-16B) struct va_arg: two 8-byte chunks, but
                 * each chunk needs its OWN GP-exhaustion check - the
                 * chunks can straddle the register-stack boundary (the
                 * last register chunk and the first stack chunk, e.g.
                 * 4 x 16B structs passed variadically: chunks 0-4 in
                 * registers, chunks 5-7 on the caller's stack). */
                for (int ch = 0; ch < 2; ch++) {
                    int cp = zc_alloc_temp(c);
                    ir_emit_deref(c, cp, addr_ptr_field);    /* fresh stream ptr */
                    int ce = zc_alloc_temp(c);
                    ir_emit_deref(c, ce, addr_end_field);    /* fresh end sentinel */
                    int cd = zc_alloc_temp(c);
                    ir_emit_sub(c, cd, cp, ce);
                    uint16_t o_lbl = c->ir_next_label++;
                    uint16_t a_lbl = c->ir_next_label++;
                    ir_emit_je(c, (uint32_t)cd, o_lbl);
                    int dst = (ch == 0) ? result_temp : VREG_2;
                    ir_emit_deref(c, dst, cp);
                    int n8 = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, n8, -8);
                    int cp2 = zc_alloc_temp(c);
                    ir_emit_add(c, cp2, cp, n8);
                    ir_emit_store_64(c, cp2, addr_ptr_field);
                    ir_emit_jmp(c, a_lbl);
                    ir_emit_label(c, o_lbl);
                    int co = zc_alloc_temp(c);
                    ir_emit_deref(c, co, addr_ovf_field);
                    ir_emit_deref(c, dst, co);
                    int p8 = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, p8, 8);
                    int co2 = zc_alloc_temp(c);
                    ir_emit_add(c, co2, co, p8);
                    ir_emit_store_64(c, co2, addr_ovf_field);
                    ir_emit_label(c, a_lbl);
                }
            } else {
            int t_ptr = zc_alloc_temp(c);
            ir_emit_deref(c, t_ptr, addr_ptr_field);
            int t_end = zc_alloc_temp(c);
            ir_emit_deref(c, t_end, addr_end_field);
            int t_diff = zc_alloc_temp(c);
            ir_emit_sub(c, t_diff, t_ptr, t_end);
            ir_emit_je(c, (uint32_t)t_diff, ovf_label);

            /* Save-area path. The save slots extend toward MORE
             * negative offsets (slot i at -(base + i*8)), so the
             * stream pointer DECREASES by 8 per argument; it is
             * exhausted when it reaches the end sentinel. */
            if (is_fp) {
                if (t_t->kind == TYPE_KIND_FLOAT) ir_emit_vloadss(c, (uint32_t)result_temp, (uint32_t)t_ptr);
                else ir_emit_vloadsd(c, (uint32_t)result_temp, (uint32_t)t_ptr);
                int t_neg8 = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_neg8, -8);
                int t_ptr2 = zc_alloc_temp(c);
                ir_emit_add(c, t_ptr2, t_ptr, t_neg8);
                ir_emit_store_64(c, t_ptr2, addr_ptr_field);
            } else {
                ir_emit_deref(c, result_temp, t_ptr);
                int t_neg8 = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_neg8, -8);
                int t_ptr2 = zc_alloc_temp(c);
                ir_emit_add(c, t_ptr2, t_ptr, t_neg8);
                ir_emit_store_64(c, t_ptr2, addr_ptr_field);
            }
            ir_emit_jmp(c, end_label);

            /* Stack-overflow path: the caller's stack args sit at
             * fp+16, fp+24, ... - the pointer INCREASES by 8. */
            ir_emit_label(c, ovf_label);
            int t_ovf = zc_alloc_temp(c);
            ir_emit_deref(c, t_ovf, addr_ovf_field);
            if (is_fp) {
                if (t_t->kind == TYPE_KIND_FLOAT) ir_emit_vloadss(c, (uint32_t)result_temp, (uint32_t)t_ovf);
                else ir_emit_vloadsd(c, (uint32_t)result_temp, (uint32_t)t_ovf);
                int t_pos8 = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_pos8, 8);
                int t_ovf2 = zc_alloc_temp(c);
                ir_emit_add(c, t_ovf2, t_ovf, t_pos8);
                ir_emit_store_64(c, t_ovf2, addr_ovf_field);
            } else {
                ir_emit_deref(c, result_temp, t_ovf);
                int t_pos8 = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_pos8, 8);
                int t_ovf2 = zc_alloc_temp(c);
                ir_emit_add(c, t_ovf2, t_ovf, t_pos8);
                ir_emit_store_64(c, t_ovf2, addr_ovf_field);
            }
            ir_emit_label(c, end_label);
            }

            /* Narrow/widen integer targets to their true C width
             * (values arrived int-promoted / zero-extended). */
            if (!is_fp && t_t) {
                size_t tsz = type_size(t_t);
                bool tu = (t_t->modifiers & TYPE_MOD_UNSIGNED) != 0;
                if (tsz == 4) { if (tu) ir_emit_zext(c, result_temp, 4); else ir_emit_sext(c, result_temp, 4); }
                else if (tsz == 2) { if (tu) ir_emit_zext(c, result_temp, 2); else ir_emit_sext(c, result_temp, 2); }
                else if (tsz == 1) { if (tu) ir_emit_zext(c, result_temp, 1); else ir_emit_sext(c, result_temp, 1); }
            }
            c->last_expr_is_float = is_fp;
        } else {
            /* No identifiable va_list - legacy single-pointer
             * behaviour (dereference and bump) as a fallback. */
            int ap_temp = lower_expr(c, arena, node->left);
            ir_emit_deref(c, result_temp, ap_temp);
            int eight_temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, eight_temp, 8);
            int new_ap_temp = zc_alloc_temp(c);
            ir_emit_add(c, new_ap_temp, ap_temp, eight_temp);
            c->last_expr_is_float = is_fp;
        }
        return result_temp;
}
