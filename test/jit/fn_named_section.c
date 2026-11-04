/* Sprint 108: __attribute__((section("name"))) on functions.
 *
 * Exercised two ways:
 *   - `make test-opt` compiles and runs this with zerocc at -O0..-O3 in both
 *     JIT and AOT modes: the section attribute must not break codegen, and the
 *     named-section functions must be callable and return the right values.
 *     (In JIT/AOT-executable mode the code stays in the normal text region -
 *     the section is an AOT `-c` object-emission concern - so this verifies
 *     the attribute is consumed without breaking execution.)
 *   - the c-torture gate (compile/section.c) verifies the AOT `-c` object
 *     actually places the function in the named ELF section.
 *
 * Two functions share one named section (.zc_fn_sec); a third stays in .text
 * and calls both, so the call relocations resolve to the named section.
 */
#include <stdio.h>

__attribute__((section(".zc_fn_sec"))) int fn_square(int x) { return x * x; }
__attribute__((section(".zc_fn_sec"))) int fn_add(int a, int b) { return a + b; }

int main(void) {
    int s = fn_square(9);          /* 81 */
    int a = fn_add(s, 1);          /* 82 */
    if (s != 81 || a != 82) {
        printf("named-section function call fail: s=%d a=%d\n", s, a);
        return 1;
    }
    printf("named-section functions callable (2 in .zc_fn_sec): OK\n");
    return 0;
}
