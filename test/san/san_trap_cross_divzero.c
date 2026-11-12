/* zero_san cross-ISA trap-firing (Sprint 166 follow-on): a signed 32-bit
 * division by a runtime zero must TRAP on every target ISA under
 * qemu-*-static:
 *   aarch64 -> brk #0  -> SIGTRAP (rc 133)
 *   riscv64/riscv32 -> unimp -> SIGILL  (rc 132)
 * Width-4 (int is the natural word on RV32; the check runs before the
 * RV64/AArch64 width-4 result round-trip). The divisor comes from a
 * volatile load so no pass can constant-fold the zero away.
 *
 * Compiled with -fsanitize=undefined for --target aarch64/riscv64/riscv32
 * and run under qemu-*-static by tools/san_sweep.py (cross-trap section). */
int main(void) {
    volatile int v = 0;
    int d = (int)v;
    return 42 / d;
}
