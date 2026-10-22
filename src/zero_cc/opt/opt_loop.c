#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emitter.h"
#include <stdlib.h>
#include <string.h>

/* External reference to the global compiler pointer (defined in ir.c). */
extern __thread ZeroCCCompiler *g_compiler;

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
 * Loop detection via the CFG layer (Sprint 35 Phase 3).
 * Each CFG back-edge (an edge u->v with blocks[v].start <= blocks[u].start)
 * defines a natural loop: header = blocks[v].start, body = the instruction
 * range [blocks[v].start .. blocks[u].end)  (the back-edge jump is the last
 * instruction of its block, so this is the same set the legacy
 * instruction-index scan [target_idx .. i] produced).
 */
static void detect_loops(LoopAnalysisState *state) {
    int count = (int)ir_get_count();
    state->ir_count = count;
    state->loop_count = 0;

    if (!g_compiler || !cfg_build()) return;
    ZeroCCCFG *cfg = &g_compiler->cfg_cache;

    int nloops = cfg->loop_count;
    if (nloops > MAX_LOOPS) nloops = MAX_LOOPS;

    for (int l = 0; l < nloops; l++) {
        int h = cfg->loops[l].header;
        int u = cfg->loops[l].backedge_from;
        if (h < 0 || h >= cfg->block_count || u < 0 || u >= cfg->block_count) continue;

        LoopInfo *loop = &state->loops[state->loop_count];
        loop->header_idx = (int)cfg->blocks[h].start;
        loop->exit_idx = (int)cfg->blocks[u].end - 1;
        loop->preheader_idx = -1;
        loop->body_count = 0;
        /* is_invariant is indexed by INSTRUCTION index (opt_licm does
         * `invariant[idx]`), so it must be sized by the IR size, not by the
         * body length (pre-existing OOB for IRs > MAX_LOOP_BODY
         * instructions, exposed by the large synthetic function in Sprint
         * 35 Phase 4). body_indices is grown as needed. */
        loop->capacity = count + 1;
        loop->body_indices = malloc(sizeof(int) * MAX_LOOP_BODY);
        loop->is_invariant = calloc((size_t)count + 1, sizeof(bool));

        if (loop->body_indices && loop->is_invariant) {
            int body_cap = MAX_LOOP_BODY;
            bool grow_failed = false;
            for (int j = (int)cfg->blocks[h].start; j < (int)cfg->blocks[u].end; j++) {
                if (loop->body_count >= body_cap) {
                    int nc = body_cap * 2;
                    int *nb = (int *)realloc(loop->body_indices, sizeof(int) * (size_t)nc);
                    if (!nb) { grow_failed = true; break; }
                    loop->body_indices = nb;
                    body_cap = nc;
                }
                loop->body_indices[loop->body_count++] = j;
            }
            if (grow_failed) {
                free(loop->body_indices);
                free(loop->is_invariant);
                loop->body_indices = NULL;
                loop->is_invariant = NULL;
                continue;
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

static bool stack_slot_is_invariant_in_loop(int offset, int *loop_body, int body_count) {
    int ir_count = (int)ir_get_count();
    /* If the address of this stack slot is taken anywhere in the function, it can be modified through pointers */
    for (int i = 0; i < ir_count; i++) {
        Instr *ins = ir_get(i);
        if (ins && ins->op == OP_LEA && (int)ins->imm64 == offset) {
            return false;
        }
    }
    /* Check if any instruction in the loop stores to this stack offset, or performs an indirect store */
    for (int i = 0; i < body_count; i++) {
        Instr *ins = ir_get(loop_body[i]);
        if (!ins) continue;
        if (ins->op == OP_MOV_MEM && (int)ins->imm64 == offset) {
            return false;
        }
        if (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 ||
            ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
            ins->op == OP_FSTORE || ins->op == OP_VSTORE ||
            ins->op == OP_VSTORESS || ins->op == OP_VSTORESD ||
            ins->op == OP_ATOMIC_XADD || ins->op == OP_ATOMIC_CMPXCHG ||
            ins->op == OP_ATOMIC_CMPXCHG_BOOL || ins->op == OP_ATOMIC_XCHG) {
            return false;
        }
    }
    return true;
}

/**
 * Check if an instruction is loop-invariant
 * An instruction is invariant if:
 * 1. It is a pure computational operation (or read-only stack load)
 * 2. Every source operand is either defined outside the loop body or marked invariant.
 * 3. Every source operand defined outside the loop body is defined BEFORE the
 *    loop header (hoisting must not create a use-before-def for sources
 *    defined after the backedge, e.g. via gotos into the loop).
 */
static bool instr_is_invariant(Instr *instr, bool *invariant_map, int *loop_body, int body_count, int header_idx) {
    if (!instr) return false;
    
    /* Only hoist pure, deterministic operations that define a destination temp */
    if (instr->temp_dest== TEMP_NONE) return false;
    
    switch (instr->op) {
        case OP_MOV_IMM:
            return true;
        case OP_MOV:
            /* Stack load: invariant if the stack slot is never modified in the loop and never address-taken */
            if (instr->temp_src1 == TEMP_NONE) {
                return stack_slot_is_invariant_in_loop((int)instr->imm64, loop_body, body_count);
            }
            return false;
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
        case OP_SEXT8:
        case OP_SEXT16:
        case OP_SEXT32:
        case OP_ZEXT8:
        case OP_ZEXT16:
        case OP_ZEXT32:
        case OP_CVTTSD2SI:
        case OP_CVTSI2SD:
            break;
        default:
            return false;
    }

    /* The destination temp must not be modified elsewhere in the loop (must have a single definition) */
    int def_dest_count = 0;
    for (int i = 0; i < body_count; i++) {
        Instr *other = ir_get(loop_body[i]);
        if (other && other->temp_dest == instr->temp_dest) {
            def_dest_count++;
        }
    }
    if (def_dest_count > 1) return false;

    /* In-place self-reference (src == dest, e.g. SEXT32 r, r) reads the
     * pre-loop value of dest.  Hoisting it is sound ONLY for idempotent
     * pure conversions of a value that is constant across the loop (the
     * dest single-definition check above already guarantees no other
     * in-body definition).  In-place BINARY arithmetic (e.g. ADD r, r, 1)
     * is self-referential and loop-dependent: it must NOT be exempted -
     * the source checks below reject it correctly, because its only
     * in-body definition is itself, which is not marked invariant. */
    int opc = (int)instr->op;
    bool is_conversion =
        (opc == OP_SEXT8 || opc == OP_SEXT16 || opc == OP_SEXT32 ||
         opc == OP_ZEXT8 || opc == OP_ZEXT16 || opc == OP_ZEXT32 ||
         opc == OP_NOT || opc == OP_LNOT ||
         opc == OP_CVTTSD2SI || opc == OP_CVTSI2SD);
    bool self_ref_exempt = is_conversion && instr->temp_src1 == instr->temp_dest;
    bool self_ref_exempt2 = is_conversion && instr->temp_src2 == instr->temp_dest;

    /* Verify src1: every definition in the loop must be invariant, and a
     * definition outside the loop must lie before the header. */
    if (instr->temp_src1 != TEMP_NONE && !self_ref_exempt) {
        int def_pos = -1;
        for (int i = 0; i < body_count; i++) {
            Instr *def = ir_get(loop_body[i]);
            if (def && def->temp_dest == instr->temp_src1) {
                if (!invariant_map[loop_body[i]]) return false;
                if (def_pos < 0) def_pos = loop_body[i];
            }
        }
        if (def_pos < 0) {
            int fc = (int)ir_get_count();
            for (int i = 0; i < fc; i++) {
                Instr *def = ir_get(i);
                if (def && def->temp_dest == instr->temp_src1) { def_pos = i; break; }
            }
            if (def_pos < 0 || def_pos >= header_idx) return false;
        }
    }
    
    /* Verify src2: every definition in the loop must be invariant, and a
     * definition outside the loop must lie before the header. */
    if (instr->temp_src2 != TEMP_NONE && !self_ref_exempt2) {
        int def_pos = -1;
        for (int i = 0; i < body_count; i++) {
            Instr *def = ir_get(loop_body[i]);
            if (def && def->temp_dest == instr->temp_src2) {
                if (!invariant_map[loop_body[i]]) return false;
                if (def_pos < 0) def_pos = loop_body[i];
            }
        }
        if (def_pos < 0) {
            int fc = (int)ir_get_count();
            for (int i = 0; i < fc; i++) {
                Instr *def = ir_get(i);
                if (def && def->temp_dest == instr->temp_src2) { def_pos = i; break; }
            }
            if (def_pos < 0 || def_pos >= header_idx) return false;
        }
    }
    
    return true;
}

/**
 * Check if instruction has side effects that prevent moving
 */
static bool instr_has_moving_side_effects(Instr *instr) {
    if (!instr) return true;
    if (instr->temp_dest != TEMP_NONE && instr->temp_dest < VREG_USER_START) return true;
    
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
        
        /* If the loop contains function calls, hoisting loop-invariant constants/computations
         * out of the loop into the preheader extends their live ranges across all calls inside
         * the loop. If there are insufficient callee-saved registers (only 5 on x86-64),
         * caller-saved registers will be assigned and clobbered by the calls!
         * Therefore, do not hoist out of loops containing calls. */
        bool loop_has_call = false;
        for (int i = 0; i < loop->body_count; i++) {
            Instr *ins = ir_get(loop->body_indices[i]);
            if (ins && (ins->op == OP_CALL || ins->op == OP_CALL_INDIRECT)) {
                loop_has_call = true;
                break;
            }
        }
        if (loop_has_call) {
            continue;
        }

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
                
                if (instr_is_invariant(instr, invariant, loop->body_indices, loop->body_count, loop->header_idx)) {
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
    
    /* For each loop header, collect instructions to hoist before it.
     * Multiple natural loops can share one header (multiple backedges into
     * the same label, e.g. `continue` plus the main backedge).  The union of
     * ALL such loops' invariants must be emitted at the header: an invariant
     * marked for hoisting by any of them is NOP'd in place, so failing to
     * emit it here loses the definition (Sprint 35 Phase 0 regression:
     * `MOV_IMM r51, 1` in test/jit/break_continue.c was NOP'd but never
     * hoisted, leaving r51 undefined). */
    bool *emitted_at_header = calloc(ir_count, sizeof(bool));
    for (int i = 0; i < ir_count; i++) {
        /* Emit all invariant instructions belonging to any loop whose
         * header is at i, deduplicated across loops. */
        for (int l = 0; l < state.loop_count; l++) {
            LoopInfo *loop = &state.loops[l];
            if (loop->header_idx != i) continue;
            for (int b = 0; b < loop->body_count; b++) {
                int b_idx = loop->body_indices[b];
                if (loop->is_invariant[b_idx] && !emitted_at_header[b_idx]) {
                    Instr *inv_instr = ir_get(b_idx);
                    if (inv_instr) {
                        ir_arena_emit(&new_arena, *inv_instr);
                        emitted_at_header[b_idx] = true;
                        opt_profile_count_licm_moved();
                    }
                }
            }
        }
        
        /* Emit current instruction or OP_NOP if it was hoisted */
        Instr *orig_instr = ir_get(i);
        if (!orig_instr) continue;
        
        if (global_hoist[i]) {
            /* Replace original in-loop instruction with OP_NOP */
            Instr nop_instr = {
                .op = OP_NOP,
                .temp_dest= TEMP_NONE,
                .temp_src1= TEMP_NONE,
                .temp_src2= TEMP_NONE,
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
    free(emitted_at_header);
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
        if (instr->op == OP_MUL && instr->temp_dest!= TEMP_NONE && instr->temp_dest != 0) {
            // Check if one operand is a constant power of 2
            // For now, we'd need to track which temp holds the constant
            // This is a simplified placeholder
        }
        
        // Check for shift patterns that could be optimized
        if (instr->op == OP_SHL && instr->temp_dest!= TEMP_NONE && instr->temp_dest != 0) {
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
