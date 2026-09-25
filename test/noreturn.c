/* Test for _Noreturn */
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>

noreturn void fatal(void) {
    exit(1);
}

void test(void) {
    fatal(); /* control never returns here */
}

int main(void) {
    printf("Noreturn test passed (if we get here)\\n");
    return 0;
}