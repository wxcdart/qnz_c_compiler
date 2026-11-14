/* zero_rt/dlfcn/dynlink.c — Host-side in-process dynamic loader (Phase 2).
 *
 * ZeroCC's `build/bin/zerocc` and its test binaries are fully statically
 * linked against vendored musl, so there is no system dynamic loader
 * (ld.so) in the process.  musl's static archive ships dlopen/dlsym/dlclose
 * stubs that always fail; zero_rt/dlfcn/dlfcn.c replaces those stubs with a
 * self-hosting implementation that resolves symbols from the executable's
 * own ELF .symtab (pre-baked embedded table + /proc/self/exe fallback).
 *
 * Phase 1 (Sprint 153) covered dlsym on the running process's own symbol
 * namespace.  Phase 2 adds real .so loading: a JITed user program or host
 * binary can call dlopen("/path/to/libfoo.so") and ZeroCC's mini dynamic
 * linker will map the ELF PT_LOAD segments, relocate the image, resolve its
 * symbols (through the DSO chain, then the host executable's own symtab),
 * run constructors, and return an opaque handle.
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

/* ---- DSO representation ---- */

typedef struct zc_dso ZcDso;

struct zc_dso {
    char      *name;          /* .so filename / "<main executable>" */
    void      *base;          /* mmap'd base address of the loaded image */
    uintptr_t  map_start;     /* page-aligned start of the mapping window */
    size_t     map_len;       /* total length of the mmap'd window */
    Elf64_Dyn *dynamic;       /* .dynamic section entries (mmap'd address) */
    size_t     dynamic_count;
    char      *strtab;        /* .dynstr (mmap'd address) */
    Elf64_Sym *symtab;        /* .dynsym (mmap'd address) */
    size_t     symtab_count;
    uint32_t  *sysv_hash;     /* DT_HASH (mmap'd address) */
    uint32_t  *gnu_hash;      /* DT_GNU_HASH (mmap'd address) */
    Elf64_Rela *rela;         /* DT_RELA (.rela.dyn) */
    size_t      relasz;       /* DT_RELASZ (bytes) */
    Elf64_Rela *jmprel;       /* DT_JMPREL (.rela.plt) */
    size_t      pltrelsz;     /* DT_PLTRELSZ (bytes) */
    void      (*init_fn)(void);
    void     (**init_array)(void);
    size_t      init_arraysz;
    ZcDso     *deps[16];      /* DT_NEEDED dependencies (fixed cap for MVP) */
    int        dep_count;
    int        constructed;
    int        refcnt;
    ZcDso     *next;          /* global DSO list linkage */
};

/* ---- globals ---- */

static ZcDso *g_zc_head   = NULL;   /* synthetic DSO for main executable */
static ZcDso *g_zc_list   = NULL;   /* global DSO list head */
static int    g_zc_inited = 0;      /* zero_rt_dl_init() called? */

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

/* ---- dynamic section parsing ---- */

/* Walk .dynamic entries and fill in the DSO fields.  All pointers are
 * mmap'd addresses (computed via zc_va_to_ptr). */
static int zc_parse_dynamic(ZcDso *dso, int fd, void *base) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);

    /* Find PT_DYNAMIC segment to locate .dynamic. */
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
        default:
            break;
        }
    }

    /* Section headers live at file offset ehdr->e_shoff (often outside PT_LOAD
     * segments); read them from fd if symtab_count is not yet known. */
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

/* ---- symbol lookup ---- */

/* SysV hash lookup within a single DSO. */
static Elf64_Sym *zc_dso_lookup_sysv(ZcDso *dso, const char *name) {
    if (!dso->sysv_hash || !dso->symtab || !dso->strtab) return NULL;
    uint32_t nbuckets = dso->sysv_hash[0];
    uint32_t nchains  = dso->sysv_hash[1];
    if (nbuckets == 0) return NULL;
    uint32_t *buckets = dso->sysv_hash + 2;
    uint32_t *chains  = buckets + nbuckets;
    uint32_t h = zc_sysv_hash(name) % nbuckets;
    for (uint32_t idx = buckets[h]; idx != STN_UNDEF && idx < nchains; idx = chains[idx]) {
        Elf64_Sym *sym = &dso->symtab[idx];
        if (strcmp(dso->strtab + sym->st_name, name) == 0)
            return sym;
    }
    return NULL;
}

