/* AST Lowering - AST to Machine Code Pipeline (Coordinator)
 *
 * This file coordinates the lowering of AST to IR, live range analysis,
 * linear-scan register allocation, callee-save preservation, stack spill
 * slot lowering, and machine code generation.
 *
 * The implementation is organized into modular source files:
 *   - ast_lowering_stmt.c: AST statement lowering to IR (lower_stmt_*, lower_node)
 *   - ast_lowering_expr.c: AST expression lowering to IR (lower_expr)
 *   - asm_emitter.c:      IR to assembly emission (emit_ir_to_asm)
 *   - ast_lowering_mc.c:   IR to machine code emission (emit_ir_to_machine_code)
 */

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/ir/asm_emitter.h"
#include "zero_cc/opt/opt_pass.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

/* =========================================================================
 * Sprint 37: static-local mirror
 *
 * A `static` block-scope local is a data-section object (registered in
 * the global table under sym->static_global_name) whose stack slot is a
 * per-call mirror. The prologue (before the body) copies
 * data -> slot; the single shared epilogue (every return jumps to the
 * epilogue label, and natural fallthrough reaches it) copies
 * slot -> data. All access sites remain frame-relative, and the value
 * persists across calls. Frame slots are aligned to >=8 bytes, and the
 * data allocation is rounded up to 8, so 8-byte chunks (with a possibly
 * smaller final chunk) never spill into a neighbouring object.
 * ========================================================================= */
static void static_local_chunk_addr(ZeroCCCompiler *c, const ZeroCCSym *ss,
                                    size_t off, int *out_addr) {
    *out_addr = c->next_temp_id++;
    if (off == 0) {
        ir_emit_mov_global((uint16_t)*out_addr, ss->static_global_name);
    } else {
        int base = c->next_temp_id++;
        ir_emit_mov_global((uint16_t)base, ss->static_global_name);
        int o = c->next_temp_id++;
        ir_emit_mov_imm((uint16_t)o, (int64_t)off);
        ir_emit_add((uint16_t)*out_addr, (uint16_t)base, (uint16_t)o);
    }
}

/* data -> stack slot (prologue) */
static void static_local_mirror_in(ZeroCCCompiler *c, ZeroCCSym *ss) {
    size_t sz = ss->type.size ? (size_t)ss->type.size : 8;
    size_t alloc_sz = (sz + 7) & ~(size_t)7;  /* mirror copies in 8-byte chunks */
    for (size_t off = 0; off < alloc_sz; off += 8) {
        int addr;
        static_local_chunk_addr(c, ss, off, &addr);
        int val = c->next_temp_id++;
        ir_emit_deref(val, addr);
        int sa = c->next_temp_id++;
        ir_emit_lea(sa, BE(c)->fp_reg, (int32_t)off - ss->stack_offset);
        ir_emit_store_64((uint16_t)val, (uint16_t)sa);
    }
}

/* stack slot -> data (epilogue). Plain allocator temps are fine here
 * because the caller wraps the whole block with a VREG_0 (return value)
 * save/restore through the frame - see zero_cc_lower_ast(). */
