/* zero_rt/dlfcn/dynlink.c — Host-side in-process dynamic loader (Phase 2 & 3).
 *
 * ZeroCC's `build/bin/zerocc` and its test binaries are fully statically
 * linked against vendored musl, so there is no system dynamic loader
 * (ld.so) in the process.  musl's static archive ships dlopen/dlsym/dlclose
 * stubs that always fail; zero_rt/dlfcn/dlfcn.c replaces those stubs with a
 * self-hosting implementation that resolves symbols from the executable's
 * own ELF .symtab (pre-baked embedded table + /proc/self/exe fallback).
 *
 * Phase 1 (Sprint 153) covered dlsym on the running process's own symbol
 * namespace.  Phase 2 added real .so loading: mapping ELF PT_LOAD segments,
 * relocating the image, resolving symbols, running constructors.
 * Phase 3 expands dynamic loading:
 *   - Search paths: LD_LIBRARY_PATH, default system directories, and
 *     DT_RPATH / DT_RUNPATH with $ORIGIN expansion.
 *   - Recursive DT_NEEDED dependencies: automatically loading dependency
 *     chains, breadth-first scope symbol lookup, dependency constructor
 *     ordering, and cycle safety.
 *   - Symbol versioning: GNU symbol versioning (.gnu.version / DT_VERSYM,
 *     .gnu.version_d / DT_VERDEF, .gnu.version_r / DT_VERNEED), filtering
 *     hidden versions for default dlsym, and dlvsym(handle, name, version).
 *
 * Compiled by the host C compiler (GCC/Clang) and linked into
 * build/bin/zerocc and host test binaries via ZC_DYNLINK_OBJ.  NOT part of
 * the per-target zero_rt.a archives — target-ISA dlopen is future work.
 */

#define _GNU_SOURCE
#include <dlfcn.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dynlink.h"

/* Defined in zero_rt/dlfcn/dlfcn.c: looks up a symbol in the host executable
 * (embedded table first, then /proc/self/exe fallback). */
extern void *zero_rt_host_sym_lookup(const char *name);

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5UL
#endif
#ifndef VER_NDX_LOCAL
#define VER_NDX_LOCAL 0
#endif
#ifndef VER_NDX_ELIMINATE
#define VER_NDX_ELIMINATE 0xff01
#endif

/* ---- DSO representation ---- */

typedef struct zc_dso ZcDso;

struct zc_dso {
    char          *name;          /* .so filename / "<main executable>" */
    dev_t          dev;           /* device ID for deduplication */
    ino_t          ino;           /* inode ID for deduplication */
    void          *base;          /* mmap'd base address of the loaded image */
    uintptr_t      map_start;     /* page-aligned start of the mapping window */
    size_t         map_len;       /* total length of the mmap'd window */
    Elf64_Dyn     *dynamic;       /* .dynamic section entries (mmap'd address) */
    size_t         dynamic_count;
    char          *strtab;        /* .dynstr (mmap'd address) */
    Elf64_Sym     *symtab;        /* .dynsym (mmap'd address) */
    size_t         symtab_count;
    uint32_t      *sysv_hash;     /* DT_HASH (mmap'd address) */
    uint32_t      *gnu_hash;      /* DT_GNU_HASH (mmap'd address) */
    Elf64_Rela     *rela;         /* DT_RELA (.rela.dyn) */
    size_t          relasz;       /* DT_RELASZ (bytes) */
    Elf64_Rela     *jmprel;       /* DT_JMPREL (.rela.plt) */
    size_t          pltrelsz;     /* DT_PLTRELSZ (bytes) */
    void          (*init_fn)(void);
    void         (**init_array)(void);
    size_t          init_arraysz;

    /* DT_NEEDED and search paths */
    char          *rpath;         /* DT_RPATH (.dynstr pointer) */
    char          *runpath;       /* DT_RUNPATH (.dynstr pointer) */
    size_t        *needed_offsets;/* offsets into strtab for DT_NEEDED */
    size_t         needed_count;
    size_t         needed_cap;
    ZcDso        **deps;          /* DT_NEEDED dependencies (dynamic array) */
    int            dep_count;
    int            dep_cap;

    /* GNU symbol versioning */
    uint16_t      *versym;        /* DT_VERSYM (.gnu.version) */
    Elf64_Verdef  *verdef;        /* DT_VERDEF (.gnu.version_d) */
    size_t         verdef_num;    /* DT_VERDEFNUM */
    Elf64_Verneed *verneed;       /* DT_VERNEED (.gnu.version_r) */
    size_t         verneed_num;   /* DT_VERNEEDNUM */

    int            relocated;     /* relocations applied */
    int            constructed;   /* constructors run */
    int            refcnt;
    ZcDso         *next;          /* global DSO list linkage */
};

/* ---- globals ---- */