/* GNU hash lookup within a single DSO (matches musl ldso/dynlink.c). */
static Elf64_Sym *zc_dso_lookup_gnu(ZcDso *dso, const char *name) {
    if (!dso->gnu_hash || !dso->symtab || !dso->strtab) return NULL;
    uint32_t nbuckets  = dso->gnu_hash[0];
    uint32_t symoffset = dso->gnu_hash[1];
    uint32_t bloom_sz  = dso->gnu_hash[2];
    if (nbuckets == 0) return NULL;

    /* Skip 4 header words + bloom_sz 64-bit words (2 uint32_t per 64-bit word). */
    uint32_t *buckets = dso->gnu_hash + 4 + bloom_sz * (sizeof(uint64_t) / sizeof(uint32_t));
    uint32_t h1 = zc_gnu_hash(name);
    uint32_t idx = buckets[h1 % nbuckets];
    if (!idx || idx < symoffset) return NULL;

    uint32_t *hashval = buckets + nbuckets + (idx - symoffset);
    for (h1 |= 1; ; idx++) {
        uint32_t h2 = *hashval++;
        if (h1 == (h2 | 1)) {
            Elf64_Sym *sym = &dso->symtab[idx];
            if (strcmp(dso->strtab + sym->st_name, name) == 0)
                return sym;
        }
        if (h2 & 1) break;
    }
    return NULL;
}

/* Linear scan fallback when symtab_count is known. */
static Elf64_Sym *zc_dso_lookup_linear(ZcDso *dso, const char *name) {
    if (!dso->symtab || !dso->strtab || dso->symtab_count == 0) return NULL;
    for (size_t i = 1; i < dso->symtab_count; i++) {
        Elf64_Sym *sym = &dso->symtab[i];
        if (sym->st_shndx == SHN_UNDEF) continue;
        if (strcmp(dso->strtab + sym->st_name, name) == 0)
            return sym;
    }
    return NULL;
}

/* Look up a symbol within a single DSO. */
static Elf64_Sym *zc_dso_lookup(ZcDso *dso, const char *name) {
    if (!dso || !name || !name[0]) return NULL;
    if (dso->gnu_hash) {
        Elf64_Sym *s = zc_dso_lookup_gnu(dso, name);
        if (s) return s;
    }
    if (dso->sysv_hash) {
        Elf64_Sym *s = zc_dso_lookup_sysv(dso, name);
        if (s) return s;
    }
    return zc_dso_lookup_linear(dso, name);
}

/* Global symbol search: the given DSO's scope (itself, then its deps, then
 * head).  Returns 1 if found in a real DSO (fills found_dso/found_sym),
 * 2 if found in head (found_dso = head, found_sym = NULL sentinel), 0 otherwise. */
