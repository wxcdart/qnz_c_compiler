#ifndef ZERO_CC_OPT_LOOP_UNROLL_H
#define ZERO_CC_OPT_LOOP_UNROLL_H

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Loop full-unrolling pass (Sprint 150, development plan item 3).
 *
 * Replaces a natural loop whose trip count is a compile-time constant
 * (1..LU_MAX_TRIP) by that many straight-line copies of the loop body,
 * deleting the loop test.  The transform is exact:
 *
 *  - every copy keeps the body bit-for-bit (induction step included), so
 *    the loop variable keeps its per-iteration values;
 *  - a "continue" (back-jump to the header) re-aims at the NEXT copy's
 *    header, or at the loop exit for the last copy;
 *  - a "continue" to the for-step label re-aims at the SAME copy's step
 *    label (the current iteration's step still runs);
 *  - every in-loop label gets a per-copy clone, so mid-body control flow
 *    (if/else, break) stays inside one iteration and breaks still skip
 *    the remaining copies;
 *  - the last copy's backedge jumps to the loop-exit label.
 *
 * A loop qualifies only when ALL of the following hold (see
 * opt_loop_unroll.c for the gate list):
 *  - the backedge is an unconditional OP_JMP to the loop header and the
 *    header block is [label, <pure test computation>, JE/JNE flag, Lexit];
 *  - the loop-continue condition is "counter REL const_bound"
 *    (OP_CMP + OP_COND_SET, REL in {LT,LE} for the required +1 step;
 *    signed or the CF/ZF unsigned variant), with the test taken against
 *    the exit label (position past the backedge);
 *  - the counter (a promoted temp or a stack slot) is initialized to a
 *    compile-time constant before the header and is incremented by
 *    exactly +1 once per iteration in the backedge block, with no other
 *    definition (or store) in the loop span;
 *  - the body has no calls/systraps/asm/setjmp/computed-goto/&&label/
 *    VLA/atomics/fences/traps, no nested loops (any back-jump to a label
 *    that is neither the header nor the for-step label), no side entries
 *    (a jump from outside the span into it), no user label inside the
 *    span (a later-compiled nested function may nonlocal-goto into it),
 *    and the header computation defines no temp that survives past the
 *    header (the test is deleted per copy) and has no side effects
 *    (including zero_san OP_OVF_TRAP, which the transform would drop);
 *  - the span is <= LU_MAX_BODY instructions and the trip count
 *    (bound - init, or bound - init + 1 for <=) is in [1, LU_MAX_TRIP].
 *
 * One qualifying loop is unrolled per invocation (the first found); the
 * optimizer pipeline's fixed-point iteration handles the rest on later
 * rounds.  The IR arena is rebuilt in place (the LICM two-pass-rebuild
 * pattern); the in-stream OP_ASM payload (Sprint 154) travels with its
 * anchor - no side table to remap.  -O3-gated (see the opt_pass.c pass
 * table).
 */
void opt_loop_unroll(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_LOOP_UNROLL_H */
