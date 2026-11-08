/* Inline asm Phase 2 (Sprint 136): non-empty templates under JIT (--run)
 * and raw AOT (ZC_RAW_AOT=1).
 * Dual-compiler test: must build and run identically under host gcc and zerocc
 * across -O0..-O3 in both JIT and AOT modes.
 */
#include <stdio.h>

int main(void) {
    int r = 0;
    __asm__("movl $42, %0" : "=r"(r));

    int v = 5;
    __asm__("addl $7, %0" : "+r"(v));

    int m = 0;
    __asm__("movl $99, %0" : "=m"(m));

    int a = 10, b = 20;
    __asm__("addl %1, %0" : "+r"(a) : "r"(b));

    int dm = 15;
    __asm__("addl $3, %0" : "=r"(dm) : "0"(dm));

    long l = 0;
    __asm__("movq $1234567890123, %0" : "=r"(l));

    unsigned long ul = 5;
    __asm__("notq %0" : "+r"(ul) : );

    int p = 1, q = 2;
    __asm__ volatile("nop" : "+r"(p) : "r"(q));

    int c1 = 10, c2 = 15;
    __asm__("xorl %%eax, %%eax" : "+r"(c1) : "r"(c2) : "rax", "cc");

    unsigned int lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));

    printf("%d %d %d %d %d %ld %lu %d\n", r, v, m, a, dm, l, ul, (lo != 0 || hi != 0) ? 1 : 0);

    int ok = (r == 42 && v == 12 && m == 99 && a == 30 && dm == 18 &&
              l == 1234567890123L && ul == ~5UL && (lo != 0 || hi != 0));
    return ok ? 0 : 1;
}
