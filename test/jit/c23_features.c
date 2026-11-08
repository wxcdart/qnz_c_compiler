/* C23 feature tour (seed corpus for the C23 completion track).
 *
 * Constraints on this file (it must stay buildable by BOTH compilers):
 *   - `make all` compiles it with the host gcc (default std) as the
 *     reference binary that `make test` executes;
 *   - `make test-opt` compiles it with zerocc (default dialect) at
 *     -O0..-O3 in JIT + AOT.
 * So only features accepted by host gcc in default mode may appear here:
 * the C23 *keywords* (true/false, constexpr, nullptr - C23 6.4.4) and the
 * per-dialect __STDC_VERSION__ expectations live in the differential gate
 * (tools/c23/test_c23.py), which drives both compilers with matching
 * -std=gnu23 flags. Sprint 134: the stale __STDC_NO_COMPLEX__ assertion
 * (removed in Sprint 85) made this file uncompilable under zerocc; the
 * opt_sweep baseline probe (Sprint 134) now catches that class.
 */
#include <stdio.h>

/* Host gcc (default std) predates the C23 typeof_unqual keyword; the
 * original seed's fallback (kept from the pre-Sprint-134 file).  Under
 * zerocc this is a real keyword and the define is skipped. */
#ifndef __ZEROCC__
#define typeof_unqual(T) typeof(T)
#endif

/* ---------------------------------------------------------------
 * 1. typeof / __typeof__ / typeof_unqual
 * --------------------------------------------------------------- */
int g_int = 42;
typeof(g_int) g_copy = 100;
typeof(g_int) g_zero = {};

struct Point {
    int x;
    int y;
};
struct Point g_pt = {};

struct Widget {
    typeof(int) id;
    typeof(double) weight;
    typeof(struct Point) pt;
};

typedef typeof(int) my_int_t;
typedef __typeof__(double) my_double_t;

/* Dependent parameter type: the second param's type names the first
 * (Sprint 134: zerocc now resolves preceding param names in typeof
 * operands - gcc has always supported the shape). */
typeof(int) add(typeof(int) a, typeof(a) b) {
    return a + b;
}

/* C23 [[attribute]] list (accepted as an extension by host gcc) */
[[nodiscard]] int attr_fn(void) { return 1; }

/* C23 fixed underlying type for enums (gcc extension pre-C23) */
enum C23E : unsigned { C23_A = 2500 };

int main(void)
{
    /* Global empty init + typeof globals */
    if (g_zero != 0) return 2;
    if (g_pt.x != 0 || g_pt.y != 0) return 3;
    if (g_copy != 100) return 4;

    /* 2. Local typeof */
    int x = 10;
    typeof(x) y = 20;
    __typeof__(x) z = 30;
    typeof(x + y) sum = x + y;
    if (y != 20 || z != 30 || sum != 30) return 5;

    /* 3. typeof with expressions & member access */
    struct Point p = { .x = 5, .y = 7 };
    typeof(p.x) px = p.x;
    typeof(p) p_copy = p;
    if (px != 5 || p_copy.y != 7) return 6;

    /* 4. typeof with pointers */
    int val = 99;
    int *pval = &val;
    typeof(pval) pval2 = &val;
    typeof(*pval) deref_val = *pval2;
    if (*pval2 != 99 || deref_val != 99) return 7;

    /* 5. typeof_unqual removes const (ZeroCC-only: under host gcc the
     * compat define maps it to typeof, which KEEPS const) */
#ifdef __ZEROCC__
    const int c_val = 50;
    typeof_unqual(c_val) mutable_val = c_val;
    mutable_val = 77;
    if (mutable_val != 77) return 8;
#endif

    /* 6. Dependent typeof params (chain + pointer + unqual) */
    if (add(20, 22) != 42) return 9;
    my_int_t ti = add(1, 2);
    if (ti != 3) return 10;

    /* 7. Empty `{}` initializers for locals, arrays, statics */
    int empty_arr[5] = {};
    for (int i = 0; i < 5; i++) {
        if (empty_arr[i] != 0) return 9;
    }
    struct Point empty_p = {};
    if (empty_p.x != 0 || empty_p.y != 0) return 10;

    static int static_zero = {};
    static struct Point static_pt = {};
    if (static_zero != 0 || static_pt.x != 0 || static_pt.y != 0) return 11;

    /* 8. Casts & sizeof with typeof */
    double d = 3.14;
    int casted = (typeof(int))d;
    if (casted != 3) return 12;
    if (sizeof(typeof(int)) != sizeof(int)) return 13;
    if (sizeof(typeof(d)) != sizeof(double)) return 14;
    if (sizeof(typeof(p)) != sizeof(struct Point)) return 15;

    /* 9. Function call with typeof return type */
    typeof(add(1, 2)) res = add(15, 25);
    if (res != 40) return 16;

    /* 10. Struct with typeof members */
    struct Widget w = {};
    w.id = 1;
    w.weight = 2.5;
    w.pt.x = 10;
    w.pt.y = 20;
    if (w.id != 1 || w.pt.y != 20) return 17;
    my_double_t md = w.weight;
    if (md != 2.5) return 18;

    /* 11. C23 [[attribute]] list (file-scope function above) */
    {
        [[maybe_unused]] int u = 5;
        int r;
        r = attr_fn();
        if (r != 1) return 19;
    }

    /* 12. C23 fixed underlying type for enums (file-scope enum above) */
    {
        enum C23E e = C23_A;
        if (e != 2500) return 20;
        if (sizeof(e) < 2) return 21;
    }

    return 0;
}
