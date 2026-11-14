/* test/unit/dlfcn_dynlink.c — Host-side in-process dynamic loader (Phase 2, 3, & 4).
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
 *   9. Phase 4 dladdr(addr, &info): DSO/symbol reverse inspection for loaded
 *      DSOs and host executable symbols, closest symbol matching, interior offsets.
 *  10. Phase 4 dlsym(RTLD_NEXT, name): interposition symbol resolution in
 *      dependency chain and from main namespace.
 *  11. Phase 4 Lifecycle & Destructors: DT_FINI_ARRAY/DT_FINI execution on
 *      refcount 0, DSO unmapping (munmap), unlinking, and dladdr invalidation.
 *  12. Phase 4 Explicit Path Override: explicit path "./libc.so" bypassing
 *      static host library folding and loading from disk.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* dlvsym and dladdr prototypes (matches virtual_headers.h and glibc) */
extern void *dlvsym(void *handle, const char *symbol, const char *version);
extern int dladdr(const void *addr, Dl_info *info);
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

static const char *g_dtor_src =
    "static int *g_flag_ptr = 0;\n"
    "void set_flag_target(int *p) { g_flag_ptr = p; }\n"
    "__attribute__((destructor)) static void on_fini(void) {\n"
    "    if (g_flag_ptr) *g_flag_ptr = 888;\n"
    "}\n"
    "int dtor_dummy(void) { return 42; }\n";

static const char *g_next_base_src =
    "int next_worker(int x) {\n"
    "    return x + 10;\n"
    "}\n";

static const char *g_next_wrap_src =
    "#define _GNU_SOURCE\n"
    "#include <dlfcn.h>\n"
    "int next_worker(int x) {\n"
    "    int (*orig)(int) = (int (*)(int))dlsym(RTLD_NEXT, \"next_worker\");\n"
    "    if (!orig) return -999;\n"
    "    return orig(x) * 3;\n"
    "}\n";

