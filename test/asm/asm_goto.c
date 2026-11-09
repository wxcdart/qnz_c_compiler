/* Inline asm Phase 3: asm goto, CFG branching, label operands (%l0 / %l[name]),
 * and dialect alternatives ({att | intel}).
 * Differential test: verified across -S (text gas), --run (JIT MC), and
 * ZC_RAW_AOT=1 (raw ELF MC) against host gcc -O2.
 */
#include <stdio.h>

static int test_uncond(void) {
    __asm__ goto ("jmp %l0" : : : : target);
    return 1;
target:
    return 0;
}

static int test_named(void) {
    __asm__ goto ("jmp %l[target]" : : : : target);
    return 1;
target:
    return 0;
}

static int test_cond(int a, int b) {
    __asm__ goto ("cmp %1, %0; jl %l[is_less]" : : "r"(a), "r"(b) : "cc" : is_less);
    return 0;
is_less:
    return 1;
}

static int test_multi(int code) {
    __asm__ goto ("cmp $1, %0; je %l[l1]; cmp $2, %0; je %l[l2]"
                  : : "r"(code) : "cc" : l1, l2);
    return 0;
l1:
    return 10;
l2:
    return 20;
}

static int test_goto_out(int in) {
    int out = 0;
    __asm__ goto ("movl %1, %0; testl %0, %0; jz %l[zero]"
                  : "=r"(out)
                  : "r"(in)
                  : "cc"
                  : zero);
    return out;
zero:
    return -1;
}

static int test_empty_goto(void) {
    __asm__ goto ("" : : : : target);
    return 42;
target:
    return 99;
}

static int test_dialect(int a) {
    int out = 0;
    __asm__ ("{movl %1, %0 | mov %0, %1}" : "=r"(out) : "r"(a));
    return out;
}

int main(void) {
    int u = test_uncond();
    int n = test_named();
    int c1 = test_cond(3, 5);   /* 3 < 5 -> 1 */
    int c2 = test_cond(7, 2);   /* 7 not < 2 -> 0 */
    int m0 = test_multi(0);     /* 0 */
    int m1 = test_multi(1);     /* 10 */
    int m2 = test_multi(2);     /* 20 */
    int o1 = test_goto_out(42);  /* 42 */
    int o2 = test_goto_out(0);   /* -1 */
    int eg = test_empty_goto(); /* 42 */
    int d = test_dialect(99);    /* 99 */

    printf("%d %d %d %d %d %d %d %d %d %d %d\n",
           u, n, c1, c2, m0, m1, m2, o1, o2, eg, d);
    int ok = (u == 0 && n == 0 && c1 == 1 && c2 == 0 &&
              m0 == 0 && m1 == 10 && m2 == 20 &&
              o1 == 42 && o2 == -1 && eg == 42 && d == 99);
    printf("%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
