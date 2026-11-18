/* linker.c - ZeroCC's own static linker (T2): x86-64 and AArch64.
 *
 * Pipeline (see linker.h):
 *   1. load input objects (all must share one e_machine)
 *   2. resolve global symbols (builds the name table incl. UNDs)
 *   3. extract archive members that resolve UNDs (greedy to fixpoint)
 *   4. re-resolve (duplicate-definition check now covers archive members)
 *   5. lay out .text/.rodata/.data/.bss at the conventional static base
 *   6. apply relocations (per machine):
 *        x86-64:  R_X86_64_{64,PC32,PLT32,32,32S,RELATIVE}
 *        aarch64: R_AARCH64_{ABS64,ABS32,ADR_PREL_PG_HI21,
 *                       ADD_ABS_LO12_NC,CALL26}
 *   7. emit ET_EXEC with 2 PT_LOAD segments (RX, RW)
 *
 * Layout (Linux non-PIE convention):
 *   x86-64:  0x400000 ELF header + phdrs; 0x401000 .text
 *   aarch64: 0x000000 header; 0x400000 .text (gcc -static convention)
 *   .rodata 8-aligned after .text; .data/.bss page-aligned (RW segment).
 */
#include "zero_cc/linker/linker.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "zero_cc/linker/elf_reader.h"
#include "zero_cc/linker/ar_reader.h"
#include "zero_cc/linker/linker_script.h"

#define ZC_MAX_INPUTS 2048
#define ZC_MAX_SYMS 65536
#define ZC_MAX_NAME 512

#define ZC_TEXT_BASE 0x401000ULL
#define ZC_TEXT_BASE_AARCH32 0x8000ULL
#define ZC_PAGE 0x1000ULL

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define ET_EXEC 2
#define ET_DYN 3
/* Static-PIE self-relocating _start stub (x86-64; see the emission in
 * emit_exec).  The first PT_LOAD is emitted RWX so the stub can execute
 * and write while applying the .rela.dyn entries; the final mprotect
 * (R+X) restores the intended protections before the target is called.
 *  Each entry is an absolute write - the placeholder sites are zero, so
 *  the site must end up as base + addend (not 0 + addend). */
#define ZC_PIE_STUB_SIZE 95
#define EM_X86_64 62
#define EM_AARCH64 183
#define EM_ARM 40        /* Sprint 185: AArch32 (ARMv7-A) */
#define EM_RISCV 243
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ZC_SHN_ABS 0xfff1
#define ZC_SHN_COMMON 0xfff2

#define ZC_STB_LOCAL 0
#define ZC_STB_GLOBAL 1
#define ZC_STB_WEAK 2
#define ZC_STT_OBJECT 1
#define ZC_STT_FUNC 2
#define ZC_STT_SECTION 3

typedef struct {
    char name[ZC_MAX_NAME];
    int obj;            /* owning object index, -1 = undefined/common */
    int sym;            /* symbol index in the owning object's symtab */
    uint64_t vma;       /* final value (filled during layout) */
    bool is_weak;
    bool is_common;
    bool is_referenced; /* referenced by a live section or root */
    bool is_section;    /* section symbol: vma follows its output section */
    uint64_t sec_off;   /* for section symbols: offset within the section */
    uint64_t size;
    uint8_t stype;      /* STT for the output symtab */
} ZcLinkSym;

typedef struct {
    uint64_t r_offset; /* in-image offset of the 8-byte site (base 0) */
    uint64_t r_addend; /* loaded value = image_base + r_addend */
} ZcRelaDyn;

typedef struct {
    char name[16];
    bool write, exec, nobits;
    uint64_t vma;
    uint64_t lma;
    uint64_t file_off;
    uint64_t size;      /* content size written to file (bss: 0) */
    uint64_t memsz;     /* .bss virtual size */
    uint64_t cursor;
    uint64_t max_align;
    uint8_t *content;
    size_t cap;
} ZcOutSec;

typedef struct {
    ZcElfObj *obj;
    uint32_t insec;
    uint64_t out_off;   /* offset of this input section in the out section */
} ZcInsecMap;

typedef struct ZcLink {
    ZcLinkSym *syms;
    size_t sym_count, sym_cap;
    int32_t *sym_hash;
    size_t hash_cap;

    ZcElfObj *objs[ZC_MAX_INPUTS];
    int obj_count;

    uint16_t machine; /* e_machine of the inputs (all must agree) */
    bool is_32;       /* true if 32-bit ELF (ELFCLASS32, e.g. riscv32) */

    ZcOutSec outsec[4]; /* .text .rodata .data .bss */
    ZcInsecMap *map;
    size_t map_count, map_cap;

    bool have_bss;
    bool stub_at_front; /* linker-generated _start stub in .text[0..16) */
    /* The stub's call target: "zeroc_main_wrapper" when zero_rt was linked
     * (runs atexit on a plain `return` from main), else "main". A driver
     * request (entry_wrapper, e.g. zero_rt's "zeroc_leak_main_wrapper" for
     * -fsanitize=leak) wins when that symbol is in the image. */
    const char *stub_target;
    const char *entry_wrapper; /* driver-requested stub target (or NULL) */
    /* x86-64 far-rel32 thunk pool (detour-style): reserved at the end of
     * .text during layout, 14 bytes per slot -
     *   ff 25 00 00 00 00   jmp [rip+0]
     *   <8-byte absolute target>
     * A 32-bit relative relocation (call/jmp/rip-relative data access) whose
     * final delta is outside +-2GB is re-aimed at a pool slot, which is
     * always in range as long as .text itself stays under 2GB.  Unclaimed
     * slots remain zero and are never executed. */
    uint64_t text_thunk_base; /* offset within .text where the pool starts */
    size_t text_thunk_used;
    /* Static-PIE (ET_DYN) link: sections are laid out from base 0, every
     * R_X86_64_64 becomes an R_X86_64_RELATIVE entry in .rela.dyn, and the
     * linker-generated _start stub applies them at startup (musl 1.2.6 has
     * no static-PIE self-relocation of its own). */
    bool pie;
    bool baremetal;
    bool gc_sections;
    bool print_gc_sections;
    uint8_t *sec_live[ZC_MAX_INPUTS];
    ZcLinkScript script;
    bool has_script;
    ZcRelaDyn *rela_dyn;
    size_t rela_dyn_count, rela_dyn_cap;
    size_t rela_slots;       /* reserved .rela.dyn slots (upper bound) */
    uint64_t rela_dyn_vma, rela_dyn_size; /* 0 = no .rela.dyn section */
} ZcLink;

#include "zero_cc/linker/linker_reloc.h"

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static bool is_power_of_two_64(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---------------- symbol table ---------------- */

static inline uint32_t str_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return h;
}

static ZcLinkSym *sym_find(ZcLink *L, const char *name) {
    if (!L->sym_hash || L->hash_cap == 0) return NULL;
    uint32_t mask = (uint32_t)(L->hash_cap - 1);
    uint32_t idx = str_hash(name) & mask;
    while (L->sym_hash[idx] != -1) {
        int32_t si = L->sym_hash[idx];
        if (strcmp(L->syms[si].name, name) == 0) return &L->syms[si];
        idx = (idx + 1) & mask;
    }
    return NULL;
}

static int sym_hash_grow(ZcLink *L) {
    size_t new_cap = L->hash_cap ? L->hash_cap * 2 : 1024;
    int32_t *new_hash = malloc(new_cap * sizeof(int32_t));
    if (!new_hash) return -1;
    memset(new_hash, -1, new_cap * sizeof(int32_t));
    uint32_t mask = (uint32_t)(new_cap - 1);
    for (size_t i = 0; i < L->sym_count; i++) {
        uint32_t idx = str_hash(L->syms[i].name) & mask;
        while (new_hash[idx] != -1) {
            idx = (idx + 1) & mask;
        }
        new_hash[idx] = (int32_t)i;
    }
    free(L->sym_hash);
    L->sym_hash = new_hash;
    L->hash_cap = new_cap;
    return 0;
}

static ZcLinkSym *sym_get(ZcLink *L, const char *name) {
    ZcLinkSym *found = sym_find(L, name);
    if (found) return found;

    if (L->sym_count >= L->sym_cap) {
        if (L->sym_cap >= ZC_MAX_SYMS) return NULL;
        size_t nc = L->sym_cap ? L->sym_cap * 2 : 1024;
        if (nc > ZC_MAX_SYMS) nc = ZC_MAX_SYMS;
        ZcLinkSym *n = realloc(L->syms, nc * sizeof(ZcLinkSym));
        if (!n) return NULL;
        L->syms = n;
        L->sym_cap = nc;
    }
    if (L->hash_cap == 0 || (L->sym_count + 1) * 10 >= L->hash_cap * 7) {
        if (sym_hash_grow(L) != 0) return NULL;
    }
    uint32_t si = (uint32_t)L->sym_count++;
    ZcLinkSym *s = &L->syms[si];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, ZC_MAX_NAME, "%s", name);
    s->obj = -1;
    s->sym = -1;

    uint32_t mask = (uint32_t)(L->hash_cap - 1);
    uint32_t idx = str_hash(name) & mask;
    while (L->sym_hash[idx] != -1) {
        idx = (idx + 1) & mask;
    }
    L->sym_hash[idx] = (int32_t)si;
    return s;
}

/* ---------------- input loading ---------------- */

static int add_object(ZcLink *L, const uint8_t *data, size_t size,
                      const char *path, char *err, size_t errsz) {
    if (L->obj_count >= ZC_MAX_INPUTS) {
        snprintf(err, errsz, "too many input objects (%d)", L->obj_count);
        return -1;
    }
    ZcElfObj *o = calloc(1, sizeof(ZcElfObj));
    if (!o) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    if (zc_elf_obj_load_mem(data, size, path, o, err, errsz) != 0) {
        free(o);
        return -1;
    }
    if (L->machine == 0) {
        L->machine = o->machine;
        L->is_32 = o->is_32;
    } else if (L->machine != o->machine || L->is_32 != o->is_32) {
        snprintf(err, errsz, "%s: mixed input architectures "
                 "(e_machine %u/is_32 %d vs %u/is_32 %d); link one ISA at a time",
                 path, (unsigned)o->machine, (int)o->is_32,
                 (unsigned)L->machine, (int)L->is_32);
        zc_elf_obj_free(o);
        free(o);
        return -1;
    }
    L->objs[L->obj_count++] = o;
    return 0;
}

static int load_input_file(ZcLink *L, const char *path, char *err,
                           size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "cannot seek %s", path);
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        snprintf(err, errsz, "cannot size %s", path);
        fclose(f);
        return -1;
    }
    rewind(f);
    uint8_t *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        snprintf(err, errsz, "out of memory");
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, errsz, "short read on %s", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    int rc = add_object(L, buf, (size_t)sz, path, err, errsz);
    free(buf);
    return rc;
}

static bool has_defined_sym(ZcLink *L, const char *name) {
    if (!name) return false;
    ZcLinkSym *ls = sym_find(L, name);
    return ls && ls->obj >= 0;
}

/* ---------------- archive extraction ---------------- */

static bool member_resolves_need(ZcLink *L, ZcElfObj *m) {
    const ZcSection *st = zc_elf_symtab(m);
    if (!st) return false;
    for (size_t i = 1; i < st->sym_count; i++) {
        const ZcSymbol *s = &st->syms[i];
        if (s->bind == 0 || s->name[0] == '\0' || s->shndx == 0) continue;
        ZcLinkSym *ls = sym_find(L, s->name);
        if (ls && ls->obj == -1 && !ls->is_common) return true;
        if (ls && ls->obj != -1 && ls->is_weak && s->bind != ZC_STB_WEAK) return true;
    }
    return false;
}

/* True if the member defines `name` (a global symbol with a section). */
static bool member_defines(ZcElfObj *m, const char *name) {
    const ZcSection *st = zc_elf_symtab(m);
    if (!st) return false;
    for (size_t i = 1; i < st->sym_count; i++) {
        const ZcSymbol *s = &st->syms[i];
        if (s->bind != 0 && s->shndx != 0 && strcmp(s->name, name) == 0)
            return true;
    }
    return false;
}

/* Forward declaration for use in extract_archives. */
static int resolve_symbols(ZcLink *L, char *err, size_t errsz);

/* zero_rt's entry wrapper: force-extracted so the generated _start stub can
 * route main through exit() (atexit support) whenever zero_rt is linked.
 * NULL when no archive provides it (plain programs keep the direct call).
 * A driver-requested entry wrapper (e.g. the -fsanitize=leak variant) takes
 * its place as the force-extract probe when given. */
