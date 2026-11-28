/* zero_san UBSan (Sprint 266): load of invalid boolean value must trap
 * (SIGILL, rc 132) at every -O level. */
#include <stdbool.h>

int main(void) {
    volatile unsigned char c = 42;
    bool b = *(bool *)&c;
    (void)b;
    return 0;
}
