/* -fsanitize=leak (Sprint 169): the one live block at exit is an
 * intentionally-resident buffer, marked with zeroc_rt_leak_ignore (the
 * zero_rt analogue of __lsan_ignore_object). The report must skip it and
 * the process exits 0. */
#include <stdlib.h>

int main(void) {
    char *a = malloc(128);
    zeroc_rt_leak_ignore(a); /* intentionally resident for the process */
    (void) a;
    return 0;
}