#define RT_ENTRY_WRAPPER "zeroc_main_wrapper"

static int extract_archives(ZcLink *L, const char *const *archives,
                            int archive_count, char *err, size_t errsz) {
    if (archive_count <= 0) return 0;
    ZcAr *ar = calloc((size_t)archive_count, sizeof(ZcAr));
    if (!ar) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    int ar_count = 0;
    int rc = -1;
    for (int i = 0; i < archive_count; i++) {
        if (zc_ar_open(archives[i], &ar[ar_count], err, errsz) != 0) goto cleanup;
        ar_count++;
    }

    const void *extracted[ZC_AR_MAX_MEMBERS];
    int extracted_count = 0;

    /* Greedy to fixpoint (classic archive algorithm; rescans handle members
     * that define symbols made undefined by earlier extractions). */
    bool changed = true;
    int rounds = 0;
    while (changed) {
        changed = false;
        if (++rounds > 100) {
            snprintf(err, errsz, "archive extraction did not converge");
            goto cleanup;
        }
        for (int a = 0; a < ar_count; a++) {
            for (size_t m = 0; m < ar[a].member_count; m++) {
                const ZcArMember *mem = &ar[a].members[m];
                bool in = false;
                for (int k = 0; k < extracted_count; k++) {
                    if (extracted[k] == mem->data) { in = true; break; }
                }
                ZcElfObj probe;
                if (zc_elf_obj_load_mem(mem->data, mem->size, mem->name,
                                        &probe, err, errsz) != 0) {
                    continue; /* not an ELF (index etc.) - skip */
                }
                /* Force-extract the member defining the (requested or
                 * default) entry wrapper so the stub can route main through
                 * it. The requested name is preferred; the default stays in
                 * the probe so the requested member (same archive member in
                 * practice) also covers the fallback symbol. */
                bool baremetal_entry = L->baremetal &&
                    !has_defined_sym(L, "Reset_Handler") &&
                    !has_defined_sym(L, "_start") &&
                    (!L->has_script || !L->script.has_entry || !has_defined_sym(L, L->script.entry)) &&
                    (member_defines(&probe, "Reset_Handler") || member_defines(&probe, "_start") ||
                     (L->has_script && L->script.has_entry && member_defines(&probe, L->script.entry)));
                bool take = member_resolves_need(L, &probe) ||
                            (!L->baremetal && member_defines(&probe, RT_ENTRY_WRAPPER)) ||
                            (L->entry_wrapper &&
                             member_defines(&probe, L->entry_wrapper)) ||
                            baremetal_entry;
                zc_elf_obj_free(&probe);
                if (take && !in) {
                    char mpath[600];
                    snprintf(mpath, sizeof(mpath), "%s(%s)",
                             ar[a].path, mem->name);
                    if (add_object(L, mem->data, mem->size, mpath,
                                   err, errsz) != 0) {
                        goto cleanup;
                    }
                    if (extracted_count < ZC_AR_MAX_MEMBERS)
                        extracted[extracted_count++] = mem->data;
                    changed = true;
                }
            }
        }
        if (changed) {
            /* Re-resolve symbols so newly extracted objects' undefined
             * refs are visible to subsequent archive scans. */
            if (resolve_symbols(L, err, errsz) != 0)
                goto cleanup;
        }
    }
    rc = 0;

cleanup:
    for (int a = 0; a < ar_count; a++) zc_ar_free(&ar[a]);
    free(ar);
    return rc;
}

/* ---------------- symbol resolution ---------------- */

static int resolve_symbols(ZcLink *L, char *err, size_t errsz) {
    for (int oi = 0; oi < L->obj_count; oi++) {
        const ZcSection *st = zc_elf_symtab(L->objs[oi]);
        if (!st) continue;
        for (size_t i = 0; i < st->sym_count; i++) {
            const ZcSymbol *s = &st->syms[i];
            if (s->name[0] == '\0' || s->bind == 0) continue;
            ZcLinkSym *ls = sym_get(L, s->name);
            if (!ls) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
            if (s->shndx == 0) continue; /* UND: registered, nothing more */
            if (ls->obj == oi && ls->sym == (int)i) {
                continue; /* already handled by an earlier resolve pass */
            }

            if (s->type == ZC_STT_SECTION) {
                if (ls->obj != -1 || ls->is_common) continue; /* def wins */
                if (s->shndx >= L->objs[oi]->shnum) continue;
                ls->is_section = true;
                ls->obj = oi;
                ls->sym = (int)i;
                ls->sec_off = s->value;
                ls->is_weak = false;
                ls->stype = ZC_STT_SECTION;
                continue;
            }
            if (s->shndx == ZC_SHN_COMMON) {
                if (ls->obj != -1 && !ls->is_section) continue; /* strong wins */
                if (ls->is_section) ls->is_section = false;
                if (!ls->is_common) {
                    ls->is_common = true;
                    ls->size = s->size;
                } else if (s->size > ls->size) {
                    ls->size = s->size;
                }
                continue;
            }
            /* Defined in a section. */
            bool is_weak = (s->bind == ZC_STB_WEAK);
            if (ls->obj == -1 && !ls->is_common) {
                ls->obj = oi;
                ls->sym = (int)i;
                ls->is_weak = is_weak;
                ls->is_section = false;
                ls->size = s->size;
                ls->stype = s->type;
            } else if (ls->is_common) {
                /* Strong definition materializes the common. */
                ls->is_common = false;
                ls->obj = oi;
                ls->sym = (int)i;
                ls->is_weak = is_weak;
                ls->size = s->size;
                ls->stype = s->type;
            } else {
                if (!ls->is_weak && !is_weak) {
                    snprintf(err, errsz,
                             "multiple definition of `%s': %s and %s",
                             s->name, L->objs[ls->obj]->path,
                             L->objs[oi]->path);
                    return -1;
                }
                if (ls->is_weak && !is_weak) {
                    ls->obj = oi;
                    ls->sym = (int)i;
                    ls->is_weak = false;
                    ls->size = s->size;
                    ls->stype = s->type;
                }
            }
        }
    }
    return 0;
}

/* ---------------- layout ---------------- */

static int outsec_add(ZcOutSec *s, const uint8_t *data, size_t n) {
    if (s->cursor + n > s->cap) {
        size_t nc = s->cap ? s->cap : 256;
        while (nc < s->cursor + n) nc *= 2;
        uint8_t *nb = realloc(s->content, nc);
        if (!nb) return -1;
        s->content = nb;
        s->cap = nc;
    }
    memcpy(s->content + s->cursor, data, n);
    s->cursor += n;
    return 0;
}

/* Extend a section's content with zeros (common-symbol allocation). */
static int outsec_zero_extend(ZcOutSec *s, uint64_t new_cursor) {
    if (new_cursor <= s->cap) {
        memset(s->content + s->cursor, 0, (size_t)(new_cursor - s->cursor));
        s->cursor = new_cursor;
        return 0;
    }
    size_t nc = s->cap ? s->cap : 256;
    while (nc < new_cursor) nc *= 2;
    uint8_t *nb = realloc(s->content, nc);
    if (!nb) return -1;
    memset(nb + s->cursor, 0, (size_t)(new_cursor - s->cursor));
    s->content = nb;
    s->cap = nc;
    s->cursor = new_cursor;
    return 0;
}

/* Map an input alloc-section name to an output section index
 * (0=.text 1=.rodata 2=.data 3=.bss), or -1 if unmapped. Exact names first,
 * then the gcc-style family (.text.*, .rodata.str1.1, .data.rel.ro, ...). */
static int outsec_for_name(const char *name) {
    if (strcmp(name, ".text") == 0) return 0;
    if (strcmp(name, ".rodata") == 0) return 1;
    if (strcmp(name, ".data") == 0) return 2;
    if (strcmp(name, ".bss") == 0) return 3;
    if (strncmp(name, ".text", 5) == 0) return 0;
    if (strncmp(name, ".rodata", 7) == 0) return 1;
    if (strncmp(name, ".data", 5) == 0) return 2;
    if (strncmp(name, ".bss", 4) == 0) return 3;
    if (strncmp(name, ".isr_vector", 11) == 0) return 0;
    if (strncmp(name, ".vectors", 8) == 0) return 0;
    return -1;
}

static int outsec_for_section(const ZcLink *L, const ZcSection *sec) {
    int out = outsec_for_name(sec->name);
    if (out >= 0) return out;
    if (L && L->has_script) {
        if (sec->flags & SHF_EXECINSTR) return 0;
        if (sec->flags & SHF_WRITE) return 2;
        return 1;
    }
    return -1;
}

/* ---------------- dead section elimination (--gc-sections) ---------------- */

typedef struct {
    int obj;
    int sec;
} ZcGcWorkItem;

static bool zc_pattern_match(const char *pat, const char *str) {
    if (!pat || !str) return false;
    if (strcmp(pat, "*") == 0) return true;
    size_t plen = strlen(pat);
    if (plen >= 2 && pat[0] == '*' && pat[plen - 1] == '*') {
        char sub[64];
        if (plen - 2 < sizeof(sub)) {
            memcpy(sub, pat + 1, plen - 2);
            sub[plen - 2] = '\0';
            return strstr(str, sub) != NULL;
        }
    }
    if (plen > 1 && pat[plen - 1] == '*') {
        return strncmp(pat, str, plen - 1) == 0;
    }
    if (plen > 1 && pat[0] == '*') {
        size_t slen = strlen(str);
        if (slen < plen - 1) return false;
        return strcmp(str + (slen - (plen - 1)), pat + 1) == 0;
    }
    return strcmp(pat, str) == 0;
}

