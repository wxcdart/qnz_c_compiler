/* AST Lowering - OP_CALL Machine Code Emission
 *
 * Extracted from ast_lowering_mc.c.
 */

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/ir/machine_emitter.h"

void emit_mc_call(ZeroCCCompiler *c, size_t i, const Instr *instr,
                  int32_t spill_base_offset, int *call_idx) {
    /* Extract function name from imm64 (stored as pointer during lowering) */
    const char *fn_name = (const char *)(intptr_t)instr->imm64;

    /* Collect all call arguments: args 0/1 from OP_CALL itself,
     * args 2+ from OP_CALL_ARG pseudo-instructions that follow
     * in the stream (Sprint 151c: no instr_index-keyed side
     * table). */
    int total_arg_count = (int)instr->temp_dest;
    int call_arg_temps[IR_CALL_MAX_ARGS] = {0};
    int call_arg_is_fp[IR_CALL_MAX_ARGS] = {0};
    uint8_t call_arg_stack_pos[IR_CALL_MAX_ARGS] = {0};

    if (total_arg_count > 0 && instr->temp_src1 != TEMP_NONE) {
        call_arg_temps[0] = (int)instr->temp_src1;
        call_arg_is_fp[0] = instr->cond;   /* Sprint 151c: is_fp of arg 0 */
        /* Sprint 158: arg 0/1 stack positions in imm (255 = register). */
        call_arg_stack_pos[0] = (uint8_t)(instr->imm & 0xFF);
    }
    if (total_arg_count > 1 && instr->temp_src2 != TEMP_NONE) {
        call_arg_temps[1] = (int)instr->temp_src2;
        call_arg_is_fp[1] = instr->width;  /* Sprint 151c: is_fp of arg 1 */
        call_arg_stack_pos[1] = (uint8_t)((instr->imm >> 8) & 0xFF);
    }
    for (int a = 2; a < total_arg_count; a++) {
        size_t ca_idx = i + 1 + (a - 2);
        if (ca_idx >= ir_get_count(c)) break;
        const Instr *ca_instr = ir_get(c, ca_idx);
        if (ca_instr->op != OP_CALL_ARG) break;
        call_arg_temps[a] = (int)ca_instr->temp_src1;
        call_arg_is_fp[a] = ca_instr->cond;
        call_arg_stack_pos[a] = ca_instr->width;
    }

    int indirect_callee_temp = -1;
    int aarch64_sret_temp = -1;
    int arg_temps[IR_CALL_MAX_ARGS] = {0};
    int arg_count = 0;
    for (int a = 0; a < total_arg_count; a++) {
        if (call_arg_is_fp[a] == 2) {
            indirect_callee_temp = call_arg_temps[a];
        } else if (call_arg_is_fp[a] == 3) {
            aarch64_sret_temp = call_arg_temps[a];
        } else if (call_arg_is_fp[a] == 0 &&
                   call_arg_stack_pos[a] >= 255) {
            /* Sprint 158: only REGISTER int args go to the
             * push/pop register phase - System V memory-arg
             * chunks (pos < 255) ride the caller stack and are
             * handled by the stack-arg push loop. */
            arg_temps[arg_count++] = call_arg_temps[a];
        }
    }

    /* If indirect call, save callee register into scratch_reg2 before argument setup */
    if (!fn_name && indirect_callee_temp >= 0) {
        int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_callee_temp);
        int sp_callee = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_callee_temp);
        if (sp_callee >= 0) {
            int32_t off = spill_base_offset + (sp_callee + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
        } else if (BE(c)->emit_mov_reg) {
            BE(c)->emit_mov_reg(c, BE(c)->scratch_reg2, r_callee);
        }
    }

    /* If AArch64 sret (AAPCS64), move sret buffer address into x8 (Indirect Result Location Register) */
    if (aarch64_sret_temp >= 0) {
        int r_sret = regalloc_get_reg_for_temp(&c->regalloc, aarch64_sret_temp);
        int sp_sret = regalloc_get_spill_slot_for_temp(&c->regalloc, aarch64_sret_temp);
        if (sp_sret >= 0) {
            int32_t off = spill_base_offset + (sp_sret + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, 8 /* x8 */, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
        } else if (BE(c)->emit_mov_reg) {
            BE(c)->emit_mov_reg(c, 8 /* x8 */, r_sret);
        }
    }

    /* Extract fp_reg_idx (number of float args) from label_idx */
    int fp_reg_idx = (int)(instr->label_idx & 0xFF);
    if ((instr->label_idx & 0xFF00) == 0x0100) {
        fp_reg_idx = 0;
    }

    /* Load float arguments from their spill slots into the FP
     * argument registers of the target ABI (xmm0..7 / d0..7 /
     * fa0..7), using the calling-convention table (x86/AArch64
     * arg regs 0..7, RISC-V fa0..fa7).
     * MUST run before the stack-argument pushes below: those
     * shift the stack pointer (n_stack*8 + stack_pad), and the
     * frame-relative slot offsets would then address the wrong
     * bytes (mispassing every float arg on 7+ int args). The
     * -S path (x86_emit_call_asm & co) emits FP args first for
     * the same reason. */
    int cidx = *call_idx;
    bool have_fp_table = (cidx < c->lowering.ir_ctx.call_count);
    if (have_fp_table) {
        int fp_count = c->lowering.ir_ctx.call_fp_spill_counts[cidx];
        int fp_reg_cap = BE(c)->conv ? BE(c)->conv->fp_arg_reg_count : 8;
        for (int f = 0; f < fp_count && f < fp_reg_cap; f++) {
            int32_t spill_off = c->lowering.ir_ctx.call_fp_spills[cidx][f];
            int fp_arg_reg = backend_get_fp_arg_reg(BE(c), f);
            if (fp_arg_reg < 0) fp_arg_reg = f;
            /* Load FP arg from stack slot into FP register */
            if (BE(c)->emit_load_fp) {
                BE(c)->emit_load_fp(c, fp_arg_reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -spill_off));
            }
        }
    } else {
        /* Capped-table fallback: the call_fp_spills side table
         * covers only the first 64 calls of a function (see
         * lowering_state.h); for calls past the cap, load the
         * FP register args from their reload temps - the same
         * mechanism the -S emitter uses (x86_emit_call_asm).
         * Without this, functions with >64 calls pass garbage
         * FP args on the MC path while -S/JIT-via-asm work. */
        int fp_reg_cap = BE(c)->conv ? BE(c)->conv->fp_arg_reg_count : 8;
        int fp_seen = 0;
        for (int ai = 0; ai < total_arg_count && fp_seen < fp_reg_cap; ai++) {
            if (call_arg_is_fp[ai] != 1) continue;
            if (call_arg_stack_pos[ai] < 255) continue; /* FP stack arg, pushed below */
            int temp = call_arg_temps[ai];
            int fp_arg_reg = backend_get_fp_arg_reg(BE(c), fp_seen);
            if (fp_arg_reg < 0) break;
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, fp_arg_reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -off));
                }
            } else {
                int phys = regalloc_get_reg_for_temp(&c->regalloc, temp);
                if (phys >= 0 && phys != fp_arg_reg && BE(c)->emit_fp_mov) {
                    BE(c)->emit_fp_mov(c, fp_arg_reg, phys);
                }
            }
            fp_seen++;
        }
    }
    (*call_idx)++;

    /* Stack args: everything the lowering marked stack_pos < 255
     * (integer args beyond the GP registers, FP args beyond the
     * FP registers). They occupy the call stack in original
     * argument order, so push in DESCENDING stack_pos order,
     * padding to keep the stack 16-byte aligned. FP stack args
     * are re-read as their 8-byte bit pattern from the same
     * spill slot the lowering stored (their XMM temp is unused).
     * (C2b fallback without the side table: the old int-only
     * formula. */
    int int_arg_count = BE(c)->conv->int_arg_reg_count;
    int n_stack;
    int stack_meta_temp[IR_CALL_MAX_ARGS];  /* per stack slot: int arg temp, -1 for FP */
    int stack_meta_fp[IR_CALL_MAX_ARGS];    /* per stack slot: FP stream index, -1 for int */
    int stack_meta_set[IR_CALL_MAX_ARGS];   /* slot was assigned (uninit guard) */
    for (int _s = 0; _s < IR_CALL_MAX_ARGS; _s++) stack_meta_set[_s] = 0;
    /* Sprint 158 (x86-64): range memory-arg (>32-chunk >16B
     * struct value) recorded by the lowering: its chunk span
     * [range_pos, range_pos+range_count) on the caller stack
     * is loaded from the frame buffer at -range_off. The span
     * can far exceed the per-slot tables, so it is handled in
     * the push loop without a table entry. */
    /* NOTE: *call_idx was already incremented above (before the
     * stack-arg section) - this call's ir_ctx slot is
     * *call_idx - 1. */
    int range_cidx = *call_idx - 1;
    int range_count = (range_cidx >= 0 && range_cidx < c->lowering.ir_ctx.call_count)
                      ? c->lowering.ir_ctx.call_range_counts[range_cidx] : 0;
    int range_pos = (range_cidx >= 0 && range_cidx < c->lowering.ir_ctx.call_count)
                    ? c->lowering.ir_ctx.call_range_pos[range_cidx] : 0;
    int32_t range_off = (range_cidx >= 0 && range_cidx < c->lowering.ir_ctx.call_count)
                        ? c->lowering.ir_ctx.call_range_offs[range_cidx] : 0;
    if (total_arg_count > 0) {
        n_stack = 0;
        int fp_seen = 0;
        for (int ai = 0; ai < total_arg_count; ai++) {
            if (call_arg_is_fp[ai] == 1) {
                int f = fp_seen++;
                int pos = call_arg_stack_pos[ai];
                if (pos < 255 && pos < IR_CALL_MAX_ARGS) {
                    stack_meta_temp[pos] = -1;
                    stack_meta_fp[pos] = f;
                    stack_meta_set[pos] = 1;
                    if (pos + 1 > n_stack) n_stack = pos + 1;
                }
                continue;
            }
            if (call_arg_is_fp[ai] == 2 || call_arg_is_fp[ai] == 3) continue;
            int pos = call_arg_stack_pos[ai];
            if (pos < 255 && pos < IR_CALL_MAX_ARGS) {
                stack_meta_temp[pos] = call_arg_temps[ai];
                stack_meta_fp[pos] = -1;
                stack_meta_set[pos] = 1;
                if (pos + 1 > n_stack) n_stack = pos + 1;
            }
        }
        if (range_count > 0 && range_pos + range_count > n_stack)
            n_stack = range_pos + range_count;
    } else {
        n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
    }
    int slot_sz = (c->isa == ISA_RISCV32) ? 4 : 8;
    int stack_bytes = n_stack * slot_sz;
    int stack_pad = ((stack_bytes + 15) & ~15) - stack_bytes;
    int32_t extra_sp = 0;
    if (n_stack > 0) {
        if (stack_pad) {
            BE(c)->emit_stack_adjust(c, -stack_pad);
            extra_sp += stack_pad;
        }
        for (int pos = n_stack - 1; pos >= 0; pos--) {
            int r;
            bool fp_stack_done = false;
            if (range_count > 0 && pos >= range_pos &&
                pos < range_pos + range_count) {
                /* Sprint 158: chunk (pos - range_pos) of the
                 * block-copied range buffer lives at frame
                 * -(range_off + 8*ch) - load and push it. */
                int ch = pos - range_pos;
                /* The buffer's first byte sits at frame -range_off
                 * (the frame grows down), so chunk ch is at
                 * -range_off + 8*ch. */
                if (BE(c)->emit_load)
                    BE(c)->emit_load(c, BE(c)->scratch_reg1,
                                     zero_cc_frame_base_reg(c),
                                     zero_cc_frame_disp_at(c, -range_off + 8 * ch, extra_sp));
                BE(c)->emit_push_reg(c, BE(c)->scratch_reg1);
                extra_sp += slot_sz;
                continue;
            }
            if (pos >= IR_CALL_MAX_ARGS) continue; /* range-only span slot */
            if (!stack_meta_set[pos]) {
                /* Unassigned/padding slot: adjust stack to keep slot indexing aligned */
                BE(c)->emit_stack_adjust(c, -slot_sz);
                extra_sp += slot_sz;
                continue;
            }
            if (stack_meta_fp[pos] >= 0) {
                /* FP stack arg: bits live in the lowering's spill
                 * slot (or, past the 64-call table cap, in the
                 * arg's reload temp). */
                if (have_fp_table) {
                    int f = stack_meta_fp[pos];
                    int32_t fslot = c->lowering.ir_ctx.call_fp_spills[cidx][f];
                    if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -fslot, extra_sp));
                    r = BE(c)->scratch_reg1;
                } else {
                    int t = -1;
                    for (int ai = 0; ai < total_arg_count; ai++) {
                        if (call_arg_is_fp[ai] == 1 && call_arg_stack_pos[ai] == pos) {
                            t = call_arg_temps[ai];
                            break;
                        }
                    }
                    r = BE(c)->scratch_reg1;
                    if (t >= 0) {
                        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, t);
                        if (sp >= 0) {
                            int32_t off = spill_base_offset + (sp + 1) * 8;
                            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                        } else {
                            /* Bits live in the reload temp's FP
                             * register: store them just below RSP
                             * and claim that slot (same shape as
                             * the -S emitter's FP stack args). */
                            int phys = regalloc_get_reg_for_temp(&c->regalloc, t);
                            if (phys >= 0 && BE(c)->emit_store_fp) {
                                BE(c)->emit_store_fp(c, phys, BE(c)->sp_reg, -slot_sz);
                                BE(c)->emit_stack_adjust(c, -slot_sz);
                                extra_sp += slot_sz;
                                fp_stack_done = true;
                            }
                        }
                    }
                }
            } else {
                int t = stack_meta_temp[pos];
                r = regalloc_get_reg_for_temp(&c->regalloc, t);
                int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, t);
                if (sp >= 0) {
                    int32_t off = spill_base_offset + (sp + 1) * 8;
                    if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
                    r = BE(c)->scratch_reg1;
                }
            }
            if (fp_stack_done) continue; /* slot already claimed via [rsp-8] store */
            BE(c)->emit_push_reg(c, r);
            extra_sp += slot_sz;
        }
    }

    /* Safe parallel move: push all src registers then pop into dst registers */
    int n_args = (arg_count < int_arg_count) ? arg_count : int_arg_count;
    int src_regs[8];
    for (int a = 0; a < n_args; a++) {
        int r = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[a]);
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[a]);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, zero_cc_frame_base_reg(c), zero_cc_frame_disp_at(c, -off, extra_sp));
            r = BE(c)->scratch_reg1;
        }
        src_regs[a] = r;
        BE(c)->emit_push_reg(c, src_regs[a]);
        extra_sp += slot_sz;
    }
    for (int a = n_args - 1; a >= 0; a--) {
        int dst_reg = backend_get_int_arg_reg(BE(c), a);
        if (dst_reg < 0) {
            /* Argument is stack-passed, skip register assignment */
            continue;
        }
        BE(c)->emit_pop_reg(c, dst_reg);
        extra_sp -= slot_sz;
    }

    /* System V AMD64 ABI: For variadic functions (or unprototyped/indirect
     * calls that may reach a variadic definition), %al must hold the number
     * of SSE registers used. Skip when calling a known non-variadic prototype. */
    bool need_variadic_al = true;
    if (fn_name) {
        const ZeroCCFunction *callee_fn = zero_cc_find_function_by_symbol(c, fn_name);
        if (callee_fn && callee_fn->has_prototype && !callee_fn->is_variadic) {
            need_variadic_al = false;
        }
    }
    if (need_variadic_al && BE(c)->emit_variadic_al_setup) {
        BE(c)->emit_variadic_al_setup(c, fp_reg_idx);
    }

    /* Caller-saved registers across calls on AArch64 (x0-x18) are
     * handled by live_range_compute() marking intervals that span
     * an OP_CALL with spans_call=true, and alloc_reg() in regalloc.h
     * restricting spans_call intervals to callee-saved registers
     * (x19-x28) or stack spill slots. */
    /* Emit the call */
    if (!fn_name && indirect_callee_temp >= 0 && BE(c)->emit_call_reg) {
        BE(c)->emit_call_reg(c, BE(c)->scratch_reg2 /* r11 */);
    } else {
        BE(c)->emit_call(c, fn_name);
    }

    /* Caller cleans up stack-passed arguments (and alignment pad) */
    if (n_stack > 0) {
        BE(c)->emit_stack_adjust(c, stack_bytes + stack_pad);
    }

    /* Handle struct return (9-16 bytes) unpacking */
    if ((instr->label_idx & 0xFF00) == 0x0100) {
        int struct_size = instr->label_idx & 0xFF;
        if (struct_size > 8 && struct_size <= 16) {
            /* Physical rdx contains upper 8 bytes */
            /* This is handled by caller store logic */
        }
    }
}
