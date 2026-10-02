/* Test for _Static_assert */
#include <stdio.h>
#include <assert.h>

static_assert(1 == 1, "one is one");
static_assert(sizeof(int) == 4, "int is 32-bit");

/* Uncomment the following line to see error handling */
// static_assert(0, "this should fail");

int main(void) {
    printf("Static assert test passed\\n");
    return 0;
}