static int gc_mark_and_sweep(ZcLink *L, char *err, size_t errsz) {
    for (int oi = 0; oi < L->obj_count; oi++) {
        size_t n = L->objs[oi]->shnum ? (size_t)L->objs[oi]->shnum : 1;
        L->sec_live[oi] = calloc(n, sizeof(uint8_t));
        if (!L->sec_live[oi]) {
            snprintf(err, errsz, "out of memory in --gc-sections");
            return -1;
        }
    }

    size_t q_cap = 256;
    size_t q_head = 0, q_tail = 0;
    ZcGcWorkItem *q = malloc(q_cap * sizeof(ZcGcWorkItem));
    if (!q) {
        snprintf(err, errsz, "out of memory in --gc-sections");
        return -1;
    }

    #define GC_ENQUEUE(o_idx, s_idx) do { \
        int _oi = (o_idx); int _si = (s_idx); \
        if (_oi >= 0 && _oi < L->obj_count && _si > 0 && _si < L->objs[_oi]->shnum) { \
            if (!L->sec_live[_oi][_si]) { \
                L->sec_live[_oi][_si] = 1; \
                if (q_tail >= q_cap) { \
                    size_t _nc = q_cap * 2; \
                    ZcGcWorkItem *_nq = realloc(q, _nc * sizeof(ZcGcWorkItem)); \
                    if (!_nq) { free(q); snprintf(err, errsz, "out of memory"); return -1; } \
                    q = _nq; q_cap = _nc; \
                } \
                q[q_tail].obj = _oi; \
                q[q_tail].sec = _si; \
                q_tail++; \
            } \
        } \
    } while (0)

    #define GC_ROOT_SYM(symname) do { \
        const char *_sname = (symname); \
        if (_sname) { \
            ZcLinkSym *_ls = sym_find(L, _sname); \
            if (_ls) { \
                _ls->is_referenced = true; \
                if (_ls->obj >= 0 && _ls->sym >= 0) { \
                    ZcElfObj *_o = L->objs[_ls->obj]; \
                    if (_o->sec_symtab >= 0) { \
                        const ZcSection *_st = &_o->secs[_o->sec_symtab]; \
                        if ((size_t)_ls->sym < _st->sym_count) { \
                            uint16_t _shndx = _st->syms[_ls->sym].shndx; \
                            if (_shndx > 0 && _shndx < _o->shnum) { \
                                GC_ENQUEUE(_ls->obj, _shndx); \
                            } \
                        } \
                    } \
                } \
            } \
        } \
    } while (0)

    /* 1. Mark entry points */
    if (L->has_script && L->script.has_entry) {
        GC_ROOT_SYM(L->script.entry);
    }
    GC_ROOT_SYM("_start");
    GC_ROOT_SYM("Reset_Handler");
    GC_ROOT_SYM("main");
    if (L->entry_wrapper) {
        GC_ROOT_SYM(L->entry_wrapper);
    }
    GC_ROOT_SYM(RT_ENTRY_WRAPPER);
    GC_ROOT_SYM("zeroc_leak_main_wrapper");

    /* 2. Mark non-discardable / KEEP sections */
    for (int oi = 0; oi < L->obj_count; oi++) {
        ZcElfObj *o = L->objs[oi];
        for (int si = 1; si < o->shnum; si++) {
            const ZcSection *sec = &o->secs[si];
            if (!(sec->flags & SHF_ALLOC)) continue;
            if (sec->type != SHT_PROGBITS && sec->type != SHT_NOBITS) continue;

            bool keep = false;
            if (strncmp(sec->name, ".init", 5) == 0 ||
                strncmp(sec->name, ".fini", 5) == 0 ||
                strncmp(sec->name, ".ctors", 6) == 0 ||
                strncmp(sec->name, ".dtors", 6) == 0 ||
                strncmp(sec->name, ".note", 5) == 0 ||
                strncmp(sec->name, ".jcr", 4) == 0 ||
                strncmp(sec->name, ".isr_vector", 11) == 0 ||
                strncmp(sec->name, ".vectors", 8) == 0) {
                keep = true;
            }
            if (!keep && L->has_script) {
                for (int k = 0; k < L->script.keep_count; k++) {
                    if (zc_pattern_match(L->script.keep_patterns[k], sec->name)) {
                        keep = true;
                        break;
                    }
                }
            }
            if (keep) {
                GC_ENQUEUE(oi, si);
            }
        }
    }

    /* 3. Mark & sweep loop: process worklist */
    while (q_head < q_tail) {
        int cur_obj = q[q_head].obj;
        int cur_sec = q[q_head].sec;
        q_head++;

        ZcElfObj *o = L->objs[cur_obj];
        for (int ri = 0; ri < o->shnum; ri++) {
            const ZcSection *rs = &o->secs[ri];
            if (rs->type != SHT_RELA || rs->info != (uint32_t)cur_sec) continue;
            const ZcSection *st = zc_elf_symtab(o);
            if (!st) continue;

            for (size_t r = 0; r < rs->reloc_count; r++) {
                const ZcReloc *rel = &rs->relocs[r];
                if (rel->sym >= st->sym_count) continue;
                const ZcSymbol *sym = &st->syms[rel->sym];

                if (sym->shndx == 0) {
                    /* Undefined symbol in this object: resolve globally */
                    ZcLinkSym *ls = sym_find(L, sym->name);
                    if (ls) {
                        ls->is_referenced = true;
                        if (ls->obj >= 0 && ls->sym >= 0) {
                            ZcElfObj *tgt = L->objs[ls->obj];
                            if (tgt->sec_symtab >= 0) {
                                const ZcSection *tst = &tgt->secs[tgt->sec_symtab];
                                if ((size_t)ls->sym < tst->sym_count) {
                                    uint16_t tshndx = tst->syms[ls->sym].shndx;
                                    if (tshndx > 0 && tshndx < tgt->shnum) {
                                        GC_ENQUEUE(ls->obj, tshndx);
                                    }
                                }
                            }
                        }
                    }
                } else if (sym->shndx == ZC_SHN_COMMON) {
                    ZcLinkSym *ls = sym_find(L, sym->name);
                    if (ls) ls->is_referenced = true;
                } else if (sym->bind == ZC_STB_LOCAL || sym->type == ZC_STT_SECTION) {
                    /* Local symbol or section symbol: targets section in cur_obj */
                    if (sym->shndx > 0 && sym->shndx < o->shnum) {
                        GC_ENQUEUE(cur_obj, sym->shndx);
                    }
                } else {
                    /* Defined non-local symbol: could be overridden or in cur_obj */
                    ZcLinkSym *ls = sym_find(L, sym->name);
                    if (ls) {
                        ls->is_referenced = true;
                        if (ls->obj >= 0 && ls->sym >= 0) {
                            ZcElfObj *tgt = L->objs[ls->obj];
                            if (tgt->sec_symtab >= 0) {
                                const ZcSection *tst = &tgt->secs[tgt->sec_symtab];
                                if ((size_t)ls->sym < tst->sym_count) {
                                    uint16_t tshndx = tst->syms[ls->sym].shndx;
                                    if (tshndx > 0 && tshndx < tgt->shnum) {
                                        GC_ENQUEUE(ls->obj, tshndx);
                                    }
                                }
                            }
                        }
                    } else if (sym->shndx > 0 && sym->shndx < o->shnum) {
                        GC_ENQUEUE(cur_obj, sym->shndx);
                    }
                }
            }
        }
    }

    #undef GC_ROOT_SYM
    #undef GC_ENQUEUE
    free(q);

    /* 4. If print_gc_sections is requested, report all discarded alloc sections */
    if (L->print_gc_sections) {
        for (int oi = 0; oi < L->obj_count; oi++) {
            ZcElfObj *o = L->objs[oi];
            for (int si = 1; si < o->shnum; si++) {
                const ZcSection *ins = &o->secs[si];
                if (!(ins->flags & SHF_ALLOC)) continue;
                if (ins->type != SHT_PROGBITS && ins->type != SHT_NOBITS) continue;
                if (ins->size == 0) continue;
                if (outsec_for_section(L, ins) < 0) continue;
                if (strncmp(ins->name, ".eh_frame", 9) == 0) continue;
                if (!L->sec_live[oi][si]) {
                    printf("removing unused section '%s' in file '%s'\n", ins->name, o->path);
                }
            }
        }
    }

    return 0;
}

