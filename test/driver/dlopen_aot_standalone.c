/* test/driver/dlopen_aot_standalone.c — Sprint 183 (Host Runtime #11 Phase 6).
 *
 * Verifies standalone target AOT packaging and non-POSIX / baremetal
 * portability layers:
 *   1. Dynamic loading in a 100% standalone static executable (--zerocc-link)
 *      with no system ld.so: dlopen, dlsym, dladdr, dlclose.
 *   2. Freestanding / Baremetal in-memory symbol engine (zero_rt/dlfcn/baremetal.h):
 *      BaremetalSym table registration, lookup, and cache flush.
 */

#include <stdio.h>
#include <dlfcn.h>
#include "zero_rt/dlfcn/baremetal.h"

static int firmware_multiply(int a, int b) {
    return a * b;
}

int main(void) {
    printf("=== Test 1: Freestanding / Baremetal Symbol Engine ===\n");
    static const BaremetalSym fw_symbols[] = {
        { "firmware_multiply", (void *)firmware_multiply },
    };
    zero_rt_baremetal_register_symbols(fw_symbols, sizeof(fw_symbols) / sizeof(fw_symbols[0]));
    void *fn_ptr = zero_rt_baremetal_lookup("firmware_multiply");
    if (!fn_ptr) {
        printf("FAIL: baremetal lookup returned NULL\n");
        return 1;
    }
    int (*fw_mult)(int, int) = (int (*)(int, int))fn_ptr;
    int mres = fw_mult(6, 7);
    if (mres != 42) {
        printf("FAIL: firmware_multiply(6, 7) = %d, expected 42\n", mres);
        return 2;
    }
    zero_rt_cache_flush(fn_ptr, 64);
    printf("PASS: baremetal symbol lookup & cache flush ok (6 * 7 = %d)\n", mres);

    printf("=== Test 2: In-Process Dynamic Loading in Standalone AOT ===\n");
    const char *test_lib = "/tmp/libplugin_test.so";
    void *h = dlopen(test_lib, RTLD_NOW);
    if (!h) {
        printf("FAIL: dlopen('%s') failed: %s\n", test_lib, dlerror());
        return 3;
    }

    int (*calc)(int, int) = (int (*)(int, int))dlsym(h, "plugin_calc");
    if (!calc) {
        printf("FAIL: dlsym('plugin_calc') failed: %s\n", dlerror());
        dlclose(h);
        return 4;
    }

    int cres = calc(4, 2);
    if (cres != 42) {
        printf("FAIL: plugin_calc(4, 2) = %d, expected 42\n", cres);
        dlclose(h);
        return 5;
    }

    Dl_info info;
    if (!dladdr((void *)calc, &info)) {
        printf("FAIL: dladdr failed for plugin_calc\n");
        dlclose(h);
        return 6;
    }
    if (!info.dli_sname || !info.dli_fname) {
        printf("FAIL: dladdr returned incomplete info\n");
        dlclose(h);
        return 7;
    }

    if (dlclose(h) != 0) {
        printf("FAIL: dlclose failed\n");
        return 8;
    }

    printf("PASS: standalone AOT dlopen/dlsym/dladdr/dlclose succeeded (4*10+2 = %d)\n", cres);
    printf("dlopen-aot-standalone ok\n");
    return 0;
}
