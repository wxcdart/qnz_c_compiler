/* Sprint 234: General Sibling Call Optimization (TCO Phase 2) unit test
 *
 * Verifies that tail calls between different functions (sibling calls)
 * reuse the caller's stack frame via tail jump, eliminating stack growth
 * and enabling deep mutual recursion without stack overflow.
 */

#include <stdio.h>
#include <assert.h>

/* Forward declarations for mutual recursion */
int is_odd(int n);
int is_even(int n);

int is_even(int n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}

int is_odd(int n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}

/* Sibling calls with multiple arguments and arithmetic */
int ping_add(int n, int acc);
int pong_add(int n, int acc);

int ping_add(int n, int acc) {
    if (n <= 0) return acc;
    return pong_add(n - 1, acc + 2);
}

int pong_add(int n, int acc) {
    if (n <= 0) return acc;
    return ping_add(n - 1, acc + 3);
}

/* Sibling call returning void */
static int global_val = 0;
void target_void(int x) {
    global_val += x;
}

void caller_void(int x) {
    target_void(x * 2);
}

/* Safety test: call taking address of local variable must NOT tail-jump */
int inspect_ptr(int *p) {
    return *p + 10;
}

int caller_with_local_addr(int v) {
    int local = v;
    return inspect_ptr(&local);
}

int main(void) {
    /* 1. Deep mutual recursion (depth 100,000): tests O(1) stack behavior */
    assert(is_even(100000) == 1);
    assert(is_odd(100000) == 0);
    assert(is_even(100001) == 0);
    assert(is_odd(100001) == 1);

    /* 2. Multi-arg mutual recursion */
    /* 10 iterations: ping_add(10, 0) -> pong(9, 2) -> ping(8, 5) ... total acc = 5*2 + 5*3 = 25 */
    int res = ping_add(10, 0);
    assert(res == 25);

    /* 3. Void sibling call */
    caller_void(5);
    assert(global_val == 10);

    /* 4. Local address guard */
    int local_res = caller_with_local_addr(7);
    assert(local_res == 17);

    printf("SIBLING_CALL_OK\n");
    return 0;
}
