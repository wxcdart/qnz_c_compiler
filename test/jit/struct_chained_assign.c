/* Sprint 153 regression: chained struct assignment `c = a = y` where the
 * inner assignment's RHS is a generic struct value (>16B and 9-16B,
 * global/local/member targets).
 *
 * C99 6.5.16.1: an assignment expression's value is its destination
 * lvalue, so the inner `a = y` copies y into a and REPORTS &a; the
 * outer `c = ...` must chunk-copy from &a. `ast_expr_is_struct_addr`
 * classifies a struct/union assignment (size > 8) as address-form and
 * every >8B struct-assignment lowering site returns the destination's
 * address.
 *
 * Before the fix, the generic RHS fallback in
 * zero_cc_emit_struct_copy_to_addr allocated a value temp for a
 * multi-chunk struct and emitted no load to fill it, so the outer copy
 * read an untracked temp and the opt_temp_renumber backstop rejected
 * the function (c-torture compile/pr71109: `c = a = y` on a 36-byte
 * struct, all -O levels). */
#include <stdio.h>
#include <string.h>

struct Big { long a[5]; int b; };   /* 44 bytes, > 16 */
struct Mid { int a; char b[8]; int c; }; /* 20 bytes, 9-16 */

static struct Big gb1, gb2, gb3;
static struct Mid gm1, gm2, gm3;

struct Holder { struct Big big; int tag; };
static struct Holder h1, h2, h3;

int main(void) {
    struct Big y, a, c;
    memset(&y, 0xAB, sizeof y);
    y.a[0] = 111;
    y.a[4] = 222;
    y.b = 333;

    /* Local chained assign, >16B. */
    c = a = y;
    if (memcmp(&c, &y, sizeof y) != 0) { printf("FAIL local big c\n"); return 1; }
    if (memcmp(&a, &y, sizeof y) != 0) { printf("FAIL local big a\n"); return 1; }

    /* Global chained assign, >16B. */
    gb3 = gb2 = gb1 = y;
    if (memcmp(&gb3, &y, sizeof y) != 0) { printf("FAIL global big\n"); return 1; }
    if (memcmp(&gb1, &y, sizeof y) != 0) { printf("FAIL global big chain\n"); return 1; }

    /* 9-16B chained assigns (local + global). */
    gm1.a = 42; gm1.b[0] = 1; gm1.b[7] = 2; gm1.c = 84;
    gm3 = gm2 = gm1;
    if (gm3.a != 42 || gm3.b[0] != 1 || gm3.b[7] != 2 || gm3.c != 84) {
        printf("FAIL global mid\n"); return 1;
    }
    struct Mid m1, m2, m3;
    m1.a = 7; m1.b[3] = 9; m1.c = 21;
    m3 = m2 = m1;
    if (m3.a != 7 || m3.b[3] != 9 || m3.c != 21) { printf("FAIL local mid\n"); return 1; }

    /* Member struct chained assign, >16B. */
    h1.big = y; h1.tag = 1;
    h3.big = h2.big = h1.big;
    if (memcmp(&h3.big, &y, sizeof y) != 0) { printf("FAIL member big\n"); return 1; }
    if (memcmp(&h2.big, &y, sizeof y) != 0) { printf("FAIL member big chain\n"); return 1; }

    printf("ok\n");
    return 0;
}
