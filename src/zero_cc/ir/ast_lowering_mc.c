/* AST Lowering - IR to Machine Code Emission */

#include "zero_cc/ir/ast_lowering_internal.h"

/* Emit function prologue directly (without IR instruction) */
void emit_prologue_mc(ZeroCCCompiler *c, size_t frame_size) {
    /* Use the backend's emit_prologue function */
    if (BE(c)->emit_prologue) {
        BE(c)->emit_prologue(c, frame_size, 0);
    }
    /* Save used callee-saved GPRs in the prologue before function body executes */
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    if (mask && BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->emit_store) {
        const int *callee_regs = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int reg = callee_regs[i];
            if ((mask >> reg) & 1u) {
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                BE(c)->emit_store(c, reg, BE(c)->fp_reg, -slot);
            }
        }
    }
}

/* Emit machine code from IR with allocated registers */
void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset) {
    const Instr *instrs = ir_get_buffer();
    size_t count = ir_get_count();
    size_t label_count = c->ir_next_label;
    bool *label_defined = label_count ? (bool *)calloc(label_count, sizeof(bool)) : NULL;
    size_t *label_offsets = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    size_t **label_patches = label_count ? (size_t **)calloc(label_count, sizeof(size_t *)) : NULL;
    size_t *label_patch_counts = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    size_t *label_patch_caps = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
    int call_idx = 0;

    for (size_t i = 0; i < count; i++) {
        const Instr *instr = &instrs[i];
        if (instr->op == OP_DEAD || instr->op == OP_NOP) {
            continue;
        }

        /* Frame pointer (0xFF) should NOT be allocated - use actual FP register */
        int phys_dst = (instr->temp_dest== TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
        int phys_src1 = (instr->temp_src1== TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
        int phys_src2 = (instr->temp_src2== TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src2);

        /* Check for spilled temps that need reloading / storing.
         * OP_CALL / OP_SYSCALL pack arg_count in temp_dest and resolve their
         * argument temps themselves (from the side table). */
        int spill_src1 = (instr->op == OP_CALL || instr->op == OP_SYSCALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src1);
        int spill_src2 = (instr->op == OP_CALL || instr->op == OP_SYSCALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src2);
        int spill_dst  = (instr->op == OP_CALL || instr->op == OP_SYSCALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_dest);

        /* Reload spilled sources into scratch registers. FP-class values
         * use the FP scratch XMMs with movsd (emit_load_fp); GPR values use
         * r10/r11 (C8). */
        if (spill_src1 >= 0) {
            int32_t slot_off = spill_base_offset + (spill_src1 + 1) * 8;
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1) == REG_CLASS_XMM) {
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, BE(c)->fp_scratch_reg1, BE(c)->fp_reg, -slot_off);
                }
                phys_src1 = BE(c)->fp_scratch_reg1;
            } else {
                if (BE(c)->emit_load) {
                    BE(c)->emit_load(c, BE(c)->scratch_reg1 /* r10 */, BE(c)->fp_reg, -slot_off);
                }
                phys_src1 = BE(c)->scratch_reg1;
            }
        }
        if (spill_src2 >= 0) {
            int32_t slot_off = spill_base_offset + (spill_src2 + 1) * 8;
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_src2) == REG_CLASS_XMM) {
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, BE(c)->fp_scratch_reg2, BE(c)->fp_reg, -slot_off);
                }
                phys_src2 = BE(c)->fp_scratch_reg2;
            } else {
                if (BE(c)->emit_load) {
                    BE(c)->emit_load(c, BE(c)->scratch_reg2 /* r11 */, BE(c)->fp_reg, -slot_off);
                }
                phys_src2 = BE(c)->scratch_reg2;
            }
        }
        if (spill_dst >= 0) {
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
                phys_dst = BE(c)->fp_scratch_reg1;
            } else {
                phys_dst = BE(c)->scratch_reg1;
            }
        }

        switch (instr->op) {
            /* Parameter saving */
            case OP_PARAM_SAVE_INT:
                if (BE(c)->emit_param_save_int) {
                    BE(c)->emit_param_save_int(c, (int)instr->imm, (int32_t)instr->imm64);
                }
                break;

            case OP_PARAM_SAVE_FP:
                if (BE(c)->emit_param_save_fp) {
                    BE(c)->emit_param_save_fp(c, (int)instr->imm, (int32_t)instr->imm64);
                }
                break;

            case OP_PARAM_LOAD: {
                bool is_fp = instr->cond != 0;
                int arg_idx = (int)instr->imm;
                if (is_fp) {
                    if (arg_idx >= 0 && arg_idx < BE(c)->conv->fp_arg_reg_count) {
                        int src_reg = BE(c)->conv->fp_arg_regs[arg_idx];
                        if (phys_dst != src_reg && BE(c)->emit_fp_mov) {
                            BE(c)->emit_fp_mov(c, phys_dst, src_reg);
                        }
                    }
                } else {
                    if (arg_idx >= 0 && arg_idx < BE(c)->conv->int_arg_reg_count) {
                        int src_reg = BE(c)->conv->int_arg_regs[arg_idx];
                        if (phys_dst != src_reg && BE(c)->emit_mov_reg) {
                            BE(c)->emit_mov_reg(c, phys_dst, src_reg);
                        }
                    }
                }
                break;
            }

            case OP_VARIADIC_SPILL:
                if (BE(c)->emit_variadic_spill) {
                    BE(c)->emit_variadic_spill(c, (int)instr->imm, (int32_t)instr->imm64);
                }
                break;

            case OP_MOV_IMM:
                BE(c)->emit_mov_imm(c, phys_dst, instr->imm64);
                break;

            case OP_MOV_MEM:
                BE(c)->emit_store(c, phys_src1, phys_src2, (int32_t)instr->imm64);
                break;

            case OP_MOV:
                BE(c)->emit_load(c, phys_dst, phys_src1, (int32_t)instr->imm64);
                break;

            case OP_MOV_REG:
                if (phys_dst != phys_src1) {
                    BE(c)->emit_mov_reg(c, phys_dst, phys_src1);
                }
                break;

            case OP_LEA:
                BE(c)->emit_lea(c, phys_dst, phys_src1, (int32_t)instr->imm64);
                break;

            case OP_ADD:
                BE(c)->emit_add(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_SUB:
                BE(c)->emit_sub(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_NEG:
                if (BE(c)->emit_neg) {
                    BE(c)->emit_neg(c, phys_dst, phys_src1);
                }
                break;

            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                if (BE(c)->emit_extend) {
                    int ext_width = (instr->op == OP_SEXT16 || instr->op == OP_ZEXT16) ? 2
                                     : (instr->op == OP_SEXT32 || instr->op == OP_ZEXT32) ? 4 : 1;
                    int ext_sign  = (instr->op >= OP_SEXT8 && instr->op <= OP_SEXT32) ? 1 : 0;
                    BE(c)->emit_extend(c, phys_dst, phys_src1, ext_width, ext_sign);
                }
                break;

            case OP_MUL:
                BE(c)->emit_mul(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_DIV:
                BE(c)->emit_div(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_MOD:
                BE(c)->emit_mod(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_AND:
                BE(c)->emit_and(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_OR:
                BE(c)->emit_or(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_XOR:
                BE(c)->emit_xor(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_SHL:
                BE(c)->emit_shl(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_SHR:
                BE(c)->emit_shr(c, phys_dst, phys_src1, phys_src2);
                break;

            case OP_CMP: {
                /* Store operands for RISC-V backend (flags materialization) */
                c->lowering.cmp_src1 = phys_src1;
                c->lowering.cmp_src2 = phys_src2;
                RegClass rc1 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1);
                RegClass rc2 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src2);
                int cmp_fp = (rc1 == REG_CLASS_XMM || rc1 == REG_CLASS_VEC ||
                              rc2 == REG_CLASS_XMM || rc2 == REG_CLASS_VEC);
                c->lowering.cmp_is_fp = cmp_fp;
                if (cmp_fp && BE(c)->emit_fp_cmp) {
                    BE(c)->emit_fp_cmp(c, phys_src1, phys_src2);
                } else {
                    BE(c)->emit_cmp(c, phys_src1, phys_src2);
                }
                break;
            }

            case OP_COND_SET:
                /* set dst based on flags and condition code */
                if (BE(c)->emit_cond_set) {
                    BE(c)->emit_cond_set(c, phys_dst, (int)instr->cond);
                }
                break;

            case OP_DEREF:
                BE(c)->emit_deref(c, phys_dst, phys_src1);
                break;

            case OP_LOAD_BYTE:
                BE(c)->emit_load_byte(c, phys_dst, phys_src1);
                break;

            case OP_CMP_ZERO:
                /* RISC-V flags materialization: cmp-zero records the value
                 * against the zero register so a later OP_COND_SET can
                 * re-materialize the predicate (see live-range extension in
                 * live_range_compute for RISC-V). */
                {
                    RegClass rc0 = regalloc_get_class_for_temp(&c->regalloc, instr->temp_src1);
                    c->lowering.cmp_src1 = phys_src1;
                    c->lowering.cmp_src2 = 0;  /* zero register */
                    c->lowering.cmp_is_fp = (rc0 == REG_CLASS_XMM || rc0 == REG_CLASS_VEC);
                }
                BE(c)->emit_cmp_zero(c, phys_src1);
                break;

            case OP_CVTTSD2SI:
                if (BE(c)->emit_fp_cvtsi) {
                    BE(c)->emit_fp_cvtsi(c, phys_dst, phys_src1);  /* convert xmm<src> to GPR dst */
                }
                break;

            case OP_CVTSI2SD:
                if (BE(c)->emit_fp_cvtsf) {
                    BE(c)->emit_fp_cvtsf(c, phys_dst, phys_src1);  /* convert GPR src to xmm<dst> */
                }
                break;

            case OP_LOAD_16:
                if (BE(c)->emit_load_16) {
                    BE(c)->emit_load_16(c, phys_dst, phys_src1);
                }
                break;

            case OP_LOAD_32:
                if (BE(c)->emit_load_32) {
                    BE(c)->emit_load_32(c, phys_dst, phys_src1);
                }
                break;

            case OP_STORE_BYTE:
                if (BE(c)->emit_store_byte) {
                    BE(c)->emit_store_byte(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_16:
                if (BE(c)->emit_store_16) {
                    BE(c)->emit_store_16(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_32:
                if (BE(c)->emit_store_32) {
                    BE(c)->emit_store_32(c, phys_src1, phys_src2);
                }
                break;

            case OP_STORE_64:
                if (BE(c)->emit_store_64) {
                    BE(c)->emit_store_64(c, phys_src1, phys_src2);
                }
                break;

            case OP_VLA_ALLOC:
                if (BE(c)->emit_vla_alloc) {
                    BE(c)->emit_vla_alloc(c, phys_dst, phys_src1);
                }
                break;

            case OP_CALLEE_SAVE_STORES:
                /* Handled in emit_prologue_mc at function entry */
                break;

            case OP_CALLEE_SAVE_LOADS:
                /* Emit callee-saved register loads using mask from IR instruction */
                /* Generic implementation using backend's callee_saved_gpr array and slot offsets */
                if (BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->emit_load) {
                    uint32_t mask = (uint32_t)instr->imm64;
                    const int *callee_regs = BE(c)->conv->callee_saved_gpr;
                    const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
                    int count = BE(c)->conv->callee_saved_gpr_count;
                    for (int i = 0; i < count; i++) {
                        int reg = callee_regs[i];
                        if ((mask >> reg) & 1u) {
                            /* slot_offsets is indexed by array position (i), not physical reg number */
                            int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                            BE(c)->emit_load(c, reg, BE(c)->fp_reg, -slot);
                        }
                    }
                }
                break;

            case OP_ATOMIC_XADD:
                if (BE(c)->emit_atomic_xadd) {
                    BE(c)->emit_atomic_xadd(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_CMPXCHG: {
                int phys_expected = (instr->label_idx == TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg) {
                    BE(c)->emit_atomic_cmpxchg(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_CMPXCHG_BOOL: {
                int phys_expected = (instr->label_idx == TEMP_NONE) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg_bool) {
                    BE(c)->emit_atomic_cmpxchg_bool(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_XCHG:
                if (BE(c)->emit_atomic_xchg) {
                    BE(c)->emit_atomic_xchg(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_MFENCE:
                if (BE(c)->emit_mfence) {
                    BE(c)->emit_mfence(c);
                }
                break;

            case OP_SYSCALL: {
                /* OS syscall intrinsic. Arguments are packed like OP_CALL:
                 * temp_dest = arg_count, full list in the side table. The
                 * backend moves them into the syscall argument registers and
                 * issues the instruction; the result is in VREG_0 (rax). */
                if (!BE(c)->emit_syscall) {
                    c->had_error = true;
                    snprintf(c->error_msg, sizeof(c->error_msg),
                             "__zerocc_syscall is not supported on this target (x86-64 only for now)");
                    break;
                }
                int s_arg_temps[8] = {0};
                int s_arg_count = 0;
                const IRCallArgs *s_args = ir_call_args_find(i);
                if (s_args) {
                    for (int a = 0; a < s_args->count && a < 7; a++) {
                        if (s_args->is_fp[a] != 0) continue; /* syscall args are integer */
                        s_arg_temps[s_arg_count++] = (int)s_args->temps[a];
                    }
                }
                if (s_arg_count == 0 && instr->temp_dest > 0) {
                    /* C2b fallback (side table missing - should not happen). */
                    if (instr->temp_src1 != TEMP_NONE) s_arg_temps[s_arg_count++] = (int)instr->temp_src1;
                    if (instr->temp_dest > 1 && instr->temp_src2 != TEMP_NONE) s_arg_temps[s_arg_count++] = (int)instr->temp_src2;
                }
                BE(c)->emit_syscall(c, s_arg_count, s_arg_temps);
                break;
            }

            case OP_FMOV_IMM: {
                /* Load float literal via RIP-relative offset in rodata */
                size_t rodata_offset = (size_t)instr->imm64;
                if (BE(c)->emit_load_fp_rodata) {
                    BE(c)->emit_load_fp_rodata(c, phys_dst, rodata_offset);
                }
                break;
            }

            case OP_FLOAD:
                if (BE(c)->emit_load_fp) {
                    BE(c)->emit_load_fp(c, phys_dst, phys_src1, (int32_t)instr->imm64);
                }
                break;

            case OP_FSTORE:
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, phys_src1, phys_src2, (int32_t)instr->imm64);
                }
                break;

            case OP_FADD:
                if (BE(c)->emit_fp_add) {
                    BE(c)->emit_fp_add(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FSUB:
                if (BE(c)->emit_fp_sub) {
                    BE(c)->emit_fp_sub(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FMUL:
                if (BE(c)->emit_fp_mul) {
                    BE(c)->emit_fp_mul(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FDIV:
                if (BE(c)->emit_fp_div) {
                    BE(c)->emit_fp_div(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_FMOV:
                if (BE(c)->emit_fp_mov) {
                    BE(c)->emit_fp_mov(c, phys_dst, phys_src1);
                }
                break;

            /* =========================================================================
             * Vector Instruction Emission (SSE2/NEON/RVV)
             * ========================================================================= */

            case OP_VLOAD:
                if (c->emit_asm && BE(c)->emit_vload_asm) {
                    /* Assembly mode: emit textual assembly */
                    BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vload) {
                    /* JIT binary mode: emit machine code directly */
                    BE(c)->emit_vload(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VLOADSS:
                if (c->emit_asm && BE(c)->emit_vload_asm) {
                    BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vloadss) {
                    /* Scalar 4-byte float load (movss) */
                    BE(c)->emit_vloadss(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VLOADSD:
                if (c->emit_asm && BE(c)->emit_vload_asm) {
                    BE(c)->emit_vload_asm(c, phys_dst, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vloadsd) {
                    /* Scalar 8-byte float load (movsd) */
                    BE(c)->emit_vloadsd(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORE:
                if (c->emit_asm && BE(c)->emit_vstore_asm) {
                    /* Assembly mode: emit textual assembly: emit_vstore_asm(c, src_val, base, offset, out) */
                    BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vstore) {
                    /* JIT binary mode: emit machine code directly: emit_vstore(c, src_val, base, offset) */
                    BE(c)->emit_vstore(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORESS:
                if (c->emit_asm && BE(c)->emit_vstore_asm) {
                    BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vstoress) {
                    /* Scalar 4-byte float store (movss) */
                    BE(c)->emit_vstoress(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_VSTORESD:
                if (c->emit_asm && BE(c)->emit_vstore_asm) {
                    BE(c)->emit_vstore_asm(c, phys_src2, phys_src1, instr->imm, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vstoredsd) {
                    /* Scalar 8-byte float store (movsd) */
                    BE(c)->emit_vstoredsd(c, phys_src2, phys_src1, instr->imm);
                }
                break;

            case OP_VADDSS:
                if (c->emit_asm && BE(c)->emit_vaddss_asm) {
                    BE(c)->emit_vaddss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vaddps) {
                    /* Use VADDPS for scalar too (lower 32 bits) */
                    BE(c)->emit_vaddps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VSUBSS:
                if (c->emit_asm && BE(c)->emit_vsubss_asm) {
                    BE(c)->emit_vsubss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vsubps) {
                    BE(c)->emit_vsubps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMULSS:
                if (c->emit_asm && BE(c)->emit_vmulss_asm) {
                    BE(c)->emit_vmulss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmulps) {
                    BE(c)->emit_vmulps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VDIVSS:
                if (c->emit_asm && BE(c)->emit_vdivss_asm) {
                    BE(c)->emit_vdivss_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vdivps) {
                    BE(c)->emit_vdivps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VADDPS:
                if (c->emit_asm && BE(c)->emit_vaddps_asm) {
                    BE(c)->emit_vaddps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vaddps) {
                    BE(c)->emit_vaddps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VSUBPS:
                if (c->emit_asm && BE(c)->emit_vsubps_asm) {
                    BE(c)->emit_vsubps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vsubps) {
                    BE(c)->emit_vsubps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMULPS:
                if (c->emit_asm && BE(c)->emit_vmulps_asm) {
                    BE(c)->emit_vmulps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmulps) {
                    BE(c)->emit_vmulps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VDIVPS:
                if (c->emit_asm && BE(c)->emit_vdivps_asm) {
                    BE(c)->emit_vdivps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vdivps) {
                    BE(c)->emit_vdivps(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VADDSD:
                if (BE(c)->emit_vaddsd_asm) {
                    BE(c)->emit_vaddsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VSUBSD:
                if (BE(c)->emit_vsubsd_asm) {
                    BE(c)->emit_vsubsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VMULSD:
                if (BE(c)->emit_vmulsd_asm) {
                    BE(c)->emit_vmulsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VDIVSD:
                if (BE(c)->emit_vdivsd_asm) {
                    BE(c)->emit_vdivsd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VADDPD:
                if (c->emit_asm && BE(c)->emit_vaddpd_asm) {
                    BE(c)->emit_vaddpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vaddpd) {
                    BE(c)->emit_vaddpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VSUBPD:
                if (c->emit_asm && BE(c)->emit_vsubpd_asm) {
                    BE(c)->emit_vsubpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vsubpd) {
                    BE(c)->emit_vsubpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMULPD:
                if (c->emit_asm && BE(c)->emit_vmulpd_asm) {
                    BE(c)->emit_vmulpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmulpd) {
                    BE(c)->emit_vmulpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VDIVPD:
                if (c->emit_asm && BE(c)->emit_vdivpd_asm) {
                    BE(c)->emit_vdivpd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vdivpd) {
                    BE(c)->emit_vdivpd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VPADDD:
                if (c->emit_asm && BE(c)->emit_vpaddd_asm) {
                    BE(c)->emit_vpaddd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vpaddd) {
                    BE(c)->emit_vpaddd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VPSUBD:
                if (c->emit_asm && BE(c)->emit_vpsubd_asm) {
                    BE(c)->emit_vpsubd_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vpsubd) {
                    BE(c)->emit_vpsubd(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VPMULLD:
                if (c->emit_asm && BE(c)->emit_vpmulld_asm) {
                    BE(c)->emit_vpmulld_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vpmulld) {
                    BE(c)->emit_vpmulld(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_VMOVSS:
                if (BE(c)->emit_vmovss_asm) {
                    BE(c)->emit_vmovss_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_VMOVAPS:
                if (c->emit_asm && BE(c)->emit_vmovaps_asm) {
                    BE(c)->emit_vmovaps_asm(c, phys_dst, phys_src1, c->asm_out);
                } else if (!c->emit_asm && BE(c)->emit_vmovaps) {
                    BE(c)->emit_vmovaps(c, phys_dst, phys_src1);
                }
                break;

            case OP_VMOVAPD:
                if (BE(c)->emit_vmovapd_asm) {
                    BE(c)->emit_vmovapd_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_VANDPS:
                if (BE(c)->emit_vandps_asm) {
                    BE(c)->emit_vandps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VORPS:
                if (BE(c)->emit_vorps_asm) {
                    BE(c)->emit_vorps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VXORPS:
                if (BE(c)->emit_vxorps_asm) {
                    BE(c)->emit_vxorps_asm(c, phys_dst, phys_src1, phys_src2, c->asm_out);
                }
                break;

            case OP_VSHUFPS:
                if (BE(c)->emit_vshufps_asm) {
                    BE(c)->emit_vshufps_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, c->asm_out);
                }
                break;

            case OP_VCVT_SI2SS:
                if (BE(c)->emit_vcvt_si2ss_asm) {
                    BE(c)->emit_vcvt_si2ss_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_VCVT_SS2SI:
                if (BE(c)->emit_vcvt_ss2si_asm) {
                    BE(c)->emit_vcvt_ss2si_asm(c, phys_dst, phys_src1, c->asm_out);
                }
                break;

            case OP_VCMPSS:
                if (BE(c)->emit_vcmpss_asm) {
                    BE(c)->emit_vcmpss_asm(c, phys_dst, phys_src1, phys_src2, instr->imm, c->asm_out);
                }
                break;

            case OP_LEA_RIP: {
                /* LEA RIP-relative for string literals */
                size_t rodata_offset = (size_t)instr->imm64;
                if (BE(c)->emit_lea_rip) {
                    BE(c)->emit_lea_rip(c, phys_dst, rodata_offset);
                }
                break;
            }

            case OP_MOV_GLOBAL: {
                /* Global variable load with relocation */
                const char *symbol_name = (const char *)(intptr_t)instr->imm64;
                if (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32) {
                    /* RISC-V: auipc dst, 0; addi dst, dst, 0 */
                    int r = phys_dst & 0x1F;
                    uint32_t auipc = (uint32_t)(((uint32_t)r << 7) | 0x17u);
                    uint32_t addi  = (uint32_t)(((uint32_t)r << 15) | ((uint32_t)r << 7) | 0x13u);
                    zero_cc_emit_u32(&c->text_section, auipc);
                    zero_cc_emit_u32(&c->text_section, addi);
                } else if (c->isa == ISA_AARCH64) {
                    /* AArch64: adrp dst, 0; add dst, dst, 0 */
                    int r = phys_dst & 0x1F;
                    uint32_t adrp = 0x90000000u | (uint32_t)r;
                    uint32_t add  = 0x91000000u | ((uint32_t)r << 5) | (uint32_t)r;
                    zero_cc_emit_u32(&c->text_section, adrp);
                    zero_cc_emit_u32(&c->text_section, add);
                } else if (BE(c)->emit_mov_imm) {
                    BE(c)->emit_mov_imm(c, phys_dst, 0);
                }
                /* Store symbol name for relocation patching. x86-64 movabs is
                 * 10 bytes (8-byte reloc field at the end); RISC-V emits an
                 * auipc+addi pair; AArch64 emits an adrp+add pair (4-byte lo12 reloc
                 * field at the end, companion hi21/pcrel_hi added by ELF writer). */
                size_t reloc_field = (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32 || c->isa == ISA_AARCH64) ? 4 : 8;
                if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
                    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                    r->text_patch_offset = BE(c)->get_code_offset(c) - reloc_field;
                    snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", symbol_name);
                    r->is_call = false;
                }
                break;
            }

            case OP_JMP_LABEL: {
                /* Unconditional jump to label (for break/continue/goto) */
                BE(c)->emit_jmp(c, "");
                /* Store patch info for later resolution */
                /* Note: label resolution is handled separately in emit_return_jumps */
                break;
            }

            case OP_CALL: {
                /* Extract function name from imm64 (stored as pointer during lowering) */
                const char *fn_name = (const char *)(intptr_t)instr->imm64;

                /* Extract integer argument info packed into the instruction */
                int arg_count = instr->temp_dest;
                int arg_temps[IR_CALL_MAX_ARGS] = {0};
                int indirect_callee_temp = -1;
                int aarch64_sret_temp = -1;
                const IRCallArgs *full_args = ir_call_args_find(i);
                if (full_args) {
                    int int_c = 0;
                    for (int a = 0; a < full_args->count; a++) {
                        if (full_args->is_fp[a] == 2) {
                            indirect_callee_temp = full_args->temps[a];
                        } else if (full_args->is_fp[a] == 3) {
                            aarch64_sret_temp = full_args->temps[a];
                        } else if (full_args->is_fp[a] == 0) {
                            arg_temps[int_c++] = full_args->temps[a];
                        }
                    }
                    arg_count = int_c;
                } else {
                    /* C2b fallback (side table missing - should not happen):
                     * only args 0/1 are recoverable from the instruction. */
                    if (arg_count > 2) arg_count = 2;
                    if (arg_count > 0) arg_temps[0] = instr->temp_src1;
                    if (arg_count > 1) arg_temps[1] = instr->temp_src2;
                }

                /* If indirect call, save callee register into scratch_reg2 before argument setup */
                if (!fn_name && indirect_callee_temp >= 0) {
                    int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_callee_temp);
                    int sp_callee = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_callee_temp);
                    if (sp_callee >= 0) {
                        int32_t off = spill_base_offset + (sp_callee + 1) * 8;
                        if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, BE(c)->fp_reg, -off);
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
                        if (BE(c)->emit_load) BE(c)->emit_load(c, 8 /* x8 */, BE(c)->fp_reg, -off);
                    } else if (BE(c)->emit_mov_reg) {
                        BE(c)->emit_mov_reg(c, 8 /* x8 */, r_sret);
                    }
                }

                /* Extract fp_reg_idx (number of float args) from label_idx */
                int fp_reg_idx = (int)(instr->label_idx & 0xFF);
                if ((instr->label_idx & 0xFF00) == 0x0100) {
                    fp_reg_idx = 0;
                }

                /* Args beyond int_arg_count go on the stack:
                 * so push them in reverse order, padding to keep stack 16-byte aligned. */
                int int_arg_count = BE(c)->conv->int_arg_reg_count;
                int n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
                int stack_pad = (n_stack & 1) ? 8 : 0;
                if (n_stack > 0) {
                    if (stack_pad) BE(c)->emit_stack_adjust(c, -stack_pad);
                    for (int a = arg_count - 1; a >= int_arg_count; a--) {
                        int r = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[a]);
                        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[a]);
                        if (sp >= 0) {
                            int32_t off = spill_base_offset + (sp + 1) * 8;
                            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, BE(c)->fp_reg, -off);
                            r = BE(c)->scratch_reg1;
                        }
                        BE(c)->emit_push_reg(c, r);
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
                        if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg1, BE(c)->fp_reg, -off);
                        r = BE(c)->scratch_reg1;
                    }
                    src_regs[a] = r;
                    BE(c)->emit_push_reg(c, src_regs[a]);
                }
                for (int a = n_args - 1; a >= 0; a--) {
                    int dst_reg = backend_get_int_arg_reg(BE(c), a);
                    if (dst_reg < 0) {
                        /* Argument is stack-passed, skip register assignment */
                        continue;
                    }
                    BE(c)->emit_pop_reg(c, dst_reg);
                }

                /* Load float arguments from spill slots into the FP arg
                 * registers of the target ABI (xmm0..7 / d0..7 / fa0..7).
                 * Using the calling-convention table keeps x86/AArch64
                 * (arg regs 0..7) identical while giving RISC-V fa0..fa7. */
                if (call_idx < c->lowering.ir_ctx.call_count) {
                    int fp_count = c->lowering.ir_ctx.call_fp_spill_counts[call_idx];
                    for (int f = 0; f < fp_count && f < 8; f++) {
                        int32_t spill_off = c->lowering.ir_ctx.call_fp_spills[call_idx][f];
                        int fp_arg_reg = backend_get_fp_arg_reg(BE(c), f);
                        if (fp_arg_reg < 0) fp_arg_reg = f;
                        /* Load FP arg from stack slot into FP register */
                        if (BE(c)->emit_load_fp) {
                            BE(c)->emit_load_fp(c, fp_arg_reg, BE(c)->fp_reg, -spill_off);
                        }
                    }
                    call_idx++;
                }

                /* System V AMD64 ABI: For variadic functions, %al must hold number of SSE registers used */
                /* TODO: Check if function is variadic - for now emit for all calls */
                if (BE(c)->emit_variadic_al_setup) {
                    BE(c)->emit_variadic_al_setup(c, fp_reg_idx);
                }

                /* Caller-saved register fix for AArch64:
                 * x0-x18 are caller-saved. The callee may clobber these registers.
                 * We need to save/restore any caller-saved registers that contain
                 * live values across the call.
                 * For now, always save x3-x7 BEFORE the call and restore after.
                 * TODO: Proper fix is to mark intervals as spanning calls in register allocator.
                 */
                if (c->isa == ISA_AARCH64) {
                    /* Save x3-x7 before the call */
                    for (int r = 3; r <= 7; r++) {
                        int spill = regalloc_get_spill_slot_for_temp(&c->regalloc, r);
                        if (spill >= 0) {
                            /* Register r has a spill slot - save it */
                            int32_t off = spill_base_offset + (spill + 1) * 8;
                            if (BE(c)->emit_store) BE(c)->emit_store(c, r, BE(c)->fp_reg, -off);
                        }
                    }
                }

                /* Emit the call */
                if (!fn_name && indirect_callee_temp >= 0 && BE(c)->emit_call_reg) {
                    BE(c)->emit_call_reg(c, BE(c)->scratch_reg2 /* r11 */);
                } else {
                    BE(c)->emit_call(c, fn_name);
                }

                /* Restore x3-x7 after the call */
                if (c->isa == ISA_AARCH64) {
                    for (int r = 3; r <= 7; r++) {
                        int spill = regalloc_get_spill_slot_for_temp(&c->regalloc, r);
                        if (spill >= 0) {
                            /* Register r has a spill slot - restore it */
                            int32_t off = spill_base_offset + (spill + 1) * 8;
                            if (BE(c)->emit_load) BE(c)->emit_load(c, r, BE(c)->fp_reg, -off);
                        }
                    }
                }

                /* Caller cleans up stack-passed arguments (and alignment pad) */
                if (n_stack > 0) {
                    BE(c)->emit_stack_adjust(c, n_stack * 8 + stack_pad);
                }

                /* Handle struct return (9-16 bytes) unpacking */
                if ((instr->label_idx & 0xFF00) == 0x0100) {
                    int struct_size = instr->label_idx & 0xFF;
                    if (struct_size > 8 && struct_size <= 16) {
                        /* Physical rdx contains upper 8 bytes */
                        /* This is handled by caller store logic */
                    }
                }
                break;
            }

            case OP_NOT:
                BE(c)->emit_not(c, phys_dst, phys_src1);
                break;

            case OP_LNOT:  /* Logical NOT: cmp_zero src; cond_set dst, EQ */
                BE(c)->emit_cmp_zero(c, phys_src1);
                if (BE(c)->emit_cond_set) {
                    BE(c)->emit_cond_set(c, phys_dst, CC_EQ);
                }
                break;

            case OP_LABEL:
                if (instr->label_idx < label_count) {
                    size_t label_idx = instr->label_idx;
                    label_defined[label_idx] = true;
                    label_offsets[label_idx] = BE(c)->get_code_offset(c);
                    for (size_t j = 0; j < label_patch_counts[label_idx]; j++) {
                        BE(c)->patch_jmp(c, label_patches[label_idx][j], label_offsets[label_idx]);
                    }
                    label_patch_counts[label_idx] = 0;
                }
                break;

            case OP_JMP:
            case OP_JE:
            case OP_JNE:
            case OP_JL:
            case OP_JLE:
            case OP_JG:
            case OP_JCOND: {
                if (instr->op == OP_JMP) {
                    /* Fallthrough optimization: skip jump if next active instruction is target label */
                    size_t next_idx = i + 1;
                    while (next_idx < count && (instrs[next_idx].op == OP_DEAD || instrs[next_idx].op == OP_NOP)) {
                        next_idx++;
                    }
                    if (next_idx < count && instrs[next_idx].op == OP_LABEL && instrs[next_idx].label_idx == instr->label_idx) {
                        break;
                    }
                    /* Option 2: Inline epilogue on early returns when no static-local mirror or CSR reload is required */
                    if (instr->label_idx == c->lowering.epilogue_label &&
                        c->lowering.static_local_rt_off == 0 &&
                        c->regalloc.used_callee_saved_gpr_mask == 0 &&
                        BE(c)->emit_leave && BE(c)->emit_ret) {
                        BE(c)->emit_leave(c);
                        BE(c)->emit_ret(c);
                        break;
                    }
                }
                size_t patch_pos = 0;
                switch (instr->op) {
                    case OP_JMP: patch_pos = BE(c)->emit_jmp(c, ""); break;
                    case OP_JE:
                        BE(c)->emit_test(c, phys_src1, phys_src1);
                        patch_pos = BE(c)->emit_jeq(c, "");
                        break;
                    case OP_JNE:
                        BE(c)->emit_test(c, phys_src1, phys_src1);
                        patch_pos = BE(c)->emit_jne(c, "");
                        break;
                    case OP_JL:  patch_pos = BE(c)->emit_jlt(c, ""); break;
                    case OP_JLE: patch_pos = BE(c)->emit_jle(c, ""); break;
                    case OP_JG:  patch_pos = BE(c)->emit_jgt(c, ""); break;
                    case OP_JCOND:
                        patch_pos = BE(c)->emit_jcond(c, (int)instr->temp_src1, "");
                        break;
                    default: break;
                }
                if (instr->label_idx < label_count) {
                    size_t label_idx = instr->label_idx;
                    if (label_defined[label_idx]) {
                        BE(c)->patch_jmp(c, patch_pos, label_offsets[label_idx]);
                    } else {
                        if (label_patch_counts[label_idx] >= label_patch_caps[label_idx]) {
                            size_t new_cap = label_patch_caps[label_idx] ? label_patch_caps[label_idx] * 2 : 4;
                            size_t *new_patches = (size_t *)realloc(label_patches[label_idx], new_cap * sizeof(size_t));
                            if (new_patches) {
                                label_patches[label_idx] = new_patches;
                                label_patch_caps[label_idx] = new_cap;
                            }
                        }
                        if (label_patches[label_idx] && label_patch_counts[label_idx] < label_patch_caps[label_idx]) {
                            label_patches[label_idx][label_patch_counts[label_idx]++] = patch_pos;
                        }
                    }
                }
                break;
            }

            case OP_CALL_INDIRECT:
                /* Call through register pointer */
                if (BE(c)->emit_call_reg) {
                    BE(c)->emit_call_reg(c, phys_src1);
                }
                break;

            case OP_PUSH:
                /* Push register to stack */
                if (BE(c)->emit_push_reg) {
                    BE(c)->emit_push_reg(c, phys_src1);
                }
                break;

            case OP_POP:
                /* Pop register from stack */
                if (BE(c)->emit_pop_reg) {
                    BE(c)->emit_pop_reg(c, phys_dst);
                }
                break;

            case OP_ADD_IMM:
                /* Add immediate to register */
                if (BE(c)->emit_add_imm) {
                    BE(c)->emit_add_imm(c, phys_dst, (int16_t)instr->imm);
                }
                break;

            case OP_SUB_IMM:
                /* Subtract immediate from register */
                if (BE(c)->emit_sub_imm) {
                    BE(c)->emit_sub_imm(c, phys_dst, (int16_t)instr->imm);
                }
                break;

            case OP_INC:
                /* Increment register */
                if (BE(c)->emit_inc) {
                    BE(c)->emit_inc(c, phys_dst);
                }
                break;

            case OP_DEC:
                /* Decrement register */
                if (BE(c)->emit_dec) {
                    BE(c)->emit_dec(c, phys_dst);
                }
                break;

            case OP_SAR:
                /* Arithmetic shift right */
                if (BE(c)->emit_sar) {
                    BE(c)->emit_sar(c, phys_dst, phys_src1, phys_src2);
                }
                break;

            case OP_RET:
                /* Return from function - already handled by emit_leave/emit_ret after loop */
                break;

            case OP_EPILOGUE:
                /* Function epilogue - already handled by emit_leave/emit_ret after loop */
                break;

            case OP_STACK_ALIGN:
                /* Align stack pointer */
                if (BE(c)->emit_stack_align) {
                    BE(c)->emit_stack_align(c, (uint8_t)instr->imm);
                }
                break;

            default:
                break;
        }

        /* C99 width semantics: if the op produced a narrow-int result,
         * extend it to full 64-bit width now (sign or zero per the mark),
         * before any spill-back so the stored value is the extended one. */
        if (instr->width != 0 && instr->temp_dest!= TEMP_NONE && BE(c)->emit_extend) {
            BE(c)->emit_extend(c, phys_dst, phys_dst,
                               (int)(instr->width & 7), (instr->width < 8) ? 1 : 0);
        }

        /* If destination temp was spilled, store the scratch register back
         * to the frame spill slot (FP scratch + movsd for FP-class). */
        if (spill_dst >= 0) {
            int32_t slot_off = spill_base_offset + (spill_dst + 1) * 8;
            if (regalloc_get_class_for_temp(&c->regalloc, instr->temp_dest) == REG_CLASS_XMM) {
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, BE(c)->fp_scratch_reg1, BE(c)->fp_reg, -slot_off);
                }
            } else if (BE(c)->emit_store) {
                BE(c)->emit_store(c, BE(c)->scratch_reg1, BE(c)->fp_reg, -slot_off);
            }
        }
    }

    for (size_t i = 0; i < label_count; i++) {
        free(label_patches ? label_patches[i] : NULL);
    }
    free(label_defined);
    free(label_offsets);
    free(label_patches);
    free(label_patch_counts);
    free(label_patch_caps);
}


