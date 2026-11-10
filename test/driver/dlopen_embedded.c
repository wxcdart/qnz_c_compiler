/* test/driver/dlopen_embedded.c — Sprint 153 (Host Runtime #11 Phase 1).
 *
 * Driver-level end-to-end test: this program is JITed by zerocc
 * (`zerocc --run`), and every host function it touches (dlopen, dlsym,
 * dlerror, dlclose, printf, malloc) is a JIT far call resolved through
 * zero_rt/dlfcn/dlfcn.c — which, since Sprint 153, resolves from the
 * pre-baked embedded table baked into build/bin/zerocc
 * (tools/embed_symtab.py) instead of re-reading /proc/self/exe.
 *
 * tools/driver_tests.py runs it three ways:
 *   default env            - embedded table preferred
 *   ZERO_RT_DL=embedded    - /proc/self/exe disallowed (the
 *                            hardened-container probe); any incomplete
 *                            table aborts the JIT link
 *   ZERO_RT_DL=proc        - legacy /proc/self/exe fallback
 *
 * JIT-only: AOT binaries link zero_rt, which carries no dlfcn (host-only).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int rc = 0;
    void *h = dlopen(0, 0);
    if (!h) { printf("FAIL: dlopen(NULL)\n"); return 1; }

    /* resolved function pointers, called through the pointer */
    void *pm = dlsym(h, "malloc");
    if (!pm) { printf("FAIL: dlsym(malloc)\n"); rc = 1; }
    else {
        char *b = ((void *(*)(unsigned long))pm)(32);
        if (!b) { printf("FAIL: resolved malloc returned NULL\n"); rc = 1; }
        else {
            b[0] = 9;
            if (b[0] != 9) { printf("FAIL: resolved malloc buffer\n"); rc = 1; }
            ((void (*)(void *))pm)(b);
        }
    }
    void *pp = dlsym(h, "printf");
    if (!pp) { printf("FAIL: dlsym(printf)\n"); rc = 1; }
    else ((int (*)(const char *))pp)("dlopen via driver table\n");

    /* a zerocc-internal symbol: proves the table is the driver's own */
    if (!dlsym(h, "zc_alloc_temp")) {
        printf("FAIL: dlsym(zc_alloc_temp)\n"); rc = 1;
    }

    /* negative lookup */
    if (dlsym(h, "zerocc_no_such_symbol_qzxw") != 0) {
        printf("FAIL: unknown symbol resolved\n"); rc = 1;
    } else if (dlerror() == 0) {
        printf("FAIL: dlerror not set after miss\n"); rc = 1;
    }

    if (dlclose(h) != 0) { printf("FAIL: dlclose\n"); rc = 1; }

    if (rc == 0) printf("dlopen-embedded ok\n");
    return rc;
}
