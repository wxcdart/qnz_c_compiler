#include "zero_cc/opt/opt_dce.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/types/symbols.h"
#include <stdlib.h>
#include <string.h>

/* External reference to global compiler pointer */
extern __thread ZeroCCCompiler *g_compiler;

/**
 * Flag DCE: Remove CMP instructions whose flags are never used
 *
 * This pass runs BEFORE general DCE and specifically targets flag-setting
 * instructions (OP_CMP, OP_CMP_ZERO) that have no flag-reading successors.
 */
void opt_flag_dce(ZeroCCCompiler *c) {
    (void)c;

    int count = (int)ir_get_count();
    if (count == 0) {
        return;
    }

    // Forward pass: find dead flag-setting instructions
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;

        // Only analyze flag-setting instructions
        if (instr->op != OP_CMP && instr->op != OP_CMP_ZERO) {
            continue;
        }

        // Check if flags are used by subsequent instructions
        bool flags_used = false;
        for (int j = i + 1; j < count; j++) {
            Instr *next = ir_get(j);
            if (!next) continue;

            // Stop at control flow boundary
            if (next->op == OP_JMP || next->op == OP_JCOND ||
                next->op == OP_JE || next->op == OP_JNE ||
                next->op == OP_JL || next->op == OP_JLE ||
                next->op == OP_JG || next->op == OP_RET ||
                next->op == OP_LABEL || next->op == OP_CALL ||
                next->op == OP_SYSCALL) {
                break;
            }

            // Check if this instruction reads flags
            if (next->flags_in == REG_FLAGS) {
                flags_used = true;
                break;
            }
        }

        // Mark as dead if flags never used
        if (!flags_used) {
            instr->op = OP_DEAD;
        }
    }
}

/**
 * Liveness tracking
 */
#define MAX_VREGS 65536  /* C2b: full 16-bit temp-id space */
#define LIVENESS_MAX_SWEEPS 10
static bool g_live_temp[MAX_VREGS];
static bool g_live_prev[MAX_VREGS];

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
 * Note: temp 0 (VREG_0/rax) is a valid temporary, not "no temp"
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
        case OP_MOV_MEM:
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_SYSCALL:
        case OP_RET:
        case OP_JMP:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
        case OP_JMP_LABEL:
        case OP_LABEL:
        case OP_EPILOGUE:
        case OP_VLA_ALLOC:
        case OP_ATOMIC_XADD:
        case OP_ATOMIC_OR:
        case OP_ATOMIC_AND:
        case OP_ATOMIC_XOR:
        case OP_ATOMIC_CMPXCHG:
        case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG:
        case OP_MFENCE:
        // Comparison/flag-setting instructions have side effects (affect jumps)
        case OP_CMP:
        case OP_CMP_ZERO:
        case OP_COND_SET:
            return 1;

        // Stack operations have side effects
        case OP_PUSH:
        case OP_POP:
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
    int arg_count = instr->temp_dest;

    // Args 1-2 from temp_src1/temp_src2
    if (arg_count > 0 && instr->temp_src1!= TEMP_NONE) {
        mark_live(instr->temp_src1);
    }
    if (arg_count > 1 && instr->temp_src2!= TEMP_NONE) {
        mark_live(instr->temp_src2);
    }

    // Args 3-4 packed in imm (byte 0 = arg3, byte 1 = arg4)
    // Note: instr->imm is int16_t, so we extract bytes, not 16-bit words
    if (arg_count > 2) {
        int arg3 = (instr->imm >> 0) & 0xFF;
        if (arg3 != 0xFF) {
            mark_live(arg3);
        }
    }
    if (arg_count > 3) {
        int arg4 = (instr->imm >> 8) & 0xFF;
        if (arg4 != 0xFF) {
            mark_live(arg4);
        }
    }

    // Args 5+ from call args table
    if (g_compiler) {
        IRCallArgsArena *call_arena = &g_compiler->ir_call_args;
        for (size_t t = 0; t < call_arena->count; t++) {
            IRCallArgs *entry = &call_arena->table[t];
            if (entry && entry->instr_index == (size_t)(instr - ir_get_buffer())) {
                // This is a call arg entry for this instruction
                for (int a = 0; a < entry->count; a++) {
                    int arg_temp = entry->temps[a];
                    if (arg_temp != TEMP_NONE) {
                        mark_live(arg_temp);
                    }
                }
                break;
            }
        }
    }
}

/**
 * Mark all temps used by an instruction as live
 */
