/* san_ok_hoisted_invariant.c - Sprint 168 regression: the zero_san overflow
 * check must not clobber operands that are LIVE across it.
 *
 * The compare-based check on aarch64/riscv used to compute its temporaries
 * in the operand registers (`eor b, b, res` / `xor b, b, res`), on the
 * assumption that the operands were dead after the checked op. At -O2/-O3
 * LICM hoists a loop-invariant operand out of the loop, so the check then
 * destroyed the hoisted value on every iteration: f64loop
 * (`s = s + 12345L` in a loop) returned 3440441 instead of 123450 at
 * -O2/-O3 on aarch64/riscv64 and 61725 on riscv32. The checks now use
 * LRA-unallocated scratch (x16/x17 on aarch64, t3/t4 on riscv), and the
 * cross smoke (san_ok_cross_smoke.c, which carries f64loop) runs at every
 * -O level. This file pins the same pattern host-side (JIT + AOT, all
 * levels): loop-accumulated add/sub/mul with invariant addends and a
 * separately-live invariant.
 *
 * No overflow occurs anywhere: the sanitizer flag must not change the
 * result (ok test: expect exit 0, no report, at -O0..-O3). */
long acc_add(int n) { long s = 0; for (int i = 0; i < n; i++) s = s + 12345L; return s; }
long acc_sub(int n) { long s = 0; for (int i = 0; i < n; i++) s = s - 12345L; return s; }
long acc_mul(int n) { long s = 1; for (int i = 0; i < n; i++) s = s * 2L; return s; }
long live_operand(int n) {
    long c = 100000L;  /* loop-invariant AND used after the loop */
    long s = 0;
    for (int i = 0; i < n; i++) s = s + c;
    return s + c;
}
int main(void) {
    if (acc_add(10) != 123450L) return 1;
    if (acc_sub(10) != -123450L) return 2;
    if (acc_mul(10) != 1024L) return 3;
    if (live_operand(4) != 500000L) return 4;
    return 0;
}
