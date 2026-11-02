/* test/jit/complex_int_test.c — Comprehensive integer complex verification */

extern void abort(void);
extern void exit(int);

_Complex int add_ci(_Complex int a, _Complex int b) {
    return a + b;
}

_Complex long mul_cl(_Complex long a, _Complex long b) {
    return a * b;
}

_Complex int test_call(_Complex int c) {
    return c;
}

_Complex long test_call_l(_Complex long c) {
    return c;
}

_Complex int arr[4];

int main(void) {
    _Complex int a = 10 + 20.0i;
    _Complex int b = 3 + 4.0i;
    _Complex int sum = add_ci(a, b);
    if (__real__ sum != 13 || __imag__ sum != 24) abort();

    _Complex int diff = a - b;
    if (__real__ diff != 7 || __imag__ diff != 16) abort();

    _Complex long la = 10000000000L + 20000000000.0i;
    _Complex long lb = 2L + 3.0i;
    _Complex long prod = mul_cl(la, lb);
    /* (10e9*2 - 20e9*3) = -40e9; (10e9*3 + 20e9*2) = 70e9 */
    if (__real__ prod != -40000000000L || __imag__ prod != 70000000000L) abort();

    /* Comparisons */
    if (a == b) abort();
    if (!(a != b)) abort();
    _Complex int a2 = 10 + 20.0i;
    if (!(a == a2)) abort();

    /* Conjugate ~ */
    _Complex int conj_a = ~a;
    if (__real__ conj_a != 10 || __imag__ conj_a != -20) abort();

    /* Logical ! */
    _Complex int zero = 0;
    if (!zero != 1) abort();
    if (!a != 0) abort();

    /* Part assignment */
    _Complex int p = 0;
    __real__ p = 42;
    __imag__ p = 99;
    if (__real__ p != 42 || __imag__ p != 99) abort();

    /* Array indexing */
    for (int i = 0; i < 4; i++) {
        arr[i] = -1;
    }
    for (int i = 0; i < 4; i++) {
        if (__real__ arr[i] != -1 || __imag__ arr[i] != 0) abort();
    }

    /* Passing scalar to complex parameter */
    _Complex int from_scalar = test_call(7);
    if (__real__ from_scalar != 7 || __imag__ from_scalar != 0) abort();

    _Complex long from_scalar_l = test_call_l(42L);
    if (__real__ from_scalar_l != 42L || __imag__ from_scalar_l != 0L) abort();

    return 0;
}
