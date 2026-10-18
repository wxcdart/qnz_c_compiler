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

#include "ast_lowering_internal.h"
#include "asm_emitter.h"
#include "../opt/opt_pass.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* Initialize lowering state */
    memset(&c->lowering, 0, sizeof(c->lowering));
    memset(&c->label_patcher, 0, sizeof(c->label_patcher));
    ir_emitter_init(&c->lowering.ir_ctx);
    
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

    /* Emit IR instructions to save incoming parameter registers into their assigned local stack slots */
    if (c->function_count > 0) {
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
            int32_t base_spill_offset = (int32_t)fn->param_count > 0 ? c->locals[fn->param_count - 1].stack_offset + 8 : BE(c)->callee_save_frame_reserve + 8;
            ir_emit_variadic_spill((int)fn->param_count, base_spill_offset);
        }
    }

    c->lowering.epilogue_label = c->ir_next_label++;
    
    /* Lower AST to IR (instruction selection) */
    lower_node(c, arena, root);
    ir_emit_label(c->lowering.epilogue_label);

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
    }
    
    live_range_free(&lra);
    
    /* Allocate frame spill slots for any temps that were spilled */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    /* Ensure spill_base_offset is at least CALLEE_SAVE_FRAME_RESERVE to avoid overlapping with callee-saved register area */
    if (c->current_stack_frame < BE(c)->callee_save_frame_reserve) {
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
    
    /* Emit machine code or assembly from IR with allocated registers */
    if (c->emit_asm) {
        /* For assembly: emit function label, then prologue, then IR (param saves in IR) */
        emit_function_label_asm(c);
        emit_prologue_asm(c, total_frame_size);
        emit_ir_to_asm(c, spill_base_offset);
    } else {
        /* For machine code: emit prologue first, then IR (param saves in IR) */
        emit_prologue_mc(c, total_frame_size);
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
    
    /* Clear IR buffer for next function */
    ir_clear();
    
    /* text_offset was already set in compile.h BEFORE the prologue was emitted,
     * so fn->text_offset correctly points to the function's first byte. */
}
