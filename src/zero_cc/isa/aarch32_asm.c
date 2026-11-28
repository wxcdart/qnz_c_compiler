/*
 * AArch32 Assembly Emitter (ARM and Thumb-2 unified syntax, Sprint 275)
 * Included directly into src/zero_cc/isa/aarch32.c
 */

static const char *aarch32_get_hw_reg_name(int phys_reg);
static void aarch32_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out);
static void aarch32_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
static void aarch32_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out);
static void aarch32_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);

static void aarch32_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_isr = cur_fn && cur_fn->is_interrupt;
    if (c->lowering.is_frameless && !is_isr) return;
    if (is_isr) {
        fprintf(out, "\tpush\t{r0, r1, r2, r3, r12}\n");
        if (c->lowering.is_frameless) return;
    }
    if (c->lowering.frame_align > 16) {
        size_t frame = (size_t)c->lowering.frame_delta;
        uint32_t N = (uint32_t)c->lowering.frame_align;
        fprintf(out, "\tpush\t{r11, lr}\n");
        fprintf(out, "\tadd\tr11, sp, #0\n");
        uint32_t enc = arm32_encode_imm((int64_t)(N - 1));
        if (enc != 0xFFFFFFFFu) {
            fprintf(out, "\tbic\tsp, sp, #%u\n", N - 1);
        } else {
            uint32_t mask = (uint32_t)(~((int64_t)N - 1));
            fprintf(out, "\tmovw\tr12, #%u\n", mask & 0xFFFFu);
            fprintf(out, "\tmovt\tr12, #%u\n", (mask >> 16) & 0xFFFFu);
            fprintf(out, "\tand\tsp, sp, r12\n");
        }
        if (frame > 0) {
            if (arm32_encode_imm((int64_t)frame) != 0xFFFFFFFFu) {
                fprintf(out, "\tsub\tsp, sp, #%zu\n", frame);
            } else {
                aarch32_emit_mov_imm_asm(c, 12, (int64_t)frame, out);
                fprintf(out, "\tsub\tsp, sp, r12\n");
            }
        }
    } else {
        size_t sz = (stack_size + 7) & ~7;
        fprintf(out, "\tpush\t{r11, lr}\n");
        if (c->is_thumb) {
            fprintf(out, "\tmov\tr11, sp\n");
        } else {
            fprintf(out, "\tadd\tr11, sp, #0\n");
        }
        if (stack_size > 0) {
            if (c->is_thumb) {
                if (sz <= 508 && (sz & 3) == 0) {
                    fprintf(out, "\tsub\tsp, sp, #%zu\n", sz);
                } else {
                    aarch32_emit_mov_imm_asm(c, 12, (int64_t)sz, out);
                    fprintf(out, "\tsub\tsp, sp, r12\n");
                }
            } else {
                if (arm32_encode_imm((int64_t)sz) != 0xFFFFFFFFu) {
                    fprintf(out, "\tsub\tsp, sp, #%zu\n", sz);
                } else {
                    aarch32_emit_mov_imm_asm(c, 12, (int64_t)sz, out);
                    fprintf(out, "\tsub\tsp, sp, r12\n");
                }
            }
        }
    }
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    if (mask && BE(c)->conv && BE(c)->conv->callee_saved_gpr) {
        const int *callee_saved = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int reg = callee_saved[i];
            if (reg == 11) continue;
            if (!((mask >> reg) & 1u)) continue;
            int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
            aarch32_emit_store_asm(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot), out);
        }
    }
}
static void aarch32_emit_epilogue_asm(ZeroCCCompiler *c) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_isr = cur_fn && cur_fn->is_interrupt;
    if (c->lowering.is_frameless && !is_isr) {
        fprintf(out, "\tmov\tpc, lr\n");
        return;
    }
    if (!c->lowering.is_frameless) {
        uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
        if (mask && BE(c)->conv && BE(c)->conv->callee_saved_gpr) {
            const int *callee_saved = BE(c)->conv->callee_saved_gpr;
            const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
            int count = BE(c)->conv->callee_saved_gpr_count;
            for (int i = 0; i < count; i++) {
                int reg = callee_saved[i];
                if (reg == 11) continue;
                if (!((mask >> reg) & 1u)) continue;
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                aarch32_emit_load_asm(c, reg, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -slot), out);
            }
        }
        fprintf(out, "\tmov\tsp, r11\n");
        fprintf(out, "\tpop\t{r11, lr}\n");
    }
    if (is_isr) {
        fprintf(out, "\tpop\t{r0, r1, r2, r3, r12}\n");
        if (c->is_thumb) {
            fprintf(out, "\tbx\tlr\n");
        } else if (cur_fn->interrupt_mode <= 1) {
            fprintf(out, "\tsubs\tpc, lr, #4\n");
        } else {
            fprintf(out, "\tmovs\tpc, lr\n");
        }
        return;
    }
    if (c->is_thumb) {
        fprintf(out, "\tbx\tlr\n");
    } else {
        fprintf(out, "\tmov\tpc, lr\n");
    }
}
static void aarch32_emit_stack_guard_init_asm(ZeroCCCompiler *c, int32_t slot_off, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\tr12, =__stack_chk_guard\n");
    fprintf(out, "\tstr\tr12, [r11, #-%d]\n", slot_off);
}
static void aarch32_emit_stack_guard_check_asm(ZeroCCCompiler *c, int32_t slot_off, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\tr12, [r11, #-%d]\n", slot_off);
    fprintf(out, "\tldr\tr12, [r12]\n");
    fprintf(out, "\tcmp\tr12, r12\n");
    fprintf(out, "\tbne\t__stack_chk_fail\n");
}
static const char *aarch32_get_hw_reg_name(int phys_reg) {
    static const char *names[16] = {"r0","r1","r2","r3","r4","r5","r6","r7",
                                     "r8","r9","r10","r11","r12","sp","lr","pc"};
    if (phys_reg >= 0 && phys_reg < 16) return names[phys_reg];
    return "??";
}
static const char *aarch32_get_vec_reg_name(int vec_reg_idx) {
    static const char *names[16] = {"q0","q1","q2","q3","q4","q5","q6","q7",
                                     "q8","q9","q10","q11","q12","q13","q14","q15"};
    if (vec_reg_idx >= 0 && vec_reg_idx < 16) return names[vec_reg_idx];
    return "??";
}

/* Basic asm emitters for Phase 1 */
static void aarch32_emit_mov_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    (void)c;
    const char *rname = aarch32_get_hw_reg_name(dst);
    uint32_t v = (uint32_t)imm;
    if (arm32_encode_imm(imm) != 0xFFFFFFFFu) {
        fprintf(out, "\tmov\t%s, #%lld\n", rname, (long long)(int32_t)v);
    } else if (imm < 0 && arm32_encode_imm(~imm) != 0xFFFFFFFFu) {
        fprintf(out, "\tmvn\t%s, #%lld\n", rname, (long long)(uint32_t)(~imm));
    } else {
        uint32_t lo16 = v & 0xFFFFu;
        uint32_t hi16 = (v >> 16) & 0xFFFFu;
        fprintf(out, "\tmovw\t%s, #%u\n", rname, lo16);
        if (hi16 != 0) {
            fprintf(out, "\tmovt\t%s, #%u\n", rname, hi16);
        }
    }
}
static void aarch32_emit_mov_reg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    if (dst != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_load_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    int max_neg = c->is_thumb ? -255 : -4095;
    if (offset >= max_neg && offset <= 4095) {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_mov_imm_asm(c, scratch, offset, out);
        fprintf(out, "\tldr\t%s, [%s, %s]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_store_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    int max_neg = c->is_thumb ? -255 : -4095;
    if (offset >= max_neg && offset <= 4095) {
        fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12 && src != 12) ? 12 : ((base != 9 && src != 9) ? 9 : 14);
        aarch32_emit_mov_imm_asm(c, scratch, offset, out);
        fprintf(out, "\tstr\t%s, [%s, %s]\n", aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(base), aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tadd\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tsub\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tmul\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    fprintf(out, "\t%s\t%s, %s, %s\n",
            c->lowering.div_is_unsigned ? "udiv" : "sdiv",
            aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_mod_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    int t = (src1 != 12 && src2 != 12) ? 12 : ((src1 != 9 && src2 != 9) ? 9 : 14);
    aarch32_emit_div_asm(c, t, src1, src2, out);
    aarch32_emit_mul_asm(c, t, t, src2, out);
    aarch32_emit_sub_asm(c, dst, src1, t, out);
}

/* 64-bit multiply/divide ASM emitters */
static void aarch32_emit_mul64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    (void)c;
    fprintf(out, "\tmul\tlr, %s, %s\n", aarch32_get_hw_reg_name(a_lo), aarch32_get_hw_reg_name(b_hi));
    fprintf(out, "\tmla\tlr, %s, %s, lr\n", aarch32_get_hw_reg_name(a_hi), aarch32_get_hw_reg_name(b_lo));
    fprintf(out, "\tumull\t%s, %s, %s, %s\n",
            aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(dhi),
            aarch32_get_hw_reg_name(a_lo), aarch32_get_hw_reg_name(b_lo));
    fprintf(out, "\tadd\t%s, %s, lr\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(dhi));
}

static void aarch32_emit_64div_call_asm(ZeroCCCompiler *c, const char *fn,
                                        int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(b_hi));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(b_lo));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(a_hi));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(a_lo));
    fprintf(out, "\tldr\tr0, [sp], #4\n");
    fprintf(out, "\tldr\tr1, [sp], #4\n");
    fprintf(out, "\tldr\tr2, [sp], #4\n");
    fprintf(out, "\tldr\tr3, [sp], #4\n");
    fprintf(out, "\tbl\t%s\n", fn);
    if (dlo == 1) {
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
    } else {
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
    }
}

