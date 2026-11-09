/* Sprint 147: Stack protection tests (-fstack-protector[-all|-strong]).
 *
 * Exercises stack canary insertion and verification across:
 * - array >= 8 bytes
 * - struct containing array
 * - address taken of local variable
 * - VLA
 * - nested function calls
 */

#include <stddef.h>

static int test_array_large(int x) {
    char buf[16];
    for (int i = 0; i < 16; i++) {
        buf[i] = (char)(x + i);
    }
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += (int)buf[i];
    }
    return sum;
}

struct Inner {
    char arr[4];
};

struct Outer {
    int id;
    struct Inner inner;
};

static int test_struct_with_array(int val) {
    struct Outer s;
    s.id = val;
    s.inner.arr[0] = 1;
    s.inner.arr[1] = 2;
    s.inner.arr[2] = 3;
    s.inner.arr[3] = 4;
    return s.id + s.inner.arr[0] + s.inner.arr[1] + s.inner.arr[2] + s.inner.arr[3];
}

static void helper_inc(int *p) {
    *p += 42;
}

static int test_addr_taken(int initial) {
    int local = initial;
    helper_inc(&local);
    return local;
}

static int test_vla(int n) {
    if (n <= 0) return 0;
    char vla[n];
    for (int i = 0; i < n; i++) {
        vla[i] = (char)i;
    }
    int acc = 0;
    for (int i = 0; i < n; i++) {
        acc += (int)vla[i];
    }
    return acc;
}

int main(void) {
    int r1 = test_array_large(1);
    if (r1 != 136) return 1;

    int r2 = test_struct_with_array(10);
    if (r2 != 20) return 2;

    int r3 = test_addr_taken(8);
    if (r3 != 50) return 3;

    int r4 = test_vla(10);
    if (r4 != 45) return 4;

    return 0;
}
