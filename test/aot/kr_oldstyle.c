/* Sprint 98: K&R (old-style, C89 6.4.2) function definitions.
 *
 * - typed parameter declarations (any order, multiple declarators)
 * - implicit int return type (gnu89/C89)
 * - parameter without a declaration defaults to int
 * - pointer / struct / register / const parameter forms
 * - nested K&R definitions (with and without captures)
 * Compiled with zerocc (AOT + JIT); expected exit code 0. */

int kr_basic(a, b) int a; char *b; { return a + (int)*b; }

kr_implicit(a) int a; { return a + 1; }          /* default int return (C89) */

int kr_no_decl_list(a) { return a * 2; }         /* no decl list: a is int */

int kr_reordered(b, a) int a; int b; { return a + b; }

int kr_multi(a, b) int a, b; { return a * b; }

int kr_array(a) int a[]; { return a[0] + a[1]; } /* decays to pointer */

struct kr_pt { int x; int y; };
int kr_struct(p) struct kr_pt p; { return p.x + p.y; }

int kr_register(a) register int a; { return a; }

int kr_const(a) const int a; { return a; }

int kr_nested_enc(int n) {
    int kr_inner(a) int a; { return a + n; }
    int kr_inner_nodcl(a) { return a + n; }
    return kr_inner(10) + kr_inner_nodcl(1);
}

int main(void) {
    int x = 4;
    int arr[2] = { 1, 2 };
    struct kr_pt p = { 3, 5 };
    int ok = 1;
    ok &= kr_basic(1, &x) == 5;
    ok &= kr_implicit(1) == 2;
    ok &= kr_no_decl_list(3) == 6;
    ok &= kr_reordered(1, 2) == 3;
    ok &= kr_multi(3, 4) == 12;
    ok &= kr_array(arr) == 3;
    ok &= kr_struct(p) == 8;
    ok &= kr_register(9) == 9;
    ok &= kr_const(7) == 7;
    ok &= kr_nested_enc(5) == 21;
    return ok ? 0 : 1;
}
