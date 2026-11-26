#ifndef ZERO_CC_OPT_PEEPHOLE_H
#define ZERO_CC_OPT_PEEPHOLE_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Advanced Algebraic & Peephole Simplification (Tier 1, Item #2)
 *
 * Applies mathematical identities and peephole simplifications:
 * - Self identities: x - x -> 0, x ^ x -> 0, x & x -> x, x | x -> x
 * - Neutral elements: x + 0 -> x, x - 0 -> x, x * 1 -> x, x / 1 -> x,
 *                     x | 0 -> x, x ^ 0 -> x, x & -1 -> x, x << 0 -> x, x >> 0 -> x
 * - Annihilators: x * 0 -> 0, x & 0 -> 0
 * - Inverses: -(-x) -> x, ~(~x) -> x
 * - Additive constant reassociation: (x + c1) + c2 -> x + (c1 + c2)
 * - Self moves: MOV_REG x, x -> NOP
 */
bool opt_peephole_simplify(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_PEEPHOLE_H */