static void aarch32_emit_div64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_sdiv64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}
static void aarch32_emit_mod64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_smod64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}
static void aarch32_emit_udiv64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_udiv64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}
static void aarch32_emit_umod64_asm(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    aarch32_emit_64div_call_asm(c, "zc_umod64", dlo, dhi, a_lo, a_hi, b_lo, b_hi, out);
}

static void aarch32_emit_64shift_call_asm(ZeroCCCompiler *c, const char *fn,
                                          int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(cnt));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(shi));
    fprintf(out, "\tstr\t%s, [sp, #-4]!\n", aarch32_get_hw_reg_name(slo));
    fprintf(out, "\tldr\tr0, [sp], #4\n");
    fprintf(out, "\tldr\tr1, [sp], #4\n");
    fprintf(out, "\tldr\tr2, [sp], #4\n");
    fprintf(out, "\tbl\t%s\n", fn);
    if (dlo == 1) {
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
    } else {
        if (dlo != 0) fprintf(out, "\tmov\t%s, r0\n", aarch32_get_hw_reg_name(dlo));
        if (dhi != 1) fprintf(out, "\tmov\t%s, r1\n", aarch32_get_hw_reg_name(dhi));
    }
}

static void aarch32_emit_shl64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    aarch32_emit_64shift_call_asm(c, "zc_shl64", dlo, dhi, slo, shi, cnt, out);
}
static void aarch32_emit_shr64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    aarch32_emit_64shift_call_asm(c, "zc_shr64", dlo, dhi, slo, shi, cnt, out);
}
static void aarch32_emit_sar64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, int cnt, FILE *out) {
    aarch32_emit_64shift_call_asm(c, "zc_sar64", dlo, dhi, slo, shi, cnt, out);
}

