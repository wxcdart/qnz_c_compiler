#include <stdio.h>
#include <assert.h>

int test_stack_buffer(void) {
    int arr[10];
    for (int i = 0; i < 10; i++) {
        arr[i] = i * 42;
    }
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += arr[i];
    }
    return sum;
}

int test_local_vars(void) {
    int a = 100;
    int b = 200;
    int *pa = &a;
    int *pb = &b;
    *pa += 50;
    *pb += 50;
    return *pa + *pb;
}

int main(void) {
    int s = test_stack_buffer();
    printf("s = %d\n", s);
    int lv = test_local_vars();
    printf("lv = %d\n", lv);
    assert(s == 1890);
    assert(lv == 400);
    printf("asan_test passed\n");
    return 0;
}
