/* test/unit/dlfcn_embedded_test.c — Sprint 153 (Host Runtime #11 Phase 1).
 *
 * Unit test for zero_rt/dlfcn/dlfcn.c's pre-baked embedded symbol table
 * (tools/embed_symtab.py). The test binary is linked with dlfcn.c (the
 * Makefile test rule adds ZC_DLCFN_OBJ) and supplies its OWN
 * g_zero_rt_embedded_syms table (the same layout the generator emits for
 * the zerocc driver), so both resolution paths are exercised against a
 * controlled table:
 *
 *   1. ZERO_RT_DL=embedded  - only the 3 fake-table entries resolve;
 *      `malloc` (present in the real process symtab, absent from the
 *      fake table) must NOT resolve — proves /proc is bypassed.
 *   2. (default)            - embedded preferred when non-empty.
 *   3. ZERO_RT_DL=proc      - legacy /proc/self/exe path: the full
 *      process symtab resolves (malloc, test_fn_a, ...).
 *   4. Negative lookup, dlerror, dlclose, folded-lib dlopen.
 *
 * Host-compiled (musl-gcc via `make test`); not in the opt_sweep corpus.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; const void *addr; } ZeroRtEmbeddedSym;

int test_fn_a(int x) { return x * 3 + 1; }
int test_fn_b(void) { return 42; }

/* The fake pre-baked table (sorted bytewise by name, sentinel-terminated)
 * that dlfcn.c picks up via its extern declaration. */
const ZeroRtEmbeddedSym g_zero_rt_embedded_syms[] = {
    { "printf", (const void *)&printf },
    { "test_fn_a", (const void *)&test_fn_a },
    { "test_fn_b", (const void *)&test_fn_b },
    { 0, 0 }
};
const unsigned long g_zero_rt_embedded_sym_count = 3;

int main(void) {
    int rc = 0;
    void *h = dlopen(0, 0);
    if (!h) { printf("FAIL: dlopen(NULL): %s\n", dlerror()); return 1; }

    /* 1. embedded-only: exactly the fake table, no /proc. */
    setenv("ZERO_RT_DL", "embedded", 1);
    int (*fa)(int) = (int (*)(int))dlsym(h, "test_fn_a");
    if (!fa || fa(7) != 22) { printf("FAIL: embedded test_fn_a\n"); rc = 1; }
    int (*fb)(void) = (int (*)(void))dlsym(h, "test_fn_b");
    if (!fb || fb() != 42) { printf("FAIL: embedded test_fn_b\n"); rc = 1; }
    if (!dlsym(h, "printf")) { printf("FAIL: embedded printf\n"); rc = 1; }
    if (dlsym(h, "malloc") != 0) {
        printf("FAIL: embedded mode resolved malloc via /proc\n"); rc = 1;
    }

    /* 2. default mode: embedded preferred (table non-empty). */
    unsetenv("ZERO_RT_DL");
    if (dlsym(h, "test_fn_a") == 0) { printf("FAIL: default test_fn_a\n"); rc = 1; }

    /* 3. proc-only: the full process symtab. */
    setenv("ZERO_RT_DL", "proc", 1);
    if (dlsym(h, "test_fn_a") == 0) { printf("FAIL: proc test_fn_a\n"); rc = 1; }
    if (dlsym(h, "malloc") == 0) { printf("FAIL: proc malloc\n"); rc = 1; }
    if (dlsym(h, "printf") == 0) { printf("FAIL: proc printf\n"); rc = 1; }

    /* 4. negative lookup + dlerror + dlclose. */
    unsetenv("ZERO_RT_DL");
    if (dlsym(h, "zerocc_no_such_symbol_qzxw") != 0) {
        printf("FAIL: unknown symbol resolved\n"); rc = 1;
    } else if (dlerror() == 0) {
        printf("FAIL: dlerror not set after miss\n"); rc = 1;
    }
    if (dlclose(h) != 0) { printf("FAIL: dlclose\n"); rc = 1; }

    /* 5. folded library names: the static build folds them in. */
    if (!dlopen("libm.so.6", 0)) {
        printf("FAIL: folded dlopen(libm.so.6)\n"); rc = 1;
    }

    if (rc == 0) printf("dlfcn embedded table ok\n");
    return rc;
}
