#ifndef ZERO_CC_OPT_COPY_PROP_H
#define ZERO_CC_OPT_COPY_PROP_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Copy Propagation
 *
 * Propagates copy assignments through the IR to enable other optimizations.
 *
 * Example:
 *   MOV r20, r19    ; r20 = r19
 *   ADD r21, r20, r18  ; Before: r20, After: r19
 *
 * After copy propagation:
 *   ADD r21, r19, r18  ; Now GVN can see this matches other ADDs with r19
 *
 * This pass:
 * 1. Tracks copy relationships (temp -> source temp)
 * 2. Replaces uses with the original source
 * 3. Invalidates copies when source is redefined
 *
 * Works well with GVN - after propagation, GVN can see value duplicates
 * that were hidden by intermediate copy temps.
 */

/* Initialize copy propagation pass */
void opt_init_copy_prop(void);

/* Run copy propagation optimization */
void opt_copy_propagate(ZeroCCCompiler *c);

/* Shutdown copy propagation pass */
void opt_shutdown_copy_prop(void);

#endif /* ZERO_CC_OPT_COPY_PROP_H */
