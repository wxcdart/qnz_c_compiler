/* zero_san UBSan (Sprint 266): valid pointers, VLAs, and booleans must never
 * trap under -fsanitize=undefined across -O0..-O3 in JIT and AOT. */
#include <stdbool.h>

static int test_pointers(void) {
    int val = 123;
    int *p = &val;
    *p = 456;
    if (*p != 456) return 1;
    struct S { int a; int b; } s = {10, 20};
    struct S *sp = &s;
    if (sp->a + sp->b != 30) return 2;
    return 0;
}

static int test_vla(int n) {
    int arr[n];
    for (int i = 0; i < n; i++) {
        arr[i] = i * 2;
    }
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}

static int test_bool(void) {
    bool t = true;
    bool f = false;
    if (!t) return 1;
    if (f) return 2;
    unsigned char z = 0, o = 1;
    bool b0 = *(bool *)&z;
    bool b1 = *(bool *)&o;
    if (b0) return 3;
    if (!b1) return 4;
    return 0;
}

int main(void) {
    if (test_pointers() != 0) return 1;
    if (test_vla(10) != 90) return 2;
    if (test_bool() != 0) return 3;
    return 0;
}
