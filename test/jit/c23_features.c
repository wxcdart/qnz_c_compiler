#include <stdio.h>

#ifdef __ZEROCC__
#ifndef __STDC_NO_COMPLEX__
#error "__STDC_NO_COMPLEX__ should be defined"
#endif

#if __STDC_NO_COMPLEX__ != 1
#error "__STDC_NO_COMPLEX__ should be 1"
#endif
#else
/* Fallback when compiled by host GCC in `make all` */
#define __STDC_NO_COMPLEX__ 1
#define typeof_unqual(T) typeof(T)
#endif

/* 1. Global with typeof and empty initializer */
int g_int = 42;
typeof(g_int) g_copy = 100;
typeof(g_int) g_zero = {};

struct Point {
    int x;
    int y;
};
struct Point g_pt = {};

/* 2. Struct with typeof members */
struct Widget {
    typeof(int) id;
    typeof(double) weight;
    typeof(g_pt) pt;
};

/* 3. Typedef with typeof */
typedef typeof(int) my_int_t;
typedef __typeof__(double) my_double_t;

/* 4. Functions with typeof return and parameters */
typeof(int) add(typeof(int) a, typeof(a) b) {
    return a + b;
}

int main() {
    /* Check __STDC_NO_COMPLEX__ */
    if (__STDC_NO_COMPLEX__ != 1) return 1;

    /* Check global empty init */
    if (g_zero != 0) return 2;
    if (g_pt.x != 0 || g_pt.y != 0) return 3;
    if (g_copy != 100) return 4;

    /* 5. Local variables with typeof */
    int x = 10;
    typeof(x) y = 20;
    __typeof__(x) z = 30;
    typeof(x + y) sum = x + y;
    if (y != 20 || z != 30 || sum != 30) return 5;

    /* 6. typeof with expressions & member access */
    struct Point p = { .x = 5, .y = 7 };
    typeof(p.x) px = p.x;
    typeof(p) p_copy = p;
    if (px != 5 || p_copy.y != 7) return 6;

    /* 7. typeof with pointers */
    int val = 99;
    int *pval = &val;
    typeof(pval) pval2 = &val;
    typeof(*pval) deref_val = *pval2;
    if (*pval2 != 99 || deref_val != 99) return 7;

    /* 8. typeof_unqual removes const (tested under ZeroCC) */
#ifdef __ZEROCC__
    const int c_val = 50;
    typeof_unqual(c_val) mutable_val = c_val;
    mutable_val = 77;
    if (mutable_val != 77) return 8;
#endif

    /* 9. Empty `{}` initializers for locals and statics */
    int empty_arr[5] = {};
    for (int i = 0; i < 5; i++) {
        if (empty_arr[i] != 0) return 9;
    }
    struct Point empty_p = {};
    if (empty_p.x != 0 || empty_p.y != 0) return 10;

    static int static_zero = {};
    static struct Point static_pt = {};
    if (static_zero != 0 || static_pt.x != 0 || static_pt.y != 0) return 11;

    /* 10. Casts with typeof */
    double d = 3.14;
    int casted = (typeof(int))d;
    if (casted != 3) return 12;

    /* 11. sizeof with typeof */
    if (sizeof(typeof(int)) != sizeof(int)) return 13;
    if (sizeof(typeof(d)) != sizeof(double)) return 14;
    if (sizeof(typeof(p)) != sizeof(struct Point)) return 15;

    /* 12. Function call with typeof */
    typeof(add(1, 2)) res = add(15, 25);
    if (res != 40) return 16;

    /* 13. Struct with typeof member */
    struct Widget w = {};
    w.id = 1;
    w.weight = 2.5;
    w.pt.x = 10;
    w.pt.y = 20;
    if (w.id != 1 || w.pt.y != 20) return 17;

    return 0;
}
