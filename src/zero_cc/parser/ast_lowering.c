/* AST Lowering - AST to Machine Code Pipeline (Coordinator)
 *
 * This file coordinates the lowering of AST to IR, live range analysis,
 * linear-scan register allocation, callee-save preservation, stack spill
 * slot lowering, and machine code generation.
 *
 * The implementation is organized into modular source files:
 *   - ast_lowering_stmt.c: AST statement lowering to IR (lower_stmt_*, lower_node)
 *   - ast_lowering_expr.c: AST expression lowering to IR (lower_expr)
 *   - ast_lowering_mc.c:   IR to machine code emission (emit_ir_to_machine_code)
 */

#include "ast_lowering_internal.h"

/* Statement lowering (block, if, while, for, switch, return, decl, etc.) */
#include "ast_lowering_stmt.c"

/* Expression lowering (arithmetic, calls, member access, casts, ternary, etc.) */
#include "ast_lowering_expr.c"

/* Machine code emission from IR */
#include "ast_lowering_mc.c"

/* Optimization passes */
#include "opt_pass.c"

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* Initialize lowering state */
    memset(&c->lowering, 0, sizeof(c->lowering));
    memset(&c->label_patcher, 0, sizeof(c->label_patcher));
    ir_emitter_init(&c->lowering.ir_ctx);
    
    /* Reset register allocator for this function */
    regalloc_init(&c->regalloc);
    c->regalloc.target_isa = c->isa;  /* Set target ISA for ISA-specific allocation */
    c->next_temp_id = VREG_USER_START;
    c->ir_next_label = 0;
    
    /* Reset label patcher state */
    label_patcher_init(&c->label_patcher);

    /* Reset IR buffer */
    ir_reset();

    /* Emit placeholder OP_PROLOGUE (will be patched with exact frame size after regalloc) */
    ir_emit_prologue(0);

    /* Emit IR instructions to save incoming parameter registers into their assigned local stack slots */
    if (c->function_count > 0) {
        ZeroCCFunction *fn = &c->functions[c->function_count - 1];
        int xmm_idx = 0;
        int int_idx = 0;
        int stack_idx = 0;

        for (size_t p = 0; p < fn->param_count && p < (size_t)c->local_count; p++) {
            ZeroCCSym *sym = &c->locals[p];
            if (!sym->is_param) continue;
            bool is_fp = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);

            if (is_fp && xmm_idx < BE(c)->conv->fp_arg_reg_count) {
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
            int32_t base_spill_offset = (int32_t)fn->param_count > 0 ? c->locals[fn->param_count - 1].stack_offset + 8 : CALLEE_SAVE_FRAME_RESERVE + 8;
            ir_emit_variadic_spill((int)fn->param_count, base_spill_offset);
        }
    }

    c->lowering.epilogue_label = c->ir_next_label++;
    
    /* Lower AST to IR (instruction selection) */
    lower_node(c, arena, root);
    ir_emit_label(c->lowering.epilogue_label);
    
    /* 1. Run IR optimization passes FIRST (before register allocation) */
    opt_init(OPT_LEVEL_O1);
    opt_run_passes(c);
    opt_shutdown();
    
    /* 2. Compute live ranges and allocate registers on optimized IR */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get_buffer(), ir_get_count());
    live_range_compute(&lra, &c->regalloc);

    regalloc_allocate(&c->regalloc);

    /* Emit callee-save IR instructions after register allocation (Phase D: IR-based callee-save) */
    uint32_t callee_save_mask = c->regalloc.used_callee_saved_gpr_mask;
    if (callee_save_mask) {
        ir_emit_callee_save_stores(callee_save_mask);
        ir_emit_callee_save_loads(callee_save_mask);
    }
    
    live_range_free(&lra);
    
    /* Allocate frame spill slots for any temps that were spilled */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    /* Ensure spill_base_offset is at least CALLEE_SAVE_FRAME_RESERVE to avoid overlapping with callee-saved register area */
    if (c->current_stack_frame < CALLEE_SAVE_FRAME_RESERVE) {
        c->current_stack_frame = CALLEE_SAVE_FRAME_RESERVE;
    }
    int32_t spill_base_offset = c->current_stack_frame;
    if (c->regalloc.next_spill_slot > 0) {
        c->current_stack_frame += c->regalloc.next_spill_slot * 8;
    }

    /* Calculate total 16-byte aligned stack frame size and patch OP_PROLOGUE */
    size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);
    Instr *prologue_instr = ir_get(0);
    if (prologue_instr && prologue_instr->op == OP_PROLOGUE) {
        prologue_instr->imm64 = (uint64_t)total_frame_size;
    }
    
    /* Emit machine code from IR with allocated registers */

    emit_ir_to_machine_code(c, spill_base_offset);

    
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
