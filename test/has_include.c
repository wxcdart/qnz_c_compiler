/* Test for __has_include */
#include <stdio.h>
#if __has_include(<stdio.h>)
#   define HAVE_STDIO 1
#else
#   define HAVE_STDIO 0
#endif

#if __has_include("nonexistent.h")
#   define HAVE_NONE 1
#else
#   define HAVE_NONE 0
#endif

_Static_assert(HAVE_STDIO == 1, "stdio.h should be found");
_Static_assert(HAVE_NONE == 0, "nonexistent.h should not be found");

int main(void) {
    printf("Has include test passed\\n");
    return 0;
}