static ZcDso *g_zc_head   = NULL;   /* synthetic DSO for main executable */
static ZcDso *g_zc_list   = NULL;   /* global DSO list head */
static int    g_zc_inited = 0;      /* zero_rt_dl_init() called? */

/* Forward declaration */
static void *zc_dlopen_internal(const char *filename, ZcDso *caller, int flags);

/* ---- small helpers ---- */

static uint32_t zc_sysv_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t zc_gnu_hash(const char *name) {
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h = (h << 5) + h + *p;
    }
    return h;
}

static void zc_seterr(int err) {
    errno = err;
}

static void zc_dso_list_insert(ZcDso *d) {
    d->next = g_zc_list;
    g_zc_list = d;
}

static void *zc_head_lookup(const char *name) {
    if (!name || !name[0]) return NULL;
    return zero_rt_host_sym_lookup(name);
}

/* Check if a library name is folded into the host executable. */
static int zc_is_folded_lib(const char *name) {
    if (!name || !name[0]) return 0;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    static const char *folded[] = {
        "libm", "libgcc", "libgcc_s", "libc", "libpthread", "libdl",
        "librt", "libutil", "libcrypt", "libresolv", "libxnet", NULL,
    };
    for (int i = 0; folded[i]; i++) {
        size_t len = strlen(folded[i]);
        if (strncmp(base, folded[i], len) == 0 &&
            (base[len] == '.' || base[len] == '\0' || base[len] == '-')) {
            return 1;
        }
    }
    if ((strncmp(base, "ld-musl-", 8) == 0 || strncmp(base, "ld-linux-", 9) == 0) &&
        strstr(base, ".so")) {
        return 1;
    }
    return 0;
}

/* ---- ELF64 loading ----
 *
 * Virtual-address -> mmap'd-address translation:
 *   mmap_addr(vaddr) = dso->base + (vaddr - dso->map_start)
 */

static void *zc_va_to_ptr(ZcDso *dso, uintptr_t vaddr) {
    return (void *)((char *)dso->base + (vaddr - dso->map_start));
}

/* Map all PT_LOAD segments of an ELF image into anonymous memory.
 * Returns the mmap'd base address on success, NULL on failure (errno set).
 * Sets *map_start and *map_len for the caller. */
static void *zc_map_segments(int fd, uintptr_t *map_start, size_t *map_len) {
    Elf64_Ehdr ehdr;
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        zc_seterr(EINVAL);
        return NULL;
    }
    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        zc_seterr(EINVAL);
        return NULL;
    }
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        zc_seterr(EINVAL);
        return NULL;
    }
    if (ehdr.e_machine != EM_X86_64) {
        zc_seterr(EINVAL);
        return NULL;  /* MVP: x86-64 only */
    }

    /* Read all program headers. */
    Elf64_Phdr *phdrs = malloc(ehdr.e_phnum * sizeof(Elf64_Phdr));
    if (!phdrs) {
        zc_seterr(ENOMEM);
        return NULL;
    }
    if (lseek(fd, (off_t)ehdr.e_phoff, SEEK_SET) < 0 ||
        read(fd, phdrs, ehdr.e_phnum * sizeof(Elf64_Phdr)) !=
            (ssize_t)(ehdr.e_phnum * sizeof(Elf64_Phdr))) {
        free(phdrs);
        zc_seterr(EINVAL);
        return NULL;
    }

    /* Find the total extent of PT_LOAD segments. */
    uintptr_t min_vaddr = ~(uintptr_t)0, max_vend = 0;
    int       has_load = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        has_load = 1;
        if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
        uintptr_t vend = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (vend > max_vend) max_vend = vend;
    }
    if (!has_load) {
        free(phdrs);
        zc_seterr(EINVAL);
        return NULL;
    }

    /* Page-align the mapping window. */
    long page_sz = sysconf(_SC_PAGESIZE);
    uintptr_t ms = min_vaddr & ~(uintptr_t)(page_sz - 1);
    uintptr_t me = (max_vend + page_sz - 1) & ~(uintptr_t)(page_sz - 1);
    size_t  sz   = (size_t)(me - ms);

    void *map = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        free(phdrs);
        zc_seterr(ENOMEM);
        return NULL;
    }

    /* Read each PT_LOAD segment from its exact file offset into the mapping. */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uintptr_t seg_vaddr  = phdrs[i].p_vaddr;
        uintptr_t seg_offset = phdrs[i].p_offset;
        uintptr_t seg_filesz = phdrs[i].p_filesz;
        uintptr_t seg_memsz  = phdrs[i].p_memsz;

        void *dst = (char *)map + (seg_vaddr - ms);
        if (seg_filesz > 0) {
            if (lseek(fd, (off_t)seg_offset, SEEK_SET) < 0 ||
                read(fd, dst, seg_filesz) != (ssize_t)seg_filesz) {
                munmap(map, sz);
                free(phdrs);
                zc_seterr(EIO);
                return NULL;
            }
        }
        /* Zero-fill memsz - filesz (BSS-like trailing bytes) */
        if (seg_memsz > seg_filesz) {
            size_t rest = (size_t)(seg_memsz - seg_filesz);
            memset((char *)dst + seg_filesz, 0, rest);
        }
    }
    free(phdrs);

    *map_start = ms;
    *map_len   = sz;
    return map;
}

