/* zero_san cross-ISA smoke: valid C must run clean (exit 0) under
 * -fsanitize=undefined when compiled for a non-x86 target. Guards the
 * invariant that the x86 check bytes never leak into other ISAs' raw
 * binaries and that the check is cleanly skipped there (value preservation,
 * no crash). Run by tools/san_sweep.py under qemu-*-static.
 *
 * The `int * long long` mixed-width multiply case (mul_il) also pins the
 * Sprint 117 parameter pre-coloring fix: without it, RISC-V assigned the
 * sign-extension a register that still held the long operand's ABI-slot
 * value and computed i*i instead of i*l.
 */
long long f_mixed(int i, long long l) { return i + l; }
long long g_mixed(long long l, int i) { return l - i; }
unsigned uwrap(unsigned a, unsigned b) { return a + b; }
unsigned long long ulwrap(unsigned long long a, unsigned long long b) { return a + b; }
long long f64(long long a, long long b) { long long x = a + b; return x; }
long long f64loop(int n) { long long s = 0; for (int i = 0; i < n; i++) s = s + 12345LL; return s; }
/* Sprint 168: loop variants of the sub/mul checks. The OVF check used to
 * clobber its operand registers in place (assumed dead); at -O2/-O3 LICM
 * hoists the invariant addend out of the loop and the check destroyed it
 * every iteration (f64loop returned 3440441 instead of 123450). The sweep
 * now runs this file at -O0..-O3 so the sub/mul check computations are
 * covered under hoisting as well. */
long long f64loop_sub(int n) { long long s = 0; for (int i = 0; i < n; i++) s = s - 12345LL; return s; }
long long f64loop_mul(int n) { long long s = 1; for (int i = 0; i < n; i++) s = s * 2LL; return s; }
long long mul_ll(long long a, long long b) { return a * b; }
long long mul_il(int i, long long l) { return (long long)i * l; }
/* Sprint 166 follow-on: valid 32-bit div/mod/shift under the flag. int is
 * the natural word on RV32 and 32-bit elsewhere, so this exercises the new
 * div-zero / shift-count checks (and proves they skip cleanly) on all
 * three ISAs. Counts 4..30 must not trap (the bound is 32 bits). */
int div32(int n, int d) { return n / d; }
int rem32(int n, int d) { return n % d; }
unsigned ushl32(unsigned n, unsigned c) { return n << c; }
int sshr32(int n, int c) { return n >> c; }
int main(void) {
    if (f_mixed(2000000000, 2000000000LL) != 4000000000LL) return 1;
    if (g_mixed(2000000000LL, 1) != 1999999999LL) return 2;
    if (uwrap(0xFFFFFFFFu, 10u) != 9u) return 3;
    if (ulwrap(0xFFFFFFFFFFFFFFF0ULL, 100ULL) != 84ULL) return 4;
    if (f64(100000LL, 200000LL) != 300000LL) return 5;
    if (f64(-100000LL, -200000LL) != -300000LL) return 6;
    if (f64loop(10) != 123450LL) return 7;
    if (mul_ll(2000000000LL, 2LL) != 4000000000LL) return 8;
    if (mul_il(-123456, 100000LL) != -12345600000LL) return 9;
    if (mul_il(2000000000, 2LL) != 4000000000LL) return 10;
    if (div32(100, 7) != 14) return 11;
    if (div32(-100, -7) != 14) return 12;
    if (rem32(100, 7) != 2) return 13;
    if (ushl32(1u, 4) != 16u) return 14;
    if (ushl32(1u, 30) != 0x40000000u) return 15;
    if (sshr32(-8, 1) != -4) return 16;
    if (f64loop_sub(10) != -123450LL) return 17;
    if (f64loop_mul(10) != 1024LL) return 18;
    return 0;
}