static void aarch32_emit_add64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\tadds\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\tadc\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_sub64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\tsubs\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\tsbc\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_and64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\tand\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\tand\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_or64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\torr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\torr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_xor64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo1, int shi1, int slo2, int shi2, FILE *out) {
    (void)c;
    fprintf(out, "\teor\t%s, %s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo1), aarch32_get_hw_reg_name(slo2));
    fprintf(out, "\teor\t%s, %s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi1), aarch32_get_hw_reg_name(shi2));
}
static void aarch32_emit_not64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    (void)c;
    fprintf(out, "\tmvn\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
    fprintf(out, "\tmvn\t%s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
}
static void aarch32_emit_neg64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    (void)c;
    fprintf(out, "\trsbs\t%s, %s, #0\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
    fprintf(out, "\trsc\t%s, %s, #0\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
}
static void aarch32_emit_cmp64_asm(ZeroCCCompiler *c, int a_lo, int a_hi, int b_lo, int b_hi, FILE *out) {
    (void)out;
    c->lowering.cmp_is_64 = 1;
    c->lowering.cmp64_s1_lo = a_lo; c->lowering.cmp64_s1_hi = a_hi;
    c->lowering.cmp64_s2_lo = b_lo; c->lowering.cmp64_s2_hi = b_hi;
}
static void aarch32_emit_mov64_asm(ZeroCCCompiler *c, int dlo, int dhi, int slo, int shi, FILE *out) {
    (void)c;
    if (dlo == shi) {
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
    } else {
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(slo));
        fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(shi));
    }
}
static void aarch32_emit_load64_asm(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset, FILE *out) {
    (void)c;
    if (dlo == base) {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(base), offset + 4);
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(base), offset);
    } else {
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(base), offset);
        fprintf(out, "\tldr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(base), offset + 4);
    }
}
static void aarch32_emit_store64_asm(ZeroCCCompiler *c, int slo, int shi, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(slo), aarch32_get_hw_reg_name(base), offset);
    fprintf(out, "\tstr\t%s, [%s, #%d]\n", aarch32_get_hw_reg_name(shi), aarch32_get_hw_reg_name(base), offset + 4);
}
static void aarch32_emit_zext32_64_asm(ZeroCCCompiler *c, int dlo, int dhi, int src, FILE *out) {
    (void)c;
    if (dlo != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(src));
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dhi));
}
static void aarch32_emit_sext32_64_asm(ZeroCCCompiler *c, int dlo, int dhi, int src, FILE *out) {
    (void)c;
    if (dlo != src) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dlo), aarch32_get_hw_reg_name(src));
    fprintf(out, "\tasr\t%s, %s, #31\n", aarch32_get_hw_reg_name(dhi), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_trunc64_32_asm(ZeroCCCompiler *c, int dst, int slo, int shi, FILE *out) {
    (void)c; (void)shi;
    if (dst != slo) fprintf(out, "\tmov\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(slo));
}
static void aarch32_emit_mov64_imm_asm(ZeroCCCompiler *c, int dlo, int dhi, int64_t val, FILE *out) {
    (void)c;
    uint32_t vlo = (uint32_t)val;
    uint32_t vhi = (uint32_t)(val >> 32);
    fprintf(out, "\tmovw\t%s, #%u\n", aarch32_get_hw_reg_name(dlo), vlo & 0xFFFF);
    if (vlo >> 16) fprintf(out, "\tmovt\t%s, #%u\n", aarch32_get_hw_reg_name(dlo), vlo >> 16);
    fprintf(out, "\tmovw\t%s, #%u\n", aarch32_get_hw_reg_name(dhi), vhi & 0xFFFF);
    if (vhi >> 16) fprintf(out, "\tmovt\t%s, #%u\n", aarch32_get_hw_reg_name(dhi), vhi >> 16);
}

static void aarch32_emit_and_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tand\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_or_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\torr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_xor_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\teor\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_shl_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    (void)c;
    fprintf(out, "\tlsl\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(shift));
}
static void aarch32_emit_shr_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    (void)c;
    fprintf(out, "\tlsr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(shift));
}
static void aarch32_emit_sar_asm(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out) {
    (void)c;
    fprintf(out, "\tasr\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src), aarch32_get_hw_reg_name(shift));
}
static void aarch32_emit_extend_asm(ZeroCCCompiler *c, int dst, int src, int width, int sign, FILE *out) {
    (void)c;
    if (width == 1 && sign) fprintf(out, "\tsxtb\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
    else if (width == 1) fprintf(out, "\tuxtb\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
    else if (width == 2 && sign) fprintf(out, "\tsxth\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
    else if (width == 2) fprintf(out, "\tuxth\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(src1), aarch32_get_hw_reg_name(src2));
}
static void aarch32_emit_64_pred_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    int alo = c->lowering.cmp64_s1_lo, ahi = c->lowering.cmp64_s1_hi;
    int blo = c->lowering.cmp64_s2_lo, bhi = c->lowering.cmp64_s2_hi;

    if (cond == 4) { /* CC_EQ */
        fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(ahi), aarch32_get_hw_reg_name(bhi));
        if (c->is_thumb) fprintf(out, "\tit\teq\n");
        fprintf(out, "\tcmpeq\t%s, %s\n", aarch32_get_hw_reg_name(alo), aarch32_get_hw_reg_name(blo));
        fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
        if (c->is_thumb) fprintf(out, "\tit\teq\n");
        fprintf(out, "\tmoveq\t%s, #1\n", aarch32_get_hw_reg_name(dst));
        return;
    }
    if (cond == 5) { /* CC_NE */
        fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(ahi), aarch32_get_hw_reg_name(bhi));
        if (c->is_thumb) fprintf(out, "\tit\teq\n");
        fprintf(out, "\tcmpeq\t%s, %s\n", aarch32_get_hw_reg_name(alo), aarch32_get_hw_reg_name(blo));
        fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
        if (c->is_thumb) fprintf(out, "\tit\tne\n");
        fprintf(out, "\tmovne\t%s, #1\n", aarch32_get_hw_reg_name(dst));
        return;
    }
    if (cond >= 6 && cond <= 9) { /* Unsigned */
        fprintf(out, "\tcmp\t%s, %s\n", aarch32_get_hw_reg_name(ahi), aarch32_get_hw_reg_name(bhi));
        if (c->is_thumb) fprintf(out, "\tit\teq\n");
        fprintf(out, "\tcmpeq\t%s, %s\n", aarch32_get_hw_reg_name(alo), aarch32_get_hw_reg_name(blo));
        fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
        const char *cc_str = (cond == 6) ? "cc" : (cond == 7) ? "ls" : (cond == 8) ? "hi" : "cs";
        if (c->is_thumb) fprintf(out, "\tit\t%s\n", cc_str);
        fprintf(out, "\tmov%s\t%s, #1\n", cc_str, aarch32_get_hw_reg_name(dst));
        return;
    }
    int s_alo = alo, s_ahi = ahi, s_blo = blo, s_bhi = bhi;
    const char *check_cc = "lt";
    if (cond == 0) {
        check_cc = "lt";
    } else if (cond == 3) {
        check_cc = "ge";
    } else if (cond == 2) {
        s_alo = blo; s_ahi = bhi; s_blo = alo; s_bhi = ahi;
        check_cc = "lt";
    } else if (cond == 1) {
        s_alo = blo; s_ahi = bhi; s_blo = alo; s_bhi = ahi;
        check_cc = "ge";
    }
    fprintf(out, "\tsubs\tr12, %s, %s\n", aarch32_get_hw_reg_name(s_alo), aarch32_get_hw_reg_name(s_blo));
    fprintf(out, "\tsbcs\tr12, %s, %s\n", aarch32_get_hw_reg_name(s_ahi), aarch32_get_hw_reg_name(s_bhi));
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
    if (c->is_thumb) fprintf(out, "\tit\t%s\n", check_cc);
    fprintf(out, "\tmov%s\t%s, #1\n", check_cc, aarch32_get_hw_reg_name(dst));
}

static void aarch32_emit_cond_set_asm(ZeroCCCompiler *c, int dst, int cond, FILE *out) {
    if (c->lowering.cmp_is_64) {
        c->lowering.cmp_is_64 = 0;
        aarch32_emit_64_pred_asm(c, dst, cond, out);
        return;
    }
    static const char *arm_cc_str[14] = {
        "lt", "le", "gt", "ge", "eq", "ne",
        "cc", "ls", "hi", "cs", "eq", "ne",
        "vs", "vc"
    };
    const char *cc = (cond >= 0 && cond < 14) ? arm_cc_str[cond] : "eq";
    fprintf(out, "\tmov\t%s, #0\n", aarch32_get_hw_reg_name(dst));
    if (c->is_thumb) fprintf(out, "\tit\t%s\n", cc);
    fprintf(out, "\tmov%s\t%s, #1\n", cc, aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_neg_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\trsb\t%s, %s, #0\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_add_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    (void)c;
    fprintf(out, "\tadd\t%s, %s, #%ld\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst), imm);
}
static void aarch32_emit_sub_imm_asm(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out) {
    (void)c;
    fprintf(out, "\tsub\t%s, %s, #%ld\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst), imm);
}
static void aarch32_emit_inc_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    (void)c;
    fprintf(out, "\tadd\t%s, %s, #1\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_dec_asm(ZeroCCCompiler *c, int dst, FILE *out) {
    (void)c;
    fprintf(out, "\tsub\t%s, %s, #1\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_not_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tmvn\t%s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_lea_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    if (offset >= 0 && arm32_encode_imm((int64_t)offset) != 0xFFFFFFFFu) {
        fprintf(out, "\tadd\t%s, %s, #%d\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), offset);
    } else if (offset < 0 && arm32_encode_imm(-(int64_t)offset) != 0xFFFFFFFFu) {
        fprintf(out, "\tsub\t%s, %s, #%d\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), -offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_mov_imm_asm(c, scratch, offset, out);
        fprintf(out, "\tadd\t%s, %s, %s\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(base), aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_deref_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tldr\t%s, [%s]\n", aarch32_get_hw_reg_name(dst), aarch32_get_hw_reg_name(src));
}
static void aarch32_emit_call_asm(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, int *stack_pos, FILE *out) {
    static const char *int_arg_regs[] = {"r0", "r1", "r2", "r3"};
    static const char *fp_arg_regs[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};

    int int_arg_idx = 0;
    int fp_arg_idx = 0;
    int indirect_temp = -1;
    int int_src_regs[8];
    int int_arg_temps[IR_CALL_MAX_ARGS];
    int total_int_count = 0;
    int32_t spill_base_offset = c->regalloc.spill_base_offset;

    for (int i = 0; i < arg_count; i++) {
        int temp = arg_temps[i];
        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        int arg_is_fp = is_fp ? is_fp[i] : 0;

        if (arg_is_fp == 2) {
            indirect_temp = temp;
        } else if (arg_is_fp == 1 && (stack_pos ? stack_pos[i] : 255) >= 255 && fp_arg_idx < 8) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tfldd\t%s, [%s, #%d]\n", fp_arg_regs[fp_arg_idx],
                        BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                        (int)zero_cc_frame_disp(c, -off));
            } else if (phys_reg >= 0) {
                fprintf(out, "\tfcpyd\t%s, d%d\n", fp_arg_regs[fp_arg_idx], phys_reg);
            }
            fp_arg_idx++;
        } else if (arg_is_fp == 0) {
            int_arg_temps[total_int_count++] = temp;
            if (int_arg_idx < 4) {
                int_src_regs[int_arg_idx++] = phys_reg;
            }
        }
    }

    /* Save indirect callee into r12 (ip) before clobbering argument registers */
    if (!func_name && indirect_temp >= 0) {
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, indirect_temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                    BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                    (int)zero_cc_frame_disp(c, -off));
        } else {
            int r_callee = regalloc_get_reg_for_temp(&c->regalloc, indirect_temp);
            fprintf(out, "\tmov\tr12, %s\n", BE(c)->get_hw_reg_name(r_callee));
        }
    }

    /* Stack args (from stack_pos): integer args beyond r0..r3 AND FP args
     * beyond d0..d7, in original argument order.
     * In AArch32 AAPCS, stack arguments are 4-byte aligned, but overall SP
     * must be 8-byte aligned at the call boundary. */
    int n_stack = 0;
    int stack_pos_list[IR_CALL_MAX_ARGS];
    for (int i = 0; i < arg_count && i < IR_CALL_MAX_ARGS; i++) {
        int arg_pos = stack_pos ? stack_pos[i] : 255;
        if (arg_pos < 255 && arg_pos < IR_CALL_MAX_ARGS) {
            stack_pos_list[arg_pos] = i;
            if (arg_pos + 1 > n_stack) n_stack = arg_pos + 1;
        }
    }
    int legacy_n_stack = (!stack_pos && total_int_count > 4) ? total_int_count - 4 : 0;
    int stack_alloc = 0;
    if (stack_pos) {
        stack_alloc = ((n_stack * 4) + 7) & ~7;
        if (n_stack > 0) {
            fprintf(out, "\tsub\tsp, sp, #%d\n", stack_alloc);
            for (int pos = 0; pos < n_stack; pos++) {
                int i = stack_pos_list[pos];
                int arg_is_fp = is_fp ? is_fp[i] : 0;
                if (arg_is_fp == 1) {
                    int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                    fprintf(out, "\tfstd\td%d, [sp, #%d]\n", phys_reg, pos * 4);
                } else {
                    int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[i]);
                    if (sp >= 0) {
                        int32_t off = spill_base_offset + (sp + 1) * 8;
                        fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                                BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                                (int)zero_cc_frame_disp_at(c, -off, stack_alloc));
                        fprintf(out, "\tstr\tr12, [sp, #%d]\n", pos * 4);
                    } else {
                        int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[i]);
                        fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(phys_reg), pos * 4);
                    }
                }
            }
        }
    } else {
        /* Legacy: int args beyond 4, in order */
        n_stack = legacy_n_stack;
        stack_alloc = ((n_stack * 4) + 7) & ~7;
        if (n_stack > 0) {
            fprintf(out, "\tsub\tsp, sp, #%d\n", stack_alloc);
            for (int a = 4; a < total_int_count; a++) {
                int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
                if (sp >= 0) {
                    int32_t off = spill_base_offset + (sp + 1) * 8;
                    fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                            BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                            (int)zero_cc_frame_disp_at(c, -off, stack_alloc));
                    fprintf(out, "\tstr\tr12, [sp, #%d]\n", (a - 4) * 4);
                } else {
                    int phys_reg = regalloc_get_reg_for_temp(&c->regalloc, int_arg_temps[a]);
                    fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(phys_reg), (a - 4) * 4);
                }
            }
        }
    }

    /* Parallel move of integer arguments via stack so r0..r3 don't clobber each other */
    if (int_arg_idx > 0) {
        int alloc_bytes = ((int_arg_idx * 4) + 7) & ~7;
        fprintf(out, "\tsub\tsp, sp, #%d\n", alloc_bytes);
        for (int a = 0; a < int_arg_idx; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, int_arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                        BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                        (int)zero_cc_frame_disp_at(c, -off, stack_alloc + alloc_bytes));
                fprintf(out, "\tstr\tr12, [sp, #%d]\n", a * 4);
            } else {
                fprintf(out, "\tstr\t%s, [sp, #%d]\n", BE(c)->get_hw_reg_name(int_src_regs[a]), a * 4);
            }
        }
        for (int a = 0; a < int_arg_idx; a++) {
            fprintf(out, "\tldr\t%s, [sp, #%d]\n", int_arg_regs[a], a * 4);
        }
        fprintf(out, "\tadd\tsp, sp, #%d\n", alloc_bytes);
    }

    if (!func_name && indirect_temp >= 0) {
        fprintf(out, "\tblx\tr12\n");
    } else if (func_name) {
        fprintf(out, "\tbl\t%s\n", func_name);
    }

    /* Clean up stack arguments after call */
    if (stack_alloc > 0) {
        fprintf(out, "\tadd\tsp, sp, #%d\n", stack_alloc);
    }
}
static void aarch32_emit_ret_asm(ZeroCCCompiler *c, FILE *out) {
    ZeroCCFunction *cur_fn = (c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                             ? &c->functions[c->lowering.current_func_idx] : NULL;
    if (cur_fn && cur_fn->is_interrupt) {
        if (cur_fn->interrupt_mode <= 1) {
            fprintf(out, "\tsubs\tpc, lr, #4\n");
        } else {
            fprintf(out, "\tmovs\tpc, lr\n");
        }
        return;
    }
    fprintf(out, "\tbx\tlr\n");
}
static void aarch32_emit_label_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "%s:\n", label);
}
static void aarch32_emit_jmp_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tb\t%s\n", label);
}
static void aarch32_emit_jeq_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbeq\t%s\n", label);
}
static void aarch32_emit_jne_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbne\t%s\n", label);
}
static void aarch32_emit_jlt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tblt\t%s\n", label);
}
static void aarch32_emit_jle_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tble\t%s\n", label);
}
static void aarch32_emit_jgt_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbgt\t%s\n", label);
}
static void aarch32_emit_jge_asm(ZeroCCCompiler *c, const char *label, FILE *out) {
    (void)c;
    fprintf(out, "\tbge\t%s\n", label);
}
static void aarch32_emit_load_fp_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    if (offset >= -1020 && offset <= 1020 && (offset & 3) == 0) {
        fprintf(out, "\tfldd\td%d, [%s, #%d]\n", dst, aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_lea_asm(c, scratch, base, offset, out);
        fprintf(out, "\tfldd\td%d, [%s, #0]\n", dst, aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_store_fp_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    if (offset >= -1020 && offset <= 1020 && (offset & 3) == 0) {
        fprintf(out, "\tfstd\td%d, [%s, #%d]\n", src, aarch32_get_hw_reg_name(base), offset);
    } else {
        int scratch = (base != 12) ? 12 : 9;
        aarch32_emit_lea_asm(c, scratch, base, offset, out);
        fprintf(out, "\tfstd\td%d, [%s, #0]\n", src, aarch32_get_hw_reg_name(scratch));
    }
}
static void aarch32_emit_fp_add_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfaddd\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_sub_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfsubd\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_mul_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfmuld\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_div_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfdivd\td%d, d%d, d%d\n", dst, src1, src2);
}
static void aarch32_emit_fp_cmp_asm(ZeroCCCompiler *c, int src1, int src2, FILE *out) {
    (void)c;
    fprintf(out, "\tfcmpd\td%d, d%d\n", src1, src2);
}
static void aarch32_emit_fp_mov_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tfcpyd\td%d, d%d\n", dst, src);
}
static void aarch32_emit_fp_cvtsf_asm(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out) {
    (void)c;
    fprintf(out, "\tsitos\td%d, %s\n", fp_dst, aarch32_get_hw_reg_name(int_src));
}
static void aarch32_emit_fp_cvtsi_asm(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out) {
    (void)c;
    fprintf(out, "\tftosizs\t%s, d%d\n", aarch32_get_hw_reg_name(int_dst), fp_src);
}
static void aarch32_emit_cvtss2sd_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tvcvt.f64.f32\td%d, s%d\n", dst, src * 2);
}
static void aarch32_emit_cvtsd2ss_asm(ZeroCCCompiler *c, int dst, int src, FILE *out) {
    (void)c;
    fprintf(out, "\tvcvt.f32.f64\ts%d, d%d\n", dst * 2, src);
}
static void aarch32_emit_vla_alloc_asm(ZeroCCCompiler *c, int dst, int size, FILE *out) {
    (void)c; (void)dst; (void)size; (void)out;
    fprintf(out, "\tsub\tsp, sp, %s\n", aarch32_get_hw_reg_name(size));
    fprintf(out, "\tmov\t%s, sp\n", aarch32_get_hw_reg_name(dst));
}
static void aarch32_emit_param_save_int_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out) {
    aarch32_emit_store_asm(c, reg_idx, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset), out);
}
static void aarch32_emit_param_save_fp_asm(ZeroCCCompiler *c, int reg_idx, int32_t offset, int is_single, FILE *out) {
    (void)is_single;
    if (reg_idx >= 0 && reg_idx < 8) {
        aarch32_emit_store_fp_asm(c, reg_idx, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -offset), out);
    }
}
static void aarch32_emit_variadic_spill_asm(ZeroCCCompiler *c, int param_count, int32_t base_offset, FILE *out) {
    (void)param_count;
    int gp_regs = BE(c)->conv->int_arg_reg_count;
    for (int i = 0; i < gp_regs; i++) {
        int r = BE(c)->conv->int_arg_regs[i];
        aarch32_emit_store_asm(c, r, zero_cc_frame_base_reg(c), zero_cc_frame_disp(c, -(base_offset + 16 - i * 4)), out);
    }
}
static void aarch32_emit_syscall_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    static const char *syscall_reg_names[7] = {"r7", "r0", "r1", "r2", "r3", "r4", "r5"};
    int n = (arg_count < 7) ? arg_count : 7;
    int32_t spill_base_offset = c->regalloc.spill_base_offset;

    /* Save callee-saved registers that we will overwrite */
    int save_regs[4];
    int n_save = 0;
    save_regs[n_save++] = 7;
    if (n >= 6) save_regs[n_save++] = 4;
    if (n >= 7) save_regs[n_save++] = 5;
    int save_bytes = ((n_save * 4) + 7) & ~7;
    fprintf(out, "\tsub\tsp, sp, #%d\n", save_bytes);
    for (int r = 0; r < n_save; r++) {
        fprintf(out, "\tstr\t%s, [sp, #%d]\n", aarch32_get_hw_reg_name(save_regs[r]), r * 4);
    }

    /* Parallel move of the syscall arguments via stack */
    if (n > 0) {
        int alloc_bytes = ((n * 4) + 7) & ~7;
        fprintf(out, "\tsub\tsp, sp, #%d\n", alloc_bytes);
        for (int a = 0; a < n; a++) {
            int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, arg_temps[a]);
            if (sp >= 0) {
                int32_t off = spill_base_offset + (sp + 1) * 8;
                fprintf(out, "\tldr\tr12, [%s, #%d]\n",
                        BE(c)->get_hw_reg_name(zero_cc_frame_base_reg(c)),
                        (int)zero_cc_frame_disp_at(c, -off, save_bytes + alloc_bytes));
                fprintf(out, "\tstr\tr12, [sp, #%d]\n", a * 4);
            } else {
                int r = regalloc_get_reg_for_temp(&c->regalloc, arg_temps[a]);
                fprintf(out, "\tstr\t%s, [sp, #%d]\n", aarch32_get_hw_reg_name(r), a * 4);
            }
        }
        for (int a = 0; a < n; a++) {
            fprintf(out, "\tldr\t%s, [sp, #%d]\n", syscall_reg_names[a], a * 4);
        }
        fprintf(out, "\tadd\tsp, sp, #%d\n", alloc_bytes);
    }

    fprintf(out, "\tsvc\t#0\n");

    /* Restore callee-saved registers (r0 has return value, leave untouched) */
    for (int r = 0; r < n_save; r++) {
        fprintf(out, "\tldr\t%s, [sp, #%d]\n", aarch32_get_hw_reg_name(save_regs[r]), r * 4);
    }
    fprintf(out, "\tadd\tsp, sp, #%d\n", save_bytes);
}
static void aarch32_emit_clone_asm(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out) {
    aarch32_emit_syscall_asm(c, arg_count, arg_temps, out);
}
static void aarch32_emit_inline_asm_asm(ZeroCCCompiler *c, size_t instr_index, FILE *out) {
    (void)c; (void)instr_index; (void)out;
}
static void aarch32_emit_inline_asm_mc(ZeroCCCompiler *c, size_t instr_index) {
    (void)c; (void)instr_index;
}
static void aarch32_emit_filescope_asm_mc(ZeroCCCompiler *c, const char *asm_str) {
    (void)c; (void)asm_str;
}

