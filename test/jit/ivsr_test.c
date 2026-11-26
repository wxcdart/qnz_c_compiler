/* Sprint 235: Loop Induction Variable Strength Reduction (IVSR - Tier 4, Item #18)
 *
 * Verifies that array indexing addressing calculations:
 *   addr = base + i * scale
 * are reduced to induction pointer increments:
 *   ptr += step * scale
 */

#include <stdio.h>
#include <assert.h>

#define N 64

/* Test 1: Single array accumulation (read IVSR) */
int test_array_sum(const int *a, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i];
    }
    return sum;
}

/* Test 2: Array initialization with varying expression (write IVSR) */
void test_array_fill(int *a, int n) {
    for (int i = 0; i < n; i++) {
        a[i] = i * 3 + 1;
    }
}

/* Test 3: 64-bit long long array (scale = 8) */
long long test_int64_sum(const long long *a, int n) {
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i];
    }
    return sum;
}

/* Test 4: Multiple independent array accesses in same loop */
void test_multi_array(const int *a, const int *b, int *c, int n) {
    for (int i = 0; i < n; i++) {
        c[i] = a[i] + b[i] + 7;
    }
}

int main(void) {
    int a[N];
    int b[N];
    int c[N];
    long long ll[N];

    for (int i = 0; i < N; i++) {
        a[i] = i + 1;
        b[i] = (i + 1) * 2;
        ll[i] = (long long)(i + 1) * 1000LL;
    }

    /* 1. Array sum: 1 + 2 + ... + 64 = 64 * 65 / 2 = 2080 */
    int s1 = test_array_sum(a, N);
    assert(s1 == 2080);

    /* 2. Array fill */
    test_array_fill(c, N);
    for (int i = 0; i < N; i++) {
        assert(c[i] == i * 3 + 1);
    }

    /* 3. 64-bit sum: 2080 * 1000 = 2080000 */
    long long s3 = test_int64_sum(ll, N);
    assert(s3 == 2080000LL);

    /* 4. Multi array: a[i] + b[i] + 7 = 3 * (i + 1) + 7 */
    test_multi_array(a, b, c, N);
    for (int i = 0; i < N; i++) {
        assert(c[i] == (i + 1) * 3 + 7);
    }

    printf("ALL IVSR TESTS PASSED\n");
    return 0;
}
