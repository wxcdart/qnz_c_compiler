/* Sprint 164: signed int compound assignment overflow must trap on every ISA,
 * raw/MC path included (see san_trap_cross_incdec.c). */
#include <limits.h>
int main(void) {
    volatile int a = INT_MAX;
    volatile int b = 1;
    a += b;
    return 0;
}
