/* Test for _Alignas and _Alignof */
#include <stdio.h>
#include <stddef.h>
#include <stdalign.h>

_Static_assert(alignof(char) == 1, "char alignment");
_Static_assert(alignof(int) == 4, "int alignment");
_Static_assert(alignof(long long) == 8, "long long alignment");

typedef struct { alignas(16) int x; } S;
_Static_assert(alignof(S) == 16, "S alignment");
_Static_assert(offsetof(S, x) == 0, "offset");
_Static_assert(sizeof(S) == 16, "size");

_Static_assert(alignof(int [10]) == alignof(int), "array alignment");

int alignas(32) y;
_Static_assert(alignof(y) == 32, "y alignment");

int main(void) {
    printf("Align test passed\\n");
    return 0;
}