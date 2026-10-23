#include <stdio.h>
#include <assert.h>

// Helper functions for function pointer testing
static int fn_a(float x) { return (int)x + 1; }
static int fn_b(float x) { return (int)x + 2; }
static int fn_c(float x) { return (int)x + 3; }

static int sample_arr[4] = { 10, 20, 30, 40 };
#ifdef __ZEROCC__
static void *get_sample_arr(int dummy) {
    (void)dummy;
    return &sample_arr;
}
#else
typedef int (*RetArrPtr)[4];
static RetArrPtr get_sample_arr(int dummy) {
    (void)dummy;
    return (RetArrPtr)&sample_arr;
}
#endif

int main(void) {
    // 1. Function returning pointer to array[4]
    // Local function prototype returning pointer to array[4]
    int (*get_sample_arr(int dummy))[4];
    int (*arr_ptr2)[4] = get_sample_arr(0);
    assert(arr_ptr2[0][0] == 10);
    assert(arr_ptr2[0][1] == 20);
    assert(arr_ptr2[0][2] == 30);
    assert(arr_ptr2[0][3] == 40);

    // Pointer to function returning pointer to array[4]
    int (*(*fp)(int))[4] = (void *)get_sample_arr;
    int (*arr_ptr)[4] = fp(0);
    assert(arr_ptr[0][0] == 10);
    assert(arr_ptr[0][1] == 20);
    assert(arr_ptr[0][2] == 30);
    assert(arr_ptr[0][3] == 40);

    // 2. Array[3] of function(float) -> int
    int (*a[3])(float);
    a[0] = fn_a;
    a[1] = fn_b;
    a[2] = fn_c;
    assert(a[0](10.0f) == 11);
    assert(a[1](10.0f) == 12);
    assert(a[2](10.0f) == 13);

    // 3. Interleaved qualifiers: const int **restrict *p
    int val = 42;
    int *p_val = &val;
    const int *cp_val = p_val;
    const int **pp_val = &cp_val;
    const int **restrict *p = &pp_val;
    assert(***p == 42);

    // 4. Abstract declarators in sizeof and casts
    assert(sizeof(int (*)(void)) == sizeof(void *));
    assert(sizeof(int (*)[4]) == sizeof(void *));
    assert(sizeof(int (*[3])(float)) == 3 * sizeof(void *));

    void *generic_fp = (void *)fn_a;
    int (*cast_fp)(float) = (int (*)(float))generic_fp;
    assert(cast_fp(4.0f) == 5);

    // 5. Existing cases: int *p_arr[10]
    int x1 = 1, x2 = 2;
    int *p_arr[10];
    p_arr[0] = &x1;
    p_arr[9] = &x2;
    assert(*p_arr[0] == 1);
    assert(*p_arr[9] == 2);

    // 6. int (*q)(float)
    int (*q)(float) = fn_c;
    assert(q(7.0f) == 10);

    // 7. Multi-declarator mixing pointers and arrays
    int *m1, m_arr[5] = {10, 20, 30, 40, 50}, *m2;
    m1 = &m_arr[1];
    m2 = &m_arr[4];
    assert(*m1 == 20);
    assert(*m2 == 50);

    // 8. VLA
    int n = 5;
    int vla[n];
    for (int i = 0; i < n; i++) {
        vla[i] = (i + 1) * 10;
    }
    assert(vla[0] == 10);
    assert(vla[4] == 50);

    printf("All declarator stack tests passed successfully!\n");
    return 0;
}
