#ifndef OPT_VECTORIZE_H
#define OPT_VECTORIZE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Auto-vectorization pass
 * 
 * Transforms scalar loops into vectorized loops using SIMD instructions.
 * 
 * Supported patterns:
 * - Array element-wise operations (add, sub, mul)
 * - Countable loops with stride-1 accesses
 * - Simple reduction operations
 * 
 * Vector widths:
 * - x86-64 SSE2: 4x float32, 2x float64
 * - AArch64 NEON: 4x float32, 2x float64
 * - RISC-V RVV: configurable (default 4x float32)
 */

/**
 * Vectorization info for a loop
 */
typedef struct {
    bool is_vectorizable;      /* Can this loop be vectorized? */
    int vector_width;          /* Number of elements per vector (4 for float32) */
    int loop_var_temp;         /* Loop variable temporary */
    int array_base_temp;       /* Base address of array */
    int array_stride;          /* Element stride (typically 4 for float32) */
    int body_start_idx;        /* Start of loop body */
    int body_end_idx;          /* End of loop body */
    int iter_count_temp;       /* Temporary for iteration count */
} VectorizationInfo;

/**
 * Vectorization analysis state
 */
typedef struct {
    VectorizationInfo *loops;
    int loop_count;
    int capacity;
    int vector_width;          /* Target vector width */
    bool *invariant_map;       /* Loop-invariant instruction map */
    int invariant_count;
} VectorizationState;

/**
 * Initialize vectorization state
 */
void vectorization_init(VectorizationState *state, int target_isa);

/**
 * Free vectorization resources
 */
void vectorization_free(VectorizationState *state);

/**
 * Analyze loops for vectorization opportunities
 */
void vectorization_analyze(VectorizationState *state);

/**
 * Transform scalar loop to vectorized loop
 * Returns true if transformation was successful
 */
bool vectorization_transform(ZeroCCCompiler *c, VectorizationState *state, int loop_idx);

/**
 * Public API: run auto-vectorization pass
 */
void opt_auto_vectorize(ZeroCCCompiler *c);

/**
 * Get vector width for target ISA
 */
int vectorization_get_width(int target_isa);

#endif /* OPT_VECTORIZE_H */
