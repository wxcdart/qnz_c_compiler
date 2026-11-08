/* Inline asm Phase 1: clobbers, volatile memory barriers, and asm in
 * loops. Clobbered-register liveness is the crux: values that are live
 * across the asm must survive (spilled by the clobber pass when the
 * template overwrites their register).
 */
#include <stdio.h>

volatile int vmem = 41;

int main(void) {
    /* Several values live across a clobber of rax/rdx/r8: the allocator
     * may place any of p/q/s in the clobbered registers; all must survive. */
    int p = 100, q = 200, s = 300;
    __asm__ volatile("movl $1, %%eax; movl $2, %%edx; movl $3, %%r8d"
                     : "+r"(p) : "r"(q) : "rax", "rdx", "r8", "cc", "memory");
    p += q + s;

    /* memory clobber orders the volatile store before the load */
    vmem = 77;
    __asm__ volatile("" ::: "memory");
    int w = vmem + 3;

    /* in-loop asm (exercises LICM/branchless arena rebuilds) */
    int acc = 0;
    for (int i = 0; i < 5; i++) {
        int t = i * 3;
        __asm__("addl $10, %0" : "+r"(t));
        acc += t;
    }

    /* cc clobber: flags are not part of the IR; value must survive
     * (rax is clobbered by the template and declared as such) */
    int c1 = 9, c2 = 11;
    __asm__("xorl %%eax, %%eax" : "+r"(c1) : "r"(c2) : "rax", "cc");
    c1 += c2;

    printf("%d %d %d %d\n", p, w, acc, c1);
    int ok = (p == 600 && w == 80 && acc == 80 && c1 == 20);
    printf("%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
