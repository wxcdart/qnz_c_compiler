/* zero_san v1 (Sprint 161): array-ELEMENT compound assignment / inc/dec that
 * overflow must trap (SIGILL, rc 132). Pins the correction to the Sprint 161
 * docs: an element lvalue `arr[i]` is checked at the ELEMENT's type (width +
 * signedness), NOT skipped. The index node carries the element TypeInfo, so
 * zero_cc_expr_int_info resolves it to the element — only a *decayed* array in
 * pointer arithmetic (arr + 1) is unsigned/pointer and excluded. Covers the
 * plain local element (int), the long element (width 8), a nested a[i][j]
 * element, and a pointer subscript p[i]; all must trap. */
#include <limits.h>

int main(void) {
    int a[8];
    a[0] = INT_MAX;
    a[0] += 1;              /* int element overflow: must trap */
    long b[8];
    b[0] = LONG_MAX;
    b[0] += 1L;             /* long element overflow (width 8): must trap */
    int m[4][4];
    m[2][3] = INT_MAX;
    m[2][3] += 1;           /* nested a[i][j] element overflow: must trap */
    int *p = a;
    p[1] = INT_MAX;
    p[1] += 1;              /* pointer subscript element overflow: must trap */
    return 0;
}
