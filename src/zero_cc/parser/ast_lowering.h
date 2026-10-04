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
#include "lowering_state.h"
#include "label_patcher.h"

/* Helper: Get current function's return type (last function added to compiler) */
static inline ZeroCCType zero_cc_get_current_fn_ret_type(ZeroCCCompiler *c) {
    if (c->function_count > 0) {
        return c->functions[c->function_count - 1].return_type;
    }
    return zero_cc_type_int();
}



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
    /* Initialize lowering state */
    memset(&c->lowering, 0, sizeof(c->lowering));
    memset(&c->label_patcher, 0, sizeof(c->label_patcher));
    ir_emitter_init(&c->lowering.ir_ctx);
    
    /* Reset register allocator for this function */
    regalloc_init(&c->regalloc);
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

            if (is_fp && xmm_idx < 8) {
                ir_emit_param_save_fp(xmm_idx, sym->stack_offset);
                xmm_idx++;
            } else if (!is_fp && int_idx < 6) {
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
    
    /* Compute live ranges and allocate registers */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get_buffer(), ir_get_count());
    live_range_compute(&lra, &c->regalloc);
    regalloc_allocate(&c->regalloc);
    
    live_range_free(&lra);
    
    /* Allocate frame spill slots for any temps that were spilled */
    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
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
    
    /* Patch epilogue jumps - target is the epilogue (before leave/ret) */
    size_t epilogue_target = me_get_offset(c);
    
    /* Restore callee-saved registers before the epilogue leave/ret. */
    emit_callee_save_loads(c);
    
    /* Emit epilogue */
    me_emit_leave(c);
    BE(c)->emit_ret(c);
    regalloc_destroy(&c->regalloc);
    
    /* text_offset was already set in compile.h BEFORE the prologue was emitted,
     * so fn->text_offset correctly points to the function's first byte. */
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_H */