static void static_local_mirror_out(ZeroCCCompiler *c, ZeroCCSym *ss) {
    size_t sz = ss->type.size ? (size_t)ss->type.size : 8;
    size_t alloc_sz = (sz + 7) & ~(size_t)7;  /* mirror copies in 8-byte chunks */
    for (size_t off = 0; off < alloc_sz; off += 8) {
        int sa = c->next_temp_id++;
        ir_emit_lea(sa, BE(c)->fp_reg, (int32_t)off - ss->stack_offset);
        int val = c->next_temp_id++;
        ir_emit_deref(val, sa);
        int addr;
        static_local_chunk_addr(c, ss, off, &addr);
        ir_emit_store_64((uint16_t)val, (uint16_t)addr);
    }
}

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* Initialize lowering state */
    memset(&c->lowering, 0, sizeof(c->lowering));
    memset(&c->label_patcher, 0, sizeof(c->label_patcher));
    ir_emitter_init(&c->lowering.ir_ctx);

    /* Block scope starts unbounded at the function top level */
    c->local_lookup_bound = 0;

    /* Initialize VLA state */
    c->lowering.vla.has_runtime_vla = false;
    c->lowering.vla.vla_count = 0;
    c->lowering.vla.block_depth = 0;
    c->lowering.vla.loop_depth = 0;

    /* Reset register allocator for this function */
    regalloc_init(&c->regalloc);
    regalloc_set_isa(&c->regalloc, c->isa);  /* Set target ISA and reserved registers */
    c->next_temp_id = VREG_USER_START;
    c->ir_next_label = 0;

    /* Reset label patcher state */
    label_patcher_init(&c->label_patcher);

    /* Set current compiler for IR emission */
    ir_set_compiler(c);

    /* Reset IR buffer */
    ir_reset();

    /* Determine if incoming parameters can be kept directly in virtual registers (frameless leaf) */
    bool can_promote_params = false;
    if (c->function_count > 0) {
        ZeroCCFunction *fn = &c->functions[c->function_count - 1];
        int max_int_params = (c->isa == ISA_X86_64) ? 3 : 8;
        if (!fn->is_variadic && fn->ret_struct_sz <= 16 &&
            c->local_count == fn->param_count &&
            !zero_cc_ast_has_call(arena, root, NULL)) {
            /* Member access on a parameter (p->m / s.m) is rejected: the
             * member base-load paths read the parameter from its frame slot
             * (stack_offset, which is 0 when promoted) instead of the
             * promoted vreg -- in a frameless function that slot is the
             * caller's saved %rbp. See zero_cc_ast_member_on_param. */
            if (!zero_cc_ast_addr_of_param(arena, root, fn->param_names, (int)fn->param_count) &&
                !zero_cc_ast_member_on_param(arena, root, fn->param_names, (int)fn->param_count) &&
                !zero_cc_ast_assigns_to_param(arena, root, fn->param_names, (int)fn->param_count)) {
                can_promote_params = true;
                int chk_int = 0;
                int chk_fp = 0;
                for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
                    ZeroCCSym *sym = &c->locals[p];
                    if (!sym->is_param) continue;
                    bool is_fp = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
                    if (is_fp) {
                        if (chk_fp >= BE(c)->conv->fp_arg_reg_count) { can_promote_params = false; break; }
                        chk_fp++;
                    } else {
                        if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8) {
                            can_promote_params = false; break;
                        }
                        if (chk_int >= max_int_params) { can_promote_params = false; break; }
                        chk_int++;
                    }
                }
            }
        }
    }

    if (can_promote_params) {
        c->current_stack_frame = 0;
        ZeroCCFunction *fn = &c->functions[c->function_count - 1];
        int int_idx = 0;
        int fp_idx = 0;
        for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
            ZeroCCSym *sym = &c->locals[p];
            if (!sym->is_param) continue;
            sym->stack_offset = 0;
            sym->is_promoted = true;
            sym->promoted_vreg = c->next_temp_id++;
            bool is_fp = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            int arg_idx = is_fp ? fp_idx++ : int_idx++;
            ir_emit_param_load(sym->promoted_vreg, arg_idx, is_fp);
            if (!is_fp) {
                zero_cc_emit_int_extend(c, sym->promoted_vreg, &sym->type);
            }
        }
    } else if (c->function_count > 0) {
        ZeroCCFunction *fn = &c->functions[c->function_count - 1];
        int xmm_idx = 0;
        int int_idx = 0;
        int stack_idx = 0;

        /* >16 byte struct returns use hidden sret pointer:
         * - System V AMD64 ABI (x86-64): RDI (param_idx=0), real params start at RSI (int_idx=1)
         * - RISC-V LP64D ABI: a0 (param_idx=0), real params start at a1 (int_idx=1)
         * - AArch64 AAPCS64 ABI: x8 (Indirect Result Location Register, param_idx=8), real params start at x0 (int_idx=0) */
        bool uses_sret = fn->ret_struct_sz > 16;
        if (uses_sret) {
            if (c->isa == ISA_AARCH64) {
                ir_emit_param_save_int(8, fn->sret_stack_offset);  /* param_idx=8 means x8 on AArch64 */
                int_idx = 0;  /* Real params still start at x0 */
            } else {
                ir_emit_param_save_int(0, fn->sret_stack_offset);  /* param_idx=0 means RDI / a0 */
                int_idx = 1;  /* Real params start at RSI / a1 */
            }
        }

        for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
            ZeroCCSym *sym = &c->locals[p];
            if (!sym->is_param) continue;
            bool is_fp = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            bool is_multi_chunk_struct = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8;

            if (is_multi_chunk_struct) {
                int num_chunks = (int)((sym->type.size + 7) / 8);
                for (int ch = 0; ch < num_chunks; ch++) {
                    int32_t chunk_off = sym->stack_offset - ch * 8;
                    if (int_idx < BE(c)->conv->int_arg_reg_count) {
                        ir_emit_param_save_int(int_idx, chunk_off);
                        int_idx++;
                    } else {
                        int32_t caller_off = 16 + 8 * stack_idx;
                        ir_emit_load(VREG_0, BE(c)->fp_reg, caller_off);
                        ir_emit_store(VREG_0, BE(c)->fp_reg, -chunk_off);
                        stack_idx++;
                    }
                }
            } else if (is_fp && xmm_idx < BE(c)->conv->fp_arg_reg_count) {
                ir_emit_param_save_fp(xmm_idx, sym->stack_offset);
                xmm_idx++;
            } else if (!is_fp && int_idx < BE(c)->conv->int_arg_reg_count) {
                ir_emit_param_save_int(int_idx, sym->stack_offset);
                int_idx++;
            } else if (!is_fp) {
                int32_t caller_off = 16 + 8 * stack_idx;
                ir_emit_load(VREG_0, BE(c)->fp_reg, caller_off);
                ir_emit_store(VREG_0, BE(c)->fp_reg, -sym->stack_offset);
                stack_idx++;
            }
        }

        if (fn->is_variadic) {
            /* Save area: (int_arg_reg_count + fp_arg_reg_count) 8-byte slots
             * above ALL named locals - the integer arg registers first, then
             * the FP arg registers (16 slots on x86-64/AArch64/RV64).
             * va_start/va_arg walk them as two streams (see the NODE_VA_START
             * lowering below). Placed above the locals (not right after the
             * last parameter, as before) so body stores to locals cannot
             * clobber spilled variadic args. */
            int va_slots = BE(c)->conv->int_arg_reg_count + BE(c)->conv->fp_arg_reg_count;
            int32_t base_spill_offset = c->current_stack_frame + 8;
            c->current_stack_frame = (int32_t)(((size_t)(base_spill_offset + va_slots * 8) + 15) & ~(size_t)15);
            ir_emit_variadic_spill((int)fn->param_count, base_spill_offset);
        }
    }

    c->lowering.epilogue_label = c->ir_next_label++;

    /* Sprint 37: static locals. When the function has at least one, reserve
     * an 8-byte frame slot that shields VREG_0 (return value / sret
     * pointer) across the epilogue mirror-out below: the mirror-out uses
     * allocator temps that may be mapped onto the return register, and the
     * return value is already live in VREG_0 by the time the epilogue runs.
     * (Must happen before the frame size is computed below.) */
    c->lowering.static_local_rt_off = 0;
    for (size_t i = 0; i < c->local_count; i++) {
        if (c->locals[i].is_static_local) {
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            c->lowering.static_local_rt_off = (int)c->current_stack_frame;
            break;
        }
    }

    /* Sprint 37: static locals - data section -> stack slot, once per call,
     * before any body statement can read the (mirrored) slot. */
    for (size_t i = 0; i < c->local_count; i++) {
        if (c->locals[i].is_static_local) static_local_mirror_in(c, &c->locals[i]);
    }

    /* Lower AST to IR (instruction selection) */
    lower_node(c, arena, root);
    ir_emit_label(c->lowering.epilogue_label);

    /* Sprint 37: static locals - stack slot -> data section. Every return
     * jumps to the epilogue label and natural fallthrough reaches it, so
     * this single block runs on every exit path.
     *
     * The mirror-out's allocator temps may be assigned to the return
     * register, so protect VREG_0 in the reserved frame slot around the
     * block (integer return values and the sret pointer; FP results live
     * in XMM registers, which the integer-only mirror-out cannot touch).
     * CALLEE_SAVE_LOADS runs after the restore and re-establishes all
     * callee-saved state. */
    if (c->lowering.static_local_rt_off != 0) {
        ir_emit_store(VREG_0, BE(c)->fp_reg, -c->lowering.static_local_rt_off);
        for (size_t i = 0; i < c->local_count; i++) {
            if (c->locals[i].is_static_local) static_local_mirror_out(c, &c->locals[i]);
        }
        ir_emit_load(VREG_0, BE(c)->fp_reg, -c->lowering.static_local_rt_off);
    }

    /* C2b guard: temp ids are 16-bit (TEMP_NONE = 0xFFFF is the "no temp"
     * sentinel). next_temp_id is an int, so a pathological function could
     * still exhaust the 16-bit space; fail loudly instead of wrapping
     * (the pre-C2b 8-bit wrap silently miscompiled). */
    if (c->next_temp_id - 1 >= TEMP_NONE) {
        const char *fn_name = "<anon>";
        if (c->function_count > 0) fn_name = c->functions[c->function_count - 1].name;
        diag_report_error_at_token(c,
            "function '%s' uses %d temporaries; max is 65534 per function "
            "(16-bit temp ids). Split the function into smaller helper "
            "functions (each has its own temp-id space).",
            fn_name, c->next_temp_id - VREG_USER_START);
        return;
    }

    /* 1. Run IR optimization passes FIRST (before register allocation) */
    opt_init(c->opt_level);
    opt_run_passes(c);
    opt_shutdown();

    /* 2. Compute live ranges and allocate registers on optimized IR */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get_buffer(), ir_get_count());
    live_range_compute(&lra, &c->regalloc);

    regalloc_allocate(&c->regalloc);

    if (c->dump_ir) {
        fprintf(stderr, "=== Allocated Intervals (%zu) ===\n", c->regalloc.interval_count);
        for (size_t i = 0; i < c->regalloc.interval_count; i++) {
            LiveInterval *inv = &c->regalloc.intervals[i];
            fprintf(stderr, "temp r%d (class %d): start=%zu, end=%zu -> reg %d (spill=%d)\n",
                    inv->temp_id, inv->reg_class, inv->start, inv->end, inv->assigned_reg, inv->spill_slot);
        }
    }

    /* Emit callee-save IR instructions after register allocation (Phase D: IR-based callee-save) */
    uint32_t callee_save_mask = c->regalloc.used_callee_saved_gpr_mask;
    if (callee_save_mask) {
        ir_emit_callee_save_stores(callee_save_mask);
        ir_emit_callee_save_loads(callee_save_mask);
    }

    /* Callee-save IR append (Phase D) mutated the IR: invalidate CFG cache. */
    c->cfg_version++;

    /* x86 float-compare CC remap (C12): on this host class (WSL2),
     * comisd/ucomisd sets CF/ZF but NOT SF/OF, so the signed condition
     * codes (setl/setle/setg/setge) misfire on float compares while
     * integer cmp (which sets SF) is unaffected. For ordered values the
     * CF/ZF bits encode the total order, so remap CC_LT/LE/GT/GE on the
     * COND_SET that follows an FP CMP to the CF/ZF-only variants
     * (setb/setbe/seta/setae) - the same scheme gcc uses for doubles. */
    if (c->isa == ISA_X86_64) {
        size_t n = ir_get_count();
        for (size_t i = 1; i < n; i++) {
            const Instr *cur = ir_get(i);
            const Instr *prev = ir_get(i - 1);
            if (!cur || !prev) break;
            if (cur->op == OP_COND_SET && prev->op == OP_CMP && cur->cond <= 3) {
                RegClass rc1 = regalloc_get_class_for_temp(&c->regalloc, prev->temp_src1);
                RegClass rc2 = regalloc_get_class_for_temp(&c->regalloc, prev->temp_src2);
                if (rc1 == REG_CLASS_XMM || rc1 == REG_CLASS_VEC ||
                    rc2 == REG_CLASS_XMM || rc2 == REG_CLASS_VEC) {
                    ir_get(i)->cond = (uint8_t)(cur->cond + 6);
                }
            }
        }
        /* In-place IR mutation: invalidate the cached CFG (Phase 2). */
        c->cfg_version++;
    }

    live_range_free(&lra);

    /* Allocate frame spill slots for any temps that were spilled */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    /* Ensure spill_base_offset is at least CALLEE_SAVE_FRAME_RESERVE to avoid overlapping with callee-saved register area,
     * unless the function has no frame needs (no stack slots, no spills, and no callee-saved registers). */
    if (c->current_stack_frame == 0 && c->regalloc.next_spill_slot == 0 && c->regalloc.used_callee_saved_gpr_mask == 0) {
        c->current_stack_frame = 0;
    } else if (c->current_stack_frame < BE(c)->callee_save_frame_reserve) {
        c->current_stack_frame = BE(c)->callee_save_frame_reserve;
    }
    int32_t spill_base_offset = c->current_stack_frame;
    c->regalloc.spill_base_offset = spill_base_offset;
    if (c->regalloc.next_spill_slot > 0) {
        c->current_stack_frame += c->regalloc.next_spill_slot * 8;
    }

    /* Calculate total 16-byte aligned stack frame size */
    /* Both JIT and assembly need 16-byte alignment for proper call alignment */
    /* After push rbp (8 bytes), RSP is 0 mod 16. After sub rsp, frame_size, RSP should be 0 mod 16. */
    /* So frame_size must be 0 mod 16 (16-byte aligned). */
    size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);

    /* Determine if function is a leaf (no calls, no VLAs) */
    bool is_leaf = true;
    size_t ir_count = ir_get_count();
    const Instr *ir_buf = ir_get_buffer();
    for (size_t i = 0; i < ir_count; i++) {
        if (ir_buf[i].op == OP_CALL || ir_buf[i].op == OP_CALL_INDIRECT || ir_buf[i].op == OP_VLA_ALLOC) {
            is_leaf = false;
            break;
        }
    }

    /* Option 1: Leaf functions with no stack needs are frameless */
    c->lowering.is_frameless = (is_leaf && total_frame_size == 0 && c->regalloc.used_callee_saved_gpr_mask == 0 && c->lowering.static_local_rt_off == 0);

    /* Option 3: x86-64 System V Red Zone optimization:
     * Leaf functions with <= 128 bytes can use the 128-byte Red Zone below %rsp directly
     * without adjusting %rsp. */
    size_t alloc_frame_size = total_frame_size;
    if (c->isa == ISA_X86_64 && is_leaf && total_frame_size <= 128) {
        alloc_frame_size = 0;
    }

    /* Option 4: Frame pointer omission for fixed-frame functions.
     * The frame pointer (%rbp / x29 / s0) is a constant anchor for every
     * frame slot. For a function whose frame is fully static (no runtime
     * VLA, which lowers SP dynamically on some targets) the same anchor
     * role is served by SP plus a per-function constant delta, so the
     * prologue/epilogue drop the FP setup/teardown and every slot offset
     * is rebased: disp_SP = disp_FP + (FP - final_SP).
     *
     * Per-ISA geometry (E = entry SP, alloc = alloc_frame_size, aligned =
     * 16-round-up(alloc)):
     *   x86-64  : FP = E-8 (push rbp); the prologue subtracts X = alloc+8
     *             (alloc==0: X = 8 to keep 16-byte call alignment; red-zone
     *             leaf: X = 0). delta = X - 8.
     *   aarch64 : FP = E-16 (stp x29,x30); X = aligned+16. delta = aligned.
     *   riscv   : FP = E-16 (s0 = sp+aligned); X = aligned+16. delta = aligned.
     * RISC-V additionally requires aligned+16 <= 2047 so the FP-less
     * epilogue (ld ra, aligned+8(sp); addi sp, sp, aligned+16) fits in
     * immediates; larger frames keep the FP.
     * Policy: on by default at -O1 and above (like gcc's -fomit-frame-
     * pointer); force with -fomit-frame-pointer / -fno-omit-frame-pointer. */
    c->lowering.omit_frame_pointer = false;
    c->lowering.frame_delta = 0;
    {
        bool omit_wanted = c->omit_frame_pointer == 1 ||
                           (c->omit_frame_pointer < 0 && c->opt_level >= OPT_LEVEL_O1);
        if (omit_wanted && !c->lowering.is_frameless) {
            bool has_vla = false;
            for (size_t i = 0; i < ir_count; i++) {
                if (ir_buf[i].op == OP_VLA_ALLOC) { has_vla = true; break; }
            }
            if (!has_vla) {
                size_t aligned = (alloc_frame_size + 15) & ~(size_t)15;
                if (c->isa == ISA_X86_64) {
                    size_t X = (alloc_frame_size == 0 && is_leaf) ? 0 : alloc_frame_size + 8;
                    c->lowering.frame_delta = (int32_t)X - 8;
                    c->lowering.omit_frame_pointer = true;
                } else {
                    c->lowering.frame_delta = (int32_t)aligned;
                    bool fits = (c->isa != ISA_RISCV64 && c->isa != ISA_RISCV32) ||
                                (aligned + 16 <= 2047);
                    if (fits) {
                        c->lowering.omit_frame_pointer = true;
                    } else {
                        c->lowering.frame_delta = 0;
                    }
                }
            }
        }
    }

    /* Emit machine code or assembly from IR with allocated registers */
    if (c->emit_asm) {
        /* For assembly: emit function label, then prologue, then IR (param saves in IR) */
        emit_function_label_asm(c);
        emit_prologue_asm(c, alloc_frame_size);
        emit_ir_to_asm(c, spill_base_offset);
    } else {
        /* For machine code: emit prologue first, then IR (param saves in IR) */
        emit_prologue_mc(c, alloc_frame_size);
        emit_ir_to_machine_code(c, spill_base_offset);
    }


    /* Emit return jumps after IR-to-MC */
    emit_return_jumps(c);

    /* Resolve all label patches */
    if (BE(c)->resolve_labels) {
        BE(c)->resolve_labels(c);
    }

    /* Callee-saved register loads are now handled by OP_CALLEE_SAVE_LOADS IR instruction */

    /* Emit epilogue */
    BE(c)->emit_leave(c);
    BE(c)->emit_ret(c);
    regalloc_destroy(&c->regalloc);

    /* Debug: Dump IR if requested */
    if (c->dump_ir) {
        fprintf(stderr, "DEBUG: dump_ir flag is SET, function_count=%zu\n", c->function_count);
        if (c->function_count > 0) {
            ZeroCCFunction *fn = &c->functions[c->function_count - 1];
            fprintf(stderr, "=== IR for function '%s' ===\n", fn->name);
            ir_dump(stderr);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "DEBUG: No functions found!\n");
        }
    }

    /* Debug: Dump CFG if requested (Sprint 35 Phase 2) */
    if (c->dump_cfg) {
        cfg_dump(stderr);
        fprintf(stderr, "\n");
    }

    /* Clear IR buffer for next function */
    ir_clear();

    /* text_offset was already set in compile.h BEFORE the prologue was emitted,
     * so fn->text_offset correctly points to the function's first byte. */
}
