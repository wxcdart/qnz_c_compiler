/* ABI interop: s12 stack frames, red zone, cross recursion (shared main, compiled by BOTH compilers)
 * expect: 202
 * expect: 25
 * expect: 15
 */
#include <stdio.h>
int zc_bigframe(int); int zc_recurse_z(int);
int gc_leaf_locals(int); int gc_recurse_g(int); int gc_wrap_big(int, int);
int main(void) {
    int r1 = gc_leaf_locals(3) + gc_wrap_big(5, 7) + zc_bigframe(11);
    printf("%d\n", r1);
    printf("%d\n", gc_recurse_g(5));
    printf("%d\n", zc_recurse_z(4));
    return 0;
}

