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
        case OP_FSTORE:
        case OP_MOV_MEM_RAX:
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_RET:
        case OP_JMP:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JMP_LABEL:
        case OP_LABEL:
        case OP_PROLOGUE:
        case OP_EPILOGUE:
        case OP_VLA_ALLOC:
        case OP_ATOMIC_XADD:
        case OP_ATOMIC_CMPXCHG:
        case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG:
        case OP_MFENCE:
            return 1;
        
        // Stack operations have side effects
        case OP_PUSH_RAX:
        case OP_PUSH_RBX:
        case OP_POP_RAX:
        case OP_POP_RBX:
        case OP_STACK_ALIGN:
            return 1;
        
        // Parameter saves have side effects (write caller-passed values to stack)
        case OP_PARAM_SAVE_INT:
        case OP_PARAM_SAVE_FP:
        case OP_VARIADIC_SPILL:
            return 1;
        
        default:
            return 0;
    }
}

/**
 * Mark call argument temps as live
 * Handles args 1-2 (temp_src1/src2), args 3-4 (packed in imm), args 5+ (call args table)
 */
static void mark_call_args_live(Instr *instr) {
    // Args 1-2 from temp_src1/temp_src2
    if (instr->temp_src1 != 0xFF && instr->temp_src1 != 0) {
        mark_live(instr->temp_src1);
    }
    if (instr->temp_src2 != 0xFF && instr->temp_src2 != 0) {
        mark_live(instr->temp_src2);
    }
    
    // Args 3-4 packed in imm (lower 16 bits = arg3, upper 16 bits = arg4)
    int arg3 = instr->imm & 0xFFFF;
    int arg4 = (instr->imm >> 16) & 0xFFFF;
    if (arg3 != 0 && arg3 != 0xFF) {
        mark_live(arg3);
    }
    if (arg4 != 0 && arg4 != 0xFF) {
        mark_live(arg4);
    }
    
    // Args 5+ from call args table
    for (size_t t = 0; t < ir_call_args_count; t++) {
        IRCallArgs *entry = &ir_call_args_table[t];
        if (entry && entry->instr_index == (size_t)(instr - ir_get_buffer())) {
            // This is a call arg entry for this instruction
            for (int a = 0; a < entry->count; a++) {
                int arg_temp = entry->temps[a];
                if (arg_temp != 0 && arg_temp != 0xFF) {
                    mark_live(arg_temp);
                }
            }
            break;
        }
    }
}

/**
 * Mark all temps used by an instruction as live
 */
static void mark_uses(Instr *instr) {
    switch (instr->op) {
        case OP_CALL:
            // Special handling for call - mark all argument temps
            mark_call_args_live(instr);
            return;
        
        default:
            // Standard handling for most instructions
            if (instr->temp_src1 != 0xFF && instr->temp_src1 != 0) {
                mark_live(instr->temp_src1);
            }
            if (instr->temp_src2 != 0xFF && instr->temp_src2 != 0) {
                mark_live(instr->temp_src2);
            }
            return;
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
    
    // Mark return value temps as live
    // VREG_0 = rax (integer return)
    // VREG_2 = rdx (upper 8 bytes for 9-16 byte struct return)
    mark_live(VREG_0);
    mark_live(VREG_2);
    
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
                
                // Update call args table index if this is a call
                if (instr->op == OP_CALL) {
                    for (size_t t = 0; t < ir_call_args_count; t++) {
                        IRCallArgs *entry = &ir_call_args_table[t];
                        if (entry && entry->instr_index == (size_t)i) {
                            entry->instr_index = write_idx;
                            break;
                        }
                    }
                }
            }
            write_idx++;
        }
    }
    
    // Update IR count
    instr_count = (size_t)write_idx;
}
