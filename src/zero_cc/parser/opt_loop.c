#include "opt_loop.h"
#include "opt_profile.h"
#include "ir.h"
#include "ir_emitter.h"
#include <stdlib.h>
#include <string.h>

/**
 * Loop analysis and optimization implementation
 */

#define MAX_LOOPS 256
#define MAX_LOOP_BODY 1024

/**
 * Initialize loop analysis state
 */
void loop_analysis_init(LoopAnalysisState *state) {
    state->loops = malloc(sizeof(LoopInfo) * MAX_LOOPS);
    state->loop_count = 0;
    state->capacity = MAX_LOOPS;
    state->dominators = NULL;
    state->post_dominators = NULL;
    state->ir_count = 0;
    
    for (int i = 0; i < MAX_LOOPS; i++) {
        state->loops[i].body_indices = NULL;
        state->loops[i].is_invariant = NULL;
        state->loops[i].body_count = 0;
        state->loops[i].capacity = 0;
    }
}

/**
 * Free loop analysis resources
 */
void loop_analysis_free(LoopAnalysisState *state) {
    free(state->loops);
    free(state->dominators);
    free(state->post_dominators);
    state->loops = NULL;
    state->dominators = NULL;
    state->post_dominators = NULL;
    state->loop_count = 0;
}

/**
 * Simple loop detection: find backedges (jumps to earlier instructions)
 * This is a simplified version that works for the common case
 */
static void detect_loops(LoopAnalysisState *state) {
    int count = (int)ir_get_count();
    state->ir_count = count;
    state->loop_count = 0;
    
    /* Build label-to-instruction-index map.
     * Note: Synthetic label IDs are allocated sequentially by ir_next_label. */
    int max_label_id = -1;
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (instr && instr->op == OP_LABEL) {
            if ((int)instr->label_idx > max_label_id) {
                max_label_id = (int)instr->label_idx;
            }
        }
    }
    
    if (max_label_id < 0) return;
    
    int map_size = max_label_id + 1;
    int *label_to_idx = (int *)malloc(map_size * sizeof(int));
    if (!label_to_idx) return;
    for (int i = 0; i < map_size; i++) label_to_idx[i] = -1;
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (instr && instr->op == OP_LABEL) {
            label_to_idx[instr->label_idx] = i;
        }
    }
    
    /* Detect loops: look for jumps to earlier instructions (backedges) */
    for (int i = 0; i < count && state->loop_count < MAX_LOOPS; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        if (instr->op == OP_JMP || instr->op == OP_JE || 
            instr->op == OP_JNE || instr->op == OP_JL ||
            instr->op == OP_JLE || instr->op == OP_JG || instr->op == OP_JCOND) {
            
            int label = (int)instr->label_idx;
            if (label >= 0 && label < map_size) {
                int target_idx = label_to_idx[label];
                if (target_idx >= 0 && target_idx < i) {
                    /* Backedge found: header at target_idx, backedge jump at i */
                    LoopInfo *loop = &state->loops[state->loop_count];
                    loop->header_idx = target_idx;
                    loop->exit_idx = i;
                    loop->preheader_idx = -1;
                    loop->body_count = 0;
                    loop->capacity = MAX_LOOP_BODY;
                    loop->body_indices = malloc(sizeof(int) * MAX_LOOP_BODY);
                    loop->is_invariant = calloc(MAX_LOOP_BODY, sizeof(bool));
                    
                    if (loop->body_indices && loop->is_invariant) {
                        for (int j = target_idx; j <= i; j++) {
                            if (loop->body_count < MAX_LOOP_BODY) {
                                loop->body_indices[loop->body_count++] = j;
                            }
                        }
                        state->loop_count++;
                    } else {
                        free(loop->body_indices);
                        free(loop->is_invariant);
                        loop->body_indices = NULL;
                        loop->is_invariant = NULL;
                    }
                }
            }
        }
    }
    
    free(label_to_idx);
}

/**
 * Check if an instruction is loop-invariant
 * An instruction is invariant if:
 * 1. It is a pure computational operation (no memory accesses, jumps, calls, etc.)
 * 2. Every source operand is either defined outside the loop body or marked invariant.
 */
