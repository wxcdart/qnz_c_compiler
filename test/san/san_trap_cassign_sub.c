/* zero_san v1 (Sprint 161): compound -= signed overflow must trap (SIGILL,
 * rc 132) at width 4 (int). INT_MIN - 1 underflows. */
#include <limits.h>

int main(void) {
    int x = INT_MIN;
    x -= 1;              /* INT_MIN - 1 underflows int: must trap */
    (void)x;
    return 0;
}
