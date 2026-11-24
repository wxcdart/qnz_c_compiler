#ifndef ZERO_CC_OPT_SLP_H
#define ZERO_CC_OPT_SLP_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Superword-Level Parallelism (SLP) Auto-Vectorization Pass
 *
 * Vectorizes straight-line basic block code without requiring loops.
 * Identifies isomorphic operation groups within basic blocks:
 * - 4x32-bit (int32 or float32): vector width 4 (128-bit SIMD)
 * - 2x64-bit (int64 or float64): vector width 2 (128-bit SIMD)
 *
 * Transforms scalar loads into vector loads (OP_VLOAD), packed SIMD
 * operations (OP_VPADDD, OP_VPSUBD, OP_VPMULLD, OP_VADDPS, OP_VSUBPS,
 * OP_VMULPS, etc.), and contiguous vector stores (OP_VSTORE) or scalar unpacking.
 */
void opt_slp_vectorize(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_SLP_H */
