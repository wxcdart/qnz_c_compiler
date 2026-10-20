#include "opt_vectorize.h"
#include "opt_loop.h"
#include "opt_constant_fold.h"
#include "cfg.h"
#include "../ir/ir.h"
#include "../ir/ir_arena.h"
#include "../ir/ir_emit_ops.h"
#include "../ir/regalloc.h"
#include "../types/symbols.h"
#include "../isa/isa_backend.h"
#include <stdlib.h>
#include <string.h>

/**
 * Auto-vectorization pass implementation
 * 
 * Transforms scalar loops into vectorized SIMD loops.
 * 
 * Algorithm:
 * 1. Detect countable loops over arrays
 * 2. Check for vectorizable patterns (element-wise ops)
 * 3. Transform to vector operations with loop unrolling
 * 4. Generate tail loop for remainder iterations
 */

#define MAX_VEC_BODY 256

/**
 * Vectorization state for tracking vector temps and mappings
 */
typedef struct {
    int scalar_temp;      /* Original scalar temp */
    int vector_temp;      /* Vector temp (xmm register) */
    int temp_count;       /* Number of elements in vector */
} VectorTempMap;

/**
 * Full vectorization state
 */
typedef struct {
    VectorizationInfo loops[MAX_VECTOR_LOOPS];
    int loop_count;
    int capacity;  /* Maximum number of loops */
    int vector_width;
    int target_isa;
    
    /* Vector temp mappings */
    VectorTempMap vec_map[256];
    int vec_map_count;
    
    /* Loop body indices */
    int *body_indices;
    size_t body_count;
} VectorizationStateFull;

/**
 * Get vector width for target ISA
 * x86-64 SSE2: 4x float32, 2x float64
 * AArch64 NEON: 4x float32, 2x float64
 * RISC-V RVV: 4x float32 (fixed for simplicity)
 */
int vectorization_get_width(int target_isa) {
    (void)target_isa;
    return 4;  /* Default: 4x float32 vectors */
}

/**
 * Initialize vectorization state
 */
void vectorization_init(VectorizationState *state, int target_isa) {
    memset(state, 0, sizeof(*state));
    state->loop_count = 0;
    state->capacity = MAX_VECTOR_LOOPS;
    state->vector_width = vectorization_get_width(target_isa);
    state->target_isa = target_isa;
    
    for (int i = 0; i < MAX_VECTOR_LOOPS; i++) {
        state->loops[i].is_vectorizable = false;
        state->loops[i].vector_width = state->vector_width;
        state->loops[i].loop_var_temp = -1;
        state->loops[i].array_base_temp = -1;
        state->loops[i].array_stride = 4;  /* float32 */
        state->loops[i].body_start_idx = -1;
        state->loops[i].body_end_idx = -1;
        state->loops[i].iter_count_temp = -1;
    }
}

/**
 * Free vectorization resources
 */
void vectorization_free(VectorizationState *state) {
    state->loop_count = 0;
}

/**
 * Check if an instruction is a simple array load
 * Pattern: temp = load [base + loop_var * stride]
 */
static bool is_array_load(Instr *instr, int base_temp, int loop_var, int stride) {
    if (!instr) return false;
    
    /* Check for FLOAD or LOAD_32 pattern */
    if (instr->op != OP_FLOAD && instr->op != OP_LOAD_32) {
        return false;
    }
    
    /* Check if source matches base + loop_var * stride pattern */
    /* For simplicity, we check if src1 is the base and offset is related to loop_var */
    if (instr->temp_src1 == base_temp) {
        /* Offset should be loop_var * stride */
        if (instr->imm == loop_var * stride) {
            return true;
        }
    }
    
    return false;
}

/**
 * Check if an instruction is a simple array store
 * Pattern: store temp, [base + loop_var * stride]
 */
static bool is_array_store(Instr *instr, int base_temp, int loop_var, int stride) {
    if (!instr) return false;
    
    if (instr->op != OP_FSTORE && instr->op != OP_STORE_32) {
        return false;
    }
    
    if (instr->temp_src1 == base_temp && instr->imm == loop_var * stride) {
        return true;
    }
    
    return false;
}

/**
 * Check if instruction is a vectorizable arithmetic operation
 * Pattern: dest = src1 + src2 (or -, *)
 */
