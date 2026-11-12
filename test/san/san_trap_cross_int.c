/* Cross-ISA trap-firing smoke (Sprint 163): a signed 32-bit overflow in a
 * binary op must TRAP on every target ISA under qemu-*-static:
 *   aarch64 -> brk #0  -> SIGTRAP (rc 133)
 *   riscv64/riscv32 -> unimp -> SIGILL  (rc 132)
 * This is the compare-based check added for the non-x86 ISAs (x86-64 uses the
 * CPU OF flag / sign-extension round-trip inline). A binary op is used because
 * the raw-AOT width-4 inc/dec check has a known riscv false-negative; the
 * binary-op path is exercised uniformly across all three ISAs.
 *
 * Compiled with -fsanitize=undefined for --target aarch64/riscv64/riscv32 and
 * run under qemu-*-static by tools/san_sweep.py (cross-trap section). */
#include <limits.h>

int main(void) {
    int a = INT_MAX;
    int b = 1;
    volatile int r = a + b;   /* signed int overflow -> must trap */
    (void)r;
    return 0;
}
