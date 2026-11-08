/* Inline asm barrier forms: empty templates with operands and clobbers.
 * These take the C-level barrier lowering path (no machine text), so the
 * file is dual-compiler: host gcc and zerocc (JIT + AOT) must agree on the
 * observable behavior. The non-empty AOT text path is covered by
 * test/asm/ via `make test-asm`.
 */
#include <stdio.h>

static volatile int shared = 41;

int main(void) {
    int a = 5, b = 6;
    /* operand evaluation order around a barrier */
    __asm__("" : "+r"(a));
    a += b;

    /* read-after-barrier of a volatile */
    int v = 0;
    __asm__ volatile("" ::: "memory");
    v = shared;

    /* digit-matched operand through a barrier */
    int x = 7;
    __asm__("" : "=r"(x) : "0"(x + 1) :);
    x += 2;

    /* clobbered registers / flags must not leak into C code; +r keeps the
     * operand value defined (an empty-template "=r" output would be UB) */
    int r = 100;
    __asm__("" : "+r"(r) :: "cc", "memory");

    int sum = a + v + x + r;
    printf("%d\n", sum);
    /* a=11, v=41, x=10, r=100 */
    return sum == 162 ? 0 : 1;
}
