/* Sprint 118 regression test: constant folding under -fsanitize=undefined.
 *
 * When opt_fold_constants folds a compile-time constant ADD/SUB/MUL into a
 * MOV_IMM, any adjacent zero_san OP_OVF_TRAP must be marked OP_DEAD.
 * An orphaned width-8 trap checks the CPU OF flag via `jno + ud2`, reading
 * the stale flag of whatever instruction preceded it (false-trapping on
 * valid constant expressions).
 *
 * Exit 0 = sound; SIGILL = orphaned OVF_TRAP false-positive. */

static int dirty_flags(void) {
    /* Set flags via unsigned arithmetic (never trapped by zero_san) */
    unsigned long a = ~0UL;
    unsigned long b = 100UL;
    unsigned long c = a + b;
    return (c == 99UL) ? 0 : 1;
}

int fold_add32(void) { return 100 + 200; }
int fold_sub32(void) { return 500 - 300; }
int fold_mul32(void) { return 20 * 15; }

long fold_add64(void) { return 1000000000L + 2000000000L; }
long fold_sub64(void) { return 3000000000L - 1000000000L; }
long fold_mul64(void) { return 100000L * 20000L; }

int main(void) {
    dirty_flags();
    if (fold_add32() != 300) return 1;
    dirty_flags();
    if (fold_sub32() != 200) return 2;
    dirty_flags();
    if (fold_mul32() != 300) return 3;
    dirty_flags();
    if (fold_add64() != 3000000000L) return 4;
    dirty_flags();
    if (fold_sub64() != 2000000000L) return 5;
    dirty_flags();
    if (fold_mul64() != 2000000000L) return 6;
    return 0;
}
