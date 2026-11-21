#include <stdio.h>

#define PTR_KIND(X) _Generic((X), \
    int *: 1, \
    const int *: 2, \
    volatile int *: 3, \
    const volatile int *: 4, \
    default: 0)

#define PTR2_KIND(X) _Generic((X), \
    int **: 10, \
    const int **: 20, \
    int * const *: 30, \
    const int * const *: 40, \
    default: 0)

#define ARR_PTR_KIND(X) _Generic((X), \
    int (*)[4]: 100, \
    const int (*)[4]: 200, \
    int (*)[2][3]: 300, \
    const int (*)[2][3]: 400, \
    default: 0)

#define FNPTR_KIND(X) _Generic((X), \
    int (*)(int *): 1, \
    int (*)(const int *): 2, \
    default: 0)

static int fn_mut(int *p) { return *p; }
static int fn_const(const int *p) { return *p; }

int main(void) {
    int x = 1;
    int *p = &x;
    const int *cp = &x;
    volatile int *vp = &x;
    const volatile int *cvp = &x;

    /* 1. Single-level pointee qualifications */
    if (PTR_KIND(p) != 1 || PTR_KIND(cp) != 2 ||
        PTR_KIND(vp) != 3 || PTR_KIND(cvp) != 4) {
        printf("FAIL single ptr: %d %d %d %d\n",
               PTR_KIND(p), PTR_KIND(cp), PTR_KIND(vp), PTR_KIND(cvp));
        return 1;
    }

    /* 2. Top-level qualifiers stripped from controlling expression */
    int * const kp = &x;
    const int * const kcp = &x;
    volatile int * const kvp = &x;
    if (PTR_KIND(kp) != 1 || PTR_KIND(kcp) != 2 || PTR_KIND(kvp) != 3) {
        printf("FAIL top-level stripped: %d %d %d\n",
               PTR_KIND(kp), PTR_KIND(kcp), PTR_KIND(kvp));
        return 1;
    }

    /* Cast controlling expressions starting with const/volatile */
    if (PTR_KIND((const int *)0) != 2 || PTR_KIND((volatile int *)0) != 3) {
        printf("FAIL cast ctrl: %d %d\n",
               PTR_KIND((const int *)0), PTR_KIND((volatile int *)0));
        return 1;
    }

    /* 3. Multi-level pointer qualifications */
    int **pp = &p;
    const int **cpp = &cp;
    int * const *pcp = &kp;
    const int * const *cpcp = &kcp;
    const int * const * const kcpcp = &kcp;
    if (PTR2_KIND(pp) != 10 || PTR2_KIND(cpp) != 20 ||
        PTR2_KIND(pcp) != 30 || PTR2_KIND(cpcp) != 40 ||
        PTR2_KIND(kcpcp) != 40) {
        printf("FAIL multilevel ptr: %d %d %d %d %d\n",
               PTR2_KIND(pp), PTR2_KIND(cpp), PTR2_KIND(pcp),
               PTR2_KIND(cpcp), PTR2_KIND(kcpcp));
        return 1;
    }

    /* 4. Array-to-pointer decay & C11 6.7.3p9 array-element qualification propagation */
    int a4[4] = {1, 2, 3, 4};
    const int ca4[4] = {1, 2, 3, 4};
    const int ca23[2][3] = {{1, 2, 3}, {4, 5, 6}};
    int a23[2][3] = {{1, 2, 3}, {4, 5, 6}};
    if (PTR_KIND(a4) != 1 || PTR_KIND(ca4) != 2) {
        printf("FAIL array decay: %d %d\n", PTR_KIND(a4), PTR_KIND(ca4));
        return 1;
    }
    if (ARR_PTR_KIND(&a4) != 100 || ARR_PTR_KIND(&ca4) != 200 ||
        ARR_PTR_KIND(&a23) != 300 || ARR_PTR_KIND(&ca23) != 400) {
        printf("FAIL ptr-to-array: %d %d %d %d\n",
               ARR_PTR_KIND(&a4), ARR_PTR_KIND(&ca4),
               ARR_PTR_KIND(&a23), ARR_PTR_KIND(&ca23));
        return 1;
    }

    /* 5. Function pointer parameter qualifications */
    int (*fpm)(int *) = fn_mut;
    int (*fpc)(const int *) = fn_const;
    if (FNPTR_KIND(fpm) != 1 || FNPTR_KIND(fpc) != 2 ||
        FNPTR_KIND(fn_mut) != 1 || FNPTR_KIND(fn_const) != 2 ||
        FNPTR_KIND(&fn_mut) != 1 || FNPTR_KIND(&fn_const) != 2) {
        printf("FAIL fnptr: %d %d %d %d %d %d\n",
               FNPTR_KIND(fpm), FNPTR_KIND(fpc),
               FNPTR_KIND(fn_mut), FNPTR_KIND(fn_const),
               FNPTR_KIND(&fn_mut), FNPTR_KIND(&fn_const));
        return 1;
    }

    /* 6. Nested _Generic, __typeof__, sizeof, and _Static_assert */
    int nested = _Generic(cp,
        const int *: _Generic(kcp, const int *: 77, default: 0),
        default: 0);
    if (nested != 77) {
        printf("FAIL nested generic: %d\n", nested);
        return 1;
    }

    __typeof__(_Generic(cp, const int *: 1.0, int *: 2)) gv = 3.5;
    if (sizeof(_Generic(cp, const int *: (long long)0, int *: (char)0)) != sizeof(long long)) {
        printf("FAIL sizeof generic\n");
        return 1;
    }
    if (gv < 3.0) {
        printf("FAIL typeof generic\n");
        return 1;
    }

    _Static_assert(_Generic((const int *)0, const int *: 1, int *: 0, default: 0) == 1,
                   "_Generic const int * in _Static_assert");
    _Static_assert(_Generic((const int * const *)0, const int * const *: 1, int **: 0, default: 0) == 1,
                   "_Generic nested const ptr in _Static_assert");

    printf("generic_nested_qual passed\n");
    return 0;
}