static bool is_vec_arith(Instr *instr) {
    if (!instr) return false;
    
    switch (instr->op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
            return true;
        default:
            return false;
    }
}

/**
 * Analyze a single loop for vectorization opportunities
 */
static void analyze_loop_for_vectorization_full(VectorizationStateFull *state, LoopInfo *loop) {
    if (state->loop_count >= state->capacity) return;
    
    VectorizationInfo *vinfo = &state->loops[state->loop_count];
    vinfo->is_vectorizable = false;
    vinfo->body_start_idx = loop->header_idx;
    vinfo->body_end_idx = loop->exit_idx;
    
    /* Simple heuristic: check if loop body contains array load/store + arithmetic */
    int load_count = 0;
    int store_count = 0;
    int arith_count = 0;
    int base_temp = -1;
    int loop_var = -1;
    bool has_double = false;
    bool has_fp = false;
    bool has_call = false;   /* C11: calls/returns in the body make it non-vectorizable */
    bool has_mid_body_cond = false;  /* Vectorizer hardening: conditional branches inside the loop body (not the backedge) make it non-vectorizable */
    
    for (int i = 0; i < loop->body_count; i++) {
        int idx = loop->body_indices[i];
        Instr *instr = ir_get(idx);
        if (!instr) continue;
        
        /* C11: a function call (or return) in the loop body is a side effect
         * that cannot be replicated across vector lanes - the transform
         * would still widen the induction step (i += 1 -> i += vec_width)
         * while the scalar body (with the call) ran only once per
         * vec_width iterations, dropping loop iterations. Reject such loops. */
        if (instr->op == OP_CALL || instr->op == OP_CALL_INDIRECT || instr->op == OP_RET) {
            has_call = true;
            continue;
        }
        
        /* Look for array access pattern */
        if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD || instr->op == OP_VLOADSS || instr->op == OP_VLOADSD) {
            load_count++;
            if (base_temp < 0) {
                base_temp = instr->temp_src1;
            }
            if (instr->op == OP_VLOADSD) {
                has_double = true;
                has_fp = true;
            } else if (instr->op == OP_FLOAD || instr->op == OP_VLOADSS) {
                has_fp = true;
            }
        }
        if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE || instr->op == OP_VSTORESS || instr->op == OP_VSTORESD) {
            store_count++;
            if (instr->op == OP_VSTORESD) {
                has_double = true;
                has_fp = true;
            } else if (instr->op == OP_FSTORE || instr->op == OP_VSTORESS) {
                has_fp = true;
            }
        }
        if (instr->op == OP_FADD || instr->op == OP_FSUB || instr->op == OP_FMUL || instr->op == OP_FDIV) {
            has_fp = true;
        }
        if (is_vec_arith(instr)) {
            arith_count++;
        }
        
        /* Look for loop variable increment */
        if (instr->op == OP_ADD) {
            if (instr->imm == 1) {
                loop_var = instr->temp_src1;
            } else if (instr->temp_src2!= TEMP_NONE && ir_is_constant(instr->temp_src2) && ir_get_constant(instr->temp_src2) == 1) {
                loop_var = instr->temp_src1;
            }
        }
    }
    
    /* Vectorizer hardening (Sprint 35 Phase 3): CFG-based mid-body
     * conditional check.  The legacy per-instruction span scan flagged the
     * loop's OWN header test (a forward JE-to-exit inside the span) as a
     * mid-body conditional, so while-style loops were never vectorized.
     * The CFG check is precise: a conditional jump is permitted only if it
     * IS the backedge (the block ending at loop->exit_idx) or it is the
     * header block's loop test whose target lies outside the loop span
     * [header_idx, exit_idx].  Any other conditional in the span is
     * data-dependent control flow (e.g. an if/else in the body) and makes
     * the loop non-vectorizable. */
    if (g_compiler && cfg_build()) {
        ZeroCCCFG *cfg = &g_compiler->cfg_cache;
        for (int b = 0; b < cfg->block_count && !has_mid_body_cond; b++) {
            ZeroCCBlock *blk = &cfg->blocks[b];
            if (blk->end <= (size_t)loop->header_idx) continue;  /* before span */
            if (blk->start > (size_t)loop->exit_idx) break;     /* past span */
            int last = (int)blk->end - 1;
            Instr *t = ir_get(last);
            if (!t) continue;
            if (last == loop->exit_idx) continue;  /* the backedge itself */
            int op = (int)t->op;
            if (!(op == OP_JE || op == OP_JNE || op == OP_JL || op == OP_JLE ||
                  op == OP_JG || op == OP_JCOND)) continue;
            /* Header block's loop test targeting outside the span: OK. */
            if ((int)blk->start == loop->header_idx) {
                int lid = (int)t->label_idx;
                int target_pos = -1;
                int ic = (int)ir_get_count();
                for (int k = 0; k < ic; k++) {
                    Instr *l = ir_get(k);
                    if (l && l->op == OP_LABEL && (int)l->label_idx == lid) {
                        target_pos = k;
                        break;
                    }
                }
                if (target_pos >= 0 &&
                    (target_pos < loop->header_idx || target_pos > loop->exit_idx)) {
                    continue;
                }
            }
            has_mid_body_cond = true;
        }
    }
    
    /* Vectorizer hardening (Sprint 35 Phase 3): loop-bound safety. The
     * transform widens the induction step (i += 1 -> i += vec_width) and
     * processes vec_width elements per iteration with NO remainder
     * handling: if the loop count is not a multiple of vec_width, the
     * final VLOAD/VSTORE overflows the array (stack OOB read/write).
     * The loop is only accepted when its bound is a compile-time constant
     * that is a positive multiple of vec_width. Variable bounds, FP
     * bounds, and infinite loops (no verifiable test) are not vectorized.
     * (Identified via the loop test: header block's terminal conditional
     * targeting the exit, or a conditional backedge; chain
     * branch -> COND_SET -> CMP -> constant operand.) */
    bool bound_ok = false;
    {
        int test_idx = -1;
        if (g_compiler && cfg_build()) {
            ZeroCCCFG *cfg = &g_compiler->cfg_cache;
            for (int b = 0; b < cfg->block_count && test_idx < 0; b++) {
                ZeroCCBlock *blk = &cfg->blocks[b];
                if (blk->end <= (size_t)loop->header_idx) continue;
                if (blk->start > (size_t)loop->exit_idx) break;
                int last = (int)blk->end - 1;
                Instr *t = ir_get(last);
                if (!t) continue;
                int op = (int)t->op;
                bool is_cond = (op == OP_JE || op == OP_JNE || op == OP_JL ||
                                op == OP_JLE || op == OP_JG || op == OP_JCOND);
                if (!is_cond) continue;
                if (last == loop->exit_idx) {
                    test_idx = last;  /* conditional backedge (tail test) */
                    break;
                }
                if ((int)blk->start == loop->header_idx) {
                    int lid = (int)t->label_idx;
                    int ic = (int)ir_get_count();
                    for (int k = 0; k < ic; k++) {
                        Instr *l = ir_get(k);
                        if (l && l->op == OP_LABEL && (int)l->label_idx == lid) {
                            if (k > loop->exit_idx) test_idx = last; /* test -> exit */
                            break;
                        }
                    }
                }
            }
        }
        if (test_idx >= 0) {
            int flag = (int)ir_get(test_idx)->temp_src1;
            int condset_idx = -1;
            for (int k = test_idx - 1; k > loop->header_idx && condset_idx < 0; k--) {
                Instr *cs = ir_get(k);
                if (cs && cs->op == OP_COND_SET && (int)cs->temp_dest == flag) condset_idx = k;
            }
            int hi = (condset_idx >= 0) ? condset_idx : test_idx;
            int cmp_idx = -1;
            for (int k = hi - 1; k > loop->header_idx; k--) {
                Instr *cm = ir_get(k);
                if (cm && cm->op == OP_CMP) { cmp_idx = k; break; }
            }
            if (cmp_idx >= 0) {
                Instr *cm = ir_get(cmp_idx);
                int o1 = (int)cm->temp_src1;
                int o2 = (int)cm->temp_src2;
                bool c1 = (o1 != TEMP_NONE && ir_is_constant(o1));
                bool c2 = (o2 != TEMP_NONE && ir_is_constant(o2));
                if (c1 != c2) {
                    int bound_temp = c1 ? o1 : o2;
                    int width = has_double ? 2 : state->vector_width;
                    int bound = (int)ir_get_constant(bound_temp);
                    bound_ok = (bound > 0 && bound % width == 0);
                }
            }
        }
    }
    
    /* Vectorization criteria:
     * - At least 2 array loads (read operands)
     * - At least 1 array store (write result inside the loop body)
     * - At least 1 arithmetic operation
     * - No function calls or complex side effects (C11: actually enforced via has_call)
     * - No mid-body conditional branches (vectorizer hardening)
     */
    if (!has_call && !has_mid_body_cond && bound_ok && load_count >= 2 && store_count >= 1 && arith_count >= 1) {
        vinfo->is_vectorizable = true;
        vinfo->is_double = has_double;
        vinfo->is_int32 = !has_fp && !has_double;
        vinfo->loop_var_temp = loop_var;
        vinfo->array_base_temp = base_temp;
        if (has_double) {
            vinfo->vector_width = 2;  /* 2x 64-bit doubles in 128-bit vector */
            vinfo->elem_size = 8;
            vinfo->array_stride = 8;
        } else {
            vinfo->vector_width = state->vector_width;  /* 4x 32-bit floats or ints */
            vinfo->elem_size = 4;
            vinfo->array_stride = 4;
        }
        state->loop_count++;
    }
}

