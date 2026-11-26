#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 1. Basic heap-to-stack promotion */
int test_basic_promotion(void) {
    char *p = (char *)malloc(64);
    for (int i = 0; i < 64; i++) {
        p[i] = (char)(i * 3 + 1);
    }
    int sum = 0;
    for (int i = 0; i < 64; i++) {
        sum += (unsigned char)p[i];
    }
    free(p);
    return sum;
}

/* 2. Multiple promoted allocations */
int test_multiple_promotions(int a, int b) {
    int *p1 = (int *)malloc(10 * sizeof(int));
    int *p2 = (int *)malloc(10 * sizeof(int));
    for (int i = 0; i < 10; i++) {
        p1[i] = a + i;
        p2[i] = b - i;
    }
    int total = 0;
    for (int i = 0; i < 10; i++) {
        total += p1[i] * p2[i];
    }
    free(p1);
    free(p2);
    return total;
}

/* 3. Dead allocation elimination (only malloc + free, no stores/loads) */
int test_dead_allocation(int x) {
    void *p = malloc(128);
    free(p);
    return x * 2;
}

/* 4. Escaping pointer (returned) - must NOT be promoted */
char *g_escaped = NULL;
char *test_escaping_return(void) {
    char *p = (char *)malloc(32);
    strcpy(p, "hello escape");
    return p;
}

/* 5. Escaping pointer (stored to global) - must NOT be promoted */
void test_escaping_global(void) {
    char *p = (char *)malloc(32);
    strcpy(p, "global store");
    g_escaped = p;
}

/* 6. Escaping pointer (passed to unknown function) - must NOT be promoted */
void external_consumer(const char *s) {
    assert(strcmp(s, "consumer") == 0);
}

void test_escaping_call(void) {
    char *p = (char *)malloc(32);
    strcpy(p, "consumer");
    external_consumer(p);
    free(p);
}

int main(void) {
    /* Test 1 */
    int sum = test_basic_promotion();
    int expected_sum = 0;
    for (int i = 0; i < 64; i++) {
        expected_sum += (unsigned char)(char)(i * 3 + 1);
    }
    assert(sum == expected_sum);

    /* Test 2 */
    int total = test_multiple_promotions(5, 15);
    int expected_total = 0;
    for (int i = 0; i < 10; i++) {
        expected_total += (5 + i) * (15 - i);
    }
    assert(total == expected_total);

    /* Test 3 */
    assert(test_dead_allocation(21) == 42);

    /* Test 4 */
    char *ret = test_escaping_return();
    assert(strcmp(ret, "hello escape") == 0);
    free(ret);

    /* Test 5 */
    test_escaping_global();
    assert(g_escaped != NULL);
    assert(strcmp(g_escaped, "global store") == 0);
    free(g_escaped);
    g_escaped = NULL;

    /* Test 6 */
    test_escaping_call();

    printf("All Escape Analysis & Heap-to-Stack Promotion tests PASSED\n");
    return 0;
}