static bool instr_is_invariant(Instr *instr, bool *invariant_map, int *loop_body, int body_count) {
    if (!instr) return false;
    
    /* Only hoist pure, deterministic operations that define a destination temp */
    if (instr->temp_dest == 0xFF) return false;
    
    switch (instr->op) {
        case OP_MOV_IMM:
            return true;
        case OP_MOV_REG:
        case OP_ADD_IMM:
        case OP_SUB_IMM:
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_NOT:
        case OP_LNOT:
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
        case OP_CVTTSD2SI:
        case OP_CVTSI2SD:
            break;
        default:
            return false;
    }
    
    /* Verify src1 */
    if (instr->temp_src1 != 0xFF) {
        bool src1_defined_in_loop = false;
        bool src1_invariant = false;
        for (int i = 0; i < body_count; i++) {
            Instr *def = ir_get(loop_body[i]);
            if (def && def->temp_dest == instr->temp_src1) {
                src1_defined_in_loop = true;
                if (invariant_map[loop_body[i]]) {
                    src1_invariant = true;
                }
                break;
            }
        }
        /* If defined in loop and not invariant, this instruction is not invariant */
        if (src1_defined_in_loop && !src1_invariant) return false;
    }
    
    /* Verify src2 */
    if (instr->temp_src2 != 0xFF) {
        bool src2_defined_in_loop = false;
        bool src2_invariant = false;
        for (int i = 0; i < body_count; i++) {
            Instr *def = ir_get(loop_body[i]);
            if (def && def->temp_dest == instr->temp_src2) {
                src2_defined_in_loop = true;
                if (invariant_map[loop_body[i]]) {
                    src2_invariant = true;
                }
                break;
            }
        }
        if (src2_defined_in_loop && !src2_invariant) return false;
    }
    
    return true;
}

/**
 * Check if instruction has side effects that prevent moving
 */
static bool instr_has_moving_side_effects(Instr *instr) {
    if (!instr) return true;
    
    switch (instr->op) {
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_RET:
        case OP_STORE_BYTE:
        case OP_STORE_16:
        case OP_STORE_32:
        case OP_STORE_64:
        case OP_FSTORE:
        case OP_VLA_ALLOC:
        case OP_ATOMIC_XADD:
        case OP_ATOMIC_CMPXCHG:
        case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG:
        case OP_MFENCE:
        case OP_LABEL:
        case OP_JMP:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
        case OP_JMP_LABEL:
        case OP_PROLOGUE:
        case OP_EPILOGUE:
        case OP_CALLEE_SAVE_STORES:
        case OP_CALLEE_SAVE_LOADS:
        case OP_PARAM_SAVE_INT:
        case OP_PARAM_SAVE_FP:
        case OP_VARIADIC_SPILL:
            return true;
        
        default:
            return false;
    }
}

/**
 * Loop-invariant code motion (LICM)
 * Hoists loop-invariant instructions to the loop preheader using a safe Two-Pass Rebuild.
 */
