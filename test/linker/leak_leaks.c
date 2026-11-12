/* -fsanitize=leak (Sprint 169): the program leaks 64 bytes and exits 0.
 * Linked with --zerocc-link -fsanitize=leak: the atexit report must print
 * the block and the process must exit 23 (LSan's convention). Linked
 * without the flag: plain exit 0, no report. */
#include <stdlib.h>

int main(void) {
    char *a = malloc(64);
    char *b = malloc(32);
    free(b);   /* only `a` is still allocated at exit */
    (void) a;
    return 0;
}
