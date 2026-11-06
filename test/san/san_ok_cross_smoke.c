/* zero_san cross-ISA smoke: valid C must run clean (exit 0) under
 * -fsanitize=undefined when compiled for a non-x86 target. Guards the
 * invariant that the x86 check bytes never leak into other ISAs' raw
 * binaries and that the check is cleanly skipped there (value preservation,
 * no crash). Run by tools/san_sweep.py under qemu-*-static.
 *
 * Note: this file intentionally omits the `int * long` mixed-width multiply
 * case - a pre-existing RISC-V backend miscompile (the sign-extension copy
 * clobbers the long operand's register; see CHANGELOG "known pre-existing
 * issues") makes that op wrong on riscv32/riscv64 with or without the flag.
 */
long f_mixed(int i, long l) { return i + l; }
long g_mixed(long l, int i) { return l - i; }
unsigned uwrap(unsigned a, unsigned b) { return a + b; }
unsigned long ulwrap(unsigned long a, unsigned long b) { return a + b; }
long f64(long a, long b) { long x = a + b; return x; }
long f64loop(int n) { long s = 0; for (int i = 0; i < n; i++) s = s + 12345L; return s; }
long mul_ll(long a, long b) { return a * b; }
int main(void) {
    if (f_mixed(2000000000, 2000000000L) != 4000000000L) return 1;
    if (g_mixed(2000000000L, 1) != 1999999999L) return 2;
    if (uwrap(0xFFFFFFFFu, 10u) != 9u) return 3;
    if (ulwrap(0xFFFFFFFFFFFFFFF0UL, 100UL) != 84UL) return 4;
    if (f64(100000L, 200000L) != 300000L) return 5;
    if (f64(-100000L, -200000L) != -300000L) return 6;
    if (f64loop(10) != 123450L) return 7;
    if (mul_ll(2000000000L, 2L) != 4000000000L) return 8;
    return 0;
}
