/* Cross-ISA trap-firing smoke, 64-bit (Sprint 163, Sprint 174 on rv32):
 * a signed 64-bit overflow in a binary op must TRAP on aarch64, riscv64,
 * and riscv32 under qemu (on rv32 a 64-bit value is a (lo,hi) GPR pair).
 *   aarch64 -> brk #0  -> SIGTRAP (rc 133)
 *   riscv64/riscv32 -> unimp -> SIGILL  (rc 132)
 *
 * long long is 64-bit on all of aarch64/riscv64 (LP64) and riscv32, so the
 * overflow width is 8 on all three targets. */
#include <limits.h>

int main(void) {
    long long a = LLONG_MAX;
    long long b = 1;
    volatile long long r = a + b;   /* signed 64-bit overflow -> must trap */
    (void)r;
    return 0;
}
