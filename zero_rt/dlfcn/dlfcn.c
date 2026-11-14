/* zero_rt/dlfcn/dlfcn.c - ZeroCC's own dlfcn for the static-musl host.
 *
 * Background: zerocc and its test binaries are fully statically linked
 * against the vendored musl (third_party/vendor/musl), so the process has
 * no dynamic loader. musl's libc.a therefore ships dlopen/dlsym/dlclose/
 * dlerror as stubs that always fail ("Dynamic loading not supported" /
 * "Symbol not found"), and the build (tools/musl_host_toolchain.sh)
 * strips those stub members from the installed libc.a so this file's
 * definitions are the ones linked (it is an ordinary object file, so it
 * precedes the whole-archived libc on the link line).
 *
 * This is a SELF-HOSTING dlfcn: in a static build there is nothing to
 * load - every symbol the process can ever use already lives in the
 * executable itself (the whole-archived musl libc, zerocc's own objects,
 * and the zero_rt helpers such as zc_udiv128). So:
 *
 *   dlopen(filename, flags)
 *     filename == NULL, or a library a static build folds in (libm,
 *     libgcc_s, libc, ...): success, returning the handle for the
 *     process's own symbol namespace.  Anything else: failure with a
 *     dlerror message (no dynamic loading is possible).
 *
 *   dlsym(handle, name)
 *     RTLD_DEFAULT / our handle: look up `name` in the running
 *     executable's own ELF .symtab. The primary source is a PRE-BAKED
 *     embedded table (Sprint 153, Host Runtime #11 Phase 1):
 *     tools/embed_symtab.py generates a .rodata array from the linked
 *     binary whose entries take each symbol's address as a link-time
 *     relocation, so `dlsym` works with zero /proc access (hardened
 *     containers, chroots, sealed /proc). The table is always non-empty
 *     in zerocc builds; when it is (or when ZERO_RT_DL=proc forces the
 *     legacy path), the fallback below re-reads the executable through
 *     /proc/self/exe; its section headers are walked for SHT_SYMTAB and
 *     every defined symbol becomes (name, base + st_value). A non-PIE
 *     static ET_EXEC (the -no-pie build) keeps st_value addresses
 *     absolute; ET_DYN images are supported by deriving the load base
 *     from /proc/self/maps. The table is sorted by name once and lookups
 *     are binary searches. The file mapping is kept alive for the
 *     process lifetime because the name pointers live inside it.
 *
 * Thread-safety: the table build is once-only (first call); lookups are
 * read-only. dlerror's message buffer is process-global (the JIT is
 * single-threaded; musl's own stubs were not thread-safe either).
 *
 * Note: this module is HOST-side (the compiler's own dependency, like
 * zero_rt/core/int128.c's JIT role). It is NOT part of the per-target
 * zero_rt.a archives that zerocc compiles for user programs.
 */
#ifdef __linux__

#include <dlfcn.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "dynlink.h"

/* ---- Sprint 153 (Host Runtime #11 Phase 1): pre-baked symbol table ----
 *
 * tools/embed_symtab.py (run by the Makefile right after the first link
 * of zerocc) generates a .c file defining this array from the linked
 * binary itself. Each entry takes the symbol's address by reference
 * (an `extern` declaration + `&name`), i.e. a link-time relocation, so
 * the table is always valid for whatever binary it is linked into — no
 * absolute constants, no /proc/self/exe.  A fresh build bootstraps with
 * a sentinel-only table (count 0), which makes the /proc fallback below
 * stay live until the first regeneration fills it in.
 *
 * ZERO_RT_DL environment variable (diagnostics/testing):
 *   "embedded"  - use only the embedded table (no /proc fallback);
 *                 the hardened-container probe.
 *   "proc"      - use only the legacy /proc/self/exe path.
 *   (unset)     - embedded when non-empty, else /proc fallback. */
typedef struct { const char *name; const void *addr; } ZeroRtEmbeddedSym;

/* The table itself is a STRONG definition in the generated file, linked
 * into the zerocc driver.  Test binaries (and any other link) that pull
 * in dlfcn.o without the generated file get these WEAK sentinel-only
 * defaults: count 0 keeps the /proc fallback below live, and the strong
 * generated definitions always win where they are present. */
__attribute__((weak)) const ZeroRtEmbeddedSym g_zero_rt_embedded_syms[] =
    { { 0, 0 } };
