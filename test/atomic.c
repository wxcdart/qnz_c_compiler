/* Test for _Atomic */
#include <stdio.h>

_Atomic int counter = 0;

int foo(void) {
    return __atomic_load_n(&counter, __ATOMIC_SEQ_CST);
}

void bar(void) {
    __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
}

_Static_assert(sizeof(_Atomic int) == sizeof(int), "atomic size");
_Static_assert(sizeof(_Atomic long long) == sizeof(long long), "atomic long long size");

int main(void) {
    printf("Atomic test passed\\n");
    return 0;
}