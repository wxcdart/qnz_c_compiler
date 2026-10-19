#include "opt_mem2reg.h"
#include "../ir/ir.h"
#include "../ir/ir_arena.h"
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
static int instr_invalidates_slots(Instr *instr, const bool *label_targeted, int max_label) {
    switch (instr->op) {
        /* Block boundaries */
        case OP_LABEL:
            if (label_targeted && (int)instr->label_idx <= max_label && !label_targeted[instr->label_idx]) {
                /* Pure fallthrough label (no jumps target this label): straight-line execution continues */
                return 0;
            }
            return 1;
        case OP_RET:
        case OP_EPILOGUE:
            return 1;

        /* Function calls may read/modify memory */
        case OP_CALL:
        case OP_CALL_INDIRECT:
            return 1;
        
        /* Unconditional control flow leaves the basic block */
        case OP_JMP:
        case OP_JMP_LABEL:
            return 1;
        
        /* Conditional branches (OP_JE, OP_JNE, OP_JL, OP_JLE, OP_JG, OP_JCOND):
         * The fallthrough path continues straight-line execution without touching memory.
         * The branch target block will be invalidated when its OP_LABEL is encountered. */
        
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
    return (instr->op == OP_MOV && instr->temp_src1== TEMP_NONE);
}

/**
 * Get frame offset from instruction (imm64 holds the full-width value;
 * .imm is int16_t and truncates offsets beyond 32767 in large frames)
 */
static int get_frame_offset(Instr *instr) {
    return (int)instr->imm64;
}

/**
 * Promote loop-carried variables (induction variables and accumulators)
 * across loop boundaries directly into virtual registers.
 *
 * A slot s is eligible for a loop L = [hdr, back] when ALL of the hold:
 *  - the loop is safe (no calls, VLA, or indirect/atomic stores),
 *  - s has exactly one stack load and one stack store inside L,
 *  - there is a store to s in the straight-line preheader (the init),
 *  - no OTHER stack load of s exists between the init and the loop exit,
 *  - no OTHER stack store to s exists anywhere else in the function.
 *
 * The last two conditions guarantee that after the rewrite (init store and
 * in-loop store become register copies, in-loop load becomes a NOP) every
 * remaining load of s is a post-loop load, and that the register copy
 * mirrors the slot on every path from the init onward.  All post-loop
 * loads are then rewritten to read the register.  Without these checks a
 * post-loop load beyond the first label after the exit would keep reading
 * the now-dead stack slot (stale value) - the Sprint 35 Phase 0 regression
 * in test/jit/break_continue.c.
 */
static void opt_promote_loop_carried(void) {
    size_t count = ir_get_count();
    if (count == 0) return;

    /* 1. Track address-taken slots anywhere in the function */
    bool slot_addr_taken[MAX_STACK_SLOTS];
    memset(slot_addr_taken, 0, sizeof(slot_addr_taken));
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (ins && ins->op == OP_LEA) {
            int slot = slot_from_offset(get_frame_offset(ins));
            if (slot >= 0 && slot < MAX_STACK_SLOTS) slot_addr_taken[slot] = true;
        }
    }

    /* 2. Build label to instruction index map */
    int max_label = -1;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx > max_label) {
            max_label = (int)ins->label_idx;
        }
    }
    if (max_label < 0) return;

    int *label_to_pos = (int *)malloc((max_label + 1) * sizeof(int));
    if (!label_to_pos) return;
    for (int i = 0; i <= max_label; i++) label_to_pos[i] = -1;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (ins && ins->op == OP_LABEL) {
            label_to_pos[ins->label_idx] = (int)i;
        }
    }

    /* 3. Detect natural loops: look for backedge jumps */
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (!ins) continue;
        if (ins->op != OP_JMP) continue;

        int lbl = (int)ins->label_idx;
        if (lbl < 0 || lbl > max_label) continue;
        int hdr_idx = label_to_pos[lbl];
        if (hdr_idx < 0 || (size_t)hdr_idx >= i) continue;

        size_t back_idx = i;

        /* Verify loop safety: no calls, no VLA, no indirect stores */
        bool safe_loop = true;
        for (size_t k = hdr_idx; k <= back_idx; k++) {
            Instr *loop_ins = ir_get(k);
            if (!loop_ins) continue;
            if (loop_ins->op == OP_CALL || loop_ins->op == OP_CALL_INDIRECT ||
                loop_ins->op == OP_VLA_ALLOC || loop_ins->op == OP_STORE_BYTE ||
                loop_ins->op == OP_STORE_16 || loop_ins->op == OP_STORE_32 ||
                loop_ins->op == OP_STORE_64 || loop_ins->op == OP_FSTORE ||
                loop_ins->op == OP_VSTORE || loop_ins->op == OP_VSTORESS ||
                loop_ins->op == OP_VSTORESD || loop_ins->op == OP_ATOMIC_XADD ||
                loop_ins->op == OP_ATOMIC_CMPXCHG || loop_ins->op == OP_ATOMIC_CMPXCHG_BOOL ||
                loop_ins->op == OP_ATOMIC_XCHG) {
                safe_loop = false;
                break;
            }
        }
        if (!safe_loop) continue;

        /* No jump from OUTSIDE the loop range may target a label inside it
         * (including the header label): such an entry would skip the
         * preheader init, leaving the promoted head_temp uninitialised on
         * that path where the original in-loop load would have read the
         * slot's pre-loop value (well-defined C, e.g. `x=10; ... goto h;`).
         * Disqualify the whole loop. */
        bool loop_entry_clean = true;
        for (size_t k = 0; k < count && loop_entry_clean; k++) {
            if (k >= hdr_idx && k <= back_idx) continue;
            Instr *j = ir_get(k);
            if (!j) continue;
            if (j->op != OP_JMP && j->op != OP_JE && j->op != OP_JNE &&
                j->op != OP_JL && j->op != OP_JLE && j->op != OP_JG &&
                j->op != OP_JCOND) continue;
            int t = (int)j->label_idx;
            if (t < 0 || t > max_label) continue;
            int tp = label_to_pos[t];
            if (tp >= (int)hdr_idx && tp <= (int)back_idx) {
                loop_entry_clean = false;
            }
        }
        if (!loop_entry_clean) continue;

        /* Find the earliest exit target: any branch or unconditional jump
         * inside the loop whose label lies after the backedge (loop exits
         * may be conditional (loop condition) or unconditional (break)). */
        int first_exit = -1;
        for (size_t k = hdr_idx; k <= back_idx; k++) {
            Instr *loop_ins = ir_get(k);
            if (!loop_ins) continue;
            if (loop_ins->op == OP_JE || loop_ins->op == OP_JNE ||
                loop_ins->op == OP_JL || loop_ins->op == OP_JLE ||
                loop_ins->op == OP_JG || loop_ins->op == OP_JCOND ||
                loop_ins->op == OP_JMP) {
                int t = (int)loop_ins->label_idx;
                if (t < 0 || t > max_label) continue;
                int tp = label_to_pos[t];
                if (tp > (int)back_idx && (first_exit < 0 || tp < first_exit)) {
                    first_exit = tp;
                }
            }
        }

        /* 4. Find candidate slots for loop promotion */
        for (int s = 0; s < MAX_STACK_SLOTS; s++) {
            if (slot_addr_taken[s]) continue;

            /* Count loads and stores of slot s in loop, and audit the rest
             * of the function: only the in-loop load/store and the preheader
             * init may touch the slot; every other load must be post-loop
             * (after first_exit) and there must be no other store at all. */
            int loop_loads = 0;
            int loop_stores = 0;
            int load_idx = -1;
            int store_idx = -1;
            int other_stores = 0;
            int bad_loads = 0;

            for (size_t k = 0; k < count; k++) {
                Instr *f_ins = ir_get(k);
                if (!f_ins) continue;
                bool in_loop = (k >= (size_t)hdr_idx && k <= back_idx);
                if (instr_is_stack_load(f_ins)) {
                    if (slot_from_offset(get_frame_offset(f_ins)) != s) continue;
                    if (in_loop) {
                        loop_loads++;
                        if (load_idx < 0) load_idx = (int)k;
                    } else if (first_exit >= 0 && (int)k > first_exit) {
                        /* post-loop load - will be forwarded */
                    } else {
                        bad_loads++;  /* load before the loop or in a dead zone */
                    }
                } else if (instr_is_stack_store(f_ins)) {
                    if (slot_from_offset(get_frame_offset(f_ins)) != s) continue;
                    if (in_loop) {
                        loop_stores++;
                        store_idx = (int)k;
                    } else {
                        other_stores++;  /* may be the preheader init; checked below */
                    }
                }
            }

            /* Candidate: exactly 1 load and 1 store in loop, no other loads
             * before the exit, no stores outside the loop except the init. */
            if (loop_loads != 1 || loop_stores != 1 || bad_loads > 0 ||
                other_stores != 1) continue;
            if (load_idx < 0 || store_idx < 0) continue;

            Instr *load_ins = ir_get(load_idx);
            Instr *store_ins = ir_get(store_idx);
            if (!load_ins || !store_ins) continue;

            int head_temp = load_ins->temp_dest;
            int update_temp = store_ins->temp_src1;
            if (head_temp < 16 || update_temp < 16) continue;

            /* Find the initialization store: the single out-of-loop store
             * found above.  It must lie before the loop header (a store to
             * the slot after the loop would leave head_temp uninitialised). */
            int init_store_idx = -1;
            for (int k = 0; k < (int)count; k++) {
                if (k >= hdr_idx && (size_t)k <= back_idx) continue;
                Instr *pre_ins = ir_get(k);
                if (pre_ins && instr_is_stack_store(pre_ins) &&
                    slot_from_offset(get_frame_offset(pre_ins)) == s) {
                    init_store_idx = k;
                    break;
                }
            }
            if (init_store_idx < 0 || init_store_idx >= hdr_idx) continue;
            if (first_exit < 0) continue; /* no reachable exit: nothing to forward */

            Instr *init_ins = ir_get(init_store_idx);
            int init_temp = init_ins->temp_src1;
            if (init_temp < 16) continue;

            /* Transformation:
             * 1. Change preheader store: init_temp -> head_temp
             * 2. Remove loop load: load_ins becomes OP_NOP
             * 3. Change loop store: store_ins becomes OP_MOV_REG head_temp, update_temp
             * 4. Forward ALL post-loop loads of the slot to head_temp.
             *    Safe: head_temp mirrors the slot on every path from the init
             *    onward (same init, same update paths), no other store to
             *    the slot exists, and its address was never taken. */
            init_ins->op = OP_MOV_REG;
            init_ins->temp_dest = head_temp;
            init_ins->temp_src1 = init_temp;
            init_ins->temp_src2 = TEMP_NONE;
            init_ins->imm = 0;
            init_ins->imm64 = 0;

            load_ins->op = OP_NOP;
            load_ins->temp_dest = TEMP_NONE;
            load_ins->temp_src1 = TEMP_NONE;
            load_ins->temp_src2 = TEMP_NONE;

            store_ins->op = OP_MOV_REG;
            store_ins->temp_dest = head_temp;
            store_ins->temp_src1 = update_temp;
            store_ins->temp_src2 = TEMP_NONE;
            store_ins->imm = 0;
            store_ins->imm64 = 0;

            for (size_t p = first_exit; p < count; p++) {
                Instr *post_ins = ir_get(p);
                if (!post_ins) continue;
                if (post_ins->op == OP_RET || post_ins->op == OP_EPILOGUE) break;
                if (instr_is_stack_store(post_ins) &&
                    slot_from_offset(get_frame_offset(post_ins)) == s) {
                    break; /* slot re-synced: later loads must read memory */
                }
                if (instr_is_stack_load(post_ins)) {
                    if (slot_from_offset(get_frame_offset(post_ins)) == s) {
                        post_ins->op = OP_MOV_REG;
                        post_ins->temp_src1 = head_temp;
                        post_ins->temp_src2 = TEMP_NONE;
                        post_ins->imm = 0;
                        post_ins->imm64 = 0;
                    }
                }
            }
        }
    }

    free(label_to_pos);
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
    
    /* First promote loop-carried variables across loop boundaries */
    opt_promote_loop_carried();
    
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
    
    /* Pre-pass: scan for max label index and record all jump targets */
    int max_label = -1;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (!ins) continue;
        if (ins->op == OP_LABEL && (int)ins->label_idx > max_label) {
            max_label = (int)ins->label_idx;
        }
    }
    
    bool *label_targeted = NULL;
    if (max_label >= 0) {
        label_targeted = (bool *)calloc(max_label + 1, sizeof(bool));
        if (label_targeted) {
            for (size_t i = 0; i < count; i++) {
                Instr *ins = ir_get(i);
                if (!ins) continue;
                if (ins->op == OP_JMP || ins->op == OP_JE || ins->op == OP_JNE ||
                    ins->op == OP_JL || ins->op == OP_JLE || ins->op == OP_JG ||
                    ins->op == OP_JCOND || ins->op == OP_JMP_LABEL) {
                    if ((int)ins->label_idx <= max_label) {
                        label_targeted[ins->label_idx] = true;
                    }
                }
            }
        }
    }
    
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
        if (instr_invalidates_slots(instr, label_targeted, max_label)) {
            for (int s = 0; s < MAX_STACK_SLOTS; s++) {
                state.slot_valid[s] = false;
            }
            continue;
        }
        
        /* Invalidate any slot holding a temp that is redefined as a destination
         * (unless it's an in-place sign/zero extension of the same temp) */
        if (instr->temp_dest != TEMP_NONE) {
            bool in_place_ext = (instr->op == OP_SEXT8 || instr->op == OP_SEXT16 || instr->op == OP_SEXT32 ||
                                 instr->op == OP_ZEXT8 || instr->op == OP_ZEXT16 || instr->op == OP_ZEXT32) &&
                                (instr->temp_dest == instr->temp_src1);
            if (!in_place_ext) {
                for (int s = 0; s < MAX_STACK_SLOTS; s++) {
                    if (state.slot_valid[s] && state.slot_to_temp[s] == instr->temp_dest) {
                        state.slot_valid[s] = false;
                    }
                }
            }
        }

        /* Handle stack stores: OP_MOV_MEM [fp+offset], reg */
        if (instr_is_stack_store(instr)) {
            int offset = get_frame_offset(instr);
            int slot = slot_from_offset(offset);
            
            if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                if (instr->temp_src1 >= 16) {
                    /* Track that this slot now holds the virtual source temp */
                    state.slot_to_temp[slot] = instr->temp_src1;
                    state.slot_valid[slot] = true;
                } else {
                    state.slot_valid[slot] = false;
                }
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
            
            if (slot >= 0 && slot < MAX_STACK_SLOTS) {
                if (state.slot_valid[slot]) {
                    int known_temp = state.slot_to_temp[slot];
                    /* Check if the known temp is still valid (not overwritten) */
                    if (known_temp != TEMP_NONE) {
                        /* Replace load with MOV from known temp */
                        instr->op = OP_MOV_REG;
                        instr->temp_src1 = known_temp;
                        instr->temp_src2= TEMP_NONE;
                        instr->imm = 0;
                        instr->imm64 = 0;
                        instr->label_idx = 0;
                        instr->cond = 0;
                        instr->flags_out = 0xFF;
                        instr->flags_in = 0xFF;
                    }
                } else if (instr->temp_dest >= 16) {
                    /* Slot wasn't tracked, but this load puts its value into temp_dest.
                     * Any subsequent load from this slot can reuse temp_dest! */
                    state.slot_to_temp[slot] = instr->temp_dest;
                    state.slot_valid[slot] = true;
                }
            }
        }
    }
    
    if (label_targeted) free(label_targeted);
}
