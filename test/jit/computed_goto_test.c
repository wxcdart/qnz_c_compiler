#include <stdio.h>

int test_simple_goto(void) {
    int x = 0;
    void *target = &&my_target;
    goto *target;
    x = 10;
my_target:
    x += 5;
    return x; /* expected 5 */
}

int test_dispatch_table(void) {
    static void *ops[] = { &&do_inc, &&do_double, &&do_exit };
    int val = 1;
    int pc = 0;

    int bytecode[] = { 0, 0, 1, 0, 2 }; /* inc, inc, double, inc, exit: 1 -> 2 -> 3 -> 6 -> 7 */
    int idx = 0;

loop:
    pc = bytecode[idx++];
    goto *ops[pc];

do_inc:
    val++;
    goto loop;

do_double:
    val *= 2;
    goto loop;

do_exit:
    return val;
}

int test_gnu_local_label(void) {
    __label__ local1, local2;
    int a = 42;
    void *p = &&local1;
    goto *p;
    a = 0;
local1:
    p = &&local2;
    goto *p;
    a = 1;
local2:
    return a; /* expected 42 */
}

int main(void) {
    if (test_simple_goto() != 5) {
        printf("FAIL: test_simple_goto\n");
        return 1;
    }
    if (test_dispatch_table() != 7) {
        printf("FAIL: test_dispatch_table\n");
        return 2;
    }
    if (test_gnu_local_label() != 42) {
        printf("FAIL: test_gnu_local_label\n");
        return 3;
    }
    printf("PASS: computed goto\n");
    return 0;
}