__attribute__((weak)) const unsigned long g_zero_rt_embedded_sym_count = 0;

/* Look up a symbol in the pre-baked embedded table.  Exported (non-static)
 * so zero_rt/dlfcn/dynlink.c can use it for head-DSO symbol resolution. */
void *zero_rt_embed_lookup(const char *name) {
    if (g_zero_rt_embedded_sym_count == 0) return NULL;
    const ZeroRtEmbeddedSym *t = g_zero_rt_embedded_syms;
    size_t lo = 0, hi = g_zero_rt_embedded_sym_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(t[mid].name, name);
        if (c == 0) return (void *)t[mid].addr;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* ---- minimal ELF64 view (little-endian; matches <elf.h>) ---- */

#define ZS_ET_EXEC    2
#define ZS_ET_DYN     3
#define ZS_PT_LOAD    1
#define ZS_SHT_SYMTAB 2
#define ZS_SHN_UNDEF  0
#define ZS_SHN_ABS    0xfff1u

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} ZsSym; /* 24 bytes */

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} ZsShdr; /* 64 bytes */

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} ZsPhdr; /* 56 bytes */

typedef struct {
    const char *name;
    void *addr;
} ZeroRtSym;

static ZeroRtSym *g_rt_syms = NULL;
static size_t g_rt_sym_count = 0;
static int g_rt_sym_state = 0; /* 0 = untried, 1 = built, -1 = failed */

/* The single namespace handle (there is only one: the executable itself). */
static int g_rt_ns;

static uint16_t zs_rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t zs_rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int zs_sym_cmp(const void *pa, const void *pb) {
    const ZeroRtSym *a = (const ZeroRtSym *)pa;
    const ZeroRtSym *b = (const ZeroRtSym *)pb;
    return strcmp(a->name, b->name);
}

/* ET_DYN (PIE) images: st_value is relative to the load base.  Find the
 * executable's first mapping in /proc/self/maps and subtract the first
 * PT_LOAD's vaddr.  Returns 0 when the base cannot be determined. */
static uintptr_t zs_dyn_base(const char *path, uint64_t first_load_vaddr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0;
        if (sscanf(line, "%llx-%llx", &start, &end) != 2) continue;
        /* Fields 1-5 are space-separated; the first '/' starts the path. */
        char *slash = strchr(line, '/');
        if (!slash) continue;
        if (strcmp(slash, path) == 0) {
            base = (uintptr_t)start - (uintptr_t)first_load_vaddr;
            break;
        }
    }
    fclose(f);
    return base;
}

