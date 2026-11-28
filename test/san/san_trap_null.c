/* zero_san UBSan (Sprint 266): null pointer dereference must trap
 * (SIGILL, rc 132) at every -O level. */
#include <stddef.h>

int main(void) {
    volatile int *p = NULL;
    int x = *p;
    (void)x;
    return 0;
}