static void mark_uses(Instr *instr) {
    switch (instr->op) {
        case OP_CALL:
        case OP_SYSCALL:
            // Special handling for call/syscall - mark all argument temps
            mark_call_args_live(instr);
            return;

        case OP_ATOMIC_CMPXCHG:
        case OP_ATOMIC_CMPXCHG_BOOL:
            // Atomic compare-and-exchange uses label_idx as the expected operand temp
            if (instr->temp_src1!= TEMP_NONE) {
                mark_live(instr->temp_src1);
            }
            if (instr->temp_src2!= TEMP_NONE) {
                mark_live(instr->temp_src2);
            }
            // label_idx holds the expected temp (not a label!)
            if (instr->label_idx != TEMP_NONE) {
                mark_live(instr->label_idx);
            }
            return;

        default:
            // Standard handling for most instructions
            // Note: temp 0 (VREG_0/rax) is valid, only 0xFF means "no temp"
            if (instr->temp_src1!= TEMP_NONE) {
                mark_live(instr->temp_src1);
            }
            if (instr->temp_src2!= TEMP_NONE) {
                mark_live(instr->temp_src2);
            }
            return;
    }
}

/**
 * Run one backward liveness sweep (returns 1 on success).
 * Fills g_live_temp; caller manages clearing/re-marking roots.
 */
static int liveness_backward_sweep(int count) {
    for (int i = count - 1; i >= 0; i--) {
        Instr *instr = ir_get(i);
        if (!instr || instr->op == OP_DEAD) continue;

        /* Special handling for OP_CALL/OP_SYSCALL: temp_dest holds arg_count,
         * not a temp ID. Always mark arguments as live - both have side effects. */
        if (instr->op == OP_CALL || instr->op == OP_SYSCALL) {
            mark_uses(instr);
            continue;
        }

        /* If the instruction's result is live at this point (or the
         * instruction must execute for its side effects), its sources are
         * live too. */
        if (instr->temp_dest != TEMP_NONE) {
            if (is_live(instr->temp_dest) || instr_has_side_effects(instr)) {
                mark_uses(instr);
            }
        } else {
            mark_uses(instr);
        }
    }
    return 1;
}

/**
 * Compute liveness to a fixpoint.
 *
 * A single backward sweep is NOT sufficient in the presence of loops: a
 * temp defined at the bottom of a loop (loop-carried value, e.g. an
 * induction variable promoted by mem2reg) and used at the top of the loop
 * appears dead at its definition on the first sweep, so DCE would delete
 * the update and turn the loop infinite.  Each additional backward sweep
 * propagates liveness one more backedge, so iterate until the live set
 * stops growing.  If it does not converge within the bound, give up on
 * liveness entirely (mark everything live) - DCE must stay conservative.
 * Returns 1 if liveness is valid, 0 if DCE should skip deletion.
 */
static int compute_liveness_fixpoint(int count) {
    clear_liveness();
    /* Return values are live at function exit */
    mark_live(VREG_0);
    mark_live(VREG_2);
    for (int sweep = 0; sweep < LIVENESS_MAX_SWEEPS; sweep++) {
        /* Mark the float/double return value (xmm0) live at exit */
        for (int i = count - 1; i >= 0; i--) {
            Instr *instr = ir_get(i);
            if (!instr) continue;
            if (instr->temp_dest != TEMP_NONE &&
                (ir_opcode_is_float(instr->op) || ir_opcode_is_simd(instr->op) ||
                 instr->op == OP_CVTSI2SD)) {
                mark_live(instr->temp_dest);
                break;
            }
        }
        liveness_backward_sweep(count);
        if (sweep > 0 && memcmp(g_live_prev, g_live_temp, sizeof(g_live_temp)) == 0) {
            return 1; /* converged */
        }
        memcpy(g_live_prev, g_live_temp, sizeof(g_live_temp));
    }
    return 0; /* did not converge: too deeply nested loop-carried values */
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

    /* Phase 1: liveness to a fixpoint (see compute_liveness_fixpoint). */
    if (!compute_liveness_fixpoint(count)) {
        /* Non-convergent liveness: mark everything live so that nothing
         * is deleted.  DCE is an optimization; it must never delete a
         * live instruction. */
        memset(g_live_temp, 1, sizeof(g_live_temp));
    }

    /* Phase 2: mark instructions whose result is dead and which have no
     * side effects. */
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr || instr->op == OP_DEAD) continue;
        if (instr->op == OP_CALL || instr->op == OP_SYSCALL) continue; /* calls always kept */
        if (instr->temp_dest == TEMP_NONE) continue; /* no result to be dead */
        if (is_live(instr->temp_dest) || instr_has_side_effects(instr)) continue;
        instr->op = OP_DEAD;
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

                // Update call args table index if this is a call/syscall
                if ((instr->op == OP_CALL || instr->op == OP_SYSCALL) && g_compiler) {
                    IRCallArgsArena *call_arena = &g_compiler->ir_call_args;
                    for (size_t t = 0; t < call_arena->count; t++) {
                        IRCallArgs *entry = &call_arena->table[t];
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
    if (g_compiler) {
        g_compiler->ir_arena.count = (size_t)write_idx;
    }
}