static int zero_rt_sym_build(void) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return 0;
    path[n] = '\0';

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < 64) {
        close(fd);
        return 0;
    }
    size_t fsz = (size_t)st.st_size;
    unsigned char *img = (unsigned char *)mmap(NULL, fsz, PROT_READ,
                                               MAP_PRIVATE, fd, 0);
    close(fd);
    if (img == MAP_FAILED) return 0;

    /* ELF64 little-endian header check + field extraction. */
    if (img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F' ||
        img[4] != 2 || img[5] != 1) {
        munmap(img, fsz);
        return 0;
    }
    uint16_t e_type = zs_rd16(img + 16);
    uint64_t e_phoff = zs_rd64(img + 32);
    uint64_t e_shoff = zs_rd64(img + 40);
    uint16_t e_phnum = zs_rd16(img + 56);
    uint16_t e_shnum = zs_rd16(img + 60);
    if (e_shoff == 0 || e_shnum == 0 ||
        e_shoff + (uint64_t)e_shnum * sizeof(ZsShdr) > fsz) {
        /* No section headers (e.g. stripped): resolution is impossible. */
        munmap(img, fsz);
        return 0;
    }

    uintptr_t base = 0;
    if (e_type == ZS_ET_DYN) {
        uint64_t first_load = 0;
        if (e_phoff + (uint64_t)e_phnum * sizeof(ZsPhdr) > fsz) {
            munmap(img, fsz);
            return 0;
        }
        for (uint16_t i = 0; i < e_phnum; i++) {
            const ZsPhdr *ph = (const ZsPhdr *)(img + e_phoff +
                                                (uint64_t)i * sizeof(ZsPhdr));
            if (ph->p_type == ZS_PT_LOAD && ph->p_filesz > 0) {
                first_load = ph->p_vaddr;
                break;
            }
        }
        base = zs_dyn_base(path, first_load);
        if (first_load > 0 && base == 0) {
            munmap(img, fsz);
            return 0;
        }
    }

    /* Locate the SHT_SYMTAB section (its sh_link points at the strtab). */
    const ZsShdr *sh = (const ZsShdr *)(img + e_shoff);
    uint64_t symtab_off = 0, symtab_sz = 0;
    uint32_t stridx = 0;
    int found = 0;
    for (uint16_t i = 0; i < e_shnum; i++) {
        if (sh[i].sh_type == ZS_SHT_SYMTAB) {
            symtab_off = sh[i].sh_offset;
            symtab_sz = sh[i].sh_size;
            stridx = sh[i].sh_link;
            found = 1;
            break;
        }
    }
    if (!found || stridx >= e_shnum) {
        munmap(img, fsz);
        return 0;
    }
    const ZsShdr *strsh = &sh[stridx];
    if (symtab_off + symtab_sz > fsz ||
        strsh->sh_offset + strsh->sh_size > fsz) {
        munmap(img, fsz);
        return 0;
    }

    const char *strtab = (const char *)img + strsh->sh_offset;
    const ZsSym *syms = (const ZsSym *)(img + symtab_off);
    size_t n_syms = symtab_sz / sizeof(ZsSym);

    ZeroRtSym *arr = (ZeroRtSym *)malloc(n_syms ? n_syms * sizeof(ZeroRtSym) : 1);
    if (!arr) {
        munmap(img, fsz);
        return 0;
    }
    size_t cnt = 0;
    for (size_t i = 0; i < n_syms; i++) {
        if (syms[i].st_shndx == ZS_SHN_UNDEF ||
            syms[i].st_shndx == ZS_SHN_ABS) continue;
        const char *nm = strtab + syms[i].st_name;
        if (!nm[0]) continue;
        arr[cnt].name = nm;
        arr[cnt].addr = (void *)(base + syms[i].st_value);
        cnt++;
    }
    /* The mapping must outlive the name pointers: keep it mapped.  (A
     * later rebuild of the executable maps a new inode on the next run.) */
    qsort(arr, cnt, sizeof(ZeroRtSym), zs_sym_cmp);
    g_rt_syms = arr;
    g_rt_sym_count = cnt;
    return 1;
}

