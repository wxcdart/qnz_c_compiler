/* AST Lowering - IR to Machine Code Emission */

#include "ast_lowering_internal.h"

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

        /* Frame pointer (0xFF) should NOT be allocated - use actual FP register */
        int phys_dst = (instr->temp_dest == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
        int phys_src1 = (instr->temp_src1 == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
        int phys_src2 = (instr->temp_src2 == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src2);

        /* Check for spilled temps that need reloading / storing */
        int spill_src1 = (instr->op == OP_CALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src1);
        int spill_src2 = (instr->op == OP_CALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_src2);
        int spill_dst = (instr->op == OP_CALL) ? -1 : regalloc_get_spill_slot_for_temp(&c->regalloc, instr->temp_dest);

        /* Reload spilled sources into scratch registers (r10 for src1, r11 for src2) */
        if (spill_src1 >= 0) {
            int32_t slot_off = spill_base_offset + (spill_src1 + 1) * 8;
            if (BE(c)->emit_load) {
                BE(c)->emit_load(c, 10 /* r10 */, BE(c)->fp_reg, -slot_off);
            }
            phys_src1 = 10;
        }
        if (spill_src2 >= 0) {
            int32_t slot_off = spill_base_offset + (spill_src2 + 1) * 8;
            if (BE(c)->emit_load) {
                BE(c)->emit_load(c, 11 /* r11 */, BE(c)->fp_reg, -slot_off);
            }
            phys_src2 = 11;
        }
        if (spill_dst >= 0) {
            phys_dst = 10;
        }

        switch (instr->op) {
            case OP_PROLOGUE:
                /* Pass callee_save_mask to emit_prologue so it can emit callee-save stores */
                if (BE(c)->emit_prologue) {
                    BE(c)->emit_prologue(c, (size_t)instr->imm64, c->regalloc.used_callee_saved_gpr_mask);
                }
                break;

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

            case OP_VARIADIC_SPILL:
                if (BE(c)->emit_variadic_spill) {
                    BE(c)->emit_variadic_spill(c, (int)instr->imm, (int32_t)instr->imm64);
                }
                break;

            case OP_MOV_IMM:
                BE(c)->emit_mov_imm(c, phys_dst, instr->imm64);
                break;
            
            case OP_MOV_MEM:
                BE(c)->emit_store(c, phys_src1, phys_src2, instr->imm);
                break;
            
            case OP_MOV:
                BE(c)->emit_load(c, phys_dst, phys_src1, instr->imm);
                break;
            
            case OP_MOV_REG:
                BE(c)->emit_mov_reg(c, phys_dst, phys_src1);
                break;
            
            case OP_LEA:
                BE(c)->emit_lea(c, phys_dst, phys_src1, instr->imm);
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
            
            case OP_CMP:
                BE(c)->emit_cmp(c, phys_src1, phys_src2);
                break;
            
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

            case OP_ATOMIC_XADD:
                if (BE(c)->emit_atomic_xadd) {
                    BE(c)->emit_atomic_xadd(c, phys_dst, phys_src1, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;

            case OP_ATOMIC_CMPXCHG: {
                int phys_expected = (instr->label_idx == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
                if (BE(c)->emit_atomic_cmpxchg) {
                    BE(c)->emit_atomic_cmpxchg(c, phys_dst, phys_src1, phys_expected, phys_src2, instr->imm ? instr->imm : 4);
                }
                break;
            }

            case OP_ATOMIC_CMPXCHG_BOOL: {
                int phys_expected = (instr->label_idx == 0xFF) ? BE(c)->fp_reg : regalloc_get_reg_for_temp(&c->regalloc, instr->label_idx);
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
                    BE(c)->emit_load_fp(c, phys_dst, phys_src1, instr->imm);
                }
                break;

            case OP_FSTORE:
                if (BE(c)->emit_store_fp) {
                    BE(c)->emit_store_fp(c, phys_src1, phys_src2, instr->imm);
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
                if (BE(c)->emit_mov_imm) {
                    BE(c)->emit_mov_imm(c, phys_dst, 0);
                }
                /* Store symbol name for relocation patching */
                if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
                    ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                    r->text_patch_offset = BE(c)->get_code_offset(c) - 8;
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
                const IRCallArgs *full_args = ir_call_args_find(i);
                if (full_args) {
                    arg_count = full_args->count;
                    for (int a = 0; a < arg_count; a++) arg_temps[a] = full_args->temps[a];
                } else {
                    if (arg_count > 4) arg_count = 4;
                    if (arg_count > 0) arg_temps[0] = instr->temp_src1;
                    if (arg_count > 1) arg_temps[1] = instr->temp_src2;
                    if (arg_count > 2) {
                        /* Unpack remaining temps from imm (16-bit) */
                        arg_temps[2] = (int)((instr->imm >> 0) & 0xFF);
                        if (arg_count > 3) arg_temps[3] = (int)((instr->imm >> 8) & 0xFF);
                    }
                }
                
                /* Extract fp_reg_idx (number of float args) from label_idx */
                int fp_reg_idx = (int)instr->label_idx;
                
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
                            if (BE(c)->emit_load) BE(c)->emit_load(c, 10 /* r10 */, BE(c)->fp_reg, -off);
                            r = 10;
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
                        if (BE(c)->emit_load) BE(c)->emit_load(c, 10 /* r10 */, BE(c)->fp_reg, -off);
                        r = 10;
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
                
                /* Load float arguments from spill slots into xmm0..xmmN */
                if (call_idx < c->lowering.ir_ctx.call_count) {
                    int fp_count = c->lowering.ir_ctx.call_fp_spill_counts[call_idx];
                    for (int f = 0; f < fp_count && f < 8; f++) {
                        int32_t spill_off = c->lowering.ir_ctx.call_fp_spills[call_idx][f];
                        /* Load FP arg from stack slot into FP register */
                        if (BE(c)->emit_load_fp) {
                            BE(c)->emit_load_fp(c, f, BE(c)->fp_reg, -spill_off);
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
                BE(c)->emit_call(c, fn_name);
                
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
                    BE(c)->emit_call_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1));
                }
                break;
            
            case OP_PUSH:
                /* Push register to stack */
                if (BE(c)->emit_push_reg) {
                    BE(c)->emit_push_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1));
                }
                break;
            
            case OP_POP:
                /* Pop register from stack */
                if (BE(c)->emit_pop_reg) {
                    BE(c)->emit_pop_reg(c, regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest));
                }
                break;
            
            case OP_ADD_IMM:
                /* Add immediate to register */
                phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
                if (BE(c)->emit_add_imm) {
                    BE(c)->emit_add_imm(c, phys_dst, (int16_t)instr->imm);
                }
                break;
            
            case OP_SUB_IMM:
                /* Subtract immediate from register */
                phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
                if (BE(c)->emit_sub_imm) {
                    BE(c)->emit_sub_imm(c, phys_dst, (int16_t)instr->imm);
                }
                break;
            
            case OP_INC:
                /* Increment register */
                phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
                if (BE(c)->emit_inc) {
                    BE(c)->emit_inc(c, phys_dst);
                }
                break;
            
            case OP_DEC:
                /* Decrement register */
                phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
                if (BE(c)->emit_dec) {
                    BE(c)->emit_dec(c, phys_dst);
                }
                break;
            
            case OP_SAR:
                /* Arithmetic shift right */
                phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
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

        /* If destination temp was spilled, store scratch register (r10) back to frame spill slot */
        if (spill_dst >= 0) {
            int32_t slot_off = spill_base_offset + (spill_dst + 1) * 8;
            if (BE(c)->emit_store) {
                BE(c)->emit_store(c, 10 /* r10 */, BE(c)->fp_reg, -slot_off);
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


