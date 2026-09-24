/* Test for _Alignas and _Alignof */
#include <stdio.h>
#include <stddef.h>

_Static_assert(_Alignof(char) == 1, "char alignment");
_Static_assert(_Alignof(int) == 4, "int alignment");
_Static_assert(_Alignof(long long) == 8, "long long alignment");

typedef struct { _Alignas(16) int x; } S;
_Static_assert(_Alignof(S) == 16, "S alignment");
_Static_assert(offsetof(S, x) == 0, "offset");
_Static_assert(sizeof(S) == 16, "size");

_Static_assert(_Alignof(int [10]) == _Alignof(int), "array alignment");

int _Alignas(32) y;
_Static_assert(_Alignof(y) == 32, "y alignment");

int main(void) {
    printf("Align test passed\\n");
    return 0;
}