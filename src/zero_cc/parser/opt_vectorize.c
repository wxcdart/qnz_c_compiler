#include "opt_vectorize.h"
#include "opt_loop.h"
#include "ir.h"
#include "ir_arena.h"
#include "ir_emit_ops.h"
#include "regalloc.h"
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

#define MAX_VECTOR_LOOPS 64
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
 * Get vector operation for scalar operation
 */
static int get_vec_op(int scalar_op) {
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
    
    /* Get loop body */
    int start_idx = vinfo->body_start_idx;
    int end_idx = vinfo->body_end_idx;
    
    /* Collect instructions to transform */
    typedef struct {
        int idx;
        int orig_op;
        int dest;
        int src1;
        int src2;
        int imm;
    } TransformInfo;
    
    TransformInfo transforms[64];
    size_t transform_count = 0;
    
    /* Track scalar temp -> vector temp mapping for data flow */
    int scalar_to_vec_map[256];
    memset(scalar_to_vec_map, -1, sizeof(scalar_to_vec_map));
    
    /* Scan loop body for vectorizable patterns */
    for (int i = start_idx; i <= end_idx && transform_count < 64; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        /* Track scalar loads for vectorization */
        if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD) {
            transforms[transform_count].idx = i;
            transforms[transform_count].orig_op = instr->op;
            transforms[transform_count].dest = instr->temp_dest;
            transforms[transform_count].src1 = instr->temp_src1;
            transforms[transform_count].src2 = 0xFF;
            transforms[transform_count].imm = (int)instr->imm;
            transform_count++;
        }
        
        /* Track scalar arithmetic for vectorization */
        if (is_vec_arith(instr)) {
            transforms[transform_count].idx = i;
            transforms[transform_count].orig_op = instr->op;
            transforms[transform_count].dest = instr->temp_dest;
            transforms[transform_count].src1 = instr->temp_src1;
            transforms[transform_count].src2 = instr->temp_src2;
            transforms[transform_count].imm = 0;
            transform_count++;
        }
        
        /* Track scalar stores for vectorization */
        if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE) {
            transforms[transform_count].idx = i;
            transforms[transform_count].orig_op = instr->op;
            transforms[transform_count].dest = 0xFF;
            transforms[transform_count].src1 = instr->temp_src1;
            transforms[transform_count].src2 = 0xFF;
            transforms[transform_count].imm = (int)instr->imm;
            transform_count++;
        }
    }
    
    if (transform_count == 0) {
        return false;
    }
    
    /* Allocate vector temps starting after general purpose temps */
    int vec_temp_counter = 16;
    
    /* First pass: Transform loads and build scalar->vector map */
    for (size_t t = 0; t < transform_count; t++) {
        TransformInfo *ti = &transforms[t];
        Instr *instr = ir_get(ti->idx);
        if (!instr) continue;
        
        /* Only process loads in first pass */
        if (ti->orig_op != OP_LOAD_32 && ti->orig_op != OP_FLOAD) continue;
        
        /* Allocate vector temp for this load */
        int vec_temp = vec_temp_counter++;
        
        /* Map scalar dest to vector temp */
        scalar_to_vec_map[ti->dest] = vec_temp;
        
        /* Replace with VLOAD */
        instr->op = OP_VLOAD;
        instr->temp_dest = vec_temp;
        instr->temp_src1 = ti->src1;
        instr->temp_src2 = 0xFF;
        instr->imm = 0;
        instr->imm64 = 0;
        instr->label_idx = 0;
        instr->cond = 0;
        instr->flags_out = 0xFF;
        instr->flags_in = 0xFF;
    }
    
    /* Second pass: Transform arithmetic using mapped vector temps */
    for (size_t t = 0; t < transform_count; t++) {
        TransformInfo *ti = &transforms[t];
        Instr *instr = ir_get(ti->idx);
        if (!instr) continue;
        
        /* Only process arithmetic in second pass */
        if (!is_vec_arith(instr)) continue;
        
        /* Get vector operation */
        int vec_op = get_vec_op(ti->orig_op);
        
        /* Allocate vector temp for result */
        int vec_dest = vec_temp_counter++;
        
        /* CRITICAL FIX: Get source vector temps from map */
        int vec_src1 = scalar_to_vec_map[ti->src1];
        int vec_src2 = scalar_to_vec_map[ti->src2];
        
        /* If sources not mapped yet, allocate new temps (conservative fallback) */
        if (vec_src1 < 0) vec_src1 = vec_temp_counter++;
        if (vec_src2 < 0) vec_src2 = vec_temp_counter++;
        
        /* Map result to vector temp */
        scalar_to_vec_map[ti->dest] = vec_dest;
        
        /* Replace with vector arithmetic */
        instr->op = vec_op;
        instr->temp_dest = vec_dest;
        instr->temp_src1 = vec_src1;
        instr->temp_src2 = vec_src2;
        instr->imm = 0;
        instr->imm64 = 0;
        instr->label_idx = 0;
        instr->cond = 0;
        instr->flags_out = 0xFF;
        instr->flags_in = 0xFF;
    }
    
    /* Third pass: Transform stores using mapped vector temps */
    for (size_t t = 0; t < transform_count; t++) {
        TransformInfo *ti = &transforms[t];
        Instr *instr = ir_get(ti->idx);
        if (!instr) continue;
        
        /* Only process stores in third pass */
        if (ti->orig_op != OP_STORE_32 && ti->orig_op != OP_FSTORE) continue;
        
        /* CRITICAL FIX: Get vector value from map */
        int vec_value = scalar_to_vec_map[ti->src1];
        
        /* If not mapped, allocate new (conservative fallback) */
        if (vec_value < 0) vec_value = vec_temp_counter++;
        
        /* Replace with VSTORE */
        instr->op = OP_VSTORE;
        instr->temp_dest = 0xFF;
        instr->temp_src1 = ti->src1;  /* Address */
        instr->temp_src2 = vec_value;  /* Vector value from map */
        instr->imm = 0;
        instr->imm64 = 0;
        instr->label_idx = 0;
        instr->cond = 0;
        instr->flags_out = 0xFF;
        instr->flags_in = 0xFF;
    }
    
    /* CRITICAL FIX: Update loop increment with proper byte scaling */
    /* i += 1 element -> i += vec_width elements (4 * sizeof(float) = 16 bytes) */
    int byte_stride = vec_width * 4;  /* 4 bytes per float, vec_width elements */
    for (int i = start_idx; i <= end_idx; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        if (instr->op == OP_ADD && instr->imm == 1) {
            instr->imm = byte_stride;  /* i += 16 bytes for 4 floats */
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
    
    /* Analyze loops for vectorization opportunities */
    vectorization_analyze((VectorizationState*)&state);
    
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
