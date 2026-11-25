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
    bool is_double;            /* 64-bit double precision floating point */
    bool is_int32;             /* 32-bit packed integer */
    int elem_size;             /* Element size in bytes (4 or 8) */
    int inc_instr_idx;         /* Instruction index of loop counter increment */
    bool inc_use_src2;         /* True if OP_ADD counter is in temp_src2 */
    bool is_aligned;           /* True if all stack array bases are 16-byte aligned */
    int stride_bytes;          /* Detected access stride in bytes (elem_size for unit stride) */
    bool needs_peel;           /* True if loop trip count requires remainder peel */
    int64_t v_bound;           /* Upper bound for vectorized loop */
    int test_instr_idx;        /* Index of loop exit test JE */
    int cmp_instr_idx;         /* Index of loop exit CMP */
    bool bound_is_src2;        /* True if bound is cmp operand temp_src2 */
    bool is_scalable;          /* True if scalable loop (RVV vsetvli or SVE whilelt) */
    bool is_rvv;               /* True if RVV scalable loop */
    bool is_sve;               /* True if SVE scalable loop */
    uint32_t bound_temp;       /* Temporary holding upper bound n */
    int32_t bound_slot;        /* Stack slot holding upper bound n, or 0x7fffffff */
    int32_t counter_slot;      /* Stack slot holding counter i, or 0x7fffffff */
    /* Advanced SIMD Vector Reductions (Sprint 210) */
    bool is_reduction;         /* True if this loop is a reduction loop */
    int reduction_op;          /* Reduction operation (OP_ADD, OP_FADD, OP_MIN, OP_MAX) */
    int32_t reduction_slot;    /* Stack slot holding accumulator, or 0x7fffffff */
    uint32_t reduction_temp;   /* Temporary holding accumulator, or TEMP_NONE */
    int red_load_idx;          /* Index of accumulator load instruction */
    int red_store_idx;         /* Index of accumulator store instruction */
    int red_arith_idx;         /* Index of accumulator arithmetic instruction */
    bool is_reduction_fp;      /* True if floating point reduction */
    /* Strided & Non-Unit Stride Auto-Vectorization */
    bool has_strided_access;   /* True if loop contains non-unit stride memory accesses */
    /* Indexed Gather / Scatter Auto-Vectorization */
    bool has_indexed_access;   /* True if loop contains indirect array accesses (gather/scatter) */
    /* Conditional Vectorization (Predication / Blend) (Sprint 219) */
    bool has_cond_select;      /* True if loop contains conditional diamonds for SIMD blend */
    int cond_diamond_count;    /* Number of detected conditional diamonds */
} VectorizationInfo;

#define MAX_VECTOR_LOOPS 64

/**
 * Vectorization analysis state
 */
typedef struct {
    VectorizationInfo loops[MAX_VECTOR_LOOPS];
    int loop_count;
    int capacity;
    int vector_width;          /* Target vector width */
    int target_isa;
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
