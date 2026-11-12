/* -fsanitize=leak (Sprint 169): a leak AND a non-zero exit status. The
 * report still fires (stderr), but the process must keep its own exit
 * code (7) - the leak override (23) must not mask a real failure status. */
#include <stdlib.h>

int main(void) {
    char *a = malloc(64);
    (void) a;
    return 7;
}
