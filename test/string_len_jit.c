#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

/*
 * Test that a string literal is correctly emitted to .rodata and that the
 * generated code can read it and compute its length at runtime.
 *
 * The program loads the literal "hello", walks the characters until the NUL
 * terminator, counts them, and returns 1 if the length equals 5, otherwise 0.
 * The JIT execution should therefore return 1.
 */
int main(void) {
    const char *s = "hello";
    int len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len == 5 ? 1 : 0;
}
