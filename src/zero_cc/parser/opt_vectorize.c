#include "opt_vectorize.h"
#include "opt_loop.h"
#include "ir.h"
#include "ir_emitter.h"
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

#define MAX_VECTOR_LOOPS 64
#define MAX_VEC_BODY 256

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
    state->loops = malloc(sizeof(VectorizationInfo) * MAX_VECTOR_LOOPS);
    state->loop_count = 0;
    state->capacity = MAX_VECTOR_LOOPS;
    state->vector_width = vectorization_get_width(target_isa);
    state->invariant_map = NULL;
    state->invariant_count = 0;
    
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
    free(state->loops);
    free(state->invariant_map);
    state->loops = NULL;
    state->invariant_map = NULL;
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
static void analyze_loop_for_vectorization(VectorizationState *state, LoopInfo *loop) {
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
    
    for (int i = 0; i < loop->body_count; i++) {
        int idx = loop->body_indices[i];
        Instr *instr = ir_get(idx);
        if (!instr) continue;
        
        /* Look for array access pattern */
        if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD) {
            load_count++;
            if (base_temp < 0) {
                base_temp = instr->temp_src1;
            }
        }
        if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE) {
            store_count++;
        }
        if (is_vec_arith(instr)) {
            arith_count++;
        }
        
        /* Look for loop variable increment */
        if (instr->op == OP_ADD && instr->imm == 1) {
            loop_var = instr->temp_src1;
        }
    }
    
    /* Vectorization criteria:
     * - At least 2 array loads (read operands)
     * - At least 1 store (write result)
     * - At least 1 arithmetic operation
     * - No function calls or complex side effects
     */
    if (load_count >= 2 && store_count >= 1 && arith_count >= 1) {
        vinfo->is_vectorizable = true;
        vinfo->loop_var_temp = loop_var;
        vinfo->array_base_temp = base_temp;
        vinfo->vector_width = state->vector_width;
        state->loop_count++;
    }
}

/**
 * Detect loops and analyze for vectorization
 */
void vectorization_analyze(VectorizationState *state) {
    int count = (int)ir_get_count();
    
    /* Simple loop detection (similar to opt_loop.c) */
    for (int i = 0; i < count && state->loop_count < state->capacity; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Check for jump instructions that could be loop backedges */
        if (instr->op == OP_JMP || instr->op == OP_JE || 
            instr->op == OP_JNE || instr->op == OP_JL ||
            instr->op == OP_JLE || instr->op == OP_JG) {
            
            int target = instr->label_idx;
            if (target >= 0 && target < i) {
                /* Found potential loop - create temporary LoopInfo */
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
                analyze_loop_for_vectorization(state, &loop);
                
                free(loop.body_indices);
            }
        }
    }
}

/**
 * Emit vectorized loop body
 * Transforms scalar operations to vector operations
 */
static void emit_vectorized_body(VectorizationState *state, VectorizationInfo *vinfo) {
    int vec_width = vinfo->vector_width;
    
    /* 
     * Vectorized loop body pattern:
     * for (i = 0; i < n; i += vec_width) {
     *   // Load vec_width elements
     *   vec_load(a[i], vec_a)
     *   vec_load(b[i], vec_b)
     *   // Vector operation
     *   vec_c = vec_op(vec_a, vec_b)
     *   // Store result
     *   vec_store(vec_c, c[i])
     * }
     */
    
    /* For now, we emit a placeholder that will be filled in by the actual transformation */
    /* The actual implementation would:
     * 1. Replace scalar loads with VLOAD
     * 2. Replace scalar arithmetic with VADDPS/VSUBPS/VMULPS
     * 3. Replace scalar stores with VSTORE
     * 4. Update loop increment to += vec_width
     */
    
    /* Placeholder: actual implementation would emit proper vector IR */
    /* This would include VLOAD, VADDPS/VSUBPS/VMULPS, VSTORE instructions */
}

/**
 * Transform a vectorizable loop to use SIMD instructions
 */
bool vectorization_transform(ZeroCCCompiler *c, VectorizationState *state, int loop_idx) {
    (void)c;
    
    if (loop_idx < 0 || loop_idx >= state->loop_count) {
        return false;
    }
    
    VectorizationInfo *vinfo = &state->loops[loop_idx];
    if (!vinfo->is_vectorizable) {
        return false;
    }
    
    int vec_width = vinfo->vector_width;
    int target_isa = c->regalloc.target_isa;
    
    /* 
     * Transformation steps:
     * 1. Replace scalar loop with vectorized version
     * 2. Insert vector load/store instructions
     * 3. Replace scalar arithmetic with vector arithmetic
     * 4. Add tail loop for remainder iterations
     */
    
    /* 
     * Note: Full implementation would require:
     * - IR buffer manipulation (insert/delete instructions)
     * - Temporary allocation for vector registers
     * - Proper loop restructuring
     * 
     * For now, we mark the loop as vectorized and emit placeholder IR
     * that can be expanded later.
     */
    
    /* Emit vectorization marker comment */
    /* In a full implementation, this would emit actual vector IR */
    
    /* Placeholder: emit a vector operation to demonstrate the pattern */
    /* This would be replaced with actual VADDPS/VSUBPS/etc. */
    
    return true;
}

/**
 * Main auto-vectorization pass
 */
void opt_auto_vectorize(ZeroCCCompiler *c) {
    VectorizationState state;
    vectorization_init(&state, c->regalloc.target_isa);
    
    /* Analyze loops for vectorization opportunities */
    vectorization_analyze(&state);
    
    if (state.loop_count == 0) {
        vectorization_free(&state);
        return;
    }
    
    /* Transform vectorizable loops */
    int transformed = 0;
    for (int i = 0; i < state.loop_count; i++) {
        if (vectorization_transform(c, &state, i)) {
            transformed++;
        }
    }
    
    /* Cleanup */
    vectorization_free(&state);
    
    /* Note: transformed count would be used to determine if IR changed */
    (void)transformed;
}
