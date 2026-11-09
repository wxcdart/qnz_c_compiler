/* pie_test.c - static-PIE link with ZeroCC's own linker (x86-64).
 *
 * A two-TU program exercising absolute (R_X86_64_64) references - a
 * static global, a static string, and a function pointer initialized to
 * a symbol defined in the other TU - is compiled with `zerocc -c` and
 * linked twice:
 *
 *   (1) --zerocc-link       classic static EXEC (fixed base 0x400000)
 *   (2) --zerocc-link-pie   static PIE: ET_DYN laid out from base 0, every
 *                           R_64 converted to an R_X86_64_RELATIVE entry
 *                           in .rela.dyn, applied at startup by the
 *                           linker's self-relocating _start stub
 *
 * The PIE binary must be a real ET_DYN (no program can tell the load
 * base ahead of time), must carry a non-empty .rela.dyn, and must print
 * exactly what the EXEC build prints - which only happens if the
 * load-time relocation loop fixes up every absolute reference,
 * including the function pointer the call goes through.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static const char *SRC_A =
    "int printf(const char *, ...);\n"
    "static int n = 21;\n"
    "int half(int x);\n"
    "int (*fp)(int) = half;\n"
    "static char msg[] = \"pie-ok %d %d\\n\";\n"
    "int main(void) {\n"
    "    printf(msg, fp(n), n);\n"
    "    return 0;\n"
    "}\n";

static const char *SRC_B = "int half(int x) { return x / 2; }\n";

#define EXPECTED_OUT "pie-ok 10 21\n"

static const char *zerocc; /* absolute path to build/bin/zerocc */