/* ---- search path resolution ---- */

static void zc_get_origin_dir(const char *dso_path, char *buf, size_t sz) {
    if (!dso_path || !dso_path[0]) {
        snprintf(buf, sz, ".");
        return;
    }
    const char *slash = strrchr(dso_path, '/');
    if (!slash) {
        snprintf(buf, sz, ".");
        return;
    }
    size_t len = (size_t)(slash - dso_path);
    if (len == 0) {
        snprintf(buf, sz, "/");
        return;
    }
    if (len >= sz) len = sz - 1;
    memcpy(buf, dso_path, len);
    buf[len] = '\0';
}

static void zc_expand_origin_token(const char *token, const char *origin_dir, char *buf, size_t sz) {
    buf[0] = '\0';
    const char *p = token;
    size_t out_len = 0;
    size_t origin_len = strlen(origin_dir);

    while (*p && out_len + 1 < sz) {
        if (strncmp(p, "$ORIGIN", 7) == 0) {
            size_t copy_len = origin_len;
            if (out_len + copy_len >= sz) copy_len = sz - 1 - out_len;
            memcpy(buf + out_len, origin_dir, copy_len);
            out_len += copy_len;
            p += 7;
        } else if (strncmp(p, "${ORIGIN}", 9) == 0) {
            size_t copy_len = origin_len;
            if (out_len + copy_len >= sz) copy_len = sz - 1 - out_len;
            memcpy(buf + out_len, origin_dir, copy_len);
            out_len += copy_len;
            p += 9;
        } else {
            buf[out_len++] = *p++;
        }
    }
    buf[out_len] = '\0';
}

static int zc_resolve_library_path(const char *filename, ZcDso *caller, char *out_path, size_t out_sz) {
    if (!filename || !filename[0] || !out_path || out_sz == 0) return -1;

    /* 1. Explicit path containing '/' */
    if (strchr(filename, '/') != NULL) {
        if (access(filename, R_OK) == 0) {
            snprintf(out_path, out_sz, "%s", filename);
            return 0;
        }
        return -1;
    }

    char candidate[2048];
    char origin_dir[512];
    if (caller && caller->name) {
        zc_get_origin_dir(caller->name, origin_dir, sizeof(origin_dir));
    } else {
        snprintf(origin_dir, sizeof(origin_dir), ".");
    }

    /* 2. Caller RPATH / RUNPATH */
    const char *rpath = (caller && caller->runpath) ? caller->runpath
                      : (caller && caller->rpath)   ? caller->rpath : NULL;
    if (rpath && rpath[0]) {
        const char *p = rpath;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t tok_len = colon ? (size_t)(colon - p) : strlen(p);
            char tok[256];
            if (tok_len >= sizeof(tok)) tok_len = sizeof(tok) - 1;
            memcpy(tok, p, tok_len);
            tok[tok_len] = '\0';

            char expanded_dir[512];
            zc_expand_origin_token(tok, origin_dir, expanded_dir, sizeof(expanded_dir));

            snprintf(candidate, sizeof(candidate), "%s/%s", expanded_dir, filename);
            if (access(candidate, R_OK) == 0) {
                snprintf(out_path, out_sz, "%s", candidate);
                return 0;
            }
            if (!colon) break;
            p = colon + 1;
        }
    }

    /* 3. LD_LIBRARY_PATH */
    const char *ld_lib = getenv("LD_LIBRARY_PATH");
    if (ld_lib && ld_lib[0]) {
        const char *p = ld_lib;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t tok_len = colon ? (size_t)(colon - p) : strlen(p);
            char tok[256];
            if (tok_len >= sizeof(tok)) tok_len = sizeof(tok) - 1;
            memcpy(tok, p, tok_len);
            tok[tok_len] = '\0';

            const char *dir = (tok[0] == '\0') ? "." : tok;
            snprintf(candidate, sizeof(candidate), "%s/%s", dir, filename);
            if (access(candidate, R_OK) == 0) {
                snprintf(out_path, out_sz, "%s", candidate);
                return 0;
            }
            if (!colon) break;
            p = colon + 1;
        }
    }

    /* 4. Default system directories */
    static const char *sys_dirs[] = {
        "/lib/x86_64-linux-gnu",
        "/usr/lib/x86_64-linux-gnu",
        "/lib64",
        "/usr/lib64",
        "/lib",
        "/usr/lib",
        "/usr/local/lib",
        NULL
    };
    for (int i = 0; sys_dirs[i]; i++) {
        snprintf(candidate, sizeof(candidate), "%s/%s", sys_dirs[i], filename);
        if (access(candidate, R_OK) == 0) {
            snprintf(out_path, out_sz, "%s", candidate);
            return 0;
        }
    }

    return -1;
}