/* Vector asm - NEON 128-bit load/store */
static void aarch32_emit_vload_asm(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tvld1.64\t{q%d}, [%s, #%d]\n", dst, aarch32_get_hw_reg_name(base), offset);
}
static void aarch32_emit_vstore_asm(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out) {
    (void)c;
    fprintf(out, "\tvst1.64\t{q%d}, [%s, #%d]\n", src, aarch32_get_hw_reg_name(base), offset);
}

static void aarch32_emit_load_sized_asm(ZeroCCCompiler *c, int phys_dst, int phys_addr, int size_bytes, FILE *out) {
    const char *dst_name = aarch32_get_hw_reg_name(phys_dst);
    const char *addr_name = aarch32_get_hw_reg_name(phys_addr);
    (void)c;
    if (size_bytes == 1) {
        fprintf(out, "\tldrb\t%s, [%s]\n", dst_name, addr_name);
    } else if (size_bytes == 2) {
        fprintf(out, "\tldrh\t%s, [%s]\n", dst_name, addr_name);
    } else {
        fprintf(out, "\tldr\t%s, [%s]\n", dst_name, addr_name);
    }
}

static void aarch32_emit_store_sized_asm(ZeroCCCompiler *c, int phys_val, int phys_addr, int size_bytes, FILE *out) {
    const char *val_name = aarch32_get_hw_reg_name(phys_val);
    const char *addr_name = aarch32_get_hw_reg_name(phys_addr);
    (void)c;
    if (size_bytes == 1) {
        fprintf(out, "\tstrb\t%s, [%s]\n", val_name, addr_name);
    } else if (size_bytes == 2) {
        fprintf(out, "\tstrh\t%s, [%s]\n", val_name, addr_name);
    } else {
        fprintf(out, "\tstr\t%s, [%s]\n", val_name, addr_name);
    }
}

static void aarch32_emit_test_zero_asm(ZeroCCCompiler *c, int phys_reg, FILE *out) {
    const char *name = aarch32_get_hw_reg_name(phys_reg);
    (void)c;
    fprintf(out, "\tcmp\t%s, #0\n", name);
}

static void aarch32_emit_cmp_regs_asm(ZeroCCCompiler *c, int phys_reg1, int phys_reg2, FILE *out) {
    const char *r1 = aarch32_get_hw_reg_name(phys_reg1);
    const char *r2 = aarch32_get_hw_reg_name(phys_reg2);
    (void)c;
    fprintf(out, "\tcmp\t%s, %s\n", r1, r2);
}


void aarch32_init_asm_emission(ZeroCCISABackend *be) {
    be->emit_prologue_asm = aarch32_emit_prologue_asm;
    be->emit_epilogue_asm = aarch32_emit_epilogue_asm;
    be->emit_stack_guard_init_asm = aarch32_emit_stack_guard_init_asm;
    be->emit_stack_guard_check_asm = aarch32_emit_stack_guard_check_asm;
    be->get_hw_reg_name = aarch32_get_hw_reg_name;
    be->get_vec_reg_name = aarch32_get_vec_reg_name;
    be->emit_load_sized_asm = aarch32_emit_load_sized_asm;
    be->emit_store_sized_asm = aarch32_emit_store_sized_asm;
    be->emit_test_zero_asm = aarch32_emit_test_zero_asm;
    be->emit_cmp_regs_asm = aarch32_emit_cmp_regs_asm;
    be->emit_mov_imm_asm = aarch32_emit_mov_imm_asm;
    be->emit_mov_reg_asm = aarch32_emit_mov_reg_asm;
    be->emit_load_asm = aarch32_emit_load_asm;
    be->emit_store_asm = aarch32_emit_store_asm;
    be->emit_add_asm = aarch32_emit_add_asm;
    be->emit_sub_asm = aarch32_emit_sub_asm;
    be->emit_mul_asm = aarch32_emit_mul_asm;
    be->emit_div_asm = aarch32_emit_div_asm;
    be->emit_mod_asm = aarch32_emit_mod_asm;
    be->emit_and_asm = aarch32_emit_and_asm;
    be->emit_or_asm = aarch32_emit_or_asm;
    be->emit_xor_asm = aarch32_emit_xor_asm;
    be->emit_shl_asm = aarch32_emit_shl_asm;
    be->emit_shr_asm = aarch32_emit_shr_asm;
    be->emit_sar_asm = aarch32_emit_sar_asm;
    be->emit_extend_asm = aarch32_emit_extend_asm;
    be->emit_cmp_asm = aarch32_emit_cmp_asm;
    be->emit_cond_set_asm = aarch32_emit_cond_set_asm;
    be->emit_neg_asm = aarch32_emit_neg_asm;
    be->emit_add_imm_asm = aarch32_emit_add_imm_asm;
    be->emit_sub_imm_asm = aarch32_emit_sub_imm_asm;
    be->emit_inc_asm = aarch32_emit_inc_asm;
    be->emit_dec_asm = aarch32_emit_dec_asm;
    be->emit_not_asm = aarch32_emit_not_asm;
    be->emit_lea_asm = aarch32_emit_lea_asm;
    be->emit_deref_asm = aarch32_emit_deref_asm;
    be->emit_call_asm = aarch32_emit_call_asm;
    be->emit_ret_asm = aarch32_emit_ret_asm;
    be->emit_label_asm = aarch32_emit_label_asm;
    be->emit_jmp_asm = aarch32_emit_jmp_asm;
    be->emit_jeq_asm = aarch32_emit_jeq_asm;
    be->emit_jne_asm = aarch32_emit_jne_asm;
    be->emit_jlt_asm = aarch32_emit_jlt_asm;
    be->emit_jle_asm = aarch32_emit_jle_asm;
    be->emit_jgt_asm = aarch32_emit_jgt_asm;
    be->emit_jge_asm = aarch32_emit_jge_asm;
    be->emit_load_fp_asm = aarch32_emit_load_fp_asm;
    be->emit_store_fp_asm = aarch32_emit_store_fp_asm;
    be->emit_fp_add_asm = aarch32_emit_fp_add_asm;
    be->emit_fp_sub_asm = aarch32_emit_fp_sub_asm;
    be->emit_fp_mul_asm = aarch32_emit_fp_mul_asm;
    be->emit_fp_div_asm = aarch32_emit_fp_div_asm;
    be->emit_fp_cmp_asm = aarch32_emit_fp_cmp_asm;
    be->emit_fp_mov_asm = aarch32_emit_fp_mov_asm;
    be->emit_vaddsd_asm = aarch32_emit_fp_add_asm;
    be->emit_vsubsd_asm = aarch32_emit_fp_sub_asm;
    be->emit_vmulsd_asm = aarch32_emit_fp_mul_asm;
    be->emit_vdivsd_asm = aarch32_emit_fp_div_asm;
    be->emit_vmovsd_asm = aarch32_emit_fp_mov_asm;
    be->emit_fp_cvtsf_asm = aarch32_emit_fp_cvtsf_asm;
    be->emit_fp_cvtsi_asm = aarch32_emit_fp_cvtsi_asm;
    be->emit_cvtss2sd_asm = aarch32_emit_cvtss2sd_asm;
    be->emit_cvtsd2ss_asm = aarch32_emit_cvtsd2ss_asm;
    be->emit_vla_alloc_asm = aarch32_emit_vla_alloc_asm;
    be->emit_param_save_int_asm = aarch32_emit_param_save_int_asm;
    be->emit_param_save_fp_asm = aarch32_emit_param_save_fp_asm;
    be->emit_variadic_spill_asm = aarch32_emit_variadic_spill_asm;
    be->emit_syscall_asm = aarch32_emit_syscall_asm;
    be->emit_clone_asm = aarch32_emit_clone_asm;
    be->emit_inline_asm_asm = aarch32_emit_inline_asm_asm;
    be->emit_vload_asm = aarch32_emit_vload_asm;
    be->emit_vstore_asm = aarch32_emit_vstore_asm;

    be->emit_mul64_asm = aarch32_emit_mul64_asm;
    be->emit_div64_asm = aarch32_emit_div64_asm;
    be->emit_mod64_asm = aarch32_emit_mod64_asm;
    be->emit_udiv64_asm = aarch32_emit_udiv64_asm;
    be->emit_umod64_asm = aarch32_emit_umod64_asm;

    be->emit_add64_asm = aarch32_emit_add64_asm;
    be->emit_sub64_asm = aarch32_emit_sub64_asm;
    be->emit_and64_asm = aarch32_emit_and64_asm;
    be->emit_or64_asm = aarch32_emit_or64_asm;
    be->emit_xor64_asm = aarch32_emit_xor64_asm;
    be->emit_not64_asm = aarch32_emit_not64_asm;
    be->emit_neg64_asm = aarch32_emit_neg64_asm;
    be->emit_shl64_asm = aarch32_emit_shl64_asm;
    be->emit_shr64_asm = aarch32_emit_shr64_asm;
    be->emit_sar64_asm = aarch32_emit_sar64_asm;
    be->emit_cmp64_asm = aarch32_emit_cmp64_asm;
    be->emit_mov64_asm = aarch32_emit_mov64_asm;
    be->emit_load64_asm = aarch32_emit_load64_asm;
    be->emit_store64_asm = aarch32_emit_store64_asm;
    be->emit_zext32_64_asm = aarch32_emit_zext32_64_asm;
    be->emit_sext32_64_asm = aarch32_emit_sext32_64_asm;
    be->emit_trunc64_32_asm = aarch32_emit_trunc64_32_asm;
    be->emit_mov64_imm_asm = aarch32_emit_mov64_imm_asm;
}
