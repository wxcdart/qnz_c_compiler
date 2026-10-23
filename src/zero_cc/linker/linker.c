/* linker.c - ZeroCC's own static x86-64 linker (T2).
 *
 * Pipeline (see linker.h):
 *   1. load input objects
 *   2. resolve global symbols (builds the name table incl. UNDs)
 *   3. extract archive members that resolve UNDs (greedy to fixpoint)
 *   4. re-resolve (duplicate-definition check now covers archive members)
 *   5. lay out .text/.rodata/.data/.bss at the conventional static base
 *   6. apply relocations (R_X86_64_{64,PC32,PLT32,32,32S,RELATIVE})
 *   7. emit ET_EXEC with 2 PT_LOAD segments (RX, RW)
 *
 * Layout (Linux x86-64 non-PIE convention):
 *   0x400000  ELF header + program headers
 *   0x401000  .text
 *   ...       .rodata (8-aligned)
 *   page      .data  (page-aligned: separate RW segment)
 *   ...       .bss   (page-aligned when .data is empty)
 */
#include "zero_cc/linker/linker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "zero_cc/linker/elf_reader.h"
#include "zero_cc/linker/ar_reader.h"

#define ZC_MAX_INPUTS 256
#define ZC_MAX_SYMS 16384
#define ZC_MAX_NAME 512

#define ZC_TEXT_BASE 0x401000ULL
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
#define EM_X86_64 62
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ZC_SHN_ABS 0xfff1
#define ZC_SHN_COMMON 0xfff2

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
    bool is_section;    /* section symbol: vma follows its output section */
    uint64_t sec_off;   /* for section symbols: offset within the section */
    uint64_t size;
    uint8_t stype;      /* STT for the output symtab */
} ZcLinkSym;

typedef struct {
    char name[16];
    bool write, exec, nobits;
    uint64_t vma;
    uint64_t file_off;
    uint64_t size;      /* content size written to file (bss: 0) */
    uint64_t memsz;     /* .bss virtual size */
    uint64_t cursor;
    uint8_t *content;
    size_t cap;
} ZcOutSec;

typedef struct {
    ZcElfObj *obj;
    uint32_t insec;
    uint64_t out_off;   /* offset of this input section in the out section */
} ZcInsecMap;

