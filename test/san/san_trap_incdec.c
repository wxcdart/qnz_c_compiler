/* zero_san v1 (Sprint 161): ++/-- signed overflow must trap (SIGILL, rc 132)
 * under -fsanitize=undefined. Guards the inc/dec OP_OVF_TRAP check at width 4
 * (int) across -O levels (the check must stay adjacent to its add through the
 * optimizer). */
#include <limits.h>

int main(void) {
    int x = INT_MAX;
    x++;                 /* INT_MAX + 1 overflows int: must trap */
    (void)x;
    return 0;
}