static int layout_sections(ZcLink *L, char *err, size_t errsz) {
    const char *names[4] = {".text", ".rodata", ".data", ".bss"};
    for (int i = 0; i < 4; i++) {
        snprintf(L->outsec[i].name, sizeof(L->outsec[i].name), "%s", names[i]);
        L->outsec[i].write = (i == 2 || i == 3);
        L->outsec[i].exec = (i == 0);
        L->outsec[i].nobits = (i == 3);
        L->outsec[i].max_align = (i == 0) ? 16 : ((i == 2) ? ZC_PAGE : 8);
    }

    /* If no input defines _start (a CRT object), reserve a 16-byte stub at
     * the front of .text:
     *   xor %rbp,%rbp ; call <target> ; mov %eax,%edi ; mov $60,%eax ; syscall
     * <target> is main, or zero_rt's zeroc_main_wrapper when that archive
     * was linked (call offset patched in emit_exec once the vma is final).
     * In baremetal mode with a reset handler or custom entry, no Linux stub is needed. */
    bool have_start = false;
    for (size_t i = 0; i < L->sym_count; i++) {
        if (L->syms[i].obj >= 0) {
            if (strcmp(L->syms[i].name, "_start") == 0 ||
                strcmp(L->syms[i].name, "Reset_Handler") == 0 ||
                (L->has_script && L->script.has_entry &&
                 strcmp(L->syms[i].name, L->script.entry) == 0)) {
                have_start = true;
                break;
            }
        }
    }
    L->stub_at_front = !have_start && !L->baremetal;

    for (int oi = 0; oi < L->obj_count; oi++) {
        for (int si = 0; si < L->objs[oi]->shnum; si++) {
            const ZcSection *ins = &L->objs[oi]->secs[si];
            if (ins->type != SHT_PROGBITS && ins->type != SHT_NOBITS) continue;
            if (!(ins->flags & SHF_ALLOC)) continue;
            if (L->gc_sections && L->sec_live[oi] && !L->sec_live[oi][si]) continue;
            int out = outsec_for_section(L, ins);
            if (out < 0) {
                /* Sections a static ET_EXEC need not carry (unwind info). */
                if (strncmp(ins->name, ".eh_frame", 9) == 0) continue;
                snprintf(err, errsz,
                         "%s: unsupported alloc section `%s' "
                         "(the own linker merges .text*/.rodata*/.data*/.bss*)",
                         L->objs[oi]->path, ins->name);
                return -1;
            }
            uint64_t al = ins->addralign;
            if (al < 8) al = 8;
            if (!is_power_of_two_64(al)) al = 8;
            ZcOutSec *os = &L->outsec[out];
            if (al > os->max_align) os->max_align = al;
            if (out == 0 && os->cursor == 0 && L->stub_at_front) {
                const TargetLinkerOps *lops = zero_cc_get_linker_ops(L->machine);
                size_t stub_sz = lops ? lops->plt_stub_size : 16;
                if (L->pie) stub_sz = ZC_PIE_STUB_SIZE; /* self-relocating */
                if (outsec_zero_extend(os, stub_sz) != 0) {
                    snprintf(err, errsz, "out of memory");
                    return -1;
                }
            }
            os->cursor = align_up(os->cursor, al);
            if (L->map_count >= L->map_cap) {
                size_t nc = L->map_cap ? L->map_cap * 2 : 64;
                ZcInsecMap *nm = realloc(L->map, nc * sizeof(ZcInsecMap));
                if (!nm) {
                    snprintf(err, errsz, "out of memory");
                    return -1;
                }
                L->map = nm;
                L->map_cap = nc;
            }
            L->map[L->map_count].obj = L->objs[oi];
            L->map[L->map_count].insec = si;
            L->map[L->map_count].out_off = os->cursor;
            L->map_count++;
            if (ins->type == SHT_PROGBITS) {
                if (outsec_add(os, L->objs[oi]->data + ins->offset,
                               ins->size) != 0) {
                    snprintf(err, errsz, "out of memory");
                    return -1;
                }
            } else {
                /* NOBITS (.bss): no file content, but the address space is
                 * consumed - advance the cursor so successive objects' BSS
                 * sections are laid out back-to-back, not on top of each
                 * other (which silently aliased their symbols). The output
                 * .bss section's file size stays 0; memsz is its vsize. */
                os->memsz += ins->size;
                os->cursor += ins->size;
            }
            if (out == 3) L->have_bss = true;
        }
    }

    /* Reserve the x86-64 far-rel32 thunk pool at the end of .text, before
     * VMA assignment, so .rodata/.data/.bss and every symbol VMA land
     * after the pool.  Bound: one 14-byte slot per PC32/PLT32 relocation
     * (usually only a handful are actually far). */
    if (L->machine == EM_X86_64) {
        size_t pc32_cnt = 0, r64_cnt = 0;
        for (int oi = 0; oi < L->obj_count; oi++) {
            ZcElfObj *o = L->objs[oi];
            for (int si = 0; si < o->shnum; si++) {
                const ZcSection *rs = &o->secs[si];
                if (rs->type != SHT_RELA || rs->info >= o->shnum) continue;
                if (outsec_for_section(L, &o->secs[rs->info]) < 0) continue;
                if (L->gc_sections && L->sec_live[oi] && !L->sec_live[oi][rs->info]) continue;
                for (size_t r = 0; r < rs->reloc_count; r++) {
                    if (rs->relocs[r].type == 2 /* R_X86_64_PC32 */ ||
                        rs->relocs[r].type == 4 /* R_X86_64_PLT32 */)
                        pc32_cnt++;
                    else if (rs->relocs[r].type == 1 /* R_X86_64_64 */)
                        r64_cnt++;
                }
            }
        }
        if (pc32_cnt > 0) {
            ZcOutSec *ttext = &L->outsec[0];
            L->text_thunk_base = ttext->cursor;
            if (outsec_zero_extend(ttext,
                                   ttext->cursor + 14 * pc32_cnt) != 0) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
        }
        if (L->pie) {
            /* .rela.dyn: one slot per R_64 (absolute -> RELATIVE) plus one
             * per PC32 as a bound (far ones become thunks whose abs64 field
             * is itself a RELATIVE target).  Placed after the RX content,
             * 8-aligned, before the page-aligned RW sections.  In PIE the
             * layout base is 0, so the RX end is computable from cursors. */
            L->rela_slots = r64_cnt + pc32_cnt;
            uint64_t rx_end = L->outsec[0].cursor;
            if (L->outsec[1].cursor > 0)
                rx_end = align_up(rx_end, 8) + L->outsec[1].cursor;
            L->rela_dyn_vma = align_up(rx_end, 8);
            L->rela_dyn_size = 24 * L->rela_slots;
        }
    }

    /* VMAs & LMAs:
     * When a linker script with explicit FLASH and RAM regions is present,
     * lay out .text and .rodata in FLASH (LMA == VMA), .data in RAM (VMA)
     * with load image in FLASH (LMA), and .bss in RAM (VMA, NOBITS).
     * Otherwise, apply conventional static layout (base 0 for static PIE). */
    if (L->has_script && L->script.has_flash && L->script.has_ram) {
        uint64_t flash_base = L->script.flash.origin;
        uint64_t ram_base = L->script.ram.origin;

        /* .text in Flash */
        L->outsec[0].vma = flash_base;
        L->outsec[0].lma = flash_base;
        L->outsec[0].size = L->outsec[0].cursor;
        uint64_t flash_curr = flash_base + L->outsec[0].size;

        /* .rodata packed in Flash */
        if (L->outsec[1].cursor > 0) {
            uint64_t al = L->outsec[1].max_align;
            if (al < 8) al = 8;
            flash_curr = align_up(flash_curr, al);
            L->outsec[1].vma = flash_curr;
            L->outsec[1].lma = flash_curr;
            L->outsec[1].size = L->outsec[1].cursor;
            flash_curr += L->outsec[1].size;
        } else {
            L->outsec[1].vma = flash_curr;
            L->outsec[1].lma = flash_curr;
            L->outsec[1].size = 0;
        }

        /* .data in RAM (VMA) loaded from Flash (LMA) */
        uint64_t ram_curr = ram_base;
        if (L->outsec[2].cursor > 0) {
            uint64_t al = L->outsec[2].max_align;
            if (al < 8) al = 8;
            ram_curr = align_up(ram_curr, al);
            flash_curr = align_up(flash_curr, al);
            L->outsec[2].vma = ram_curr;
            L->outsec[2].lma = flash_curr;
            L->outsec[2].size = L->outsec[2].cursor;
            ram_curr += L->outsec[2].size;
            flash_curr += L->outsec[2].size;
        } else {
            L->outsec[2].vma = ram_curr;
            L->outsec[2].lma = flash_curr;
            L->outsec[2].size = 0;
        }

        /* .bss in RAM (NOBITS) */
        uint64_t bss_al = L->outsec[3].max_align;
        if (bss_al < 8) bss_al = 8;
        ram_curr = align_up(ram_curr, bss_al);
        L->outsec[3].vma = ram_curr;
        L->outsec[3].lma = 0;
        L->outsec[3].size = 0;
        if (L->have_bss) {
            ram_curr += L->outsec[3].memsz;
        }

        /* Capacity checks */
        if (flash_curr > L->script.flash.origin + L->script.flash.length) {
            snprintf(err, errsz, "region FLASH overflowed by %llu bytes",
                     (unsigned long long)(flash_curr - (L->script.flash.origin + L->script.flash.length)));
            return -1;
        }
        if (ram_curr > L->script.ram.origin + L->script.ram.length) {
            snprintf(err, errsz, "region RAM overflowed by %llu bytes",
                     (unsigned long long)(ram_curr - (L->script.ram.origin + L->script.ram.length)));
            return -1;
        }
    } else {
        const TargetLinkerOps *lops = zero_cc_get_linker_ops(L->machine);
        uint64_t v = (L->pie && L->machine == EM_X86_64) ? 0 :
                     (lops ? lops->default_text_base : ZC_TEXT_BASE);
        for (int i = 0; i < 3; i++) {
            if (L->outsec[i].cursor == 0) continue;
            uint64_t al = L->outsec[i].max_align;
            if (i == 0 && al < 16) al = 16;
            if (i == 2 && al < ZC_PAGE) al = ZC_PAGE;
            if (al < 8) al = 8;
            if (i == 2 && L->pie && L->rela_dyn_size > 0)
                v = align_up(v, 8) + L->rela_dyn_size;
            v = align_up(v, al);
            L->outsec[i].vma = v;
            L->outsec[i].lma = v;
            L->outsec[i].size = L->outsec[i].cursor;
            v = L->outsec[i].vma + L->outsec[i].cursor;
        }
        if (L->pie && L->rela_dyn_size > 0 && L->outsec[2].cursor == 0) {
            /* No .data: the .bss (if any) must still clear the rela region. */
            v = L->rela_dyn_vma + L->rela_dyn_size;
        }
        if (L->have_bss) {
            uint64_t al = (L->outsec[2].cursor > 0) ? L->outsec[3].max_align : ZC_PAGE;
            if (al < 8) al = 8;
            v = align_up(v, al);
            L->outsec[3].vma = v;
            L->outsec[3].lma = v;
            L->outsec[3].size = 0;
        }
    }

    /* Finalize symbol VMAs. */
    for (size_t i = 0; i < L->sym_count; i++) {
        ZcLinkSym *ls = &L->syms[i];
        if (ls->is_section) {
            if (ls->obj < 0) continue;
            if (ls->sym < 0) continue;
            const ZcSection *ins = &L->objs[ls->obj]->secs[ls->sym];
            int out = outsec_for_section(L, ins);
            if (out < 0) continue;
            ls->vma = L->outsec[out].vma + ls->sec_off;
        } else if (ls->is_common) {
            if (L->gc_sections && !ls->is_referenced) continue;
            ZcOutSec *d = &L->outsec[2];
            if (d->vma == 0) {
                /* No .data input at all: give the section a vma. */
                d->vma = (L->has_script && L->script.has_ram)
                             ? L->script.ram.origin
                             : align_up(L->outsec[1].vma + L->outsec[1].size, ZC_PAGE);
            }
            d->cursor = align_up(d->cursor, 8);
            ls->vma = d->vma + d->cursor;
            d->cursor += ls->size;
            if (outsec_zero_extend(d, d->cursor) != 0) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
            if (d->cursor > d->size) d->size = d->cursor;
            ls->obj = -1;
            ls->stype = ZC_STT_OBJECT;
        } else if (ls->obj >= 0) {
            ZcElfObj *o = L->objs[ls->obj];
            if (ls->sym < 0 || o->sec_symtab < 0) continue;
            const ZcSection *st = &o->secs[o->sec_symtab];
            if ((size_t)ls->sym >= st->sym_count) continue;
            const ZcSymbol *sym = &st->syms[ls->sym];
            if (sym->shndx == 0 || sym->shndx >= o->shnum) continue;
            const char *secname = o->secs[sym->shndx].name;
            (void)secname;
            int out = outsec_for_section(L, &o->secs[sym->shndx]);
            if (out < 0) continue;
            uint64_t in_off = 0;
            bool found_in_map = false;
            for (size_t m = 0; m < L->map_count; m++) {
                if (L->map[m].obj == o && L->map[m].insec == sym->shndx) {
                    in_off = L->map[m].out_off;
                    found_in_map = true;
                    break;
                }
            }
            if (!found_in_map) {
                ls->vma = 0;
                continue;
            }
            ls->vma = L->outsec[out].vma + in_off + sym->value;
        }
    }

    /* Define special linker boundary symbols if referenced. */
    static const struct { const char *name; int sec; bool end; bool lma; } specials[] = {
        {"_end", 3, true, false},
        {"__bss_start", 3, false, false},
        {"__bss_start__", 3, false, false},
        {"_sbss", 3, false, false},
        {"_ebss", 3, true, false},
        {"__bss_end__", 3, true, false},
        {"_edata", 2, true, false},
        {"_sdata", 2, false, false},
        {"__data_start", 2, false, false},
        {"__data_start__", 2, false, false},
        {"_sidata", 2, false, true},
        {"__data_load", 2, false, true},
        {"__data_load__", 2, false, true},
        {"_etext", 0, true, false},
        {"__etext", 0, true, false},
        {"__TMC_END__", 2, true, false},
        {"_GLOBAL_OFFSET_TABLE_", 2, false, false},
        {"__dso_handle", 2, false, false},
    };
    for (size_t si = 0; si < sizeof(specials) / sizeof(specials[0]); si++) {
        ZcLinkSym *ls = L->baremetal ? sym_get(L, specials[si].name) : sym_find(L, specials[si].name);
        if (ls && ls->obj < 0) {
            int out = specials[si].sec;
            uint64_t val = specials[si].lma ? L->outsec[out].lma : L->outsec[out].vma;
            if (specials[si].end) {
                val += (out == 3) ? L->outsec[out].memsz : L->outsec[out].size;
            }
            ls->vma = val;
            ls->obj = 0; /* mark as defined */
        }
    }

    /* Stack top symbol */
    static const char *const stack_syms[] = {"_stack_top", "__stack_top"};
    for (size_t si = 0; si < sizeof(stack_syms) / sizeof(stack_syms[0]); si++) {
        ZcLinkSym *ls = L->baremetal ? sym_get(L, stack_syms[si]) : sym_find(L, stack_syms[si]);
        if (ls && ls->obj < 0) {
            uint64_t val = 0;
            if (L->has_script && L->script.has_stack_top) {
                val = L->script.stack_top;
            } else if (L->has_script && L->script.has_ram) {
                val = L->script.ram.origin + L->script.ram.length;
            } else {
                val = L->outsec[3].vma + L->outsec[3].memsz + 0x4000;
            }
            ls->vma = val;
            ls->obj = 0;
        }
    }
    return 0;
}

/* ---------------- relocations ---------------- */

static int rela_dyn_add(ZcLink *L, uint64_t r_offset, uint64_t r_addend) {
    if (L->rela_dyn_count >= L->rela_dyn_cap) {
        size_t nc = L->rela_dyn_cap ? L->rela_dyn_cap * 2 : 64;
        ZcRelaDyn *nr = realloc(L->rela_dyn, nc * sizeof(ZcRelaDyn));
        if (!nr) return -1;
        L->rela_dyn = nr;
        L->rela_dyn_cap = nc;
    }
    L->rela_dyn[L->rela_dyn_count].r_offset = r_offset;
    L->rela_dyn[L->rela_dyn_count].r_addend = r_addend;
    L->rela_dyn_count++;
    return 0;
}