static int write_file(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(s, f) < 0) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int run_capture(const char *cmd, char *out, size_t outsz) {
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t n = 0;
    while (n + 1 < outsz) {
        int c = fgetc(f);
        if (c == EOF) break;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return pclose(f);
}

static int file_type_is_dyn(const char *path, int *has_rela_dyn) {
    *has_rela_dyn = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[64];
    if (fread(hdr, 1, 64, f) != 64) { fclose(f); return -1; }
    if (memcmp(hdr, "\x7f" "ELF", 4) != 0) { fclose(f); return -1; }
    if (hdr[4] != 2 || hdr[5] != 1) { fclose(f); return -1; } /* 64-bit LE */
    uint16_t e_type = (uint16_t)(hdr[16] | (hdr[17] << 8));
    uint64_t shoff = 0;
    for (int i = 0; i < 8; i++) shoff |= (uint64_t)hdr[0x28 + i] << (8 * i);
    uint16_t shentsize = (uint16_t)(hdr[0x3a] | (hdr[0x3b] << 8));
    uint16_t shnum = (uint16_t)(hdr[0x3c] | (hdr[0x3d] << 8));
    uint16_t shstrndx = (uint16_t)(hdr[0x3e] | (hdr[0x3f] << 8));
    if (shoff == 0 || shnum == 0 || e_type != 3 /* ET_DYN */) {
        fclose(f);
        return e_type;
    }
    uint8_t *shs = malloc((size_t)shnum * shentsize);
    fseek(f, (long)shoff, SEEK_SET);
    if (fread(shs, 1, (size_t)shnum * shentsize, f) !=
        (size_t)shnum * shentsize) {
        free(shs); fclose(f);
        return e_type;
    }
    /* shstrtab of the section named at shstrndx */
    uint8_t *sh = shs + (size_t)shstrndx * shentsize;
    uint64_t shs_off = 0, shs_size = 0;
    for (int i = 0; i < 8; i++) {
        shs_off |= (uint64_t)sh[0x18 + i] << (8 * i);
        shs_size |= (uint64_t)sh[0x20 + i] << (8 * i);
    }
    char *shstr = malloc(shs_size ? shs_size : 1);
    fseek(f, (long)shs_off, SEEK_SET);
    if (fread(shstr, 1, shs_size, f) != shs_size) {
        free(shs); free(shstr); fclose(f);
        return e_type;
    }
    for (int i = 0; i < shnum; i++) {
        sh = shs + (size_t)i * shentsize;
        uint32_t name = (uint32_t)(sh[0] | (sh[1] << 8) | (sh[2] << 16) |
                                   (sh[3] << 24));
        uint32_t type = (uint32_t)(sh[4] | (sh[5] << 8) | (sh[6] << 16) |
                                   (sh[7] << 24));
        if (name < shs_size && strcmp(shstr + name, ".rela.dyn") == 0) {
            uint64_t sz = 0;
            for (int k = 0; k < 8; k++)
                sz |= (uint64_t)sh[0x20 + k] << (8 * k);
            *has_rela_dyn = (type == 4 /* SHT_RELA */ && sz >= 24);
            break;
        }
    }
    free(shs); free(shstr);
    fclose(f);
    return e_type;
}

int main(void) {
    int fails = 0;

    /* Locate zerocc: <repo>/build/test/linker/pie_test -> ../../bin/zerocc */
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) { fprintf(stderr, "cannot resolve /proc/self/exe\n"); return 1; }
    self[n] = 0;
    char *slash = strrchr(self, '/');
    if (slash) *slash = 0;
    char zc[600];
    snprintf(zc, sizeof zc, "%s/../../bin/zerocc", self);
    if (access(zc, X_OK) != 0) {
        fprintf(stderr, "cannot find zerocc at %s\n", zc);
        return 1;
    }
    zerocc = zc;

    char cmd[2048];
    char out[1024];

    if (write_file("/tmp/zc_pie_a.c", SRC_A) ||
        write_file("/tmp/zc_pie_b.c", SRC_B)) {
        fprintf(stderr, "cannot write sources\n");
        return 1;
    }

    /* (1) EXEC link - the reference. */
    snprintf(cmd, sizeof cmd,
             "%s -c /tmp/zc_pie_a.c -o /tmp/zc_pie_a.o 2>&1 && "
             "%s -c /tmp/zc_pie_b.c -o /tmp/zc_pie_b.o 2>&1 && "
             "%s --zerocc-link /tmp/zc_pie_a.o /tmp/zc_pie_b.o "
             "-o /tmp/zc_pie_np.bin 2>&1 && /tmp/zc_pie_np.bin",
             zerocc, zerocc, zerocc);
    int rc = run_capture(cmd, out, sizeof out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: EXEC link/run failed (rc=%d): %s\n", rc, out);
        return 1;
    }
    if (strcmp(out, EXPECTED_OUT) != 0) {
        fprintf(stderr, "FAIL: EXEC output '%s' != expected '%s'\n", out,
                EXPECTED_OUT);
        return 1;
    }

    /* (2) Static-PIE link. */
    char pie_err[1024];
    snprintf(pie_err, sizeof pie_err, "%s", out);
    snprintf(cmd, sizeof cmd,
             "%s --zerocc-link-pie /tmp/zc_pie_a.o /tmp/zc_pie_b.o "
             "-o /tmp/zc_pie_p.bin 2>&1",
             zerocc);
    rc = run_capture(cmd, pie_err, sizeof pie_err);
    if (rc != 0) {
        fprintf(stderr, "FAIL: PIE link failed (rc=%d): %s\n", rc, pie_err);
        fails++;
    } else {
        int dyn = 0, rela = 0;
        dyn = file_type_is_dyn("/tmp/zc_pie_p.bin", &rela);
        if (dyn != 3) {
            fprintf(stderr, "FAIL: PIE binary e_type=%d (want ET_DYN=3)\n",
                    dyn);
            fails++;
        }
        if (!rela) {
            fprintf(stderr, "FAIL: PIE binary has no .rela.dyn (or it is "
                    "empty / not SHT_RELA)\n");
            fails++;
        }
        snprintf(cmd, sizeof cmd, "/tmp/zc_pie_p.bin 2>&1");
        rc = run_capture(cmd, out, sizeof out);
        if (rc != 0) {
            fprintf(stderr, "FAIL: PIE binary exited %d\n", rc);
            fails++;
        } else if (strcmp(out, EXPECTED_OUT) != 0) {
            fprintf(stderr, "FAIL: PIE output '%s' != expected '%s'\n", out,
                    EXPECTED_OUT);
            fails++;
        }
    }

    /* (3) PIE on a non-x86-64 object is rejected cleanly. */
    snprintf(cmd, sizeof cmd,
             "%s --target aarch64 -c /tmp/zc_pie_b.c -o /tmp/zc_pie_b64.o "
             "2>&1 && %s --zerocc-link-pie /tmp/zc_pie_b64.o "
             "-o /tmp/zc_pie_b64.bin 2>&1",
             zerocc, zerocc);
    rc = run_capture(cmd, pie_err, sizeof pie_err);
    if (rc == 0) {
        fprintf(stderr, "FAIL: PIE link of an aarch64 object succeeded "
                "(should be a clean error)\n");
        fails++;
    } else if (strstr(pie_err, "x86-64 only") == NULL) {
        fprintf(stderr, "FAIL: unexpected aarch64 PIE error: %s\n", pie_err);
        fails++;
    }

    if (fails == 0) {
        printf("pie_test ok\n");
        return 0;
    }
    return 1;
}
