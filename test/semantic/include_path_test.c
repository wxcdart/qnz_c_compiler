/* Regression test: preprocessor include-path semantics (Sprint 42).
 *
 * 1. Quoted includes in the MAIN file resolve relative to the main
 *    file's directory (C99 6.10.2p2), not the process CWD.
 * 2. A missing include file is a hard error with a diagnostic that
 *    names the file (C99 6.10.2 constraint violation), not a silent
 *    skip.
 * 3. __FILE__ in the main file expands to the main file's path.
 * 4. The legacy zero_cc_preprocess() entry point (no main file) still
 *    resolves quoted includes through explicit -I-style include dirs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "zero_cc.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("ok - %s\n", what);
    } else {
        printf("FAIL - %s\n", what);
        failures++;
    }
}

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

int main(void) {
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "/tmp/zzinc_%d_XXXXXX", (int)getpid());
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }

    char dir[PATH_MAX + 64], subdir[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/main_dir", base);
    snprintf(subdir, sizeof(subdir), "%s/main_dir/sub", base);
    if (mkdir(dir, 0755) != 0 || mkdir(subdir, 0755) != 0) {
        perror("mkdir");
        return 1;
    }

    char prog[PATH_MAX + 64], hdr[PATH_MAX + 64];
    snprintf(prog, sizeof(prog), "%s/prog.c", dir);
    snprintf(hdr, sizeof(hdr), "%s/local.h", subdir);

    const char *hdr_src = "int local_val(void) { return 42; }\n";
    const char *prog_src = "#include \"sub/local.h\"\n"
                           "int main(void) { return local_val(); }\n";
    if (write_file(hdr, hdr_src) != 0 || write_file(prog, prog_src) != 0) {
        perror("write_file");
        return 1;
    }

    /* 1. Quoted include relative to the MAIN file's directory, even
     *    though the CWD cannot see "sub/local.h". */
    char err1[PATH_MAX + 96];
    err1[0] = '\0';
    char *prep1 = zero_cc_preprocess_ext2(prog_src, NULL, NULL, prog,
                                          err1, sizeof(err1));
    check(prep1 != NULL && err1[0] == '\0',
          "include relative to main-file dir resolves");
    check(prep1 && strstr(prep1, "int local_val(void)") != NULL,
          "included header content is in the preprocessed output");
    if (prep1) free(prep1);

    /* 2. Missing include => NULL result + diagnostic naming the file. */
    const char *bad_src = "#include \"zz_nosuch_include_zz.h\"\n"
                          "int main(void) { return 0; }\n";
    char err2[PATH_MAX + 96];
    err2[0] = '\0';
    char *prep2 = zero_cc_preprocess_ext2(bad_src, NULL, NULL, prog,
                                          err2, sizeof(err2));
    check(prep2 == NULL, "missing include is a hard error (NULL result)");
    check(strstr(err2, "zz_nosuch_include_zz.h") != NULL,
          "diagnostic names the missing header");
    check(strstr(err2, "No such file") != NULL,
          "diagnostic says the file was not found");

    /* 3. __FILE__ in the main file expands to the main file's path. */
    const char *file_src = "const char *f = __FILE__;\n";
    char err3[PATH_MAX + 96];
    err3[0] = '\0';
    char *prep3 = zero_cc_preprocess_ext2(file_src, NULL, NULL, prog,
                                          err3, sizeof(err3));
    check(prep3 && strstr(prep3, prog) != NULL,
          "__FILE__ in main file is its path");
    if (prep3) free(prep3);

    /* 4. Legacy API: explicit include dirs still work without a
     *    main file. */
    char legacy_hdr[PATH_MAX + 64];
    snprintf(legacy_hdr, sizeof(legacy_hdr), "%s/legacy.h", base);
    if (write_file(legacy_hdr, "int legacy_fn(void) { return 7; }\n") != 0) {
        perror("write_file");
        return 1;
    }
    ZeroCCIncludeDirs inc;
    memset(&inc, 0, sizeof(inc));
    inc.paths[inc.count++] = base;
    const char *legacy_src = "#include \"legacy.h\"\n";
    char *prep4 = zero_cc_preprocess(legacy_src, &inc);
    check(prep4 && strstr(prep4, "legacy_fn") != NULL,
          "legacy zero_cc_preprocess with explicit include dir");
    if (prep4) free(prep4);

    /* Cleanup */
    unlink(hdr);
    unlink(prog);
    unlink(legacy_hdr);
    rmdir(subdir);
    rmdir(dir);
    rmdir(base);

    if (failures) {
        printf("include_path_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("include_path_test: all passed\n");
    return 0;
}
