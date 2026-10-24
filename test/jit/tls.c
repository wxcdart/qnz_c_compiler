/* Test for _Thread_local (static TLS, initial-exec).
 *
 * Covers:
 *  - initialized scalars (int/long/short/double) in .tdata
 *  - uninitialized (tbss) variables
 *  - pointer TLS variables (address-of + store-through-pointer)
 *  - mixing declared .tdata and .tbss variables in either order
 *  - value round-trips through functions
 *
 * Run in JIT mode (TLS vars become plain data-section globals) and in
 * AOT mode (zerocc emits .tdata/.tbss with TPOFF32 relocations; the
 * sweep links via gcc or the own linker).
 */
#include <stdio.h>

static _Thread_local int tbss_a;        /* tbss first */
_Thread_local double d1 = 2.5;
static _Thread_local short s1 = -7;
_Thread_local int *p1 = 0;
static _Thread_local int tbss_b;
_Thread_local long l1 = 100;

int tls_get(void) {
    return d1 == 2.5 && s1 == -7 && l1 == 100 ? 1 : 0;
}

int main(void) {
    tbss_a = 3;
    tbss_b = 9;
    int local = 55;
    p1 = &local;
    *p1 = 56;
    int ok = (tbss_a == 3 && tbss_b == 9 && s1 == -7 && *p1 == 56 &&
              l1 == 100 && tls_get());
    printf("a=%d b=%d s=%d p=%d l=%ld ok=%d\n", tbss_a, tbss_b, s1, *p1, l1, ok);
    return ok ? 0 : 1;
}