void opt_licm(ZeroCCCompiler *c) {
    (void)c;
    
    LoopAnalysisState state;
    loop_analysis_init(&state);
    
    /* Detect loops */
    detect_loops(&state);
    
    if (state.loop_count == 0) {
        loop_analysis_free(&state);
        return;
    }
    
    /* Profile: count loops detected */
    opt_profile_count_licm_loop();
    
    int ir_count = (int)ir_get_count();
    bool *global_hoist = calloc(ir_count, sizeof(bool));
    if (!global_hoist) {
        loop_analysis_free(&state);
        return;
    }
    
    /* Array to track which loop header receives hoisted instructions */
    /* hoist_at_header[header_idx] will store a linked/dynamic list or we can map per loop */
    int total_hoisted = 0;
    
    /* Analyze each loop and mark invariants */
    for (int l = 0; l < state.loop_count; l++) {
        LoopInfo *loop = &state.loops[l];
        
        /* Compute invariants for this loop */
        bool *invariant = loop->is_invariant;
        memset(invariant, 0, sizeof(bool) * loop->capacity);
        
        /* Iteratively compute invariants (fixed point) */
        bool changed = true;
        int iterations = 0;
        while (changed && iterations < 10) {
            changed = false;
            iterations++;
            
            for (int i = 0; i < loop->body_count; i++) {
                int idx = loop->body_indices[i];
                if (invariant[idx]) continue;
                
                Instr *instr = ir_get(idx);
                if (!instr) continue;
                
                if (instr_is_invariant(instr, invariant, loop->body_indices, loop->body_count)) {
                    if (!instr_has_moving_side_effects(instr)) {
                        invariant[idx] = true;
                        changed = true;
                    }
                }
            }
        }
        
        /* Mark hoisted instructions */
        for (int i = 0; i < loop->body_count; i++) {
            int idx = loop->body_indices[i];
            if (invariant[idx]) {
                opt_profile_count_licm_invariant();
                if (!global_hoist[idx]) {
                    global_hoist[idx] = true;
                    total_hoisted++;
                }
            }
        }
    }
    
    if (total_hoisted == 0) {
        free(global_hoist);
        loop_analysis_free(&state);
        return;
    }
    
    /* =========================================================================
     * PASS 2: Two-Pass Rebuild into new IRArena
     * ========================================================================= */
    IRArena new_arena;
    if (!ir_arena_init(&new_arena, ir_count + total_hoisted + 16)) {
        free(global_hoist);
        loop_analysis_free(&state);
        return;
    }
    
    /* Array to map old instruction indices to new instruction indices */
    int *old_to_new = malloc(ir_count * sizeof(int));
    if (!old_to_new) {
        ir_arena_free(&new_arena);
        free(global_hoist);
        loop_analysis_free(&state);
        return;
    }
    for (int i = 0; i < ir_count; i++) old_to_new[i] = -1;
    
    /* For each loop header, collect instructions to hoist before it */
    for (int i = 0; i < ir_count; i++) {
        /* Check if i is a loop header */
        for (int l = 0; l < state.loop_count; l++) {
            LoopInfo *loop = &state.loops[l];
            if (loop->header_idx == i) {
                /* Emit all invariant instructions belonging to this loop before the header */
                for (int b = 0; b < loop->body_count; b++) {
                    int b_idx = loop->body_indices[b];
                    if (loop->is_invariant[b_idx]) {
                        Instr *inv_instr = ir_get(b_idx);
                        if (inv_instr) {
                            ir_arena_emit(&new_arena, *inv_instr);
                            opt_profile_count_licm_moved();
                        }
                    }
                }
                break;
            }
        }
        
        /* Emit current instruction or OP_NOP if it was hoisted */
        Instr *orig_instr = ir_get(i);
        if (!orig_instr) continue;
        
        if (global_hoist[i]) {
            /* Replace original in-loop instruction with OP_NOP */
            Instr nop_instr = {
                .op = OP_NOP,
                .temp_dest = 0xFF,
                .temp_src1 = 0xFF,
                .temp_src2 = 0xFF,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = 0,
                .imm64 = 0,
                .label_idx = 0
            };
            old_to_new[i] = (int)new_arena.count;
            ir_arena_emit(&new_arena, nop_instr);
        } else {
            old_to_new[i] = (int)new_arena.count;
            ir_arena_emit(&new_arena, *orig_instr);
        }
    }
    
    /* Remap IRCallArgs indices to new instruction positions */
    if (g_compiler) {
        IRCallArgsArena *call_arena = &g_compiler->ir_call_args;
        if (call_arena && call_arena->table) {
            for (size_t c_idx = 0; c_idx < call_arena->count; c_idx++) {
                size_t old_pos = call_arena->table[c_idx].instr_index;
                if (old_pos < (size_t)ir_count && old_to_new[old_pos] >= 0) {
                    call_arena->table[c_idx].instr_index = (size_t)old_to_new[old_pos];
                }
            }
        }
        
        /* Replace compiler's ir_arena buffer with new_arena */
        ir_arena_free(&g_compiler->ir_arena);
        g_compiler->ir_arena = new_arena;
    } else {
        ir_arena_free(&new_arena);
    }
    
    free(old_to_new);
    free(global_hoist);
    loop_analysis_free(&state);
}

/**
 * Strength reduction: replace multiplication with shifts/adds
 * Common pattern: x = i * constant -> x = i << n (for powers of 2)
 */
void opt_strength_reduction(ZeroCCCompiler *c) {
    (void)c;
    
    int count = (int)ir_get_count();
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        // Look for multiplication patterns
        if (instr->op == OP_MUL && instr->temp_dest != 0xFF && instr->temp_dest != 0) {
            // Check if one operand is a constant power of 2
            // For now, we'd need to track which temp holds the constant
            // This is a simplified placeholder
        }
        
        // Check for shift patterns that could be optimized
        if (instr->op == OP_SHL && instr->temp_dest != 0xFF && instr->temp_dest != 0) {
            // Already a shift, nothing to do
        }
    }
}

/**
 * Branch simplification: remove dead branches and simplify constant conditions
 */
void opt_branch_simplify(ZeroCCCompiler *c) {
    (void)c;
    
    int count = (int)ir_get_count();
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        // Look for conditional jumps with known conditions
        // This would require constant propagation data
        // For now, this is a simplified placeholder
    }
}

/**
 * Public API wrappers for opt_pass.c
 */
void opt_loop_analysis(ZeroCCCompiler *c) {
    (void)c;
    // Placeholder for future loop analysis
}
