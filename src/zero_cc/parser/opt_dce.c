#include "opt_dce.h"
#include "ir.h"
#include "regalloc.h"
#include <stdlib.h>
#include <string.h>

/**
 * Liveness tracking
 */
#define MAX_VREGS 4096
static bool g_live_temp[MAX_VREGS];

/**
 * Clear liveness tracking
 */
static void clear_liveness(void) {
    memset(g_live_temp, 0, sizeof(g_live_temp));
}

/**
 * Check if a temp is live
 */
static int is_live(int temp) {
    if (temp < 0 || temp >= MAX_VREGS) {
        return 0;
    }
    return g_live_temp[temp] ? 1 : 0;
}

/**
 * Mark a temp as live
 */
static void mark_live(int temp) {
    if (temp >= 0 && temp < MAX_VREGS) {
        g_live_temp[temp] = true;
    }
}

/**
 * Check if instruction has side effects
 */
static int instr_has_side_effects(Instr *instr) {
    switch (instr->op) {
        // Memory writes have side effects
        case OP_STORE_BYTE:
        case OP_STORE_16:
        case OP_STORE_32:
        case OP_STORE_64:
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_RET:
        case OP_JMP:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_LABEL:
        case OP_PROLOGUE:
        case OP_EPILOGUE:
        case OP_VLA_ALLOC:
        case OP_ATOMIC_XADD:
        case OP_ATOMIC_CMPXCHG:
            return 1;
        
        // Stack operations have side effects
        case OP_PUSH_RAX:
        case OP_PUSH_RBX:
        case OP_POP_RAX:
        case OP_POP_RBX:
        case OP_STACK_ALIGN:
            return 1;
        
        default:
            return 0;
    }
}

/**
 * Mark all temps used by an instruction as live
 */
static void mark_uses(Instr *instr) {
    // Source operands
    if (instr->temp_src1 != 0xFF && instr->temp_src1 != 0) {
        mark_live(instr->temp_src1);
    }
    if (instr->temp_src2 != 0xFF && instr->temp_src2 != 0) {
        mark_live(instr->temp_src2);
    }
}

/**
 * Dead code elimination pass
 */
void opt_eliminate_dead_code(ZeroCCCompiler *c) {
    (void)c;
    
    int count = (int)ir_get_count();
    if (count == 0) {
        return;
    }
    
    clear_liveness();
    
    // Mark return value as live (temp 0 convention)
    mark_live(VREG_0);
    
    // Backward pass: compute liveness
    for (int i = count - 1; i >= 0; i--) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        // If instruction produces a result, mark it live if used
        if (instr->temp_dest != 0xFF && instr->temp_dest != 0) {
            if (is_live(instr->temp_dest)) {
                // Result is used, mark uses as live
                mark_uses(instr);
            } else if (!instr_has_side_effects(instr)) {
                // Result not used and no side effects - mark as dead
                instr->op = OP_DEAD;
            }
        } else {
            // No destination, just mark uses as live
            mark_uses(instr);
        }
    }
    
    // Forward pass: compact IR (remove dead instructions)
    int write_idx = 0;
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        if (instr->op != OP_DEAD) {
            // Keep this instruction
            if (write_idx != i) {
                Instr *dst = ir_get(write_idx);
                *dst = *instr;
            }
            write_idx++;
        }
    }
    
    // Update IR count
    instr_count = (size_t)write_idx;
}
