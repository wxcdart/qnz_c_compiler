/* Cross-ISA trap-firing smoke, 64-bit (Sprint 163): a signed 64-bit overflow
 * in a binary op must TRAP on aarch64 and riscv64 under qemu (both have native
 * 64-bit integer ops). On riscv32 a 64-bit value is a (lo,hi) GPR pair and the
 * width-8 check is a documented false-negative (follow-up), so the gate skips
 * this test on riscv32.
 *   aarch64 -> brk #0  -> SIGTRAP (rc 133)
 *   riscv64 -> unimp   -> SIGILL  (rc 132)
 *
 * long long is 64-bit on all of aarch64/riscv64 (LP64) and riscv32, so the
 * overflow width is 8 on the LP64 targets. */
#include <limits.h>

int main(void) {
    long long a = LLONG_MAX;
    long long b = 1;
    volatile long long r = a + b;   /* signed 64-bit overflow -> must trap */
    (void)r;
    return 0;
}
