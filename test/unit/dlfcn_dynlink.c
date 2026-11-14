/* test/unit/dlfcn_dynlink.c — Host-side in-process dynamic loader (Phase 2).
 *
 * Tests zero_rt/dlfcn/dynlink.c + dlfcn.c loading a real ELF64 shared object
 * (.so) into the static-musl host process:
 *   1. PT_LOAD segment mapping (including non-page-aligned p_offset != p_vaddr
 *      RW segments and BSS zero-fill)
 *   2. PT_DYNAMIC (.dynamic) parsing, DT_GNU_HASH / DT_HASH symbol lookup
 *   3. RELA relocations (R_X86_64_RELATIVE, R_X86_64_GLOB_DAT, weak undefs)
 *   4. DT_INIT_ARRAY constructor execution
 *   5. dlsym(handle, ...) and dlsym(RTLD_DEFAULT, ...), dlclose(handle),
 *      refcount deduplication on repeated dlopen, and dlerror() reporting.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_dso_src =
    "static int g_ctor_val = 0;\n"
    "static int g_bss_counter;\n"
    "__attribute__((constructor)) static void on_load(void) {\n"
    "    g_ctor_val = 77;\n"
    "}\n"
    "int test_dl_add(int a, int b) {\n"
    "    return a + b;\n"
    "}\n"
    "const char *test_dl_hello(void) {\n"
    "    return \"hello from libtest_dl.so\";\n"
    "}\n"
    "int test_dl_state(int delta) {\n"
    "    g_bss_counter += delta;\n"
    "    return g_ctor_val + g_bss_counter;\n"
    "}\n";

int main(void) {
    char c_path[256];
    char so_path[256];
    char cmd[640];
    int pid = (int)getpid();

    snprintf(c_path, sizeof(c_path), "/tmp/zerocc_test_dl_%d.c", pid);
    snprintf(so_path, sizeof(so_path), "/tmp/zerocc_test_dl_%d.so", pid);

    FILE *f = fopen(c_path, "w");
    if (!f) {
        printf("FAIL: could not write %s\n", c_path);
        return 1;
    }
    fputs(g_dso_src, f);
    fclose(f);

    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -O2 %s -o %s", c_path, so_path);
    if (system(cmd) != 0) {
        unlink(c_path);
        printf("FAIL: could not build shared object with gcc\n");
        return 1;
    }

    int rc = 0;
    void *h = dlopen(so_path, RTLD_NOW);
    if (!h) {
        printf("FAIL: dlopen(%s) failed: %s\n", so_path, dlerror());
        unlink(c_path);
        unlink(so_path);
        return 1;
    }

    int (*add_fn)(int, int) = (int (*)(int, int))dlsym(h, "test_dl_add");
    if (!add_fn) {
        printf("FAIL: dlsym(test_dl_add) failed: %s\n", dlerror());
        rc = 1;
    } else if (add_fn(21, 21) != 42) {
        printf("FAIL: test_dl_add(21, 21) = %d (expected 42)\n", add_fn(21, 21));
        rc = 1;
    }

    const char *(*hello_fn)(void) = (const char *(*)(void))dlsym(h, "test_dl_hello");
    if (!hello_fn) {
        printf("FAIL: dlsym(test_dl_hello) failed: %s\n", dlerror());
        rc = 1;
    } else if (strcmp(hello_fn(), "hello from libtest_dl.so") != 0) {
        printf("FAIL: test_dl_hello() = '%s'\n", hello_fn());
        rc = 1;
    }

    int (*state_fn)(int) = (int (*)(int))dlsym(h, "test_dl_state");
    if (!state_fn) {
        printf("FAIL: dlsym(test_dl_state) failed: %s\n", dlerror());
        rc = 1;
    } else {
        int s1 = state_fn(5);
        int s2 = state_fn(10);
        if (s1 != 82 || s2 != 92) {
            printf("FAIL: test_dl_state got (%d, %d), expected (82, 92)\n", s1, s2);
            rc = 1;
        }
    }

    /* Global lookup via RTLD_DEFAULT (NULL) should also find symbols in loaded DSOs. */
    if (dlsym(NULL, "test_dl_add") != (void *)add_fn) {
        printf("FAIL: dlsym(RTLD_DEFAULT, test_dl_add) mismatch\n");
        rc = 1;
    }

    /* Missing symbol lookup should return NULL and set dlerror(). */
    if (dlsym(h, "test_dl_nonexistent_sym") != NULL) {
        printf("FAIL: nonexistent symbol resolved\n");
        rc = 1;
    } else if (dlerror() == NULL) {
        printf("FAIL: dlerror() not set after missing symbol\n");
        rc = 1;
    }

    /* Re-opening the same path returns the existing handle with bumped refcount. */
    void *h2 = dlopen(so_path, RTLD_NOW);
    if (h2 != h) {
        printf("FAIL: second dlopen returned %p, expected %p\n", h2, h);
        rc = 1;
    }
    if (dlclose(h2) != 0) {
        printf("FAIL: dlclose(h2) failed\n");
        rc = 1;
    }
    if (dlclose(h) != 0) {
        printf("FAIL: dlclose(h) failed\n");
        rc = 1;
    }

    unlink(c_path);
    unlink(so_path);

    if (rc == 0) {
        printf("dlfcn dynlink ok\n");
    }
    return rc;
}
