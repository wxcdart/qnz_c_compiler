/* test/unit/dlfcn_dynlink.c — Host-side in-process dynamic loader (Phase 2 & 3).
 *
 * Tests zero_rt/dlfcn/dynlink.c + dlfcn.c loading real ELF64 shared objects
 * (.so) into the static-musl host process:
 *   1. PT_LOAD segment mapping (non-page-aligned RW segments, BSS zero-fill)
 *   2. PT_DYNAMIC (.dynamic) parsing, DT_GNU_HASH / DT_HASH symbol lookup
 *   3. RELA relocations (R_X86_64_RELATIVE, R_X86_64_GLOB_DAT, weak undefs)
 *   4. DT_INIT_ARRAY constructor execution
 *   5. dlsym(handle, ...) and dlsym(RTLD_DEFAULT, ...), dlclose(handle),
 *      refcount deduplication on repeated dlopen, and dlerror() reporting
 *   6. Phase 3 Search Paths: bare filename resolution via LD_LIBRARY_PATH
 *   7. Phase 3 Recursive DT_NEEDED: RPATH with $ORIGIN expansion, dependency
 *      chain loading, dependency constructor ordering, and scope resolution
 *   8. Phase 3 Symbol Versioning: GNU symbol versioning (.gnu.version),
 *      default dlsym resolving unhidden version, and dlvsym() specific version.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* dlvsym prototype (matches virtual_headers.h and glibc) */
extern void *dlvsym(void *handle, const char *symbol, const char *version);
extern int setenv(const char *name, const char *value, int overwrite);

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

static const char *g_dep_b_src =
    "static int g_dep_b_ctor = 0;\n"
    "__attribute__((constructor)) static void on_load_b(void) {\n"
    "    g_dep_b_ctor = 100;\n"
    "}\n"
    "int dep_b_calc(int x) {\n"
    "    return x * 3;\n"
    "}\n"
    "int dep_b_get_ctor(void) {\n"
    "    return g_dep_b_ctor;\n"
    "}\n";

static const char *g_dep_a_src =
    "extern int dep_b_calc(int x);\n"
    "extern int dep_b_get_ctor(void);\n"
    "static int g_dep_a_ctor = 0;\n"
    "__attribute__((constructor)) static void on_load_a(void) {\n"
    "    if (dep_b_get_ctor() == 100) {\n"
    "        g_dep_a_ctor = 200;\n"
    "    }\n"
    "}\n"
    "int dep_a_calc(int x) {\n"
    "    return dep_b_calc(x) + g_dep_a_ctor;\n"
    "}\n";

static const char *g_symver_src =
    "__asm__(\".symver symver_v1, test_versioned_sym@TEST_1.0\");\n"
    "__asm__(\".symver symver_v2, test_versioned_sym@@TEST_2.0\");\n"
    "int symver_v1(void) { return 111; }\n"
    "int symver_v2(void) { return 222; }\n"
    "int unversioned_helper(void) { return 333; }\n";

static const char *g_symver_map =
    "TEST_1.0 {\n"
    "    global: test_versioned_sym;\n"
    "    local: *;\n"
    "};\n"
    "TEST_2.0 {\n"
    "    global: test_versioned_sym; unversioned_helper;\n"
    "} TEST_1.0;\n";

