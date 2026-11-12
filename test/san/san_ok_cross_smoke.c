/* zero_san cross-ISA smoke: valid C must run clean (exit 0) under
 * -fsanitize=undefined when compiled for a non-x86 target. Guards the
 * invariant that the x86 check bytes never leak into other ISAs' raw
 * binaries and that the check is cleanly skipped there (value preservation,
 * no crash). Run by tools/san_sweep.py under qemu-*-static.
 *
 * The `int * long` mixed-width multiply case (mul_il) also pins the
 * Sprint 117 parameter pre-coloring fix: without it, RISC-V assigned the
 * sign-extension a register that still held the long operand's ABI-slot
 * value and computed i*i instead of i*l.
 */
long f_mixed(int i, long l) { return i + l; }
long g_mixed(long l, int i) { return l - i; }
unsigned uwrap(unsigned a, unsigned b) { return a + b; }
unsigned long ulwrap(unsigned long a, unsigned long b) { return a + b; }
long f64(long a, long b) { long x = a + b; return x; }
long f64loop(int n) { long s = 0; for (int i = 0; i < n; i++) s = s + 12345L; return s; }
long mul_ll(long a, long b) { return a * b; }
long mul_il(int i, long l) { return (long)i * l; }
/* Sprint 166 follow-on: valid 32-bit div/mod/shift under the flag. int is
 * the natural word on RV32 and 32-bit elsewhere, so this exercises the new
 * div-zero / shift-count checks (and proves they skip cleanly) on all
 * three ISAs. Counts 4..30 must not trap (the bound is 32 bits). */
int div32(int n, int d) { return n / d; }
int rem32(int n, int d) { return n % d; }
unsigned ushl32(unsigned n, unsigned c) { return n << c; }
int sshr32(int n, int c) { return n >> c; }
int main(void) {
    if (f_mixed(2000000000, 2000000000L) != 4000000000L) return 1;
    if (g_mixed(2000000000L, 1) != 1999999999L) return 2;
    if (uwrap(0xFFFFFFFFu, 10u) != 9u) return 3;
    if (ulwrap(0xFFFFFFFFFFFFFFF0UL, 100UL) != 84UL) return 4;
    if (f64(100000L, 200000L) != 300000L) return 5;
    if (f64(-100000L, -200000L) != -300000L) return 6;
    if (f64loop(10) != 123450L) return 7;
    if (mul_ll(2000000000L, 2L) != 4000000000L) return 8;
    if (mul_il(-123456, 100000L) != -12345600000L) return 9;
    if (mul_il(2000000000, 2L) != 4000000000L) return 10;
    if (div32(100, 7) != 14) return 11;
    if (div32(-100, -7) != 14) return 12;
    if (rem32(100, 7) != 2) return 13;
    if (ushl32(1u, 4) != 16u) return 14;
    if (ushl32(1u, 30) != 0x40000000u) return 15;
    if (sshr32(-8, 1) != -4) return 16;
    return 0;
}