/**
 * Detect loops and analyze for vectorization
 */
void vectorization_analyze_full(VectorizationStateFull *state) {
    int count = (int)ir_get_count();
    
    /* Build label-to-instruction-index map */
    int label_to_idx[256];
    for (int i = 0; i < 256; i++) label_to_idx[i] = -1;
    
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        if (instr->op == OP_LABEL && instr->label_idx < 256) {
            label_to_idx[instr->label_idx] = i;
        }
    }
    
    /* Collect all backedges first so we can identify innermost loops */
    typedef struct {
        int target;
        int backedge_idx;
    } Backedge;
    Backedge backedges[MAX_VECTOR_LOOPS * 4];
    int num_backedges = 0;

    for (int i = 0; i < count && num_backedges < (int)(sizeof(backedges) / sizeof(backedges[0])); i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        int is_jump = (instr->op == OP_JMP || instr->op == OP_JE || 
            instr->op == OP_JNE || instr->op == OP_JL ||
            instr->op == OP_JLE || instr->op == OP_JG);
        if (is_jump) {
            int label = instr->label_idx;
            int target = (label >= 0 && label < 256) ? label_to_idx[label] : -1;
            if (target >= 0 && target < i) {
                backedges[num_backedges].target = target;
                backedges[num_backedges].backedge_idx = i;
                num_backedges++;
            }
        }
    }

    /* Only analyze innermost loops (loops that do not contain another loop/backedge inside them) */
    for (int b = 0; b < num_backedges && state->loop_count < state->capacity; b++) {
        int target = backedges[b].target;
        int i = backedges[b].backedge_idx;

        bool has_inner_backedge = false;
        for (int ob = 0; ob < num_backedges; ob++) {
            if (ob == b) continue;
            int t2 = backedges[ob].target;
            int i2 = backedges[ob].backedge_idx;
            /* If another loop (t2..i2) is strictly or partially inside target..i */
            if ((t2 > target && i2 <= i) || (t2 >= target && i2 < i)) {
                has_inner_backedge = true;
                break;
            }
        }
        if (has_inner_backedge) {
            continue;
        }

        /* Found potential innermost loop */
        LoopInfo loop = {0};
        loop.header_idx = target;
        loop.exit_idx = i;
        loop.body_count = 0;
        loop.capacity = MAX_VEC_BODY;
        loop.body_indices = malloc(sizeof(int) * MAX_VEC_BODY);

        for (int j = target; j <= i; j++) {
            if (loop.body_count < MAX_VEC_BODY) {
                loop.body_indices[loop.body_count++] = j;
            }
        }

        /* Analyze for vectorization */
        analyze_loop_for_vectorization_full(state, &loop);

        free(loop.body_indices);
    }
}

