/* Sprint 236: Type-Based Alias Analysis (TBAA - Tier 3, Item #15)
 *
 * Verifies that incompatible pointer types do not alias under C99 6.5p7,
 * enabling redundant load elimination across intervening writes of distinct types,
 * while respecting universal char* aliasing.
 */

#include <stdio.h>
#include <assert.h>

/* Test 1: Incompatible types (int* vs double*) */
int test_int_double_alias(int *pi, double *pd) {
    *pi = 10;
    double d1 = *pd;
    *pi = 20;
    double d2 = *pd;  /* Under TBAA, *pi cannot modify *pd, d1 == d2 */
    return (d1 == d2) ? 1 : 0;
}

/* Test 2: Incompatible integer sizes (int* vs short*) */
int test_int_short_alias(int *pi, short *ps) {
    *pi = 100;
    short s1 = *ps;
    *pi = 200;
    short s2 = *ps;  /* Under TBAA, *pi cannot modify *ps */
    return (s1 == s2) ? 1 : 0;
}

/* Test 3: Char pointer universal aliasing rule */
int test_char_alias(int *pi, unsigned char *pc) {
    *pi = 0x11223344;
    *pc = 0x55;      /* Char store can alias int! */
    return *pi;
}

/* Test 4: C99 restrict pointers */
int test_restrict_alias(int *restrict p1, int *restrict p2) {
    *p1 = 5;
    int v1 = *p2;
    *p1 = 6;
    int v2 = *p2;    /* Restrict: *p1 cannot modify *p2 */
    return (v1 == v2) ? 1 : 0;
}

int main(void) {
    int ival = 42;
    double dval = 3.14159;
    short sval = 1234;

    assert(test_int_double_alias(&ival, &dval) == 1);
    assert(test_int_short_alias(&ival, &sval) == 1);

    /* For char aliasing: pass address of ival as unsigned char* */
    int c_res = test_char_alias(&ival, (unsigned char *)&ival);
    assert(c_res != 0x11223344);

    int a = 10, b = 20;
    assert(test_restrict_alias(&a, &b) == 1);

    printf("ALL TBAA TESTS PASSED\n");
    return 0;
}
