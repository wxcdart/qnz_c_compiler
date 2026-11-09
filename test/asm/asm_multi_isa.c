/* Cross-ISA inline assembly test (Sprint 138 Phase 4).
 * Exercises inline asm with basic math, in/out operands, and goto labels
 * across x86-64, AArch64, and RISC-V 64.
 */
#include <stdio.h>

int test_add(int a, int b) {
    int r;
#if defined(__x86_64__)
    __asm__("addl %2, %0" : "=r"(r) : "0"(a), "r"(b));
#elif defined(__aarch64__)
    __asm__("add %w0, %w1, %w2" : "=r"(r) : "r"(a), "r"(b));
#elif defined(__riscv)
    __asm__("addw %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
#else
    r = a + b;
#endif
    return r;
}

long test_add_long(long a, long b) {
    long r;
#if defined(__x86_64__)
    __asm__("addq %2, %0" : "=r"(r) : "0"(a), "r"(b));
#elif defined(__aarch64__)
    __asm__("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
#elif defined(__riscv)
    __asm__("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
#else
    r = a + b;
#endif
    return r;
}

int test_inout(int a) {
#if defined(__x86_64__)
    __asm__("addl $5, %0" : "+r"(a));
#elif defined(__aarch64__)
    __asm__("add %w0, %w0, #5" : "+r"(a));
#elif defined(__riscv)
    __asm__("addiw %0, %0, 5" : "+r"(a));
#else
    a += 5;
#endif
    return a;
}

int test_goto(int a) {
#if defined(__x86_64__)
    __asm__ goto("cmpl $10, %0; jge %l[is_ge]" : : "r"(a) : "cc" : is_ge);
#elif defined(__aarch64__)
    __asm__ goto("cmp %w0, #10; b.ge %l[is_ge]" : : "r"(a) : "cc" : is_ge);
#elif defined(__riscv)
    __asm__ goto("li t0, 10; bge %0, t0, %l[is_ge]" : : "r"(a) : "t0" : is_ge);
#else
    if (a >= 10) goto is_ge;
#endif
    return 0;
is_ge:
    return 1;
}

int main(void) {
    int r1 = test_add(15, 27);          /* 42 */
    long r2 = test_add_long(100, 250);     /* 350 */
    int r3 = test_inout(20);             /* 25 */
    int g1 = test_goto(5);               /* 0 */
    int g2 = test_goto(15);              /* 1 */
    printf("%d %ld %d %d %d\n", r1, r2, r3, g1, g2);
    int ok = (r1 == 42 && r2 == 350 && r3 == 25 && g1 == 0 && g2 == 1);
    printf("%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
