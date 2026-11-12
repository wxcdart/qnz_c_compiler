/* -fsanitize=leak (Sprint 169): every allocation is freed before exit.
 * The atexit report must stay silent and the process exits 0. */
#include <stdlib.h>

int main(void) {
    char *a = malloc(64);
    a[0] = 1;
    free(a);
    return 0;
}