static int zc_scope_lookup(ZcDso *start, const char *name,
                           ZcDso **found_dso, Elf64_Sym **found_sym) {
    if (start && start != g_zc_head) {
        Elf64_Sym *sym = zc_dso_lookup(start, name);
        if (sym && ELF64_ST_TYPE(sym->st_info) != STT_SECTION &&
            sym->st_shndx != SHN_UNDEF) {
            *found_dso = start;
            *found_sym = sym;
            return 1;
        }
        for (int i = 0; i < start->dep_count; i++) {
            sym = zc_dso_lookup(start->deps[i], name);
            if (sym && ELF64_ST_TYPE(sym->st_info) != STT_SECTION &&
                sym->st_shndx != SHN_UNDEF) {
                *found_dso = start->deps[i];
                *found_sym = sym;
                return 1;
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

/* Resolve a relocation's symbol index `sym` in `dso`.  Sets *out_addr and
 * returns 0 on success (including unresolved weak symbols, where *out_addr
 * is NULL), or -1 on an unresolved strong symbol. */
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
        ZcDso *fdso = NULL;
        Elf64_Sym *fsym = NULL;
        int r = zc_scope_lookup(dso, sname, &fdso, &fsym);
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

    /* Unresolved weak symbols resolve to 0 (e.g. __cxa_finalize, __gmon_start__). */
    if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
        *out_addr = NULL;
        return 0;
    }

    zc_seterr(EINVAL);
    return -1;
}

/* Apply a single RELA relocation for x86-64. */
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

/* Process all RELA relocations (.rela.dyn + .rela.plt) from .dynamic. */
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

/* Run DT_INIT and DT_INIT_ARRAY constructors once. */
static void zc_run_constructors(ZcDso *dso) {
    if (dso->constructed) return;
    dso->constructed = 1;
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

/* ---- dlopen ---- */

/* Public: load a shared object.  filename may be:
 *   - NULL or empty        -> the main executable namespace (head)
 *   - a path (/abs/foo.so or ./foo.so) -> load directly
 *   - a bare name (libfoo.so) -> not supported in MVP (no search path)
 * Returns an opaque handle (ZcDso *) or NULL with errno/dlerror set. */
void *zc_dlopen(const char *filename, int flags) {
    (void)flags;
    if (!g_zc_inited) {
        if (zero_rt_dl_init() < 0) return NULL;
    }

    if (!filename || filename[0] == '\0') {
        return g_zc_head;
    }

    /* Already loaded? Return existing handle with bumped refcnt. */
    for (ZcDso *d = g_zc_list; d; d = d->next) {
        if (d != g_zc_head && strcmp(d->name, filename) == 0) {
            d->refcnt++;
            return d;
        }
    }

    /* Only paths with '/' are supported in MVP. */
    if (strchr(filename, '/') == NULL) {
        errno = ENOENT;
        return NULL;
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) return NULL;

    uintptr_t ms = 0;
    size_t  sz = 0;
    void   *map = zc_map_segments(fd, &ms, &sz);
    if (!map) {
        close(fd);
        return NULL;  /* errno set by zc_map_segments */
    }

    ZcDso *dso = calloc(1, sizeof(*dso));
    if (!dso) {
        munmap(map, sz);
        close(fd);
        zc_seterr(ENOMEM);
        return NULL;
    }
    dso->name = strdup(filename);
    dso->base = map;
    dso->map_start = ms;
    dso->map_len = sz;
    dso->refcnt = 1;

    (void)zc_parse_dynamic(dso, fd, map);
    close(fd);

    if (zc_do_relocs(dso) < 0) {
        free(dso->name);
        munmap(map, sz);
        free(dso);
        zc_seterr(EINVAL);
        return NULL;
    }

    zc_dso_list_insert(dso);
    zc_run_constructors(dso);
    return dso;
}

/* ---- dlsym ---- */

void *zc_dlsym(void *handle, const char *name) {
    if (!g_zc_inited || !name || !name[0]) {
        errno = EINVAL;
        return NULL;
    }
    if (!handle) {
        /* Global search across all loaded DSOs. */
        for (ZcDso *d = g_zc_list; d; d = d->next) {
            if (d == g_zc_head) continue;
            Elf64_Sym *sym = zc_dso_lookup(d, name);
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
    int r = zc_scope_lookup(dso, name, &fdso, &fsym);
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
            if (dso->refcnt > 0) dso->refcnt--;
            return 0;
        }
    }
    return -1;
}

/* ---- bootstrap ----
 *
 * zero_rt_dl_init creates the synthetic head DSO and marks the subsystem
 * initialized.  Called lazily on first use, or explicitly.  Returns 0. */
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
