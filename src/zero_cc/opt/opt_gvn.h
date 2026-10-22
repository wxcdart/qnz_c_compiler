#ifndef ZERO_CC_OPT_GVN_H
#define ZERO_CC_OPT_GVN_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Global Value Numbering (GVN)
 *
 * Assigns unique value numbers to computations and eliminates
 * redundant computations, even when they use different temps.
 *
 * Unlike CSE which looks for exact instruction duplicates, GVN tracks
 * value equivalence across different temporaries in SSA-like IR.
 *
 * Example:
 *   ADD r18, r16, r19    ; value #1 = r16 + r19
 *   ADD r21, r24, r25    ; value #2 = r24 + r25
 *
 * If r16==r24 and r19==r25 (via copy propagation), then #1==#2
 * and the second ADD can be replaced with a MOV.
 *
 * This implementation uses a simple approach:
 * 1. Build value number map for each basic block
 * 2. Track which temps hold the same value
 * 3. Replace redundant computations with copies
 */

/* Initialize GVN pass */
void opt_init_gvn(void);

/* Run GVN optimization */
void opt_eliminate_gvn(ZeroCCCompiler *c);

/* Shutdown GVN pass */
void opt_shutdown_gvn(void);

#endif /* ZERO_CC_OPT_GVN_H */
