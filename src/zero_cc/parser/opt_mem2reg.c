#include "opt_mem2reg.h"
#include "ir.h"
#include "ir_arena.h"
#include <stdlib.h>
#include <string.h>

/**
 * Local Memory-to-Register Promotion Pass (Safe Version)
 * 
 * Exploits the SSA-like nature of ZeroCC temps with safety checks:
 * - Only promotes local scalar variables with STATIC stack offsets (not VLA)
 * - Tracks VLA via OP_VLA_ALLOC (all slots become dynamic after VLA)
 * 
 * This enables mem2reg to work safely alongside VLA while still
 * providing optimization benefits for regular local variables.
 * 
 * Algorithm:
 * 1. Track stack slot -> temp mappings for local variables
 * 2. Mark all slots as "dynamic" when OP_VLA_ALLOC is seen (conservative)
 * 3. Only promote if slot is valid and not dynamic
 * 4. Invalidate on function calls or control flow
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
    bool has_vla;                         /* has VLA been seen? */
    bool invalidation_pending;            /* need to invalidate all mappings */
    size_t instruction_count;             /* total instructions for bounds */
} Mem2RegState;

/**
 * Get stack slot index from frame offset
 * Frame offsets are typically negative multiples of 8
 */
static int slot_from_offset(int offset) {
    /* Stack offsets are negative multiples of 8 */
    if (offset >= 0 || (offset % 8) != 0) return -1;
    return (-offset) / 8;
}

/**
 * Check if instruction invalidates all slot mappings
 * Function calls, control flow, and block boundaries clobber / reset local slot tracking.
 */
static int instr_invalidates_slots(Instr *instr) {
    switch (instr->op) {
        /* Block boundaries */
        case OP_LABEL:
        case OP_RET:
        case OP_EPILOGUE:
            return 1;

        /* Function calls may read/modify memory */
        case OP_CALL:
        case OP_CALL_INDIRECT:
            return 1;
        
        /* Control flow */
        case OP_JMP:
        case OP_JMP_LABEL:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
            return 1;
        
        /* Any indirect memory write or other store may alias local stack slots */
        case OP_STORE_BYTE:
        case OP_STORE_16:
        case OP_STORE_32:
        case OP_STORE_64:
        case OP_FSTORE:
        case OP_VSTORE:
        case OP_VSTORESS:
        case OP_VSTORESD:
        case OP_ATOMIC_XADD:
        case OP_ATOMIC_CMPXCHG:
        case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG:
            return 1;

        /* Address taken of a stack slot (via LEA) means it can be modified through pointer */
        case OP_LEA:
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
    return (instr->op == OP_MOV_MEM);
}

/**
 * Check if instruction is a load from a stack slot
 */
static int instr_is_stack_load(Instr *instr) {
    return (instr->op == OP_MOV && instr->temp_src1 == 0xFF);
}

/**
 * Get frame offset from instruction
 */
static int get_frame_offset(Instr *instr) {
    return (int)instr->imm;
}

/**
 * Local Memory-to-Register Promotion pass (safe version)
 * 
 * This pass exploits the SSA-like property that expression temps are
 * immutable. When we see a store followed by a load from the same
 * stack location, we can forward the temp directly.
 * 
 * SAFETY CHECKS:
 * - Skip all slots if VLA has been seen (conservative)
 * - Only promote simple scalar variables
 */
void opt_promote_mem2reg(ZeroCCCompiler *c) {
    (void)c;
    
    Mem2RegState state;
    memset(&state, 0, sizeof(state));
    
    state.instruction_count = ir_get_count();
    state.has_vla = false;  /* No VLA yet */
    
    /* Initialize slot tracking */
    for (int i = 0; i < MAX_STACK_SLOTS; i++) {
        state.slot_to_temp[i] = 0xFF;
        state.slot_valid[i] = false;
    }
    
    size_t count = ir_get_count();
    
    for (size_t i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Check for VLA allocation - disable all promotion after this */
        if (instr->op == OP_VLA_ALLOC) {
            state.has_vla = true;
            for (int s = 0; s < MAX_STACK_SLOTS; s++) {
                state.slot_valid[s] = false;
            }
            continue;
        }
        
        /* Skip if VLA has been seen - all slots are potentially dynamic */
        if (state.has_vla) {
            continue;
        }
        
        /* Check for invalidation points (control flow, calls, stores through pointers, etc.) */
        if (instr_invalidates_slots(instr)) {
            for (int s = 0; s < MAX_STACK_SLOTS; s++) {
                state.slot_valid[s] = false;
            }
            continue;
        }
        
        /* Invalidate any slot holding a temp that is redefined as a destination */
        if (instr->temp_dest != 0xFF) {
            for (int s = 0; s < MAX_STACK_SLOTS; s++) {
                if (state.slot_valid[s] && state.slot_to_temp[s] == instr->temp_dest) {
                    state.slot_valid[s] = false;
                }
            }
        }

        /* Handle stack stores: OP_MOV_MEM [fp+offset], reg */
        if (instr_is_stack_store(instr)) {
            int offset = get_frame_offset(instr);
            int slot = slot_from_offset(offset);
            
            if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                /* Track that this slot now holds the source temp */
                state.slot_to_temp[slot] = instr->temp_src1;
                state.slot_valid[slot] = true;
            } else if (offset < 0) {
                /* Non-8-byte store to stack (e.g. struct member at unaligned offset):
                 * Invalidate any overlapping 8-byte slots so stale 64-bit values aren't forwarded! */
                int s1 = (-offset) / 8;
                int s2 = (-offset + 7) / 8;
                if (s1 >= 0 && s1 < MAX_STACK_SLOTS) state.slot_valid[s1] = false;
                if (s2 >= 0 && s2 < MAX_STACK_SLOTS) state.slot_valid[s2] = false;
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
                    instr->flags_out = 0xFF;
                    instr->flags_in = 0xFF;
                }
            }
        }
    }
}
