/* Float-argument passing when a call has more than 6 integer arguments.
 *
 * The int-arg setup pushes the 7th+ integer arguments onto the stack
 * before the call, shifting the stack pointer. The FP-arg loads read
 * their spill slots with frame-relative offsets, so they must be
 * emitted BEFORE that shift - otherwise every float argument lands in
 * the wrong register slot (printf shows a stale value while in-program
 * comparisons can still be correct).
 *
 * Covers: 7 ints + 2 doubles (n_stack=1 with pad, xmm0 & xmm1) and
 * 9 ints + 1 double (n_stack=3, odd -> 8-byte pad).
 */
#include <stdio.h>

int i1 = 1, i2 = 2, i3 = 3, i4 = 4, i5 = 5, i6 = 6, i7 = 7;
double da = 1.25, db = 2.5;

int main(void) {
    printf("a=%d b=%d c=%d d=%d e=%d f=%d g=%d x=%g y=%g\n",
           i1, i2, i3, i4, i5, i6, i7, da, db);
    printf("1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d 8=%d 9=%d z=%g\n",
           i1, i2, i3, i4, i5, i6, i7, i1 + 1, i2 + 1, da);
    return 0;
}
