/* Test for _Thread_local */
#include <stdio.h>
_Thread_local int tls_var = 5;

int foo(void) {
    return tls_var;
}

int main(void) {
    printf("TLS test: %d\\n", foo());
    return 0;
}