static int apply_relocations(ZcLink *L, char *err, size_t errsz) {
    ZcRiscvPcrel *pcrel_tab = NULL;
    size_t pcrel_cnt = 0, pcrel_cap = 0;

    if (L->machine == EM_RISCV) {
        /* Pass 1: pre-collect all R_RISCV_PCREL_HI20 targets so LO12 can
         * resolve regardless of relocation order. */
        for (int oi = 0; oi < L->obj_count; oi++) {
            ZcElfObj *o = L->objs[oi];
            for (int si = 0; si < o->shnum; si++) {
                const ZcSection *rs = &o->secs[si];
                if (rs->type != SHT_RELA || rs->info >= o->shnum) continue;
                if (L->gc_sections && L->sec_live[oi] && !L->sec_live[oi][rs->info]) continue;
                int out = outsec_for_section(L, &o->secs[rs->info]);
                if (out < 0) continue;
                uint64_t in_off = 0;
                for (size_t m = 0; m < L->map_count; m++) {
                    if (L->map[m].obj == o && L->map[m].insec == rs->info) {
                        in_off = L->map[m].out_off;
                        break;
                    }
                }
                const ZcSection *st = zc_elf_symtab(o);
                if (!st) continue;
                for (size_t r = 0; r < rs->reloc_count; r++) {
                    const ZcReloc *rel = &rs->relocs[r];
                    if (rel->type != 23 /* R_RISCV_PCREL_HI20 */) continue;
                    uint64_t P = L->outsec[out].vma + in_off + rel->offset;
                    int64_t A = rel->addend;
                    int64_t S = 0;
                    if (rel->sym >= st->sym_count) continue;
                    const ZcSymbol *sym = &st->syms[rel->sym];
                    if (sym->shndx == 0) {
                        ZcLinkSym *ls = sym_find(L, sym->name);
                        if (!ls || (ls->obj == -1 && !ls->is_section)) continue;
                        S = (int64_t)ls->vma;
                    } else if (sym->type == ZC_STT_SECTION) {
                        if (sym->shndx >= o->shnum) continue;
                        const char *secname = o->secs[sym->shndx].name;
                        int sout = outsec_for_name(secname);
                        if (sout < 0) continue;
                        uint64_t s_off = 0;
                        for (size_t m = 0; m < L->map_count; m++) {
                            if (L->map[m].obj == o && L->map[m].insec == sym->shndx) {
                                s_off = L->map[m].out_off;
                                break;
                            }
                        }
                        S = (int64_t)(L->outsec[sout].vma + s_off + sym->value);
                    } else {
                        if (sym->shndx >= o->shnum) continue;
                        const char *secname = o->secs[sym->shndx].name;
                        int sout = outsec_for_name(secname);
                        if (sout < 0) continue;
                        uint64_t s_off = 0;
                        for (size_t m = 0; m < L->map_count; m++) {
                            if (L->map[m].obj == o && L->map[m].insec == sym->shndx) {
                                s_off = L->map[m].out_off;
                                break;
                            }
                        }
                        S = (int64_t)(L->outsec[sout].vma + s_off + sym->value);
                    }
                    int64_t delta = S + A - (int64_t)P;
                    if (pcrel_cnt >= pcrel_cap) {
                        size_t nc = pcrel_cap ? pcrel_cap * 2 : 64;
                        ZcRiscvPcrel *nt = (ZcRiscvPcrel *)realloc(pcrel_tab, nc * sizeof(ZcRiscvPcrel));
                        if (!nt) { free(pcrel_tab); snprintf(err, errsz, "out of memory"); return -1; }
                        pcrel_tab = nt;
                        pcrel_cap = nc;
                    }
                    pcrel_tab[pcrel_cnt].p_hi = P;
                    pcrel_tab[pcrel_cnt].delta = delta;
                    pcrel_cnt++;
                }
            }
        }
    }

    for (int oi = 0; oi < L->obj_count; oi++) {
        ZcElfObj *o = L->objs[oi];
        for (int si = 0; si < o->shnum; si++) {
            const ZcSection *rs = &o->secs[si];
            if (rs->type != SHT_RELA) continue;
            if (rs->info >= o->shnum) continue;
            if (L->gc_sections && L->sec_live[oi] && !L->sec_live[oi][rs->info]) continue;
            const ZcSection *ins = &o->secs[rs->info];
            int out = outsec_for_section(L, ins);
            if (out < 0) continue;
            ZcOutSec *os = &L->outsec[out];
            uint64_t in_off = 0;
            for (size_t m = 0; m < L->map_count; m++) {
                if (L->map[m].obj == o && L->map[m].insec == rs->info) {
                    in_off = L->map[m].out_off;
                    break;
                }
            }
            const ZcSection *st = zc_elf_symtab(o);
            for (size_t r = 0; r < rs->reloc_count; r++) {
                const ZcReloc *rel = &rs->relocs[r];
                if (rel->type == 0) continue;
                uint64_t P = os->vma + in_off + rel->offset;
                int64_t A = rel->addend;
                int64_t S = 0;
                if (!st || rel->sym >= st->sym_count) {
                    snprintf(err, errsz, "bad relocation symbol index in %s",
                             o->path);
                    return -1;
                }
                const ZcSymbol *sym = &st->syms[rel->sym];
                if (sym->shndx == 0) {
                    /* UND: resolved as a definition, or a hard error (weak UND = 0). */
                    ZcLinkSym *ls = sym_find(L, sym->name);
                    if (!ls || (ls->obj == -1 && !ls->is_section)) {
                        if (sym->bind == ZC_STB_WEAK) {
                            S = 0;
                        } else {
                            snprintf(err, errsz,
                                     "undefined symbol: `%s' (referenced by %s)",
                                     sym->name, o->path);
                            return -1;
                        }
                    } else {
                        S = (int64_t)ls->vma;
                    }
                } else if (sym->type == ZC_STT_SECTION) {
                    if (sym->shndx >= o->shnum) {
                        snprintf(err, errsz, "bad section symbol %s",
                                 sym->name);
                        return -1;
                    }
                    const char *secname = o->secs[sym->shndx].name;
                    int sout = outsec_for_name(secname);
                    if (sout < 0) {
                        snprintf(err, errsz,
                                 "no output section for `%s'", secname);
                        return -1;
                    }
                    uint64_t s_off = 0;
                    for (size_t m = 0; m < L->map_count; m++) {
                        if (L->map[m].obj == o &&
                            L->map[m].insec == sym->shndx) {
                            s_off = L->map[m].out_off;
                            break;
                        }
                    }
                    S = (int64_t)(L->outsec[sout].vma + s_off + sym->value);
                } else {
                    /* Defined symbol in this object.
                     * If it is a global/weak symbol that was overridden in the
                     * global symbol table by another definition, use ls->vma! */
                    ZcLinkSym *ls = (sym->bind != 0 && sym->name[0])
                                        ? sym_find(L, sym->name)
                                        : NULL;
                    if (ls && ls->obj >= 0 &&
                        (ls->obj != oi || ls->sym != (int)rel->sym)) {
                        S = (int64_t)ls->vma;
                    } else {
                        if (sym->shndx >= o->shnum) {
                            snprintf(err, errsz,
                                     "bad symbol section for `%s' in %s",
                                     sym->name, o->path);
                            return -1;
                        }
                        const char *secname = o->secs[sym->shndx].name;
                        int sout = outsec_for_name(secname);
                        if (sout < 0) {
                            snprintf(err, errsz, "no output section for `%s'",
                                     secname);
                            return -1;
                        }
                        uint64_t s_off = 0;
                        for (size_t m = 0; m < L->map_count; m++) {
                            if (L->map[m].obj == o &&
                                L->map[m].insec == sym->shndx) {
                                s_off = L->map[m].out_off;
                                break;
                            }
                        }
                        S = (int64_t)(L->outsec[sout].vma + s_off + sym->value);
                    }
                }
                if (P < os->vma ||
                    P - os->vma >= (os->nobits ? os->memsz : os->size)) {
                    snprintf(err, errsz,
                             "relocation at 0x%lx outside %s of %s",
                             (unsigned long)P, os->name, o->path);
                    return -1;
                }
                uint8_t *site = os->content + (P - os->vma);
                const TargetLinkerOps *lops = zero_cc_get_linker_ops(L->machine);
                if (lops && lops->apply_reloc) {
                    if (lops->apply_reloc(L, site, rel->type, P, S, A,
                                          sym->name, o->path, rs->name,
                                          pcrel_tab, pcrel_cnt, err, errsz) != 0) {
                        free(pcrel_tab);
                        return -1;
                    }
                    continue;
                }
                switch (rel->type) {
                case 1:  /* R_X86_64_64 */
                    if (L->pie) {
                        /* Static PIE: the absolute value is only known once
                         * the kernel has chosen the load base, so the site
                         * becomes an R_X86_64_RELATIVE entry applied by the
                         * _start stub: value = base + r_addend. */
                        if (rela_dyn_add(L, P, (uint64_t)(S + A)) != 0) {
                            snprintf(err, errsz, "out of memory");
                            return -1;
                        }
                    } else {
                        for (int b = 0; b < 8; b++)
                            site[b] = (uint8_t)((uint64_t)(S + A) >> (8 * b));
                    }
                    break;
                case 2:  /* R_X86_64_PC32 */
                case 4:  /* R_X86_64_PLT32 (static: no PLT stubs needed) */
                {
                    int64_t d64 = (S + A) - (int64_t)P;
                    if (d64 > (int64_t)INT32_MIN && d64 < (int64_t)INT32_MAX) {
                        int32_t d = (int32_t)d64;
                        for (int b = 0; b < 4; b++)
                            site[b] = (uint8_t)((uint32_t)d >> (8 * b));
                    } else {
                        /* Far target (>2GB away): detour-style thunk.  The
                         * 32-bit relative site is re-aimed at a
                         * `jmp [rip+0]; abs64` slot in the pool reserved at
                         * the end of .text (in rel32 range as long as .text
                         * itself is <2GB).  The re-aim preserves semantics
                         * only for control transfer, so the site must be a
                         * `call` (e8) or `jmp` (e9).  A far rip-relative
                         * DATA reference (lea/mov [rip+disp], ...) cannot be
                         * served by an executed thunk - it needs an
                         * instruction rewrite (see the displaced-code
                         * follow-up) - and is a hard error instead of a
                         * silent int32 truncation. */
                        uint8_t m = 0;
                        if (P - os->vma >= 1)
                            m = os->content[P - os->vma - 1];
                        if (m != 0xe8 && m != 0xe9) {
                            snprintf(err, errsz,
                                     "far %s to `%s' in %s at 0x%lx is a "
                                     "rip-relative data reference (0x%02x at "
                                     "the site); only call/jmp can be "
                                     "re-aimed at a thunk - needs an "
                                     "instruction rewrite",
                                     (rel->type == 4) ? "PLT32" : "PC32",
                                     sym->name, o->path, (unsigned long)P,
                                     m);
                            return -1;
                        }
                        if (L->text_thunk_base + 14 * L->text_thunk_used >=
                            L->outsec[0].size) {
                            snprintf(err, errsz,
                                     "thunk pool exhausted for far %s to `%s' in %s",
                                     (rel->type == 4) ? "PLT32" : "PC32",
                                     sym->name, o->path);
                            return -1;
                        }
                        uint64_t T = L->outsec[0].vma + L->text_thunk_base +
                                     14 * L->text_thunk_used;
                        uint8_t *tp = L->outsec[0].content +
                                      (T - L->outsec[0].vma);
                        static const uint8_t zc_thunk_hdr[6] =
                            { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00 };
                        memcpy(tp, zc_thunk_hdr, 6);
                        /* Absolute target = the address the PC-relative
                         * reference denotes: (P+4) + (S+A-P) = S+A+4.  (For
                         * zerocc/bfd-style objects A is -4, so this is the
                         * bare symbol value S. */
                        uint64_t tgt = (uint64_t)(S + A) + 4;
                        memcpy(tp + 6, &tgt, 8);
                        if (L->pie) {
                            /* PIE: the slot's abs64 cannot be a literal;
                             * it is filled at load time by the _start stub
                             * as an R_X86_64_RELATIVE entry. */
                            memset(tp + 6, 0, 8);
                            if (rela_dyn_add(L, T + 6, tgt) != 0) {
                                snprintf(err, errsz, "out of memory");
                                return -1;
                            }
                        }
                        L->text_thunk_used++;
                        /* The site's rel32 is relative to P+4 (next
                         * instruction), like the direct patch. */
                        int64_t d2 = (int64_t)T - (int64_t)P - 4;
                        if (d2 <= (int64_t)INT32_MIN ||
                            d2 >= (int64_t)INT32_MAX) {
                            snprintf(err, errsz,
                                     "thunk out of rel32 range (is .text >2GB?) for far %s to `%s' in %s",
                                     (rel->type == 4) ? "PLT32" : "PC32",
                                     sym->name, o->path);
                            return -1;
                        }
                        int32_t d = (int32_t)d2;
                        for (int b = 0; b < 4; b++)
                            site[b] = (uint8_t)((uint32_t)d >> (8 * b));
                    }
                    break;
                }
                case 9:  /* R_X86_64_GOTPCREL */
                case 41: /* R_X86_64_GOTPCRELX */
                case 42: /* R_X86_64_REX_GOTPCRELX */
                {
                    /* Relaxation: if the preceding instruction is `mov (%rip), %reg`
                     * (opcode 0x8b), convert it to `lea (%rip), %reg` (opcode 0x8d)
                     * and resolve as PC32. */
                    if (P - os->vma >= 2 && site[-2] == 0x8b) {
                        site[-2] = 0x8d;
                    }
                    int64_t d64 = (S + A) - (int64_t)P;
                    if (d64 > (int64_t)INT32_MIN && d64 < (int64_t)INT32_MAX) {
                        int32_t d = (int32_t)d64;
                        for (int b = 0; b < 4; b++)
                            site[b] = (uint8_t)((uint32_t)d >> (8 * b));
                    } else {
                        snprintf(err, errsz,
                                 "GOTPCRELX to `%s' in %s out of rel32 range",
                                 sym->name, o->path);
                        return -1;
                    }
                    break;
                }
                case 10: /* R_X86_64_32 */
                    if (L->pie) {
                        snprintf(err, errsz,
                                 "R_X86_64_32 to `%s' in %s: 32-bit absolute "
                                 "references cannot be expressed in a PIE "
                                 "image", sym->name, o->path);
                        return -1;
                    }
                    for (int b = 0; b < 4; b++)
                        site[b] = (uint8_t)((uint32_t)(uint64_t)(S + A) >> (8 * b));
                    break;
                case 11: /* R_X86_64_32S */
                    if (L->pie) {
                        snprintf(err, errsz,
                                 "R_X86_64_32S to `%s' in %s: 32-bit signed "
                                 "absolute references cannot be expressed in "
                                 "a PIE image", sym->name, o->path);
                        return -1;
                    }
                    for (int b = 0; b < 4; b++)
                        site[b] = (uint8_t)((uint32_t)(int32_t)(S + A) >> (8 * b));
                    break;
                case 8:  /* R_X86_64_RELATIVE */
                    for (int b = 0; b < 8; b++)
                        site[b] = (uint8_t)((uint64_t)A >> (8 * b));
                    break;
                default:
                    snprintf(err, errsz,
                             "unsupported relocation type %u in %s (%s)",
                             rel->type, rs->name, o->path);
                    return -1;
                }
            }
        }
    }
    free(pcrel_tab);
    return 0;
}

