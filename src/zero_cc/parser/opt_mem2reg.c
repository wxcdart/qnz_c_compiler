#include "opt_mem2reg.h"
#include "ir.h"
#include "ir_arena.h"
#include <stdlib.h>
#include <string.h>

/**
 * Local Memory-to-Register Promotion Pass
 * 
 * Exploits the SSA-like nature of ZeroCC temps:
 * - If a local variable x is assigned and read in straight-line code
 *   without intervening stores or branches, forward the temp directly
 * - Eliminates OP_MOV_MEM + OP_LOAD pairs
 * - Works on the linear IR without needing explicit CFG
 * 
 * Algorithm:
 * 1. Track stack slot -> temp mappings for local variables
 * 2. When seeing OP_MOV_MEM [fp+offset], reg: mark slot as holding reg
 * 3. When seeing OP_LOAD reg, [fp+offset]: replace with MOV reg, known_temp
 * 4. Invalidate mapping on function calls or control flow
 * 
 * This is a lightweight alternative to full SSA construction that
 * provides 80% of the benefit for 20% of the complexity.
 */

/**
 * Maximum stack slots to track (typical function has < 64 locals)
 */
#define MAX_STACK_SLOTS 256

/**
 * Mem2Reg state
 */
typedef struct {
    int slot_to_temp[MAX_STACK_SLOTS];    /* stack offset -> temp holding value */
    bool slot_valid[MAX_STACK_SLOTS];     /* is mapping valid (not invalidated) */
    bool invalidation_pending;            /* need to invalidate all mappings */
    size_t instruction_count;             /* total instructions for bounds */
} Mem2RegState;

/**
 * Get stack slot index from frame offset
 * Frame offsets are typically negative multiples of 8
 */
static int slot_from_offset(int offset) {
    /* Normalize: offset -8 -> slot 0, -16 -> slot 1, etc. */
    return (-offset) / 8;
}

/**
 * Check if instruction invalidates all slot mappings
 * Function calls and control flow can clobber stack slots
 */
static int instr_invalidates_slots(Instr *instr) {
    switch (instr->op) {
        /* Function calls may clobber stack */
        case OP_CALL:
            return 1;
        
        /* Control flow - conservative invalidation */
        case OP_JMP:
        case OP_JMP_LABEL:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
            return 1;
        
        /* Stack pointer manipulation */
        case OP_PUSH:
        case OP_POP:
            return 1;
        
        default:
            return 0;
    }
}

/**
 * Check if instruction is a store to a stack slot
 */
static int instr_is_stack_store(Instr *instr) {
    switch (instr->op) {
        case OP_MOV_MEM:
        case OP_STORE_BYTE:
        case OP_STORE_16:
        case OP_STORE_32:
            return 1;
        default:
            return 0;
    }
}

/**
 * Check if instruction is a load from a stack slot
 */
static int instr_is_stack_load(Instr *instr) {
    switch (instr->op) {
        case OP_MOV:
        case OP_LOAD_BYTE:
        case OP_LOAD_16:
            return 1;
        default:
            return 0;
    }
}

/**
 * Get frame offset from instruction
 */
static int get_frame_offset(Instr *instr) {
    return (int)instr->imm;
}

/**
 * Local Memory-to-Register Promotion pass
 * 
 * This pass exploits the SSA-like property that expression temps are
 * immutable. When we see a store followed by a load from the same
 * stack location, we can forward the temp directly.
 */
void opt_promote_mem2reg(ZeroCCCompiler *c) {
    (void)c;
    
    Mem2RegState state;
    memset(&state, 0, sizeof(state));
    
    state.instruction_count = ir_get_count();
    
    /* Initialize slot tracking */
    for (int i = 0; i < MAX_STACK_SLOTS; i++) {
        state.slot_to_temp[i] = 0xFF;
        state.slot_valid[i] = false;
    }
    
    size_t count = ir_get_count();
    
    for (size_t i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Check for invalidation points */
        if (instr_invalidates_slots(instr)) {
            state.invalidation_pending = true;
            continue;
        }
        
        /* Apply pending invalidation */
        if (state.invalidation_pending) {
            for (int s = 0; s < MAX_STACK_SLOTS; s++) {
                state.slot_valid[s] = false;
            }
            state.invalidation_pending = false;
            continue;
        }
        
        /* Handle stack stores: OP_MOV_MEM [fp+offset], reg */
        if (instr_is_stack_store(instr)) {
            int offset = get_frame_offset(instr);
            int slot = slot_from_offset(offset);
            
            if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                /* Track that this slot now holds the source temp */
                state.slot_to_temp[slot] = instr->temp_src1;
                state.slot_valid[slot] = true;
            }
        }
        
        /* Handle stack loads: OP_LOAD reg, [fp+offset] */
        else if (instr_is_stack_load(instr)) {
            int offset = get_frame_offset(instr);
            int slot = slot_from_offset(offset);
            
            if (slot >= 0 && slot < MAX_STACK_SLOTS && state.slot_valid[slot]) {
                int known_temp = state.slot_to_temp[slot];
                
                /* Check if the known temp is still valid (not overwritten) */
                if (known_temp != 0xFF) {
                    /* Replace load with MOV from known temp */
                    instr->op = OP_MOV_REG;
                    instr->temp_src1 = known_temp;
                    instr->temp_src2 = 0xFF;
                    instr->imm = 0;
                    instr->imm64 = 0;
                    instr->label_idx = 0;
                    instr->cond = 0;
                    instr->flags_out = instr->temp_dest;
                    instr->flags_in = 0xFF;
                }
            }
        }
        
        /* Handle LEA: OP_LEA reg, [fp+offset] */
        else if (instr->op == OP_LEA) {
            int offset = get_frame_offset(instr);
            int slot = slot_from_offset(offset);
            
            /* LEA takes address, so invalidate if we had a value */
            if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                state.slot_valid[slot] = false;
            }
        }
    }
}