/**
 * Get vector operation for scalar operation
 */
static int get_vec_op(int scalar_op, bool is_double, bool is_int32) {
    if (is_double) {
        switch (scalar_op) {
            case OP_ADD:
            case OP_FADD: return OP_VADDPD;
            case OP_SUB:
            case OP_FSUB: return OP_VSUBPD;
            case OP_MUL:
            case OP_FMUL: return OP_VMULPD;
            case OP_DIV:
            case OP_FDIV: return OP_VDIVPD;
            default:      return scalar_op;
        }
    } else if (is_int32) {
        switch (scalar_op) {
            case OP_ADD:  return OP_VPADDD;
            case OP_SUB:  return OP_VPSUBD;
            case OP_MUL:  return OP_VPMULLD;
            default:      return scalar_op;
        }
    } else {
        switch (scalar_op) {
            case OP_ADD:  return OP_VADDPS;
            case OP_SUB:  return OP_VSUBPS;
            case OP_MUL:  return OP_VMULPS;
            case OP_DIV:  return OP_VDIVPS;
            case OP_FADD: return OP_VADDPS;
            case OP_FSUB: return OP_VSUBPS;
            case OP_FMUL: return OP_VMULPS;
            case OP_FDIV: return OP_VDIVPS;
            default:      return scalar_op;
        }
    }
}