/* ---------------- output ---------------- */

static void put8(uint8_t **p, uint8_t v) { **p = v; (*p)++; }
static void put16(uint8_t **p, uint16_t v) {
    for (int b = 0; b < 2; b++) { **p = (uint8_t)(v >> (8 * b)); (*p)++; }
}
static void put32(uint8_t **p, uint32_t v) {
    for (int b = 0; b < 4; b++) { **p = (uint8_t)(v >> (8 * b)); (*p)++; }
}
static void put64(uint8_t **p, uint64_t v) {
    for (int b = 0; b < 8; b++) { **p = (uint8_t)(v >> (8 * b)); (*p)++; }
}

static bool sym_is_live(const ZcLink *L, const ZcLinkSym *ls) {
    if (!L->gc_sections) return true;
    if (ls->is_section) {
        if (ls->obj < 0 || ls->sym < 0) return false;
        return (L->sec_live[ls->obj] && L->sec_live[ls->obj][ls->sym]);
    }
    if (ls->is_common) {
        return ls->is_referenced;
    }
    if (ls->obj >= 0) {
        ZcElfObj *o = L->objs[ls->obj];
        if (ls->sym >= 0 && o->sec_symtab >= 0) {
            const ZcSection *st = &o->secs[o->sec_symtab];
            if ((size_t)ls->sym < st->sym_count) {
                const ZcSymbol *sym = &st->syms[ls->sym];
                if (sym->shndx > 0 && sym->shndx < o->shnum) {
                    return (L->sec_live[ls->obj] && L->sec_live[ls->obj][sym->shndx]);
                }
            }
        }
    }
    return true;
}