typedef struct {
    ZcLinkSym *syms;
    size_t sym_count, sym_cap;

    ZcElfObj *objs[ZC_MAX_INPUTS];
    int obj_count;

    ZcOutSec outsec[4]; /* .text .rodata .data .bss */
    ZcInsecMap *map;
    size_t map_count, map_cap;

    bool have_bss;
    bool stub_at_front; /* linker-generated _start stub in .text[0..16) */
} ZcLink;

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static bool is_power_of_two_64(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---------------- symbol table ---------------- */

static ZcLinkSym *sym_get(ZcLink *L, const char *name) {
    for (size_t i = 0; i < L->sym_count; i++) {
        if (strcmp(L->syms[i].name, name) == 0) return &L->syms[i];
    }
    if (L->sym_count >= L->sym_cap) {
        if (L->sym_cap >= ZC_MAX_SYMS) return NULL;
        size_t nc = L->sym_cap ? L->sym_cap * 2 : 1024;
        if (nc > ZC_MAX_SYMS) nc = ZC_MAX_SYMS;
        ZcLinkSym *n = realloc(L->syms, nc * sizeof(ZcLinkSym));
        if (!n) return NULL;
        L->syms = n;
        L->sym_cap = nc;
    }
    ZcLinkSym *s = &L->syms[L->sym_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, ZC_MAX_NAME, "%s", name);
    s->obj = -1;
    s->sym = -1;
    return s;
}

static ZcLinkSym *sym_find(ZcLink *L, const char *name) {
    for (size_t i = 0; i < L->sym_count; i++) {
        if (strcmp(L->syms[i].name, name) == 0) return &L->syms[i];
    }
    return NULL;
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

/* ---------------- archive extraction ---------------- */

static bool member_resolves_need(ZcLink *L, ZcElfObj *m) {
    const ZcSection *st = zc_elf_symtab(m);
    if (!st) return false;
    for (size_t i = 1; i < st->sym_count; i++) {
        const ZcSymbol *s = &st->syms[i];
        if (s->bind == 0 || s->name[0] == '\0' || s->shndx == 0) continue;
        ZcLinkSym *ls = sym_find(L, s->name);
        if (ls && ls->obj == -1 && !ls->is_common) return true;
    }
    return false;
}

static int extract_archives(ZcLink *L, const char *const *archives,
                            int archive_count, char *err, size_t errsz) {
    ZcAr ar[ZC_MAX_INPUTS];
    int ar_count = 0;
    for (int i = 0; i < archive_count && ar_count < ZC_MAX_INPUTS; i++) {
        if (zc_ar_open(archives[i], &ar[ar_count], err, errsz) != 0) return -1;
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
            return -1;
        }
        for (int a = 0; a < ar_count; a++) {
            for (size_t m = 0; m < ar[a].member_count; m++) {
                const ZcArMember *mem = &ar[a].members[m];
                bool in = false;
                for (int k = 0; k < extracted_count; k++) {
                    if (extracted[k] == mem->data) { in = true; break; }
                }
                if (in) continue;
                ZcElfObj probe;
                if (zc_elf_obj_load_mem(mem->data, mem->size, mem->name,
                                        &probe, err, errsz) != 0) {
                    continue; /* not an ELF (index etc.) - skip */
                }
                bool take = member_resolves_need(L, &probe);
                zc_elf_obj_free(&probe);
                if (take) {
                    char mpath[600];
                    snprintf(mpath, sizeof(mpath), "%s(%s)",
                             ar[a].path, mem->name);
                    if (add_object(L, mem->data, mem->size, mpath,
                                   err, errsz) != 0) {
                        return -1;
                    }
                    if (extracted_count < ZC_AR_MAX_MEMBERS)
                        extracted[extracted_count++] = mem->data;
                    changed = true;
                }
            }
        }
    }
    for (int a = 0; a < ar_count; a++) zc_ar_free(&ar[a]);
    return 0;
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
    return -1;
}

static int layout_sections(ZcLink *L, char *err, size_t errsz) {
    const char *names[4] = {".text", ".rodata", ".data", ".bss"};
    for (int i = 0; i < 4; i++) {
        snprintf(L->outsec[i].name, sizeof(L->outsec[i].name), "%s", names[i]);
        L->outsec[i].write = (i == 2 || i == 3);
        L->outsec[i].exec = (i == 0);
        L->outsec[i].nobits = (i == 3);
    }

    /* If no input defines _start (a CRT object), reserve a 16-byte stub at
     * the front of .text:
     *   xor %rbp,%rbp ; call main ; mov %eax,%edi ; mov $60,%eax ; syscall
     * (call offset patched in emit_exec once main's vma is final). */
    bool have_start = false;
    for (size_t i = 0; i < L->sym_count; i++) {
        if (strcmp(L->syms[i].name, "_start") == 0 &&
            L->syms[i].obj >= 0) have_start = true;
    }
    L->stub_at_front = !have_start;

    for (int oi = 0; oi < L->obj_count; oi++) {
        for (int si = 0; si < L->objs[oi]->shnum; si++) {
            const ZcSection *ins = &L->objs[oi]->secs[si];
            if (ins->type != SHT_PROGBITS && ins->type != SHT_NOBITS) continue;
            if (!(ins->flags & SHF_ALLOC)) continue;
            int out = outsec_for_name(ins->name);
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
            if (out == 0 && os->cursor == 0 && L->stub_at_front) {
                if (outsec_zero_extend(os, 16) != 0) {
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
                os->memsz += ins->size;
            }
            if (out == 3) L->have_bss = true;
        }
    }

    /* VMAs: .text at the base; .rodata packed 8-aligned after it; .data and
     * .bss page-aligned (each may anchor the RW PT_LOAD segment). */
    uint64_t v = ZC_TEXT_BASE;
    for (int i = 0; i < 3; i++) {
        if (L->outsec[i].cursor == 0) continue;
        uint64_t al = (i == 0) ? 16 : ((i == 2) ? ZC_PAGE : 8);
        v = align_up(v, al);
        L->outsec[i].vma = v;
        L->outsec[i].size = L->outsec[i].cursor;
        v = L->outsec[i].vma + L->outsec[i].cursor;
    }
    if (L->have_bss) {
        uint64_t al = (L->outsec[2].cursor > 0) ? 8 : ZC_PAGE;
        v = align_up(v, al);
        L->outsec[3].vma = v;
        L->outsec[3].size = 0;
    }

    /* Finalize symbol VMAs. */
    for (size_t i = 0; i < L->sym_count; i++) {
        ZcLinkSym *ls = &L->syms[i];
        if (ls->is_section) {
            if (ls->obj < 0) continue;
            if (ls->sym < 0) continue;
            const ZcSection *ins = &L->objs[ls->obj]->secs[ls->sym];
            int out = outsec_for_name(ins->name);
            if (out < 0) continue;
            ls->vma = L->outsec[out].vma + ls->sec_off;
        } else if (ls->is_common) {
            ZcOutSec *d = &L->outsec[2];
            if (d->vma == 0) {
                /* No .data input at all: give the section a vma. */
                d->vma = align_up(v, ZC_PAGE);
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
            int out = outsec_for_name(secname);
            if (out < 0) continue;
            uint64_t in_off = 0;
            for (size_t m = 0; m < L->map_count; m++) {
                if (L->map[m].obj == o && L->map[m].insec == sym->shndx) {
                    in_off = L->map[m].out_off;
                    break;
                }
            }
            ls->vma = L->outsec[out].vma + in_off + sym->value;
        }
    }
    return 0;
}

/* ---------------- relocations ---------------- */

static int apply_relocations(ZcLink *L, char *err, size_t errsz) {
    for (int oi = 0; oi < L->obj_count; oi++) {
        ZcElfObj *o = L->objs[oi];
        for (int si = 0; si < o->shnum; si++) {
            const ZcSection *rs = &o->secs[si];
            if (rs->type != SHT_RELA) continue;
            if (rs->info >= o->shnum) continue;
            const ZcSection *ins = &o->secs[rs->info];
            int out = outsec_for_name(ins->name);
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
                    /* UND: resolved as a definition, or a hard error. */
                    ZcLinkSym *ls = sym_find(L, sym->name);
                    if (!ls || (ls->obj == -1 && !ls->is_section)) {
                        snprintf(err, errsz,
                                 "undefined symbol: `%s' (referenced by %s)",
                                 sym->name, o->path);
                        return -1;
                    }
                    S = (int64_t)ls->vma;
                } else if (sym->type == ZC_STT_SECTION) {
                    ZcLinkSym *ls = sym_find(L, sym->name);
                    if (!ls || !ls->is_section || ls->vma == 0) {
                        /* Section of an object with no matching output
                         * section: resolve against its own object. */
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
                        S = (int64_t)ls->vma;
                    }
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
                if (P < os->vma ||
                    P - os->vma >= (os->nobits ? os->memsz : os->size)) {
                    snprintf(err, errsz,
                             "relocation at 0x%lx outside %s of %s",
                             (unsigned long)P, os->name, o->path);
                    return -1;
                }
                uint8_t *site = os->content + (P - os->vma);
                switch (rel->type) {
                case 1:  /* R_X86_64_64 */
                    for (int b = 0; b < 8; b++)
                        site[b] = (uint8_t)((uint64_t)(S + A) >> (8 * b));
                    break;
                case 2:  /* R_X86_64_PC32 */
                case 4:  /* R_X86_64_PLT32 (static: no PLT stubs needed) */
                {
                    int32_t d = (int32_t)(S + A - (int64_t)P);
                    for (int b = 0; b < 4; b++)
                        site[b] = (uint8_t)((uint32_t)d >> (8 * b));
                    break;
                }
                case 10: /* R_X86_64_32 */
                    for (int b = 0; b < 4; b++)
                        site[b] = (uint8_t)((uint32_t)(uint64_t)(S + A) >> (8 * b));
                    break;
                case 11: /* R_X86_64_32S */
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
    uint64_t rx_filesz = rx_end_vma - rx_vma; /* includes inter-section gaps */
    text->file_off = rx_off;
    rodata->file_off = rx_off + (rodata->vma - text->vma);

    uint64_t rw_off = 0, rw_vma = 0;
    if (have_rw) {
        rw_vma = data->size > 0 ? data->vma : bss->vma;
        rw_off = align_up(rx_off + rx_filesz, ZC_PAGE);
        /* keep p_offset congruent to p_vaddr mod page */
        rw_off += (rw_vma - rw_off) & (ZC_PAGE - 1);
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
    int sh_symtab = 1 + (out_shndx[0] != 0) + (out_shndx[1] != 0) +
                    (out_shndx[2] != 0) + (out_shndx[3] != 0);
    int sh_strtab = sh_symtab + 1;
    int sh_shstrtab = sh_symtab + 2;
    int nsecs = sh_shstrtab + 1;

    /* Symbol table contents: null + resolved globals + section symbols. */
    size_t nsyms = 1;
    for (size_t i = 0; i < L->sym_count; i++) {
        ZcLinkSym *ls = &L->syms[i];
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
    uint32_t sym_stroff[ZC_MAX_SYMS];
    for (size_t i = 0; i < L->sym_count; i++) {
        sym_stroff[i] = (uint32_t)str_size;
        size_t n = strlen(L->syms[i].name);
        memcpy(strtab + str_size, L->syms[i].name, n + 1);
        str_size += n + 1;
    }

    const char *secnames[7] = {
        ".text", ".rodata", ".data", ".bss", ".symtab", ".strtab", ".shstrtab"
    };
    size_t shs_size = 0;
    uint32_t shs_off[7];
    {
        size_t off = 1; /* leading NUL */
        for (int i = 0; i < 7; i++) {
            shs_off[i] = (uint32_t)off;
            size_t n = strlen(secnames[i]);
            off += n + 1;
        }
        shs_size = off;
    }
    uint8_t *shstrtab = calloc(1, shs_size);
    {
        size_t off = 1;
        for (int i = 0; i < 7; i++) {
            size_t n = strlen(secnames[i]);
            memcpy(shstrtab + off, secnames[i], n + 1);
            off += n + 1;
        }
    }

    size_t symtab_size = nsyms * 24;
    uint8_t *symtab = calloc(nsyms, 24);

    /* File layout: shdrs after the RW content, then shstrtab, symtab,
     * strtab. */
    uint64_t sh_off = have_rw
        ? align_up(rw_off + data->size, 8)
        : align_up(rx_off + rx_filesz, 8);
    uint64_t file_size = sh_off + (uint64_t)nsecs * 64 +
                         shs_size + symtab_size + str_size;

    uint8_t *img = calloc(1, file_size);
    if (!img) {
        snprintf(err, errsz, "out of memory");
        goto oom;
    }

    /* ELF header (e_entry = main or _start). */
    uint8_t *p = img;
    put8(&p, 0x7f); put8(&p, 'E'); put8(&p, 'L'); put8(&p, 'F');
    put8(&p, 2); /* 64-bit */
    put8(&p, 1); /* little endian */
    put8(&p, 1); /* ELF version */
    put8(&p, 0); /* SYSV */
    for (int i = 0; i < 8; i++) put8(&p, 0);
    put16(&p, ET_EXEC);
    put16(&p, EM_X86_64);
    put32(&p, 1);          /* e_version */
    put64(&p, entry);      /* e_entry */
    put64(&p, 64);         /* e_phoff */
    put64(&p, sh_off);     /* e_shoff */
    put32(&p, 0);          /* e_flags */
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
        put32(&p, PF_R | PF_X);
        put64(&p, rx_off);
        put64(&p, rx_vma);
        put64(&p, rx_vma);
        put64(&p, rx_filesz);
        put64(&p, rx_filesz);
        put64(&p, ZC_PAGE);
    }
    if (have_rw) {
        put32(&p, PT_LOAD);
        put32(&p, PF_R | PF_W);
        put64(&p, rw_off);
        put64(&p, rw_vma);
        put64(&p, rw_vma);
        put64(&p, data->size);
        put64(&p, data->size + (L->have_bss ? bss->memsz : 0));
        put64(&p, ZC_PAGE);
    }

    /* Section contents. */
    if (text->size) memcpy(img + text->file_off, text->content, text->size);
    if (rodata->size)
        memcpy(img + rodata->file_off, rodata->content, rodata->size);
    if (data->size) memcpy(img + data->file_off, data->content, data->size);

    /* Patch the _start stub's call to main (stub at .text offset 0:
     *   +0: 31 f9          xor %rbp,%rbp
     *   +2: e8 xx xx xx xx call main
     *   +7: 89 c7          mov %eax,%edi
     *   +9: b8 3c 00 00 00 mov $60,%eax
     *   +14: 0f 05         syscall */
    if (L->stub_at_front) {
        uint8_t *st = img + text->file_off;
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

    /* .symtab */
    {
        uint8_t *sp = symtab;
        /* null symbol */
        memset(sp, 0, 24);
        sp += 24;
        for (size_t i = 0; i < L->sym_count; i++) {
            ZcLinkSym *ls = &L->syms[i];
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
            put32(&sp, shs_off[k]);
            *sp = (uint8_t)((0 << 4) | ZC_STT_SECTION);
            sp++;
            *sp = 0; sp++;
            put16(&sp, (uint16_t)out_shndx[k]);
            put64(&sp, os->vma);
            put64(&sp, os->nobits ? os->memsz : os->size);
        }
    }

    /* Section headers. */
    {
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
            put64(&sh, 8);  /* addralign */
            put64(&sh, 0);  /* entsize */
            idx++;
        }
        /* .symtab */
        put32(&sh, shs_off[4]);
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
        put32(&sh, shs_off[5]);
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
        put32(&sh, shs_off[6]);
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
    memcpy(img + sh_off + (uint64_t)nsecs * 64, shstrtab, shs_size);
    memcpy(img + sh_off + (uint64_t)nsecs * 64 + shs_size, symtab,
           symtab_size);
    memcpy(img + sh_off + (uint64_t)nsecs * 64 + shs_size + symtab_size,
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
    free(img); free(strtab); free(shstrtab); free(symtab);
    return 0;

oom:
    free(img); free(strtab); free(shstrtab); free(symtab);
    return -1;
}

int zero_cc_link_exec(const char *out_path,
                      const char *const *objects, int object_count,
                      const char *const *archives, int archive_count,
                      char *err, size_t errsz) {
    if (object_count == 0 && archive_count == 0) {
        snprintf(err, errsz, "no input files to link");
        return -1;
    }
    ZcLink *L = calloc(1, sizeof(ZcLink));
    if (!L) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    int rc = -1;
    for (int i = 0; i < object_count; i++) {
        if (load_input_file(L, objects[i], err, errsz) != 0) goto done;
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
    if (layout_sections(L, err, errsz) != 0) goto done;
    if (apply_relocations(L, err, errsz) != 0) goto done;

    /* Entry: _start if defined (a CRT object), else the linker's _start
     * stub, which calls main (ZeroCC programs use the no-argument main). */
    uint64_t entry = 0, main_vma = 0;
    const ZcLinkSym *start = NULL, *main = NULL;
    for (size_t i = 0; i < L->sym_count; i++) {
        if (L->syms[i].obj < 0) continue;
        if (strcmp(L->syms[i].name, "_start") == 0) start = &L->syms[i];
        if (strcmp(L->syms[i].name, "main") == 0) main = &L->syms[i];
    }
    if (start) {
        entry = start->vma;
    } else if (L->stub_at_front) {
        if (!main) {
            snprintf(err, errsz,
                     "undefined reference to `main' "
                     "(provide a main() or a _start CRT object)");
            goto done;
        }
        main_vma = main->vma;
        entry = L->outsec[0].vma; /* the stub */
    } else if (main) {
        entry = main->vma;
        main_vma = main->vma;
    } else {
        snprintf(err, errsz,
                 "undefined reference to `main' "
                 "(provide a main() or a _start CRT object)");
        goto done;
    }
    if (emit_exec(L, out_path, entry, main_vma, err, errsz) != 0) goto done;
    rc = 0;

done:
    for (int i = 0; i < L->obj_count; i++) {
        zc_elf_obj_free(L->objs[i]);
        free(L->objs[i]);
    }
    for (int i = 0; i < 4; i++) free(L->outsec[i].content);
    free(L->syms);
    free(L->map);
    free(L);
    return rc;
}