/**
 * Transform a vectorizable loop to use SIMD instructions
 * 
 * Phase 1: Precision Data-Flow & Memory Indexing
 * - Single-pass transformation with proper def-use wiring
 * - Accurate address calculation for VLOAD/VSTORE
 */
bool vectorization_transform(ZeroCCCompiler *c, VectorizationState *state, int loop_idx) {
    if (loop_idx < 0 || loop_idx >= state->loop_count) {
        return false;
    }
    
    VectorizationInfo *vinfo = &state->loops[loop_idx];
    if (!vinfo->is_vectorizable) {
        return false;
    }
    
    int vec_width = vinfo->vector_width;
    int target_isa = c->regalloc.target_isa;
    
    /* Get loop body */
    int start_idx = vinfo->body_start_idx;
    int end_idx = vinfo->body_end_idx;
    
    /* Guard: Only vectorize if loop has scalar increment (i += 1) */
    bool found_scalar_increment = false;
    int loop_var_temp = -1;
    for (int i = start_idx; i <= end_idx; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        if (instr->op == OP_ADD) {
            if (instr->imm == 1) {
                found_scalar_increment = true;
                loop_var_temp = instr->temp_src1;
                break;
            } else if (instr->temp_src2!= TEMP_NONE && ir_is_constant(instr->temp_src2) && ir_get_constant(instr->temp_src2) == 1) {
                found_scalar_increment = true;
                loop_var_temp = instr->temp_src1;
                break;
            }
        }
    }
    
    if (!found_scalar_increment) {
        return false;
    }
    
    /* Store the loop variable temp for use in the increment rewrite below */
    vinfo->loop_var_temp = loop_var_temp;
    
    /* Change loop increment from i += 1 to i += vec_width (e.g. 4)
     * Only rewrite the SPECIFIC loop counter increment (matching loop_var_temp),
     * not any other ADD with constant 1 (which could be a non-counter increment). */
    for (int i = start_idx; i <= end_idx; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        if (instr->op == OP_ADD && instr->temp_src1 == loop_var_temp) {
            if (instr->imm == 1) {
                instr->imm = (int16_t)vec_width;  /* Change i += 1 to i += vec_width */
            } else if (instr->temp_src2!= TEMP_NONE && ir_is_constant(instr->temp_src2) && ir_get_constant(instr->temp_src2) == 1) {
                /* Update the constant definition to vec_width */
                for (int j = 0; j < i; j++) {
                    Instr *prev = ir_get(j);
                    if (prev && prev->op == OP_MOV_IMM && prev->temp_dest == instr->temp_src2) {
                        prev->imm = (int16_t)vec_width;
                        prev->imm64 = vec_width;
                        break;
                    }
                }
            }
        }
    }
    
    /* Track scalar temp -> vector temp mapping for data flow.
     * Sprint 35 P4: full 16-bit temp-id space (static, memset per call).
     * The old [256] local went OOB for functions with >256 temps. */
    static int16_t s_scalar_to_vec_map[65536];
    int16_t *scalar_to_vec_map = s_scalar_to_vec_map;
    memset(scalar_to_vec_map, 0xFF, sizeof(s_scalar_to_vec_map));
    
    /* Transform loads, arithmetic, and stores */
    for (int i = start_idx; i <= end_idx; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Handle scalar loads -> VLOAD */
        if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD || instr->op == OP_VLOADSS || instr->op == OP_VLOADSD) {
            int vec_temp = instr->temp_dest;
            
            /* Map scalar dest to vector temp */
            scalar_to_vec_map[instr->temp_dest] = vec_temp;
            
            /* Replace with VLOAD */
            instr->op = OP_VLOAD;
            instr->temp_dest = vec_temp;
            instr->temp_src2= TEMP_NONE;
            /* Leave instr->imm unchanged (preserves stack displacement) */
            instr->imm64 = 0;
            instr->label_idx = 0;
            instr->cond = 0;
            instr->flags_out = 0xFF;
            instr->flags_in = 0xFF;
            continue;
        }
        
        /* Handle scalar arithmetic -> vector arithmetic */
        if (is_vec_arith(instr)) {
            /* Get vector operation */
            int vec_op = get_vec_op(instr->op, vinfo->is_double, vinfo->is_int32);
            
            /* Get source vector temps from map */
            int vec_src1 = scalar_to_vec_map[instr->temp_src1];
            int vec_src2 = scalar_to_vec_map[instr->temp_src2];
            
            /* Skip if sources not mapped (loop invariants) */
            if (vec_src1 < 0 || vec_src2 < 0) {
                continue;
            }
            
            int vec_dest = instr->temp_dest;
            
            /* Map result to vector temp */
            scalar_to_vec_map[instr->temp_dest] = vec_dest;
            
            /* Replace with vector arithmetic */
            instr->op = vec_op;
            instr->temp_dest = vec_dest;
            instr->temp_src1 = vec_src1;
            instr->temp_src2 = vec_src2;
            instr->imm64 = 0;
            instr->label_idx = 0;
            instr->cond = 0;
            instr->flags_out = 0xFF;
            instr->flags_in = 0xFF;
            continue;
        }
        
        /* Handle scalar stores -> VSTORE */
        if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE || instr->op == OP_VSTORESS || instr->op == OP_VSTORESD) {
            int scalar_val_temp = -1;
            int scalar_addr_temp = -1;
            
            if (instr->op == OP_STORE_32) {
                /* STORE_32 [src2], src1: src1 is val, src2 is addr */
                scalar_val_temp = instr->temp_src1;
                scalar_addr_temp = instr->temp_src2;
            } else {
                /* FSTORE/VSTORESS/VSTORESD [src1], src2: src1 is addr, src2 is val */
                scalar_val_temp = instr->temp_src2;
                scalar_addr_temp = instr->temp_src1;
            }
            
            /* Get vector value from map */
            int vec_value = (scalar_val_temp >= 0 && scalar_val_temp < 256) ? scalar_to_vec_map[scalar_val_temp] : -1;
            
            /* Skip if value not mapped */
            if (vec_value < 0) {
                continue;
            }
            
            /* Replace with VSTORE [addr], vec_value */
            instr->op = OP_VSTORE;
            instr->temp_dest= TEMP_NONE;
            instr->temp_src1 = scalar_addr_temp;
            instr->temp_src2 = vec_value;
            /* Leave instr->imm unchanged */
            instr->imm64 = 0;
            instr->label_idx = 0;
            instr->cond = 0;
            instr->flags_out = 0xFF;
            instr->flags_in = 0xFF;
            continue;
        }
    }
    
    return true;
}

/**
 * Main auto-vectorization pass
 */
void opt_auto_vectorize(ZeroCCCompiler *c) {
    VectorizationStateFull state = {0};
    state.target_isa = c->regalloc.target_isa;
    state.vector_width = vectorization_get_width(state.target_isa);
    state.loop_count = 0;
    state.capacity = MAX_VECTOR_LOOPS;  /* Initialize capacity */
    
    /* Analyze loops for vectorization opportunities */
    vectorization_analyze_full(&state);
    
    if (state.loop_count == 0) {
        return;
    }
    
    /* Transform vectorizable loops */
    int transformed = 0;
    for (int i = 0; i < state.loop_count; i++) {
        if (vectorization_transform(c, (VectorizationState*)&state, i)) {
            transformed++;
        }
    }
    
    (void)transformed;  /* Suppress unused warning */
}