static const char *g_custom_libc_src =
    "int custom_libc_marker(void) {\n"
    "    return 0x55AA55AA;\n"
    "}\n";

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
    } else {
        /* Phase 4 dladdr test on loaded DSO function */
        Dl_info dlinfo;
        memset(&dlinfo, 0, sizeof(dlinfo));
        if (!dladdr(add_fn, &dlinfo)) {
            printf("FAIL: dladdr(add_fn) returned 0\n");
            rc = 1;
        } else {
            if (!dlinfo.dli_sname || strcmp(dlinfo.dli_sname, "test_dl_add") != 0) {
                printf("FAIL: dladdr sname mismatch: %s\n", dlinfo.dli_sname ? dlinfo.dli_sname : "(null)");
                rc = 1;
            }
            if (dlinfo.dli_saddr != (void *)add_fn) {
                printf("FAIL: dladdr saddr mismatch\n");
                rc = 1;
            }
            if (!dlinfo.dli_fbase || !dlinfo.dli_fname) {
                printf("FAIL: dladdr fbase/fname null\n");
                rc = 1;
            }
        }
        /* Test interior address (add_fn + 4) */
        memset(&dlinfo, 0, sizeof(dlinfo));
        if (!dladdr((const char *)add_fn + 4, &dlinfo)) {
            printf("FAIL: dladdr(add_fn + 4) returned 0\n");
            rc = 1;
        } else if (!dlinfo.dli_sname || strcmp(dlinfo.dli_sname, "test_dl_add") != 0) {
            printf("FAIL: dladdr(add_fn + 4) wrong sname: %s\n", dlinfo.dli_sname);
            rc = 1;
        }
        /* Test dladdr on host executable symbol (printf) */
        memset(&dlinfo, 0, sizeof(dlinfo));
        if (!dladdr((const void *)&printf, &dlinfo)) {
            printf("FAIL: dladdr(&printf) returned 0\n");
            rc = 1;
        } else if (!dlinfo.dli_fname || !dlinfo.dli_fbase) {
            printf("FAIL: dladdr(&printf) fname or fbase null\n");
            rc = 1;
        } else if (!dlinfo.dli_sname || strcmp(dlinfo.dli_sname, "printf") != 0) {
            printf("FAIL: dladdr(&printf) sname mismatch: %s\n", dlinfo.dli_sname ? dlinfo.dli_sname : "(null)");
            rc = 1;
        }
        /* Negative dladdr tests */
        if (dladdr(NULL, &dlinfo) != 0) {
            printf("FAIL: dladdr(NULL) should return 0\n");
            rc = 1;
        }
        if (dladdr(&rc, &dlinfo) != 0) {
            printf("FAIL: dladdr(stack) should return 0\n");
            rc = 1;
        }
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

    /* ---- Part 5: Phase 4 Destructors and Unmapping in dlclose() ---- */
    char c_dtor[256], so_dtor[256];
    snprintf(c_dtor, sizeof(c_dtor), "/tmp/zerocc_dtor_%d.c", pid);
    snprintf(so_dtor, sizeof(so_dtor), "/tmp/zerocc_dtor_%d.so", pid);

    f = fopen(c_dtor, "w");
    if (f) { fputs(g_dtor_src, f); fclose(f); }

    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -O2 %s -o %s", c_dtor, so_dtor);
    if (system(cmd) != 0) {
        printf("FAIL: could not build destructor shared object\n");
        rc = 1;
    }

    void *h_dtor = dlopen(so_dtor, RTLD_NOW);
    if (!h_dtor) {
        printf("FAIL: dlopen(h_dtor) failed: %s\n", dlerror());
        rc = 1;
    } else {
        void (*set_target)(int *) = (void (*)(int *))dlsym(h_dtor, "set_flag_target");
        void *dummy_fn = dlsym(h_dtor, "dtor_dummy");
        int flag_target = 0;
        if (!set_target || !dummy_fn) {
            printf("FAIL: dlsym on destructor library failed\n");
            rc = 1;
        } else {
            set_target(&flag_target);
            if (flag_target != 0) {
                printf("FAIL: flag_target modified prematurely\n");
                rc = 1;
            }
            if (dlclose(h_dtor) != 0) {
                printf("FAIL: dlclose(h_dtor) failed\n");
                rc = 1;
            } else {
                /* Destructor must have run */
                if (flag_target != 888) {
                    printf("FAIL: destructor failed to execute, flag_target=%d\n", flag_target);
                    rc = 1;
                }
                /* Address must now be unmapped from loaded DSOs */
                Dl_info unmap_info;
                memset(&unmap_info, 0, sizeof(unmap_info));
                if (dladdr(dummy_fn, &unmap_info) != 0) {
                    printf("FAIL: dladdr succeeded on unmapped DSO address\n");
                    rc = 1;
                }
            }
        }
    }
    unlink(c_dtor);
    unlink(so_dtor);

    /* ---- Part 6: Phase 4 Interposition via dlsym(RTLD_NEXT) ---- */
    char c_nbase[256], so_nbase[256];
    char c_nwrap[256], so_nwrap[256];
    snprintf(c_nbase, sizeof(c_nbase), "/tmp/zerocc_nbase_%d.c", pid);
    snprintf(so_nbase, sizeof(so_nbase), "/tmp/libnext_base_%d.so", pid);
    snprintf(c_nwrap, sizeof(c_nwrap), "/tmp/zerocc_nwrap_%d.c", pid);
    snprintf(so_nwrap, sizeof(so_nwrap), "/tmp/libnext_wrap_%d.so", pid);

    f = fopen(c_nbase, "w");
    if (f) { fputs(g_next_base_src, f); fclose(f); }
    f = fopen(c_nwrap, "w");
    if (f) { fputs(g_next_wrap_src, f); fclose(f); }

    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -O2 %s -o %s", c_nbase, so_nbase);
    if (system(cmd) != 0) {
        printf("FAIL: could not build next_base\n");
        rc = 1;
    }
    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -O2 %s -Wl,--no-as-needed -L/tmp -lnext_base_%d -Wl,-rpath,'$ORIGIN' -o %s",
             c_nwrap, pid, so_nwrap);
    if (system(cmd) != 0) {
        printf("FAIL: could not build next_wrap\n");
        rc = 1;
    }

    void *h_wrap = dlopen(so_nwrap, RTLD_NOW);
    if (!h_wrap) {
        printf("FAIL: dlopen(so_nwrap) failed: %s\n", dlerror());
        rc = 1;
    } else {
        int (*wrap_fn)(int) = (int (*)(int))dlsym(h_wrap, "next_worker");
        if (!wrap_fn) {
            printf("FAIL: dlsym(next_worker) failed: %s\n", dlerror());
            rc = 1;
        } else {
            /* next_worker in wrap calls orig(x) via dlsym(RTLD_NEXT) and multiplies by 3.
             * orig(5) = 5 + 10 = 15; 15 * 3 = 45 */
            int res = wrap_fn(5);
            if (res != 45) {
                printf("FAIL: next_worker(5) = %d (expected 45)\n", res);
                rc = 1;
            }
        }
        /* Also test dlsym(RTLD_NEXT) from the caller/main namespace */
        int (*main_next)(int) = (int (*)(int))dlsym(RTLD_NEXT, "next_worker");
        if (!main_next) {
            printf("FAIL: dlsym(RTLD_NEXT, next_worker) from main failed: %s\n", dlerror());
            rc = 1;
        }
        dlclose(h_wrap);
    }
    unlink(c_nbase);
    unlink(so_nbase);
    unlink(c_nwrap);
    unlink(so_nwrap);

    /* ---- Part 7: Phase 4 Explicit Path Override for Folded Libs (e.g. ./libc.so) ---- */
    char c_clibc[256], so_clibc[256], rel_clibc[256];
    snprintf(c_clibc, sizeof(c_clibc), "/tmp/zerocc_clibc_%d.c", pid);
    snprintf(so_clibc, sizeof(so_clibc), "/tmp/zerocc_mock_libc_%d.so", pid);
    snprintf(rel_clibc, sizeof(rel_clibc), "./zerocc_mock_libc_%d.so", pid);

    f = fopen(c_clibc, "w");
    if (f) { fputs(g_custom_libc_src, f); fclose(f); }

    snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -O2 %s -o %s", c_clibc, so_clibc);
    if (system(cmd) != 0) {
        printf("FAIL: could not build mock libc\n");
        rc = 1;
    }
    /* Create symlink in current directory to test "./mock_libc.so" */
    unlink(rel_clibc);
    if (symlink(so_clibc, rel_clibc) != 0) {
        printf("FAIL: could not create symlink %s\n", rel_clibc);
        rc = 1;
    }

    /* 1. Explicit path with slash: must NOT fold into host libc; must load from disk */
    void *h_custom = dlopen(rel_clibc, RTLD_NOW);
    if (!h_custom) {
        printf("FAIL: dlopen(%s) failed: %s\n", rel_clibc, dlerror());
        rc = 1;
    } else {
        int (*marker_fn)(void) = (int (*)(void))dlsym(h_custom, "custom_libc_marker");
        if (!marker_fn) {
            printf("FAIL: dlsym(custom_libc_marker) failed on explicit path: %s\n", dlerror());
            rc = 1;
        } else if (marker_fn() != 0x55AA55AA) {
            printf("FAIL: marker_fn() = 0x%X (expected 0x55AA55AA)\n", marker_fn());
            rc = 1;
        }
        dlclose(h_custom);
    }

    /* 2. Bare name: folds into host namespace where custom marker does NOT exist */
    void *h_bare_libc = dlopen("libc.so", RTLD_NOW);
    if (!h_bare_libc) {
        printf("FAIL: dlopen(libc.so) bare name failed: %s\n", dlerror());
        rc = 1;
    } else {
        if (dlsym(h_bare_libc, "custom_libc_marker") != NULL) {
            printf("FAIL: bare libc.so resolved custom_libc_marker (should fold into host)\n");
            rc = 1;
        }
        dlclose(h_bare_libc);
    }

    unlink(rel_clibc);
    unlink(c_clibc);
    unlink(so_clibc);

    /* ---- Part 8: Phase 5 Multi-ISA Foreign Architecture Rejection & Diagnostics ---- */
    char c_cross[256], so_aarch64[256], so_rv64[256], so_rv32[256];
    snprintf(c_cross, sizeof(c_cross), "/tmp/zerocc_cross_%d.c", pid);
    snprintf(so_aarch64, sizeof(so_aarch64), "/tmp/zerocc_foreign_aarch64_%d.so", pid);
    snprintf(so_rv64, sizeof(so_rv64), "/tmp/zerocc_foreign_rv64_%d.so", pid);
    snprintf(so_rv32, sizeof(so_rv32), "/tmp/zerocc_foreign_rv32_%d.so", pid);

    f = fopen(c_cross, "w");
    if (f) {
        fputs("int cross_test_val(int x) { return x + 42; }\n", f);
        fclose(f);
    }

    /* 1. Test AArch64 rejection on x86-64 host */
    if (system("which aarch64-linux-gnu-gcc >/dev/null 2>&1") == 0) {
        snprintf(cmd, sizeof(cmd), "aarch64-linux-gnu-gcc -shared -fPIC -O2 %s -o %s", c_cross, so_aarch64);
        if (system(cmd) == 0) {
            void *h_bad_arm = dlopen(so_aarch64, RTLD_NOW);
            if (h_bad_arm != NULL) {
                printf("FAIL: dlopen(aarch64_so) unexpectedly succeeded on x86-64\n");
                dlclose(h_bad_arm);
                rc = 1;
            } else {
                char *err = dlerror();
                if (!err || !strstr(err, "ELF machine mismatch") || !strstr(err, "AArch64 (183)")) {
                    printf("FAIL: aarch64 rejection dlerror mismatch: got '%s'\n", err ? err : "(null)");
                    rc = 1;
                }
            }
            unlink(so_aarch64);
        }
    }

    /* 2. Test RISC-V 64 rejection on x86-64 host */
    if (system("which riscv64-linux-gnu-gcc >/dev/null 2>&1") == 0) {
        snprintf(cmd, sizeof(cmd), "riscv64-linux-gnu-gcc -shared -fPIC -O2 %s -o %s", c_cross, so_rv64);
        if (system(cmd) == 0) {
            void *h_bad_rv64 = dlopen(so_rv64, RTLD_NOW);
            if (h_bad_rv64 != NULL) {
                printf("FAIL: dlopen(rv64_so) unexpectedly succeeded on x86-64\n");
                dlclose(h_bad_rv64);
                rc = 1;
            } else {
                char *err = dlerror();
                if (!err || !strstr(err, "ELF machine mismatch") || !strstr(err, "RISC-V (243)")) {
                    printf("FAIL: rv64 rejection dlerror mismatch: got '%s'\n", err ? err : "(null)");
                    rc = 1;
                }
            }
            unlink(so_rv64);
        }

        /* 3. Test RISC-V 32 (ELF32) rejection on x86-64 host */
        snprintf(cmd, sizeof(cmd), "riscv64-linux-gnu-gcc -march=rv32gc -mabi=ilp32 -shared -fPIC -nostdlib -O2 %s -o %s", c_cross, so_rv32);
        if (system(cmd) == 0) {
            void *h_bad_rv32 = dlopen(so_rv32, RTLD_NOW);
            if (h_bad_rv32 != NULL) {
                printf("FAIL: dlopen(rv32_so) unexpectedly succeeded on x86-64\n");
                dlclose(h_bad_rv32);
                rc = 1;
            } else {
                char *err = dlerror();
                if (!err || !strstr(err, "ELF machine mismatch")) {
                    printf("FAIL: rv32 rejection dlerror mismatch: got '%s'\n", err ? err : "(null)");
                    rc = 1;
                }
            }
            unlink(so_rv32);
        }
    }

    unlink(c_cross);

    if (rc == 0) {
        printf("dlfcn dynlink ok\n");
    }
    return rc;
}
