/* zero_san cross-ISA trap-firing (Sprint 166 follow-on): a 32-bit shift
 * with count == 32 (the int bit width) must TRAP on every target ISA under
 * qemu-*-static:
 *   aarch64 -> brk #0  -> SIGTRAP (rc 133)
 *   riscv64/riscv32 -> unimp -> SIGILL  (rc 132)
 * Exercises the width-4 shift-count bound on all three ISAs (32, not 4:
 * the count bound is the bit width). The count comes from a volatile load
 * so no pass can constant-fold it.
 *
 * Compiled with -fsanitize=undefined for --target aarch64/riscv64/riscv32
 * and run under qemu-*-static by tools/san_sweep.py (cross-trap section). */
int main(void) {
    volatile int v = 32;
    int c = (int)v;
    return 1 << c;
}
