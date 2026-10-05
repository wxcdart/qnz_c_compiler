#include "opt_loop.h"
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
    
    // Simple heuristic: look for jumps to earlier labels
    // In a real implementation, we'd build a CFG and find natural loops
    
    for (int i = 0; i < count && state->loop_count < MAX_LOOPS; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        // Check for jump instructions that could be loop backedges
        if (instr->op == OP_JMP || instr->op == OP_JE || 
            instr->op == OP_JNE || instr->op == OP_JL ||
            instr->op == OP_JLE || instr->op == OP_JG) {
            
            // If jump target is earlier, this might be a loop backedge
            int target = instr->label_idx;
            if (target >= 0 && target < i) {
                // Found potential loop
                LoopInfo *loop = &state->loops[state->loop_count];
                loop->header_idx = target;
                loop->exit_idx = i;
                loop->preheader_idx = -1;
                loop->body_count = 0;
                loop->capacity = MAX_LOOP_BODY;
                loop->body_indices = malloc(sizeof(int) * MAX_LOOP_BODY);
                loop->is_invariant = calloc(MAX_LOOP_BODY, sizeof(bool));
                
                // Mark instructions between header and exit as body
                for (int j = target; j <= i; j++) {
                    if (loop->body_count < MAX_LOOP_BODY) {
                        loop->body_indices[loop->body_count++] = j;
                    }
                }
                
                state->loop_count++;
            }
        }
    }
}

/**
 * Check if an instruction is loop-invariant
 * An instruction is invariant if all its operands are invariant or loop-independent
 */
static bool instr_is_invariant(Instr *instr, bool *invariant_map, int *loop_body, int body_count) {
    if (!instr) return false;
    
    // Constants are always invariant
    if (instr->op == OP_MOV_IMM) {
        return true;
    }
    
    // Check if both sources are invariant
    if (instr->temp_src1 != 0xFF && instr->temp_src1 != 0) {
        // Find where src1 is defined
        bool src1_invariant = false;
        for (int i = 0; i < body_count; i++) {
            Instr *def = ir_get(loop_body[i]);
            if (def && def->temp_dest == instr->temp_src1) {
                src1_invariant = invariant_map[loop_body[i]];
                break;
            }
        }
        if (!src1_invariant) return false;
    }
    
    if (instr->temp_src2 != 0xFF && instr->temp_src2 != 0) {
        bool src2_invariant = false;
        for (int i = 0; i < body_count; i++) {
            Instr *def = ir_get(loop_body[i]);
            if (def && def->temp_dest == instr->temp_src2) {
                src2_invariant = invariant_map[loop_body[i]];
                break;
            }
        }
        if (!src2_invariant) return false;
    }
    
    return true;
}

/**
 * Check if instruction has side effects that prevent moving
 */
static bool instr_has_moving_side_effects(Instr *instr) {
    if (!instr) return true;
    
    switch (instr->op) {
        // These have side effects that prevent moving
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
            return true;
        
        default:
            return false;
    }
}

/**
 * Loop-invariant code motion (LICM)
 * Moves loop-invariant instructions to the loop preheader
 */
void opt_licm(ZeroCCCompiler *c) {
    (void)c;
    
    LoopAnalysisState state;
    loop_analysis_init(&state);
    
    // Detect loops
    detect_loops(&state);
    
    if (state.loop_count == 0) {
        loop_analysis_free(&state);
        return;
    }
    
    int count = (int)ir_get_count();
    
    // Process each loop
    for (int l = 0; l < state.loop_count; l++) {
        LoopInfo *loop = &state.loops[l];
        
        // Compute invariants for this loop
        bool *invariant = loop->is_invariant;
        memset(invariant, 0, sizeof(bool) * loop->capacity);
        
        // Iteratively compute invariants (fixed point)
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
        
        // Move invariants to before the loop header
        // This is a simplified version - real implementation would insert at preheader
        for (int i = 0; i < loop->body_count; i++) {
            int idx = loop->body_indices[i];
            if (invariant[idx]) {
                Instr *instr = ir_get(idx);
                if (instr && !instr_has_moving_side_effects(instr)) {
                    // Mark as moved (will be handled in final pass)
                    // For now, we just note that this is invariant
                }
            }
        }
    }
    
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