static int emit_exec(ZcLink *L, const char *out_path, uint64_t entry,
                     uint64_t main_vma, char *err, size_t errsz) {
    ZcOutSec *text = &L->outsec[0];
    ZcOutSec *rodata = &L->outsec[1];
    ZcOutSec *data = &L->outsec[2];
    ZcOutSec *bss = &L->outsec[3];

    bool have_rx = text->size > 0 || rodata->size > 0;
    bool have_rw = data->size > 0 || L->have_bss;
    if (!have_rx) {
        snprintf(err, errsz, "no code to link (empty .text)");
        return -1;
    }

    /* File offsets (vmas already set by layout; only gaps are filled in). */
    uint64_t rx_off = 0x1000;
    uint64_t rx_vma = text->vma;
    uint64_t rx_end_vma = (rodata->size > 0)
                              ? rodata->vma + rodata->size
                              : text->vma + text->size;
    if (L->pie && L->rela_dyn_size > 0) {
        uint64_t rela_end = L->rela_dyn_vma + L->rela_dyn_size;
        if (rela_end > rx_end_vma) rx_end_vma = rela_end;
    }
    uint64_t rx_filesz = rx_end_vma - rx_vma; /* includes inter-section gaps */
    text->file_off = rx_off;
    rodata->file_off = rx_off + (rodata->vma - text->vma);

    uint64_t rw_off = 0, rw_vma = 0;
    if (have_rw) {
        rw_vma = data->size > 0 ? data->vma : bss->vma;
        if (L->baremetal) {
            rw_off = align_up(rx_off + rx_filesz, 8);
        } else {
            rw_off = align_up(rx_off + rx_filesz, ZC_PAGE);
            /* keep p_offset congruent to p_vaddr mod page */
            rw_off += (rw_vma - rw_off) & (ZC_PAGE - 1);
        }
        data->file_off = rw_off;
    }

    /* Output section indices: 0=NULL, then present alloc sections, then
     * symtab/strtab/shstrtab. */
    int out_shndx[4] = {0, 0, 0, 0};
    {
        int t = 1;
        for (int k = 0; k < 4; k++) {
            bool present = (k == 3) ? L->have_bss
                        : (L->outsec[k].size > 0 ||
                           (k == 2 && data->vma != 0));
            if (present) out_shndx[k] = t++;
        }
    }
    int sh_rela = 0; /* 0 = no .rela.dyn section (non-PIE, or no R_64s) */
    int sh_symtab = 1 + (out_shndx[0] != 0) + (out_shndx[1] != 0) +
                    (out_shndx[2] != 0) + (out_shndx[3] != 0);
    if (L->pie && L->rela_dyn_count > 0) sh_rela = sh_symtab;
    sh_symtab += (sh_rela != 0);
    int sh_strtab = sh_symtab + 1;
    int sh_shstrtab = sh_symtab + 2;
    int nsecs = sh_shstrtab + 1;

    /* Symbol table contents: null + resolved globals + section symbols. */
    size_t nsyms = 1;
    for (size_t i = 0; i < L->sym_count; i++) {
        ZcLinkSym *ls = &L->syms[i];
        if (!sym_is_live(L, ls)) continue;
        if (ls->is_section) { nsyms++; continue; }
        if (ls->obj == -1) continue; /* undefined or common: skip */
        nsyms++;
    }
    for (int k = 0; k < 4; k++) {
        bool present = (k == 3) ? L->have_bss
                    : (L->outsec[k].size > 0 || (k == 2 && data->vma != 0));
        if (present) nsyms++;
    }

    size_t str_cap = 1;
    for (size_t i = 0; i < L->sym_count; i++) str_cap += ZC_MAX_NAME;
    uint8_t *strtab = calloc(1, str_cap);
    size_t str_size = 1; /* leading NUL for the null symbol */
    uint32_t *sym_stroff = malloc((L->sym_count ? L->sym_count : 1) * sizeof(uint32_t));
    if (!sym_stroff) {
        snprintf(err, errsz, "out of memory");
        free(strtab);
        return -1;
    }
    for (size_t i = 0; i < L->sym_count; i++) {
        sym_stroff[i] = (uint32_t)str_size;
        size_t n = strlen(L->syms[i].name);
        memcpy(strtab + str_size, L->syms[i].name, n + 1);
        str_size += n + 1;
    }

    const char *secnames[8] = {
        ".text", ".rodata", ".data", ".bss", ".rela.dyn",
        ".symtab", ".strtab", ".shstrtab"
    };
    size_t shs_size = 0;
    uint32_t shs_off[8];
    {
        size_t off = 1; /* leading NUL */
        for (int i = 0; i < 8; i++) {
            shs_off[i] = (uint32_t)off;
            size_t n = strlen(secnames[i]);
            off += n + 1;
        }
        shs_size = off;
    }
    uint8_t *shstrtab = calloc(1, shs_size);
    {
        size_t off = 1;
        for (int i = 0; i < 8; i++) {
            size_t n = strlen(secnames[i]);
            memcpy(shstrtab + off, secnames[i], n + 1);
            off += n + 1;
        }
    }

    size_t symtab_entsize = L->is_32 ? 16 : 24;
    size_t symtab_size = nsyms * symtab_entsize;
    uint8_t *symtab = calloc(nsyms, symtab_entsize);

    size_t shdr_entsize = L->is_32 ? 40 : 64;
    size_t shdr_table_size = (size_t)nsecs * shdr_entsize;

    /* File layout: shdrs after the RW content, then shstrtab, symtab,
     * strtab. */
    uint64_t sh_off = have_rw
        ? align_up(rw_off + data->size, 8)
        : align_up(rx_off + rx_filesz, 8);
    uint64_t file_size = sh_off + shdr_table_size +
                         shs_size + symtab_size + str_size;

    uint8_t *img = calloc(1, file_size);
    if (!img) {
        snprintf(err, errsz, "out of memory");
        goto oom;
    }

    /* ELF header (e_entry = main or _start). */
    uint8_t *p = img;
    if (L->is_32) {
        put8(&p, 0x7f); put8(&p, 'E'); put8(&p, 'L'); put8(&p, 'F');
        put8(&p, 1); /* 32-bit */
        put8(&p, 1); /* little endian */
        put8(&p, 1); /* ELF version */
        put8(&p, 0); /* SYSV */
        for (int i = 0; i < 8; i++) put8(&p, 0);
        put16(&p, ET_EXEC);
        put16(&p, L->machine);
        put32(&p, 1);          /* e_version */
        put32(&p, (uint32_t)entry); /* e_entry */
        put32(&p, 52);         /* e_phoff */
        put32(&p, (uint32_t)sh_off); /* e_shoff */
        const TargetLinkerOps *lops = zero_cc_get_linker_ops(L->machine);
        put32(&p, lops ? lops->default_elf_flags : 0); /* e_flags */
        put16(&p, 52);         /* e_ehsize */
        put16(&p, 32);         /* e_phentsize */
        put16(&p, (uint16_t)(have_rx + have_rw)); /* e_phnum */
        put16(&p, 40);         /* e_shentsize */
        put16(&p, (uint16_t)nsecs);
        put16(&p, (uint16_t)sh_shstrtab);

        /* Program headers at 52 */
        p = img + 52;
        if (have_rx) {
            put32(&p, PT_LOAD);
            uint32_t l_off, l_vma, l_sz;
            if (L->baremetal) {
                l_off = (uint32_t)rx_off;
                l_vma = (uint32_t)rx_vma;
                l_sz = (uint32_t)rx_filesz;
            } else {
                l_off = (uint32_t)((rx_vma >= rx_off) ? 0 : rx_off);
                l_vma = (uint32_t)(rx_vma - (rx_off - l_off));
                l_sz = (uint32_t)(rx_filesz + (rx_off - l_off));
            }
            put32(&p, l_off);
            put32(&p, l_vma);
            put32(&p, (uint32_t)text->lma); /* p_paddr: LMA in Flash */
            put32(&p, l_sz);
            put32(&p, l_sz);
            put32(&p, PF_R | PF_X);
            put32(&p, (uint32_t)(L->baremetal ? 8 : ZC_PAGE));
        }
        if (have_rw) {
            put32(&p, PT_LOAD);
            put32(&p, (uint32_t)rw_off);
            put32(&p, (uint32_t)rw_vma);
            put32(&p, (uint32_t)data->lma); /* p_paddr: LMA in Flash */
            put32(&p, (uint32_t)data->size);
            put32(&p, (uint32_t)(data->size + (L->have_bss ? bss->memsz : 0)));
            put32(&p, PF_R | PF_W);
            put32(&p, (uint32_t)(L->baremetal ? 8 : ZC_PAGE));
        }
    } else {
        put8(&p, 0x7f); put8(&p, 'E'); put8(&p, 'L'); put8(&p, 'F');
        put8(&p, 2); /* 64-bit */
        put8(&p, 1); /* little endian */
        put8(&p, 1); /* ELF version */
        put8(&p, 0); /* SYSV */
        for (int i = 0; i < 8; i++) put8(&p, 0);
        put16(&p, L->pie ? ET_DYN : ET_EXEC);
        put16(&p, L->machine);
        put32(&p, 1);          /* e_version */
        put64(&p, entry);      /* e_entry */
        put64(&p, 64);         /* e_phoff */
        put64(&p, sh_off);     /* e_shoff */
        const TargetLinkerOps *lops = zero_cc_get_linker_ops(L->machine);
        put32(&p, lops ? lops->default_elf_flags : 0);
        put16(&p, 64);         /* e_ehsize */
        put16(&p, 56);         /* e_phentsize */
        put16(&p, (uint16_t)(have_rx + have_rw)); /* e_phnum */
        put16(&p, 64);         /* e_shentsize */
        put16(&p, (uint16_t)nsecs);
        put16(&p, (uint16_t)sh_shstrtab);

        /* Program headers at 64. */
        p = img + 64;
        if (have_rx) {
            put32(&p, PT_LOAD);
            /* Static PIE: the segment starts RWX so the self-relocating _start
             * stub can both execute and write the relocated sites; it
             * mprotects the segment back to RX at the end of relocation. */
            put32(&p, L->pie ? (PF_R | PF_W | PF_X) : (PF_R | PF_X));
            uint64_t l_off, l_vma, l_sz;
            if (L->baremetal) {
                l_off = rx_off;
                l_vma = rx_vma;
                l_sz = rx_filesz;
            } else {
                l_off = (rx_vma >= rx_off) ? 0 : rx_off;
                l_vma = rx_vma - (rx_off - l_off);
                l_sz = rx_filesz + (rx_off - l_off);
            }
            put64(&p, l_off);
            put64(&p, l_vma);
            put64(&p, text->lma); /* p_paddr: LMA in Flash */
            put64(&p, l_sz);
            put64(&p, l_sz);
            put64(&p, L->baremetal ? 8 : ZC_PAGE);
        }
        if (have_rw) {
            put32(&p, PT_LOAD);
            put32(&p, PF_R | PF_W);
            put64(&p, rw_off);
            put64(&p, rw_vma);
            put64(&p, data->lma); /* p_paddr: LMA in Flash */
            put64(&p, data->size);
            put64(&p, data->size + (L->have_bss ? bss->memsz : 0));
            put64(&p, L->baremetal ? 8 : ZC_PAGE);
        }
    }

    /* Section contents. */
    if (text->size) memcpy(img + text->file_off, text->content, text->size);
    if (rodata->size)
        memcpy(img + rodata->file_off, rodata->content, rodata->size);
    if (data->size) memcpy(img + data->file_off, data->content, data->size);

    /* .rela.dyn (static PIE): Elf64_Rela entries applied by the _start
     * stub; the RX PT_LOAD above already spans this region. */
    if (L->pie && L->rela_dyn_count > 0) {
        uint64_t ro = rx_off + (L->rela_dyn_vma - rx_vma);
        for (size_t i = 0; i < L->rela_dyn_count; i++) {
            uint8_t *e = img + ro + i * 24;
            put64(&e, L->rela_dyn[i].r_offset);
            put64(&e, 8 /* R_X86_64_RELATIVE */);
            put64(&e, L->rela_dyn[i].r_addend);
        }
    }

    /* Patch the _start stub (reserved at .text offset 0).  x86-64 (16 B):
     *   +0: 31 f9          xor %rbp,%rbp
     *   +2: e8 xx xx xx xx call <target>
     *   +7: 89 c7          mov %eax,%edi
     *   +9: b8 3c 00 00 00 mov $60,%eax
     *   +14: 0f 05         syscall
     * AArch64 (12 B + nop pad): `bl` sets LR so main's `ret` falls through
     * to the svc; x0 holds the return value:
     *   +0: 0xD2800BC8     mov x8, #94   (exit_group)
     *   +4: 0x94000000|imm bl <target>  (imm26 in bits [25:0])
     *   +8: 0xD4000001     svc #0
     *   +12: 0xD503201F    nop (pad to the 16-byte reservation) */
    if (L->stub_at_front) {
        uint8_t *st = img + text->file_off;
        const TargetLinkerOps *lops = zero_cc_get_linker_ops(L->machine);
        if (lops && lops->emit_start_stub) {
            lops->emit_start_stub(st, text->vma, main_vma);
        } else if (L->pie) {
            /* Static-PIE self-relocating _start (ZC_PIE_STUB_SIZE bytes,
             * assembled and verified; patch fields:
             *   +0x0a RX_LEN (imm32, sign-extended)   - the RX segment size
             *   +0x11 .rela.dyn rip-relative disp32   (lea at 0x0e, next-ip 0x15)
             *   +0x16 entry count (imm32)             (mov ecx at 0x15)
             *   +0x52 call rel32                      (call at 0x51, next-ip 0x56)
             * Bytes:
             *   lea rdi,[rip-7]        rdi = image base
             *   mov r9, RX_LEN
             *   lea rsi,[.rela.dyn]
             *   mov ecx, N
             *   loop: mov rax,[rsi]; mov rbx,[rsi+16];
             *         lea r8,[rdi+rbx]; mov [rdi+rax],r8; add rsi,24; loop
             *   mov eax,10; mov rsi,r9; mov edx,5; mov r10,0; syscall
             *        mprotect(base, RX_LEN, RX)
             *   mov rdi,[rsp]; mov rsi,[rsp+8]; xor rbp,rbp
             *   call <target>; mov edi,eax; mov eax,60; syscall
             */
            static const uint8_t pie_stub[ZC_PIE_STUB_SIZE] = {
                0x48, 0x8d, 0x3d, 0xf9, 0xff, 0xff, 0xff, 0x49, 0xc7, 0xc1,
                0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x35, 0x00, 0x00, 0x00,
                0x00, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x06, 0x48,
                0x8b, 0x5e, 0x10, 0x4c, 0x8d, 0x04, 0x1f, 0x4c, 0x89, 0x04,
                0x07, 0x48, 0x83, 0xc6, 0x18, 0xe2, 0xeb, 0xb8, 0x0a, 0x00,
                0x00, 0x00, 0x4c, 0x89, 0xce, 0xba, 0x05, 0x00, 0x00, 0x00,
                0x49, 0xc7, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48,
                0x8b, 0x3c, 0x24, 0x48, 0x8b, 0x74, 0x24, 0x08, 0x48, 0x31,
                0xed, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x89, 0xc7, 0xb8, 0x3c,
                0x00, 0x00, 0x00, 0x0f, 0x05,
            };
            if (rx_filesz >= 0x80000000ULL) {
                snprintf(err, errsz,
                         "static-PIE link: RX segment %llu bytes exceeds "
                         "the stub's 32-bit mprotect length (is .text >2GB?)",
                         (unsigned long long)rx_filesz);
                return -1;
            }
            memcpy(st, pie_stub, ZC_PIE_STUB_SIZE);
            /* +0x0a: RX_LEN */
            uint32_t rxlen = (uint32_t)rx_filesz;
            memcpy(st + 0x0a, &rxlen, 4);
            /* +0x11: .rela.dyn disp32 (lea at 0x0e, next-ip 0x15) */
            int32_t d_list = L->rela_dyn_count > 0
                ? (int32_t)(L->rela_dyn_vma - (text->vma + 0x15))
                : 0; /* self-ref; count 0 skips the loop */
            memcpy(st + 0x11, &d_list, 4);
            /* +0x16: entry count */
            uint32_t cnt = (uint32_t)L->rela_dyn_count;
            memcpy(st + 0x16, &cnt, 4);
            /* +0x52: call rel32 (call at 0x51, next-ip 0x56) */
            int32_t rel = (int32_t)(main_vma - (text->vma + 0x56));
            memcpy(st + 0x52, &rel, 4);
        } else {
            st[0] = 0x31; st[1] = 0xf9;
            st[2] = 0xe8;
            int32_t rel = (int32_t)(main_vma - (text->vma + 7));
            st[3] = (uint8_t)(rel);
            st[4] = (uint8_t)(rel >> 8);
            st[5] = (uint8_t)(rel >> 16);
            st[6] = (uint8_t)(rel >> 24);
            st[7] = 0x89; st[8] = 0xc7;
            st[9] = 0xb8; st[10] = 0x3c; st[11] = 0; st[12] = 0; st[13] = 0;
            st[14] = 0x0f; st[15] = 0x05;
        }
    }

    /* .symtab */
    {
        uint8_t *sp = symtab;
        if (L->is_32) {
            /* null symbol */
            memset(sp, 0, 16);
            sp += 16;
            for (size_t i = 0; i < L->sym_count; i++) {
                ZcLinkSym *ls = &L->syms[i];
                if (!sym_is_live(L, ls)) continue;
                if (ls->is_section) {
                    put32(&sp, sym_stroff[i]);
                    put32(&sp, (uint32_t)ls->vma);
                    put32(&sp, 0);
                    *sp++ = (uint8_t)((0 << 4) | ZC_STT_SECTION); /* LOCAL */
                    *sp++ = 0;
                    uint16_t shndx = 0;
                    for (int k = 0; k < 4; k++) {
                        ZcOutSec *os = &L->outsec[k];
                        bool present = (k == 3) ? L->have_bss
                                    : (os->size > 0 || (k == 2 && data->vma != 0));
                        if (present && ls->vma >= os->vma &&
                            ls->vma < os->vma + (os->nobits ? os->memsz
                                                            : os->size)) {
                            shndx = (uint16_t)out_shndx[k];
                            break;
                        }
                    }
                    put16(&sp, shndx);
                } else if (ls->obj != -1) {
                    put32(&sp, sym_stroff[i]);
                    put32(&sp, (uint32_t)ls->vma);
                    put32(&sp, (uint32_t)ls->size);
                    *sp++ = (uint8_t)((ZC_STB_GLOBAL << 4) | ls->stype);
                    *sp++ = 0;
                    uint16_t shndx = 0;
                    for (int k = 0; k < 4; k++) {
                        ZcOutSec *os = &L->outsec[k];
                        bool present = (k == 3) ? L->have_bss
                                    : (os->size > 0 || (k == 2 && data->vma != 0));
                        if (present && ls->vma >= os->vma &&
                            ls->vma < os->vma + (os->nobits ? os->memsz
                                                            : os->size)) {
                            shndx = (uint16_t)out_shndx[k];
                            break;
                        }
                    }
                    put16(&sp, shndx);
                }
            }
            /* Section symbols for the output sections. */
            for (int k = 0; k < 4; k++) {
                ZcOutSec *os = &L->outsec[k];
                bool present = (k == 3) ? L->have_bss
                            : (os->size > 0 || (k == 2 && data->vma != 0));
                if (!present) continue;
                put32(&sp, 0); /* section symbols have st_name = 0 in ELF */
                put32(&sp, (uint32_t)os->vma);
                put32(&sp, (uint32_t)(os->nobits ? os->memsz : os->size));
                *sp++ = (uint8_t)((0 << 4) | ZC_STT_SECTION);
                *sp++ = 0;
                put16(&sp, (uint16_t)out_shndx[k]);
            }
        } else {
            /* null symbol */
            memset(sp, 0, 24);
            sp += 24;
            for (size_t i = 0; i < L->sym_count; i++) {
                ZcLinkSym *ls = &L->syms[i];
                if (!sym_is_live(L, ls)) continue;
                if (ls->is_section) {
                    put32(&sp, sym_stroff[i]);
                    *sp = (uint8_t)((0 << 4) | ZC_STT_SECTION); /* LOCAL */
                    sp++;
                    *sp = 0; sp++;
                    uint16_t shndx = 0;
                    for (int k = 0; k < 4; k++) {
                        ZcOutSec *os = &L->outsec[k];
                        bool present = (k == 3) ? L->have_bss
                                    : (os->size > 0 || (k == 2 && data->vma != 0));
                        if (present && ls->vma >= os->vma &&
                            ls->vma < os->vma + (os->nobits ? os->memsz
                                                            : os->size)) {
                            shndx = (uint16_t)out_shndx[k];
                            break;
                        }
                    }
                    put16(&sp, shndx);
                    put64(&sp, ls->vma);
                    put64(&sp, 0);
                } else if (ls->obj != -1) {
                    put32(&sp, sym_stroff[i]);
                    *sp = (uint8_t)((ZC_STB_GLOBAL << 4) | ls->stype);
                    sp++;
                    *sp = 0; sp++;
                    uint16_t shndx = 0;
                    for (int k = 0; k < 4; k++) {
                        ZcOutSec *os = &L->outsec[k];
                        bool present = (k == 3) ? L->have_bss
                                    : (os->size > 0 || (k == 2 && data->vma != 0));
                        if (present && ls->vma >= os->vma &&
                            ls->vma < os->vma + (os->nobits ? os->memsz
                                                            : os->size)) {
                            shndx = (uint16_t)out_shndx[k];
                            break;
                        }
                    }
                    put16(&sp, shndx);
                    put64(&sp, ls->vma);
                    put64(&sp, ls->size);
                }
            }
            /* Section symbols for the output sections. */
            for (int k = 0; k < 4; k++) {
                ZcOutSec *os = &L->outsec[k];
                bool present = (k == 3) ? L->have_bss
                            : (os->size > 0 || (k == 2 && data->vma != 0));
                if (!present) continue;
                put32(&sp, 0); /* section symbols have st_name = 0 in ELF */
                *sp = (uint8_t)((0 << 4) | ZC_STT_SECTION);
                sp++;
                *sp = 0; sp++;
                put16(&sp, (uint16_t)out_shndx[k]);
                put64(&sp, os->vma);
                put64(&sp, os->nobits ? os->memsz : os->size);
            }
        }
    }

    /* Section headers. */
    if (L->is_32) {
        uint8_t *sh = img + sh_off;
        /* [0] NULL */
        sh += 40;
        int idx = 1;
        for (int k = 0; k < 4; k++) {
            ZcOutSec *os = &L->outsec[k];
            bool present = (k == 3) ? L->have_bss
                        : (os->size > 0 || (k == 2 && data->vma != 0));
            if (!present) continue;
            put32(&sh, shs_off[k]);
            put32(&sh, os->nobits ? SHT_NOBITS : SHT_PROGBITS);
            uint32_t flags = (uint32_t)SHF_ALLOC |
                             (os->exec ? (uint32_t)SHF_EXECINSTR : 0) |
                             (os->write ? (uint32_t)SHF_WRITE : 0);
            put32(&sh, flags);
            put32(&sh, (uint32_t)os->vma);
            put32(&sh, (uint32_t)(os->nobits ? 0 : os->file_off));
            put32(&sh, (uint32_t)(os->nobits ? os->memsz : os->size));
            put32(&sh, 0);  /* link */
            put32(&sh, 0);  /* info */
            put32(&sh, (uint32_t)(os->max_align ? os->max_align : 4));  /* addralign */
            put32(&sh, 0);  /* entsize */
            idx++;
        }
        /* .symtab */
        put32(&sh, shs_off[5]);
        put32(&sh, SHT_SYMTAB);
        put32(&sh, 0);
        put32(&sh, 0);
        put32(&sh, (uint32_t)(sh_off + (uint64_t)nsecs * 40 + shs_size));
        put32(&sh, (uint32_t)symtab_size);
        put32(&sh, (uint32_t)sh_strtab);
        put32(&sh, 1); /* info: one local (the null symbol) */
        put32(&sh, 4);
        put32(&sh, 16);
        idx++;
        /* .strtab */
        put32(&sh, shs_off[6]);
        put32(&sh, SHT_STRTAB);
        put32(&sh, 0);
        put32(&sh, 0);
        put32(&sh, (uint32_t)(sh_off + (uint64_t)nsecs * 40 + shs_size + symtab_size));
        put32(&sh, (uint32_t)str_size);
        put32(&sh, 0);
        put32(&sh, 0);
        put32(&sh, 1);
        put32(&sh, 0);
        idx++;
        /* .shstrtab */
        put32(&sh, shs_off[7]);
        put32(&sh, SHT_STRTAB);
        put32(&sh, 0);
        put32(&sh, 0);
        put32(&sh, (uint32_t)(sh_off + (uint64_t)nsecs * 40));
        put32(&sh, (uint32_t)shs_size);
        put32(&sh, 0);
        put32(&sh, 0);
        put32(&sh, 1);
        put32(&sh, 0);
        idx++;
        (void)idx;
    } else {
        uint8_t *sh = img + sh_off;
        /* [0] NULL */
        sh += 64;
        int idx = 1;
        for (int k = 0; k < 4; k++) {
            ZcOutSec *os = &L->outsec[k];
            bool present = (k == 3) ? L->have_bss
                        : (os->size > 0 || (k == 2 && data->vma != 0));
            if (!present) continue;
            put32(&sh, shs_off[k]);
            put32(&sh, os->nobits ? SHT_NOBITS : SHT_PROGBITS);
            uint64_t flags = (uint64_t)SHF_ALLOC |
                             (os->exec ? (uint64_t)SHF_EXECINSTR : 0) |
                             (os->write ? (uint64_t)SHF_WRITE : 0);
            put64(&sh, flags);
            put64(&sh, os->vma);
            put64(&sh, os->nobits ? 0 : os->file_off);
            put64(&sh, os->nobits ? os->memsz : os->size);
            put32(&sh, 0);  /* link */
            put32(&sh, 0);  /* info */
            put64(&sh, os->max_align ? os->max_align : 8);  /* addralign */
            put64(&sh, 0);  /* entsize */
            idx++;
        }
        if (sh_rela != 0) {
            /* .rela.dyn (static PIE) */
            put32(&sh, shs_off[4]);
            put32(&sh, SHT_RELA);
            put64(&sh, (uint64_t)SHF_ALLOC);
            put64(&sh, L->rela_dyn_vma);
            put64(&sh, rx_off + (L->rela_dyn_vma - rx_vma));
            put64(&sh, 24 * L->rela_dyn_count);
            put32(&sh, 0);  /* link */
            put32(&sh, 0);  /* info */
            put64(&sh, 8);  /* addralign */
            put64(&sh, 24); /* entsize */
            idx++;
        }
        /* .symtab */
        put32(&sh, shs_off[5]);
        put32(&sh, SHT_SYMTAB);
        put64(&sh, 0);
        put64(&sh, 0);
        put64(&sh, sh_off + (uint64_t)nsecs * 64 + shs_size);
        put64(&sh, symtab_size);
        put32(&sh, (uint32_t)sh_strtab);
        put32(&sh, 1); /* info: one local (the null symbol) */
        put64(&sh, 8);
        put64(&sh, 24);
        idx++;
        /* .strtab */
        put32(&sh, shs_off[6]);
        put32(&sh, SHT_STRTAB);
        put64(&sh, 0);
        put64(&sh, 0);
        put64(&sh, sh_off + (uint64_t)nsecs * 64 + shs_size + symtab_size);
        put64(&sh, str_size);
        put32(&sh, 0);
        put32(&sh, 0);
        put64(&sh, 1);
        put64(&sh, 0);
        idx++;
        /* .shstrtab */
        put32(&sh, shs_off[7]);
        put32(&sh, SHT_STRTAB);
        put64(&sh, 0);
        put64(&sh, 0);
        put64(&sh, sh_off + (uint64_t)nsecs * 64);
        put64(&sh, shs_size);
        put32(&sh, 0);
        put32(&sh, 0);
        put64(&sh, 1);
        put64(&sh, 0);
        idx++;
        (void)idx;
    }

    /* String/symbol tables into the image (after the section headers). */
    size_t shdr_stride = L->is_32 ? 40 : 64;
    memcpy(img + sh_off + (uint64_t)nsecs * shdr_stride, shstrtab, shs_size);
    memcpy(img + sh_off + (uint64_t)nsecs * shdr_stride + shs_size, symtab,
           symtab_size);
    memcpy(img + sh_off + (uint64_t)nsecs * shdr_stride + shs_size + symtab_size,
           strtab, str_size);

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot write %s", out_path);
        goto oom;
    }
    if (fwrite(img, 1, file_size, f) != file_size) {
        snprintf(err, errsz, "short write on %s", out_path);
        fclose(f);
        goto oom;
    }
    fclose(f);
    chmod(out_path, 0755); /* directly executable */
    free(img); free(strtab); free(shstrtab); free(symtab); free(sym_stroff);
    return 0;