static void *zero_rt_sym_lookup(const char *name) {
    if (!name || !name[0]) return NULL;
    /* Sprint 153: the embedded table is the primary source (no /proc).
     * ZERO_RT_DL is read per call (dlsym is rare; tests switch modes). */
    const char *m = getenv("ZERO_RT_DL");
    int proc_only = (m && strcmp(m, "proc") == 0) ? 1 : 0;
    int embed_only = (m && strcmp(m, "embedded") == 0) ? 1 : 0;
    if (!proc_only) {
        void *sym = zero_rt_embed_lookup(name);
        if (sym) return sym;
        if (embed_only) return NULL; /* embedded-only: /proc disallowed */
    }
    if (g_rt_sym_state == 0) {
        g_rt_sym_state = zero_rt_sym_build() ? 1 : -1;
    }
    if (g_rt_sym_state != 1) return NULL;
    size_t lo = 0, hi = g_rt_sym_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(g_rt_syms[mid].name, name);
        if (c == 0) return g_rt_syms[mid].addr;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void *zero_rt_host_sym_lookup(const char *name) {
    return zero_rt_sym_lookup(name);
}

/* ---- dlerror bookkeeping ---- */

static char g_rt_dl_errmsg[256];
static int g_rt_dl_errset = 0;

static void zero_rt_dl_seterr(const char *msg) {
    snprintf(g_rt_dl_errmsg, sizeof(g_rt_dl_errmsg), "%s", msg);
    g_rt_dl_errset = 1;
}

/* ---- the public dlfcn API (replaces musl's static-build stubs) ----
 *
 * Sprint 155: these are local to the link unit (visibility hidden) so that
 * in a dynamic build (e.g. the `make asan` sanitizer tree) they do NOT appear
 * in .dynsym and therefore do NOT interpose the host libc's dlfcn.  The ASan
 * runtime calls `dlsym(RTLD, "_thread_db_sizeof_pthread")` during
 * AsanInitInternal; if this binary's `dlsym` shadowed libc's, that startup
 * probe would route here and the malloc + /proc walk in zero_rt_sym_build is
 * not safe mid-sanitizer-init (silent SIGSEGV before any user code).
 * In-process callers (the JIT self-hosting path, the dlfcn tests) still bind
 * to these local definitions; hiding only removes the .dynsym export.  A
 * fully static build is unaffected (no .dynsym).
 */
#define ZC_DLFCN_HIDDEN __attribute__((visibility("hidden")))

ZC_DLFCN_HIDDEN void *dlopen(const char *filename, int flags) {
    (void)flags;
    /* NULL: the main program's namespace.  A library name that a static
     * build folds into the executable (libm, libgcc_s, libc, ...): the
     * "load" is trivially satisfied by the process itself.  Anything
     * else: there is no dynamic loader to satisfy it. */
    if (filename == NULL) return (void *)&g_rt_ns;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    /* Match on the library stem: lib<stem> with any version suffix. */
    static const char *folded[] = {
        "libm", "libgcc", "libgcc_s", "libc", "libpthread", "libdl",
        "librt", "libutil", "libcrypt", "libresolv", "libxnet", NULL,
    };
    for (int i = 0; folded[i]; i++) {
        size_t len = strlen(folded[i]);
        if (strncmp(base, folded[i], len) == 0 &&
            (base[len] == '.' || base[len] == '\0')) {
            return (void *)&g_rt_ns;
        }
    }
    if (strncmp(base, "ld-musl-", 8) == 0 && strstr(base, ".so")) {
        return (void *)&g_rt_ns;
    }
    /* Real .so loading via the in-process dynamic linker (Phase 2 & 3).
     * Supports relative/absolute paths, bare names via RPATH/RUNPATH ($ORIGIN),
     * LD_LIBRARY_PATH, and default system directories. */
    void *h = zc_dlopen(filename, flags);
    if (h) return h;

    zero_rt_dl_seterr(filename);
    errno = ENOENT;
    return NULL;
}

ZC_DLFCN_HIDDEN void *dlsym(void *handle, const char *name) {
    if (!name || !name[0]) {
        zero_rt_dl_seterr("dlsym: invalid name");
        return NULL;
    }
    void *sym = NULL;
    if (handle != NULL && handle != (void *)&g_rt_ns) {
        /* Phase 2: specific ZcDso* handle returned by zc_dlopen. */
        sym = zc_dlsym(handle, name);
    } else {
        /* NULL (RTLD_DEFAULT) or &g_rt_ns: search main executable's own
         * symtab first, then any loaded .so files. */
        sym = zero_rt_sym_lookup(name);
        if (!sym) {
            sym = zc_dlsym(NULL, name);
        }
    }
    if (!sym) {
        snprintf(g_rt_dl_errmsg, sizeof(g_rt_dl_errmsg),
                 "dlsym: Symbol not found: %s", name ? name : "(null)");
        g_rt_dl_errset = 1;
        return NULL;
    }
    return sym;
}

ZC_DLFCN_HIDDEN void *dlvsym(void *handle, const char *name, const char *version) {
    if (!name || !name[0]) {
        zero_rt_dl_seterr("dlvsym: invalid name");
        return NULL;
    }
    if (!version || !version[0]) {
        return dlsym(handle, name);
    }
    void *sym = NULL;
    if (handle != NULL && handle != (void *)&g_rt_ns) {
        sym = zc_dlvsym(handle, name, version);
    } else {
        sym = zc_dlvsym(NULL, name, version);
        if (!sym) {
            sym = zero_rt_sym_lookup(name);
        }
    }
    if (!sym) {
        snprintf(g_rt_dl_errmsg, sizeof(g_rt_dl_errmsg),
                 "dlvsym: Symbol not found: %s@%s", name ? name : "(null)",
                 version ? version : "(null)");
        g_rt_dl_errset = 1;
        return NULL;
    }
    return sym;
}

ZC_DLFCN_HIDDEN int dlclose(void *handle) {
    if (handle == (void *)&g_rt_ns) {
        /* The namespace is the process itself: nothing to unload. */
        return 0;
    }
    if (zc_dlclose(handle) == 0) {
        return 0;
    }
    zero_rt_dl_seterr("dlclose: invalid handle");
    return -1;
}

ZC_DLFCN_HIDDEN char *dlerror(void) {
    if (!g_rt_dl_errset) return NULL;
    g_rt_dl_errset = 0;
    return g_rt_dl_errmsg;
}

#else /* !__linux__ */

/* Non-Linux hosts keep the system libc's dlfcn (the static-musl build is
 * Linux-only). */

#endif /* __linux__ */
