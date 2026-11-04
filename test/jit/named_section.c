/* Sprint 107: __attribute__((section("name"))) on global objects.
 *
 * Exercised two ways:
 *   - `make test-opt` compiles and runs this with zerocc at -O0..-O3 in both
 *     JIT and AOT modes: the section attribute must not break codegen, and the
 *     objects must round-trip.  (gcc -O2 reference build also required to
 *     match, which exercises the attribute through the host compiler too.)
 *   - the c-torture gate (compile/pr23237.c, compile/pr61848.c) verifies the
 *     AOT `-c` object actually places the objects in the named ELF section.
 *
 * Two objects share one named section (.zc_named_a) to exercise the
 * multi-object per-section layout; a third lives in .zc_named_b.
 */
#include <stdio.h>

static __attribute__((section(".zc_named_a"))) int g_named_int = 0x1234;
static __attribute__((section(".zc_named_a"))) int g_named_int2 = 0x5678;
__attribute__((section(".zc_named_b"))) long g_named_long = 0x99;

int main(void) {
    int a = g_named_int;
    int b = g_named_int2;
    long c = g_named_long;
    g_named_int = 0xA0;
    g_named_int2 = 0xB0;
    g_named_long = 0x77;
    if (a != 0x1234 || b != 0x5678 || c != 0x99) {
        printf("initial read fail\n");
        return 1;
    }
    if (g_named_int != 0xA0 || g_named_int2 != 0xB0 || g_named_long != 0x77) {
        printf("write fail\n");
        return 1;
    }
    printf("named-section objects round-trip (2 in .zc_named_a, 1 in .zc_named_b)\n");
    return 0;
}