int main(void) {
    char c_path[256];
    char so_path[256];
    char cmd[1024];
    int pid = (int)getpid();
    int rc = 0;

    /* ---- Part 1: Basic DSO loading, segments, relocations, constructors ---- */
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

    /* Global lookup via RTLD_DEFAULT (NULL) should find symbols in loaded DSOs. */
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

    /* ---- Part 2: Search Paths (LD_LIBRARY_PATH bare name resolution) ---- */
    char bare_name[128];
    snprintf(bare_name, sizeof(bare_name), "zerocc_test_dl_%d.so", pid);
    setenv("LD_LIBRARY_PATH", "/tmp", 1);
    void *h_bare = dlopen(bare_name, RTLD_NOW);
    if (!h_bare) {
        printf("FAIL: dlopen(%s) via LD_LIBRARY_PATH failed: %s\n", bare_name, dlerror());
        rc = 1;
    } else {
        int (*bare_add)(int, int) = (int (*)(int, int))dlsym(h_bare, "test_dl_add");
        if (!bare_add || bare_add(5, 7) != 12) {
            printf("FAIL: bare_add(5, 7) failed\n");
            rc = 1;
        }
        if (dlclose(h_bare) != 0) {
            printf("FAIL: dlclose(h_bare) failed\n");
            rc = 1;
        }
    }
    unlink(c_path);
    unlink(so_path);

    /* ---- Part 3: Recursive DT_NEEDED & RPATH with $ORIGIN expansion ---- */
    char c_dep_b[256], so_dep_b[256];
    char c_dep_a[256], so_dep_a[256];
    snprintf(c_dep_b, sizeof(c_dep_b), "/tmp/zerocc_dep_b_%d.c", pid);
    snprintf(so_dep_b, sizeof(so_dep_b), "/tmp/zerocc_libdep_b_%d.so", pid);
    snprintf(c_dep_a, sizeof(c_dep_a), "/tmp/zerocc_dep_a_%d.c", pid);
    snprintf(so_dep_a, sizeof(so_dep_a), "/tmp/zerocc_libdep_a_%d.so", pid);

    f = fopen(c_dep_b, "w");
    if (f) { fputs(g_dep_b_src, f); fclose(f); }
    f = fopen(c_dep_a, "w");
    if (f) { fputs(g_dep_a_src, f); fclose(f); }

    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -O2 %s -o %s", c_dep_b, so_dep_b);
    if (system(cmd) != 0) {
        printf("FAIL: could not build libdep_b\n");
        rc = 1;
    }

    char libdep_b_bare[128];
    snprintf(libdep_b_bare, sizeof(libdep_b_bare), "zerocc_libdep_b_%d.so", pid);
    snprintf(cmd, sizeof(cmd),
             "gcc -shared -fPIC -O2 %s -Wl,-rpath,'$ORIGIN' -L/tmp -l:%s -o %s",
             c_dep_a, libdep_b_bare, so_dep_a);
    if (system(cmd) != 0) {
        printf("FAIL: could not build libdep_a with DT_NEEDED\n");
        rc = 1;
    }

    void *h_dep_a = dlopen(so_dep_a, RTLD_NOW);
    if (!h_dep_a) {
        printf("FAIL: dlopen(%s) with recursive DT_NEEDED failed: %s\n", so_dep_a, dlerror());
        rc = 1;
    } else {
        int (*dep_calc)(int) = (int (*)(int))dlsym(h_dep_a, "dep_a_calc");
        if (!dep_calc) {
            printf("FAIL: dlsym(dep_a_calc) failed: %s\n", dlerror());
            rc = 1;
        } else {
            /* dep_b_calc(10) = 30. If ctor_b ran before ctor_a, g_dep_a_ctor = 200. Total = 230. */
            int res = dep_calc(10);
            if (res != 230) {
                printf("FAIL: dep_a_calc(10) = %d (expected 230, ctor order / DT_NEEDED issue)\n", res);
                rc = 1;
            }
        }

        /* Scope lookup: symbols in dependency libdep_b should resolve through h_dep_a */
        int (*dep_b_fn)(int) = (int (*)(int))dlsym(h_dep_a, "dep_b_calc");
        if (!dep_b_fn || dep_b_fn(7) != 21) {
            printf("FAIL: transitive dlsym(dep_b_calc) through h_dep_a failed\n");
            rc = 1;
        }

        if (dlclose(h_dep_a) != 0) {
            printf("FAIL: dlclose(h_dep_a) failed\n");
            rc = 1;
        }
    }
    unlink(c_dep_b);
    unlink(so_dep_b);
    unlink(c_dep_a);
    unlink(so_dep_a);

    /* ---- Part 4: GNU Symbol Versioning and dlvsym ---- */
    char c_symver[256], map_symver[256], so_symver[256];
    snprintf(c_symver, sizeof(c_symver), "/tmp/zerocc_symver_%d.c", pid);
    snprintf(map_symver, sizeof(map_symver), "/tmp/zerocc_symver_%d.map", pid);
    snprintf(so_symver, sizeof(so_symver), "/tmp/zerocc_libsymver_%d.so", pid);

    f = fopen(c_symver, "w");
    if (f) { fputs(g_symver_src, f); fclose(f); }
    f = fopen(map_symver, "w");
    if (f) { fputs(g_symver_map, f); fclose(f); }

    snprintf(cmd, sizeof(cmd),
             "gcc -shared -fPIC -O2 -Wl,--version-script=%s %s -o %s",
             map_symver, c_symver, so_symver);
    if (system(cmd) != 0) {
        printf("FAIL: could not build versioned library\n");
        rc = 1;
    }

    void *h_symver = dlopen(so_symver, RTLD_NOW);
    if (!h_symver) {
        printf("FAIL: dlopen(%s) failed: %s\n", so_symver, dlerror());
        rc = 1;
    } else {
        /* Default dlsym: must resolve unhidden version (TEST_2.0 -> 222) */
        int (*def_fn)(void) = (int (*)(void))dlsym(h_symver, "test_versioned_sym");
        if (!def_fn) {
            printf("FAIL: dlsym(test_versioned_sym) default failed: %s\n", dlerror());
            rc = 1;
        } else if (def_fn() != 222) {
            printf("FAIL: default dlsym got %d (expected 222 from @@TEST_2.0)\n", def_fn());
            rc = 1;
        }

        /* dlvsym with TEST_1.0: must resolve hidden version (TEST_1.0 -> 111) */
        int (*v1_fn)(void) = (int (*)(void))dlvsym(h_symver, "test_versioned_sym", "TEST_1.0");
        if (!v1_fn) {
            printf("FAIL: dlvsym(test_versioned_sym, TEST_1.0) failed: %s\n", dlerror());
            rc = 1;
        } else if (v1_fn() != 111) {
            printf("FAIL: dlvsym TEST_1.0 got %d (expected 111)\n", v1_fn());
            rc = 1;
        }

        /* dlvsym with TEST_2.0: must resolve default version (TEST_2.0 -> 222) */
        int (*v2_fn)(void) = (int (*)(void))dlvsym(h_symver, "test_versioned_sym", "TEST_2.0");
        if (!v2_fn) {
            printf("FAIL: dlvsym(test_versioned_sym, TEST_2.0) failed: %s\n", dlerror());
            rc = 1;
        } else if (v2_fn() != 222) {
            printf("FAIL: dlvsym TEST_2.0 got %d (expected 222)\n", v2_fn());
            rc = 1;
        }

        /* dlvsym with unversioned_helper matching TEST_2.0 */
        int (*help_fn)(void) = (int (*)(void))dlvsym(h_symver, "unversioned_helper", "TEST_2.0");
        if (!help_fn || help_fn() != 333) {
            printf("FAIL: dlvsym(unversioned_helper, TEST_2.0) failed\n");
            rc = 1;
        }

        /* dlvsym with non-existent version must return NULL and set dlerror() */
        if (dlvsym(h_symver, "test_versioned_sym", "TEST_9.9") != NULL) {
            printf("FAIL: dlvsym with non-existent version succeeded\n");
            rc = 1;
        } else if (dlerror() == NULL) {
            printf("FAIL: dlerror() not set after missing version lookup\n");
            rc = 1;
        }

        if (dlclose(h_symver) != 0) {
            printf("FAIL: dlclose(h_symver) failed\n");
            rc = 1;
        }
    }
    unlink(c_symver);
    unlink(map_symver);
    unlink(so_symver);

    if (rc == 0) {
        printf("dlfcn dynlink ok\n");
    }
    return rc;
}
