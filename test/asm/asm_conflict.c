/* Inline asm Phase 1: clobber/operand conflict stress.
 * The allocator must keep asm operands out of registers the template
 * clobbers (forbidden_mask), and values live across the asm must survive.
 * Clobbering most of the allocatable pool forces the operands onto the
 * remaining registers.
 */
#include <stdio.h>

int main(void) {
    int x = 5, y = 6;
    __asm__ volatile("nop" : "+r"(x) : "r"(y)
        : "rdi", "rsi", "rdx", "r8", "r9", "rbx", "r12", "r13", "r14", "r15");
    if (x != 5 || y != 6) return 1;

    /* operand + declared clobbers with several live values: the allocator
     * must keep a/b/cc/dd out of the clobbered registers */
    int a = 111, b = 222, cc = 333, dd = 444;
    __asm__ volatile("xorl %%eax, %%eax"
                     : "+r"(a), "+r"(b) : "r"(cc), "r"(dd)
                     : "rax", "cc", "memory");
    if (a != 111 || b != 222 || cc != 333 || dd != 444) return 2;

    /* volatile asm in a nested loop: rebuild passes must keep the
     * side-table indices aligned */
    long acc = 0;
    for (int i = 0; i < 4; i++) {
        int t = i + 1;
        for (int j = 0; j < 3; j++) {
            __asm__("addl $1, %0" : "+r"(t));
        }
        acc += t;
    }
    /* i=0..3: t = i+1 + 3 = 4,5,6,7 -> acc = 22 */
    if (acc != 22) return 3;

    printf("ok\n");
    return 0;
}
