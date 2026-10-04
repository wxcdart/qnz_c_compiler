/* AST Lowering - AST to Machine Code Pipeline (Coordinator Header)
 *
 * This header coordinates the lowering of AST to IR, live range analysis,
 * linear-scan register allocation, callee-save preservation, stack spill
 * slot lowering, and machine code generation.
 *
 * The implementation is organized into modular headers:
 *   - ast_lowering_expr.h: AST expression lowering to IR (lower_expr)
 *   - ast_lowering_stmt.h: AST statement lowering to IR (lower_stmt_*, lower_node)
 *   - ast_lowering_mc.h:   IR to machine code emission (emit_ir_to_machine_code)
 */

#ifndef ZERO_CC_PARSER_AST_LOWERING_H
#define ZERO_CC_PARSER_AST_LOWERING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types/types.h"
#include "parser.h"
#include "ir_emitter.h"
#include "ir_emit_ops.h"
#include "machine_emitter.h"
#include "../isa/isa_backend.h"
#include "../types/vregs.h"
#include "../types/global_vars.h"
#include "../semantic/type.h"
#include "../common/arena.h"
#include "abi_classify.h"
#include "regalloc.h"
#include "live_range.h"

/* Helper: Get current function's return type (last function added to compiler) */
static inline ZeroCCType zero_cc_get_current_fn_ret_type(ZeroCCCompiler *c) {
    if (c->function_count > 0) {
        return c->functions[c->function_count - 1].return_type;
    }
    return zero_cc_type_int();
}

/* IR Emitter Context for state management */
static IREmitterContext g_ir_emitter_ctx;

/* Forward declarations */
static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
static int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
static void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset);

/* Statement lowering (block, if, while, for, switch, return, decl, etc.) */
#include "ast_lowering_stmt.h"

/* Expression lowering (arithmetic, calls, member access, casts, ternary, etc.) */
#include "ast_lowering_expr.h"

/* Machine code emission from IR */
#include "ast_lowering_mc.h"

/* =========================================================================
 * Callee-Saved Register Preservation Helpers
 * ========================================================================= */

/* Emit MOV [rbp - slot], reg for each callee-saved GPR that was assigned. */
static void emit_callee_save_stores(ZeroCCCompiler *c) __attribute__((unused));
static void emit_callee_save_stores(ZeroCCCompiler *c) {
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    while (mask) {
        int idx = __builtin_ctz(mask);
        int vreg = callee_save_pool_to_vreg[idx];
        int32_t slot = callee_save_slot_offset[idx];
        if (BE(c)->emit_store) {
            BE(c)->emit_store(c, vreg, BE(c)->fp_reg, -slot);
        }
        mask &= mask - 1;
    }
}

/* Emit MOV reg, [rbp - slot] for each callee-saved GPR that was assigned. */
static void emit_callee_save_loads(ZeroCCCompiler *c) __attribute__((unused));
static void emit_callee_save_loads(ZeroCCCompiler *c) {
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    while (mask) {
        int idx = __builtin_ctz(mask);
        int vreg = callee_save_pool_to_vreg[idx];
        int32_t slot = callee_save_slot_offset[idx];
        if (BE(c)->emit_load) {
            BE(c)->emit_load(c, vreg, BE(c)->fp_reg, -slot);
        }
        mask &= mask - 1;
    }
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* Initialize IR emitter context */
    ir_emitter_init(&g_ir_emitter_ctx);
    
    /* Reset register allocator for this function */
    regalloc_init(&c->regalloc);
    c->next_temp_id = VREG_USER_START;
    c->ir_next_label = 0;
    
    /* Reset user labels and loop label stack for this function */
    g_user_label_count = 0;
    g_loop_label_depth = 0;

    /* Reset IR buffer */
    ir_reset();
    g_ir_epilogue_label = c->ir_next_label++;
    
    /* Don't reset text section - accumulate all function code */
    /* c->text_section.length = 0; */
    
    /* NOTE: The function prologue is emitted in compile.h before calling this
     * function. emit_prologue() is NOT called here to avoid a double prologue. */
    
    /* Lower AST to IR (instruction selection) */
    lower_node(c, arena, root);
    ir_emit_label(g_ir_epilogue_label);
    
    /* Compute live ranges and allocate registers */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get_buffer(), ir_get_count());
    live_range_compute(&lra, &c->regalloc);
    regalloc_allocate(&c->regalloc);
    
    live_range_free(&lra);
    
    /* Preserve callee-saved registers that were assigned by the allocator.
     * Emit MOV [rbp - slot], reg for each used callee-saved GPR. */
    emit_callee_save_stores(c);
    
    /* Allocate frame spill slots for any temps that were spilled */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
    int32_t spill_base_offset = c->current_stack_frame;
    if (c->regalloc.next_spill_slot > 0) {
        c->current_stack_frame += c->regalloc.next_spill_slot * 8;
    }
    
    /* Emit machine code from IR with allocated registers */
    emit_ir_to_machine_code(c, spill_base_offset);
    
    /* Emit return jumps after IR-to-MC */
    emit_return_jumps(c);
    
    /* Resolve all label patches */
    if (BE(c)->resolve_labels) {
        BE(c)->resolve_labels(c);
    }
    
    /* Patch epilogue jumps - target is the epilogue (before leave/ret) */
    size_t epilogue_target = me_get_offset(c);
    
    /* Restore callee-saved registers before the epilogue leave/ret. */
    emit_callee_save_loads(c);
    
    /* Emit epilogue */
    me_emit_leave(c);
    BE(c)->emit_ret(c);
    for (int i = 0; i < g_ir_emitter_ctx.epilogue_patch_count; i++) {
        size_t patch_pos = g_ir_emitter_ctx.epilogue_patches[i];
        int32_t rel = (int32_t)(epilogue_target - (patch_pos + 4));
        me_patch_u32(c, patch_pos, (uint32_t)rel);
    }
    g_ir_emitter_ctx.epilogue_patch_count = 0;
    regalloc_destroy(&c->regalloc);
    
    /* text_offset was already set in compile.h BEFORE the prologue was emitted,
     * so fn->text_offset correctly points to the function's first byte. */
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_H */