oom:
    free(img); free(strtab); free(shstrtab); free(symtab); free(sym_stroff);
    return -1;
}

int zero_cc_link_exec_ext(const char *out_path,
                          const char *const *objects, int object_count,
                          const char *const *archives, int archive_count,
                          const ZcLinkOpts *opts,
                          char *err, size_t errsz) {
    bool pie = opts ? opts->pie : false;
    const char *entry_wrapper = opts ? opts->entry_wrapper : NULL;
    const char *linker_script = opts ? opts->linker_script : NULL;
    bool baremetal = opts ? opts->baremetal : false;

    if (object_count == 0 && archive_count == 0) {
        snprintf(err, errsz, "no input files to link");
        return -1;
    }
    ZcLink *L = calloc(1, sizeof(ZcLink));
    if (!L) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    L->pie = pie;
    L->entry_wrapper = entry_wrapper;
    L->baremetal = baremetal;
    L->gc_sections = opts ? opts->gc_sections : false;
    L->print_gc_sections = opts ? opts->print_gc_sections : false;

    if (linker_script) {
        if (zero_cc_parse_linker_script_file(linker_script, &L->script, err, errsz) != 0) {
            free(L);
            return -1;
        }
        L->has_script = true;
        L->baremetal = true;
    }

    int rc = -1;
    for (int i = 0; i < object_count; i++) {
        if (load_input_file(L, objects[i], err, errsz) != 0) goto done;
    }
    if (pie && L->machine != EM_X86_64) {
        snprintf(err, errsz,
                 "static-PIE link (--zerocc-link-pie) is x86-64 only");
        goto done;
    }
    if (resolve_symbols(L, err, errsz) != 0) goto done;
    if (archive_count > 0) {
        if (extract_archives(L, archives, archive_count, err, errsz) != 0)
            goto done;
        if (resolve_symbols(L, err, errsz) != 0) goto done;
    }
    if (L->obj_count == 0) {
        snprintf(err, errsz,
                 "no input objects (archives %s contributed nothing)",
                 archive_count > 0 ? "did" : "were not given");
        goto done;
    }
    if (L->gc_sections) {
        if (gc_mark_and_sweep(L, err, errsz) != 0) goto done;
    }
    if (layout_sections(L, err, errsz) != 0) goto done;
    if (pie && !L->stub_at_front) {
        snprintf(err, errsz,
                 "static-PIE link requires the linker's self-relocating "
                 "_start stub; do not supply a CRT _start object");
        goto done;
    }
    if (apply_relocations(L, err, errsz) != 0) goto done;

    /* Entry: custom entry if defined by linker script, else _start if defined
     * (a CRT object), else Reset_Handler (baremetal), else the linker's _start
     * stub, which calls main (ZeroCC programs use the no-argument main). */
    uint64_t entry = 0, main_vma = 0;
    const ZcLinkSym *custom_entry = NULL, *start = NULL, *reset_handler = NULL, *main = NULL;
    for (size_t i = 0; i < L->sym_count; i++) {
        if (L->syms[i].obj < 0) continue;
        if (L->has_script && L->script.has_entry &&
            strcmp(L->syms[i].name, L->script.entry) == 0) {
            custom_entry = &L->syms[i];
        }
        if (strcmp(L->syms[i].name, "_start") == 0) start = &L->syms[i];
        if (strcmp(L->syms[i].name, "Reset_Handler") == 0) reset_handler = &L->syms[i];
        if (strcmp(L->syms[i].name, "main") == 0) main = &L->syms[i];
    }
    if (custom_entry) {
        entry = custom_entry->vma;
    } else if (start) {
        entry = start->vma;
    } else if (reset_handler) {
        entry = reset_handler->vma;
    } else if (L->stub_at_front) {
        if (!main) {
            snprintf(err, errsz,
                     "undefined reference to `main' "
                     "(provide a main() or a _start CRT object)");
            goto done;
        }
        /* zero_rt linked (its entry wrapper was force-extracted from the
         * archive) → the stub calls the wrapper, which runs main and then
         * exit(), so the atexit list runs on a plain `return`. A driver-
         * requested wrapper (-fsanitize=leak's zeroc_leak_main_wrapper)
         * wins when present; otherwise fall back to the default wrapper. */
        const ZcLinkSym *wrapper = NULL;
        for (size_t i = 0; i < L->sym_count; i++) {
            if (L->syms[i].obj >= 0 && L->entry_wrapper &&
                strcmp(L->syms[i].name, L->entry_wrapper) == 0) {
                wrapper = &L->syms[i];
                break;
            }
        }
        if (!wrapper) {
            for (size_t i = 0; i < L->sym_count; i++) {
                if (L->syms[i].obj >= 0 &&
                    strcmp(L->syms[i].name, RT_ENTRY_WRAPPER) == 0) {
                    wrapper = &L->syms[i];
                }
            }
        }
        main_vma = wrapper ? wrapper->vma : main->vma;
        entry = L->outsec[0].vma; /* the stub */
    } else if (main) {
        entry = main->vma;
        main_vma = main->vma;
    } else {
        snprintf(err, errsz,
                 "undefined reference to `main' "
                 "(provide a main(), Reset_Handler(), or a _start CRT object)");
        goto done;
    }
    if (emit_exec(L, out_path, entry, main_vma, err, errsz) != 0) goto done;
    rc = 0;

done:
    for (int i = 0; i < L->obj_count; i++) {
        if (L->sec_live[i]) free(L->sec_live[i]);
        zc_elf_obj_free(L->objs[i]);
        free(L->objs[i]);
    }
    for (int i = 0; i < 4; i++) free(L->outsec[i].content);
    free(L->syms);
    free(L->sym_hash);
    free(L->map);
    free(L->rela_dyn);
    free(L);
    return rc;
}

int zero_cc_link_exec(const char *out_path,
                      const char *const *objects, int object_count,
                      const char *const *archives, int archive_count,
                      const char *entry_wrapper,
                      char *err, size_t errsz) {
    ZcLinkOpts opts = {0};
    opts.entry_wrapper = entry_wrapper;
    return zero_cc_link_exec_ext(out_path, objects, object_count, archives,
                                 archive_count, &opts, err, errsz);
}

int zero_cc_link_exec_pie(const char *out_path,
                          const char *const *objects, int object_count,
                          const char *const *archives, int archive_count,
                          const char *entry_wrapper,
                          char *err, size_t errsz) {
    ZcLinkOpts opts = {0};
    opts.entry_wrapper = entry_wrapper;
    opts.pie = true;
    return zero_cc_link_exec_ext(out_path, objects, object_count, archives,
                                 archive_count, &opts, err, errsz);
}