/* ---- dynamic section parsing ---- */

static int zc_parse_dynamic(ZcDso *dso, int fd, void *base) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);

    Elf64_Dyn *dyn = NULL;
    size_t     dyn_count = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)zc_va_to_ptr(dso, phdrs[i].p_vaddr);
            dyn_count = phdrs[i].p_filesz / sizeof(Elf64_Dyn);
            break;
        }
    }
    if (!dyn) {
        return -1;
    }

    dso->dynamic = dyn;
    dso->dynamic_count = dyn_count;

    size_t rpath_off = (size_t)-1;
    size_t runpath_off = (size_t)-1;

    for (size_t i = 0; i < dyn_count; i++) {
        Elf64_Sxword tag = dyn[i].d_tag;
        if (tag == DT_NULL) break;
        switch ((uint64_t)tag) {
        case DT_STRTAB:
            dso->strtab = (char *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_SYMTAB:
            dso->symtab = (Elf64_Sym *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_HASH:
            dso->sysv_hash = (uint32_t *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            dso->symtab_count = dso->sysv_hash[1]; /* nchain == dynsym count */
            break;
        case DT_GNU_HASH:
            dso->gnu_hash = (uint32_t *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_RELA:
            dso->rela = (Elf64_Rela *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_RELASZ:
            dso->relasz = (size_t)dyn[i].d_un.d_val;
            break;
        case DT_JMPREL:
            dso->jmprel = (Elf64_Rela *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            dso->pltrelsz = (size_t)dyn[i].d_un.d_val;
            break;
        case DT_INIT:
            dso->init_fn = (void (*)(void))zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_INIT_ARRAY:
            dso->init_array = (void (**)(void))zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_INIT_ARRAYSZ:
            dso->init_arraysz = (size_t)dyn[i].d_un.d_val;
            break;
        case DT_RPATH:
            rpath_off = (size_t)dyn[i].d_un.d_val;
            break;
        case DT_RUNPATH:
            runpath_off = (size_t)dyn[i].d_un.d_val;
            break;
        case DT_NEEDED:
            if (dso->needed_count == dso->needed_cap) {
                size_t ncap = dso->needed_cap ? dso->needed_cap * 2 : 8;
                size_t *noff = realloc(dso->needed_offsets, ncap * sizeof(size_t));
                if (noff) {
                    dso->needed_offsets = noff;
                    dso->needed_cap = ncap;
                }
            }
            if (dso->needed_count < dso->needed_cap) {
                dso->needed_offsets[dso->needed_count++] = (size_t)dyn[i].d_un.d_val;
            }
            break;
        case DT_VERSYM:
            dso->versym = (uint16_t *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_VERDEF:
            dso->verdef = (Elf64_Verdef *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_VERDEFNUM:
            dso->verdef_num = (size_t)dyn[i].d_un.d_val;
            break;
        case DT_VERNEED:
            dso->verneed = (Elf64_Verneed *)zc_va_to_ptr(dso, dyn[i].d_un.d_ptr);
            break;
        case DT_VERNEEDNUM:
            dso->verneed_num = (size_t)dyn[i].d_un.d_val;
            break;
        default:
            break;
        }
    }

    if (dso->strtab) {
        if (rpath_off != (size_t)-1) dso->rpath = dso->strtab + rpath_off;
        if (runpath_off != (size_t)-1) dso->runpath = dso->strtab + runpath_off;
    }

    /* Read section headers if symtab_count is not yet known. */
    if (dso->symtab_count == 0 && fd >= 0 && ehdr->e_shoff > 0 && ehdr->e_shnum > 0) {
        size_t shsz = (size_t)ehdr->e_shnum * sizeof(Elf64_Shdr);
        Elf64_Shdr *shdrs = (Elf64_Shdr *)malloc(shsz);
        if (shdrs) {
            if (lseek(fd, (off_t)ehdr->e_shoff, SEEK_SET) >= 0 &&
                read(fd, shdrs, shsz) == (ssize_t)shsz) {
                for (int i = 0; i < ehdr->e_shnum; i++) {
                    if (shdrs[i].sh_type == SHT_DYNSYM) {
                        dso->symtab_count = shdrs[i].sh_size / sizeof(Elf64_Sym);
                        break;
                    }
                }
            }
            free(shdrs);
        }
    }

    return 0;
}

/* ---- GNU symbol versioning helpers ---- */

static int zc_find_verdef_ndx(ZcDso *dso, const char *version) {
    if (!dso->verdef || !dso->strtab || dso->verdef_num == 0) return 0;
    char *curr = (char *)dso->verdef;
    for (size_t i = 0; i < dso->verdef_num; i++) {
        Elf64_Verdef *vd = (Elf64_Verdef *)curr;
        if (vd->vd_cnt > 0 && vd->vd_aux > 0) {
            Elf64_Verdaux *vda = (Elf64_Verdaux *)(curr + vd->vd_aux);
            const char *vname = dso->strtab + vda->vda_name;
            if (strcmp(vname, version) == 0) {
                return (vd->vd_ndx & 0x7fff);
            }
        }
        if (vd->vd_next == 0) break;
        curr += vd->vd_next;
    }
    return 0;
}

static int zc_find_verneed_ndx(ZcDso *dso, const char *version) {
    if (!dso->verneed || !dso->strtab || dso->verneed_num == 0) return 0;
    char *curr = (char *)dso->verneed;
    for (size_t i = 0; i < dso->verneed_num; i++) {
        Elf64_Verneed *vn = (Elf64_Verneed *)curr;
        char *vna_curr = curr + vn->vn_aux;
        for (Elf64_Half j = 0; j < vn->vn_cnt; j++) {
            Elf64_Vernaux *vna = (Elf64_Vernaux *)vna_curr;
            const char *vname = dso->strtab + vna->vna_name;
            if (strcmp(vname, version) == 0) {
                return (vna->vna_other & 0x7fff);
            }
            if (vna->vna_next == 0) break;
            vna_curr += vna->vna_next;
        }
        if (vn->vn_next == 0) break;
        curr += vn->vn_next;
    }
    return 0;
}

static int zc_find_ver_ndx(ZcDso *dso, const char *version) {
    int ndx = zc_find_verdef_ndx(dso, version);
    if (ndx > 0) return ndx;
    return zc_find_verneed_ndx(dso, version);
}

static const char *zc_get_verneed_name(ZcDso *dso, uint16_t req_v) {
    if (!dso->verneed || !dso->strtab || dso->verneed_num == 0) return NULL;
    char *curr = (char *)dso->verneed;
    for (size_t i = 0; i < dso->verneed_num; i++) {
        Elf64_Verneed *vn = (Elf64_Verneed *)curr;
        char *vna_curr = curr + vn->vn_aux;
        for (Elf64_Half j = 0; j < vn->vn_cnt; j++) {
            Elf64_Vernaux *vna = (Elf64_Vernaux *)vna_curr;
            if ((vna->vna_other & 0x7fff) == req_v) {
                return dso->strtab + vna->vna_name;
            }
            if (vna->vna_next == 0) break;
            vna_curr += vna->vna_next;
        }
        if (vn->vn_next == 0) break;
        curr += vn->vn_next;
    }
    return NULL;
}

static int zc_sym_matches_version(ZcDso *dso, uint32_t sym_idx, int target_ver_ndx, const char *version) {
    if (!dso->versym) {
        return 1;
    }
    uint16_t v = dso->versym[sym_idx];
    if (version == NULL) {
        if (v & 0x8000) return 0;
        if ((v & 0x7fff) == VER_NDX_LOCAL || v == VER_NDX_ELIMINATE) return 0;
        return 1;
    } else {
        if (target_ver_ndx <= 0) return 0;
        return ((v & 0x7fff) == target_ver_ndx);
    }
}

/* ---- symbol lookup ---- */

static Elf64_Sym *zc_dso_lookup_sysv(ZcDso *dso, const char *name, int target_ver_ndx, const char *version) {
    if (!dso->sysv_hash || !dso->symtab || !dso->strtab) return NULL;
    uint32_t nbuckets = dso->sysv_hash[0];
    uint32_t nchains  = dso->sysv_hash[1];
    if (nbuckets == 0) return NULL;
    uint32_t *buckets = dso->sysv_hash + 2;
    uint32_t *chains  = buckets + nbuckets;
    uint32_t h = zc_sysv_hash(name) % nbuckets;
    for (uint32_t idx = buckets[h]; idx != STN_UNDEF && idx < nchains; idx = chains[idx]) {
        Elf64_Sym *sym = &dso->symtab[idx];
        if (strcmp(dso->strtab + sym->st_name, name) == 0 &&
            zc_sym_matches_version(dso, idx, target_ver_ndx, version))
            return sym;
    }
    return NULL;
}

static Elf64_Sym *zc_dso_lookup_gnu(ZcDso *dso, const char *name, int target_ver_ndx, const char *version) {
    if (!dso->gnu_hash || !dso->symtab || !dso->strtab) return NULL;
    uint32_t nbuckets  = dso->gnu_hash[0];
    uint32_t symoffset = dso->gnu_hash[1];
    uint32_t bloom_sz  = dso->gnu_hash[2];
    if (nbuckets == 0) return NULL;

    uint32_t *buckets = dso->gnu_hash + 4 + bloom_sz * (sizeof(uint64_t) / sizeof(uint32_t));
    uint32_t h1 = zc_gnu_hash(name);
    uint32_t idx = buckets[h1 % nbuckets];
    if (!idx || idx < symoffset) return NULL;

    uint32_t *hashval = buckets + nbuckets + (idx - symoffset);
    for (h1 |= 1; ; idx++) {
        uint32_t h2 = *hashval++;
        if (h1 == (h2 | 1)) {
            Elf64_Sym *sym = &dso->symtab[idx];
            if (strcmp(dso->strtab + sym->st_name, name) == 0 &&
                zc_sym_matches_version(dso, idx, target_ver_ndx, version))
                return sym;
        }
        if (h2 & 1) break;
    }
    return NULL;
}

static Elf64_Sym *zc_dso_lookup_linear(ZcDso *dso, const char *name, int target_ver_ndx, const char *version) {
    if (!dso->symtab || !dso->strtab || dso->symtab_count == 0) return NULL;
    for (size_t i = 1; i < dso->symtab_count; i++) {
        Elf64_Sym *sym = &dso->symtab[i];
        if (sym->st_shndx == SHN_UNDEF) continue;
        if (strcmp(dso->strtab + sym->st_name, name) == 0 &&
            zc_sym_matches_version(dso, (uint32_t)i, target_ver_ndx, version))
            return sym;
    }
    return NULL;
}

/* Look up a symbol within a single DSO.  version may be NULL (default). */
static Elf64_Sym *zc_dso_lookup(ZcDso *dso, const char *name, const char *version) {
    if (!dso || !name || !name[0]) return NULL;
    int target_ver_ndx = 0;
    if (version && version[0]) {
        target_ver_ndx = zc_find_ver_ndx(dso, version);
        if (dso->versym && target_ver_ndx <= 0) return NULL;
    }
    if (dso->gnu_hash) {
        Elf64_Sym *s = zc_dso_lookup_gnu(dso, name, target_ver_ndx, version);
        if (s) return s;
    }
    if (dso->sysv_hash) {
        Elf64_Sym *s = zc_dso_lookup_sysv(dso, name, target_ver_ndx, version);
        if (s) return s;
    }
    return zc_dso_lookup_linear(dso, name, target_ver_ndx, version);
}

/* Global symbol search: breadth-first search of the given DSO's dependency
 * graph, then fallback to head (the host executable).
 * Returns 1 if found in a real DSO (fills found_dso/found_sym),
 * 2 if found in head (found_dso = head, found_sym = NULL sentinel), 0 otherwise. */
static int zc_scope_lookup(ZcDso *start, const char *name, const char *version,
                           ZcDso **found_dso, Elf64_Sym **found_sym) {
    if (start && start != g_zc_head) {
        ZcDso *queue[128];
        int qhead = 0, qtail = 0;
        queue[qtail++] = start;

        while (qhead < qtail) {
            ZcDso *curr = queue[qhead++];
            Elf64_Sym *sym = zc_dso_lookup(curr, name, version);
            if (sym && ELF64_ST_TYPE(sym->st_info) != STT_SECTION &&
                sym->st_shndx != SHN_UNDEF) {
                *found_dso = curr;
                *found_sym = sym;
                return 1;
            }
            for (int i = 0; i < curr->dep_count; i++) {
                ZcDso *dep = curr->deps[i];
                if (!dep || dep == g_zc_head) continue;
                int already = 0;
                for (int j = 0; j < qtail; j++) {
                    if (queue[j] == dep) {
                        already = 1;
                        break;
                    }
                }
                if (!already && qtail < 128) {
                    queue[qtail++] = dep;
                }
            }
        }
    }
    if (g_zc_head && zc_head_lookup(name)) {
        *found_dso = g_zc_head;
        *found_sym = NULL;
        return 2;
    }
    return 0;
}

/* ---- relocations ---- */

static int zc_resolve_reloc_sym(ZcDso *dso, Elf64_Xword sym_idx, void **out_addr) {
    *out_addr = NULL;
    if (sym_idx == STN_UNDEF || !dso->symtab) return 0;

    Elf64_Sym *sym = &dso->symtab[sym_idx];
    const char *sname = (dso->strtab && sym->st_name) ? (dso->strtab + sym->st_name) : NULL;

    /* Local symbol with no name (or STB_LOCAL defined in this DSO). */
    if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL && sym->st_shndx != SHN_UNDEF) {
        *out_addr = zc_va_to_ptr(dso, sym->st_value);
        return 0;
    }

    if (sname && sname[0]) {
        const char *req_version = NULL;
        if (dso->versym && dso->verneed && sym_idx < dso->symtab_count) {
            uint16_t req_v = dso->versym[sym_idx] & 0x7fff;
            if (req_v >= 2) {
                req_version = zc_get_verneed_name(dso, req_v);
            }
        }
        ZcDso *fdso = NULL;
        Elf64_Sym *fsym = NULL;
        int r = zc_scope_lookup(dso, sname, req_version, &fdso, &fsym);
        if (r == 1) {
            *out_addr = zc_va_to_ptr(fdso, fsym->st_value);
            return 0;
        }
        if (r == 2) {
            *out_addr = zc_head_lookup(sname);
            return 0;
        }
    }

    /* Defined inside this DSO. */
    if (sym->st_shndx != SHN_UNDEF) {
        *out_addr = zc_va_to_ptr(dso, sym->st_value);
        return 0;
    }

    /* Unresolved weak symbols resolve to 0. */
    if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
        *out_addr = NULL;
        return 0;
    }

    zc_seterr(EINVAL);
    return -1;
}

static int zc_apply_reloc_x86_64(ZcDso *dso, const Elf64_Rela *rela) {
    Elf64_Xword type = ELF64_R_TYPE(rela->r_info);
    Elf64_Xword sym  = ELF64_R_SYM(rela->r_info);
    uint8_t    *fixup = (uint8_t *)zc_va_to_ptr(dso, rela->r_offset);
    void       *sym_addr = NULL;

    switch (type) {
    case R_X86_64_NONE:
        break;
    case R_X86_64_RELATIVE:
        *(uintptr_t *)fixup = (uintptr_t)((char *)dso->base - dso->map_start + rela->r_addend);
        break;
    case R_X86_64_64:
        if (zc_resolve_reloc_sym(dso, sym, &sym_addr) < 0) return -1;
        *(uintptr_t *)fixup = (uintptr_t)sym_addr + (uintptr_t)rela->r_addend;
        break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
        if (zc_resolve_reloc_sym(dso, sym, &sym_addr) < 0) return -1;
        *(uintptr_t *)fixup = (uintptr_t)sym_addr + (uintptr_t)rela->r_addend;
        break;
    default:
        zc_seterr(ENOSYS);
        return -1;
    }
    return 0;
}

static int zc_do_relocs(ZcDso *dso) {
    if (dso->rela && dso->relasz > 0) {
        size_t count = dso->relasz / sizeof(Elf64_Rela);
        for (size_t j = 0; j < count; j++) {
            if (zc_apply_reloc_x86_64(dso, &dso->rela[j]) < 0) return -1;
        }
    }
    if (dso->jmprel && dso->pltrelsz > 0) {
        size_t count = dso->pltrelsz / sizeof(Elf64_Rela);
        for (size_t j = 0; j < count; j++) {
            if (zc_apply_reloc_x86_64(dso, &dso->jmprel[j]) < 0) return -1;
        }
    }
    return 0;
}

/* Run DT_INIT and DT_INIT_ARRAY constructors in dependency order. */
static void zc_run_constructors(ZcDso *dso) {
    if (!dso || dso->constructed) return;
    dso->constructed = 1;
    for (int i = 0; i < dso->dep_count; i++) {
        if (dso->deps[i] != g_zc_head) {
            zc_run_constructors(dso->deps[i]);
        }
    }
    if (dso->init_fn) {
        dso->init_fn();
    }
    if (dso->init_array && dso->init_arraysz > 0) {
        size_t count = dso->init_arraysz / sizeof(void (*)(void));
        for (size_t i = 0; i < count; i++) {
            if (dso->init_array[i]) dso->init_array[i]();
        }
    }
}

static int zc_dso_add_dep(ZcDso *dso, ZcDso *dep) {
    if (!dso || !dep) return 0;
    for (int i = 0; i < dso->dep_count; i++) {
        if (dso->deps[i] == dep) return 0;
    }
    if (dso->dep_count == dso->dep_cap) {
        int ncap = dso->dep_cap ? dso->dep_cap * 2 : 8;
        ZcDso **ndeps = realloc(dso->deps, ncap * sizeof(ZcDso *));
        if (!ndeps) return -1;
        dso->deps = ndeps;
        dso->dep_cap = ncap;
    }
    dso->deps[dso->dep_count++] = dep;
    return 0;
}

static int zc_dso_matches(ZcDso *d, const char *canon_path, const struct stat *st) {
    if (strcmp(d->name, canon_path) == 0) {
        return 1;
    }
    if (d->dev == st->st_dev && d->ino == st->st_ino) {
        /* Ensure d->name still refers to this inode (guards against inode reuse after unlink) */
        struct stat chk;
        if (stat(d->name, &chk) == 0 && chk.st_dev == d->dev && chk.st_ino == d->ino) {
            return 1;
        }
    }
    return 0;
}

/* ---- dlopen internal ---- */

static void *zc_dlopen_internal(const char *filename, ZcDso *caller, int flags) {
    (void)flags;
    if (!g_zc_inited) {
        if (zero_rt_dl_init() < 0) return NULL;
    }

    if (!filename || filename[0] == '\0') {
        return g_zc_head;
    }

    if (zc_is_folded_lib(filename)) {
        return g_zc_head;
    }

    char resolved_path[1024];
    if (zc_resolve_library_path(filename, caller, resolved_path, sizeof(resolved_path)) < 0) {
        zc_seterr(ENOENT);
        return NULL;
    }

    char canon_path[1024];
    if (!realpath(resolved_path, canon_path)) {
        snprintf(canon_path, sizeof(canon_path), "%s", resolved_path);
    }

    struct stat st;
    if (stat(canon_path, &st) < 0) {
        zc_seterr(ENOENT);
        return NULL;
    }

    /* Already loaded? Return existing handle with bumped refcnt. */
    for (ZcDso *d = g_zc_list; d; d = d->next) {
        if (d != g_zc_head && zc_dso_matches(d, canon_path, &st)) {
            d->refcnt++;
            return d;
        }
    }

    int fd = open(canon_path, O_RDONLY);
    if (fd < 0) return NULL;

    uintptr_t ms = 0;
    size_t  sz = 0;
    void   *map = zc_map_segments(fd, &ms, &sz);
    if (!map) {
        close(fd);
        return NULL;
    }

    ZcDso *dso = calloc(1, sizeof(*dso));
    if (!dso) {
        munmap(map, sz);
        close(fd);
        zc_seterr(ENOMEM);
        return NULL;
    }
    dso->name = strdup(canon_path);
    dso->dev = st.st_dev;
    dso->ino = st.st_ino;
    dso->base = map;
    dso->map_start = ms;
    dso->map_len = sz;
    dso->refcnt = 1;

    (void)zc_parse_dynamic(dso, fd, map);
    close(fd);

    /* Insert into global DSO list before loading dependencies to break cycles. */
    zc_dso_list_insert(dso);

    /* Recursively load DT_NEEDED dependencies. */
    for (size_t i = 0; i < dso->needed_count; i++) {
        const char *dep_name = dso->strtab + dso->needed_offsets[i];
        if (zc_is_folded_lib(dep_name)) {
            zc_dso_add_dep(dso, g_zc_head);
            continue;
        }
        ZcDso *dep = zc_dlopen_internal(dep_name, dso, flags);
        if (dep) {
            zc_dso_add_dep(dso, dep);
        }
    }

    /* Apply relocations. */
    if (!dso->relocated) {
        if (zc_do_relocs(dso) < 0) {
            zc_seterr(EINVAL);
            return NULL;
        }
        dso->relocated = 1;
    }

    zc_run_constructors(dso);
    return dso;
}

/* Public dlopen entry point. */
void *zc_dlopen(const char *filename, int flags) {
    return zc_dlopen_internal(filename, NULL, flags);
}

/* ---- dlsym ---- */

void *zc_dlsym(void *handle, const char *name) {
    return zc_dlvsym(handle, name, NULL);
}

/* ---- dlvsym ---- */

void *zc_dlvsym(void *handle, const char *name, const char *version) {
    if (!g_zc_inited || !name || !name[0]) {
        errno = EINVAL;
        return NULL;
    }
    if (!handle) {
        /* Global search across all loaded DSOs. */
        for (ZcDso *d = g_zc_list; d; d = d->next) {
            if (d == g_zc_head) continue;
            Elf64_Sym *sym = zc_dso_lookup(d, name, version);
            if (sym && ELF64_ST_TYPE(sym->st_info) != STT_SECTION &&
                sym->st_shndx != SHN_UNDEF) {
                return zc_va_to_ptr(d, sym->st_value);
            }
        }
        errno = EINVAL;
        return NULL;
    }

    ZcDso *dso = (ZcDso *)handle;
    ZcDso *fdso = NULL;
    Elf64_Sym *fsym = NULL;
    int r = zc_scope_lookup(dso, name, version, &fdso, &fsym);
    if (r == 1) {
        return zc_va_to_ptr(fdso, fsym->st_value);
    }
    if (r == 2) {
        return zc_head_lookup(name);
    }
    errno = EINVAL;
    return NULL;
}

/* ---- dlclose ---- */

/* Decrement refcount; actual unload is not implemented in MVP. */
int zc_dlclose(void *handle) {
    if (!handle || !g_zc_inited) return -1;
    ZcDso *dso = (ZcDso *)handle;
    if (dso == g_zc_head) return 0;
    for (ZcDso *d = g_zc_list; d; d = d->next) {
        if (d == dso) {
            if (dso->refcnt > 0) {
                dso->refcnt--;
                if (dso->refcnt == 0) {
                    for (int i = 0; i < dso->dep_count; i++) {
                        zc_dlclose(dso->deps[i]);
                    }
                }
            }
            return 0;
        }
    }
    return -1;
}

/* ---- bootstrap ---- */

int zero_rt_dl_init(void) {
    if (g_zc_inited) return 0;
    g_zc_head = calloc(1, sizeof(*g_zc_head));
    if (!g_zc_head) return -1;
    g_zc_head->name = strdup("<main executable>");
    g_zc_head->base = (void *)&g_zc_inited; /* sentinel; head has no real base */
    g_zc_head->map_start = 0;
    g_zc_head->refcnt = 1;
    zc_dso_list_insert(g_zc_head);
    g_zc_inited = 1;
    return 0;
}
