/* Test for _Generic */
#include <stdio.h>

#define FMT(X) _Generic((X), \
    int: "%d\\n", \
        double: "%f\\n", \
            void*: "%p\\n", \
            default: "unknown\\n")

int main(void) {
    int i = 42;
    double d = 3.14;
    void *p = &i;
    printf(FMT(i), i);
    printf(FMT(d), d);
    printf(FMT(p), p);
    printf(FMT("string")); // default
    return 0;
}