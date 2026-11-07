/* Sprint 122 Stage 3: declarator-stack parameter-parser parity regressions.
 *
 * Each case here hit the legacy-prototype fallback (or was a real
 * mis-parse) in the `ZC_FB_TRACE` c-torture sweep before the burn-down;
 * together they exercise:
 *   - >16 parameter prototypes (param_zt table growth, no 16-cap fallback)
 *   - standalone `_Complex` parameters (default `_Complex double`)
 *   - `T * (*) (T *)` function-pointer-return parameters (star run before
 *     the parenthesized declarator)
 *   - abstract-declarator parameters `int (int foo)` (fn pointer, C99
 *     6.7.6.1)
 *   - typedef signedness ordering (`short unsigned int`, `int unsigned`)
 *   - multi-declarator function prototypes (`double f(int), g(int);`)
 *   - anonymous `struct {..}` parameter bodies (skipped, legacy parity)
 *
 * All shapes compile and (where defined) run identically under host gcc.
 */
#include <stddef.h>

typedef short unsigned int su16;
typedef int unsigned u32t;

/* >16 params: the 17th is returned to prove full registration. */
int f17(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8,
        int a9, int a10, int a11, int a12, int a13, int a14, int a15,
        int a16, int a17) {
    return a17;
}

/* Standalone _Complex parameter (C99 6.7.2.3 / GNU default double). */
static int cx_seen = 0;
void fcomplex(_Complex z, int *out) {
    (void)z;
    *out = (int)sizeof(su16) + (int)sizeof(u32t); /* 2 + 4 */
    cx_seen = 1;
}

/* int (*fp)(int *): fp is a pointer to a function taking int* and
 * returning int. */
static int values[3] = {10, 20, 30};
int getval(int *v) { return v[0]; }
int *getptr(int *v) { return &v[1]; }

int ffnptr(int (*fp)(int *)) {
    return fp(&values[0]) + 1; /* getval -> 10 -> 11 */
}

/* int *(*fp)(int *): the star BEFORE the parenthesized declarator - fp
 * points to a function returning int* (the 921208-1 shape; the star run
 * is consumed before the `(*` branch so this shape is reachable). */
int ffnptr2(int *(*fp)(int *)) {
    return fp(&values[0])[1]; /* getptr -> &values[1] -> [1] = 30 */
}

/* Wrapped-name fnptr param with a pointee suffix: `(* const (funcp)(long))`
 * - the outer `(* ... )` group closes AFTER the pointee's params, and the
 * quick scan must consume that trailing ')' (c-torture compile/991213-2;
 * an off-by-one here left the `)` inside the param list and the following
 * comma mis-entered the multi-declarator loop). */
static long g_offset = 5;
int fwrapped(void (* const (funcp)(long)), const long offset, int * const okayp) {
    *okayp = 1;
    funcp(g_offset);
    return (int)offset;
}
static void mark(long t) {
    g_offset = t;
}

/* Abstract declarator: `int (int foo)` is a pointer to a function. */
int fabstract(int (*fp)(int)) {
    return fp(7) + 1;
}
static int addone(int x) {
    return x + 1;
}

/* Multi-declarator function prototypes (comma list). */
double dbl(int x);
double dbl2(int x);
double dbl(int x) {
    return x * 2.0;
}
double dbl2(int x) {
    return x * 3.0;
}

/* Anonymous struct parameter: body skipped (legacy-parity int64 base).
 * Defined but not called: the regression is that the parameter list
 * PARSES and the function registers (gcc warns that the anonymous struct
 * is not visible outside the definition, and the call argument is a
 * struct in gcc vs the int64 fallback here). */
void fanon(int a, struct { int b[a]; } c) {
    (void)c;
}

int main(void) {
    int r = 0;
    r += (f17(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 42) != 42);

    int cx = 0;
    fcomplex(1.0, &cx);
    r += (cx != 6);
    r += (cx_seen != 1);

    r += (ffnptr(getval) != 11);
    r += (ffnptr2(getptr) != 30);

    int okay = 0;
    r += (fwrapped(mark, 1, &okay) != 1);
    r += (okay != 1);
    r += (g_offset != 5);

    r += (fabstract(addone) != 9);

    r += (dbl(2) != 4.0);
    r += (dbl2(3) != 9.0);

    return r;
}
