/* elf_reader.c - ELF64-LE ET_REL parser for the ZeroCC own-linker (T2). */
#include "zero_cc/linker/elf_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define ET_REL 1
#define EM_X86_64 62

#define EI_NIDENT 16

/* Little-endian field loads (the host is x86-64/LE, but be explicit so the
 * reader is not silently host-endian-dependent). */
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* Section header: 40 bytes, field order per the ELF spec. */
typedef struct {
    uint32_t name, type, flags;
    uint64_t addr, offset, size, link;
    uint32_t info, addralign;
} ZcShdr;

/* Elf64_Sym: 24 bytes */
typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} ZcSym;

/* Elf64_Rela: 24 bytes */
typedef struct {
    uint64_t offset, info;
    int64_t addend;
} ZcRela;

#define ZC_SHN_UNDEF 0
#define ZC_SHN_ABS 0xfff1
#define ZC_SHN_COMMON 0xfff2

#define ZC_STB_LOCAL 0
#define ZC_STB_GLOBAL 1
#define ZC_STB_WEAK 2
#define ZC_STT_NOTYPE 0
#define ZC_STT_OBJECT 1
#define ZC_STT_FUNC 2
#define ZC_STT_SECTION 3

static char *xstrdup(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static int obj_load_core(const uint8_t *buf, size_t size, const char *path,
                         ZcElfObj *o, char *err, size_t errsz) {
    if (size < EI_NIDENT + 64) {
        snprintf(err, errsz, "%s: file too small to be an ELF file", path);
        return -1;
    }
    if (memcmp(buf, "\x7f" "ELF", 4) != 0) {
        snprintf(err, errsz, "%s: not an ELF file", path);
        return -1;
    }
    if (buf[4] != 2) {
        snprintf(err, errsz, "%s: only 64-bit ELF is supported", path);
        return -1;
    }
    if (buf[5] != 1) {
        snprintf(err, errsz, "%s: only little-endian ELF is supported", path);
        return -1;
    }
    uint16_t e_type = le16(buf + 16);
    if (e_type != ET_REL) {
        snprintf(err, errsz, "%s: not a relocatable object (e_type=%u); "
                 "the own linker links .o inputs, not executables", path,
                 (unsigned)e_type);
        return -1;
    }
    if (le16(buf + 18) != EM_X86_64) {
        snprintf(err, errsz, "%s: wrong ELF machine (only x86-64 objects are "
                 "linkable for now)", path);
        return -1;
    }

    o->data = (uint8_t *)malloc(size);
    if (!o->data) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    memcpy(o->data, buf, size);
    o->size = size;
    o->path = strdup(path);

    uint64_t shoff = le64(buf + 40);
    uint16_t shentsize = le16(buf + 58);
    uint16_t shnum = le16(buf + 60);
    uint16_t shstrndx = le16(buf + 62);
    (void)shentsize;
    if (shnum == 0 || shnum > ZC_ELF_MAX_SECTIONS || shoff == 0 ||
        shoff + (uint64_t)shnum * 64 > size) {
        snprintf(err, errsz, "%s: bad section header table", path);
        return -1;
    }
    if (shstrndx >= shnum) {
        snprintf(err, errsz, "%s: bad shstrndx", path);
        return -1;
    }

    o->shnum = shnum;
    o->secs = (ZcSection *)calloc(shnum, sizeof(ZcSection));
    if (!o->secs) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    const uint8_t *shstrtab = NULL;
    size_t shstrtab_size = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *p = buf + shoff + (size_t)i * 64;
        ZcShdr h;
        h.name = le32(p);
        h.type = le32(p + 4);
        h.flags = le32(p + 8);
        h.addr = le64(p + 16);
        h.offset = le64(p + 24);
        h.size = le64(p + 32);
        h.link = le32(p + 40);
        h.info = le32(p + 44);
        h.addralign = le64(p + 48);

        ZcSection *s = &o->secs[i];
        s->type = (uint16_t)h.type;
        s->flags = h.flags;
        s->addr = h.addr;
        s->offset = h.offset;
        s->size = h.size;
        s->link = h.link;
        s->info = h.info;
        s->addralign = h.addralign ? h.addralign : 1;
        s->name = NULL;

        if (h.type == SHT_STRTAB && (size_t)h.offset + h.size > size) {
            snprintf(err, errsz, "%s: strtab section out of bounds", path);
            return -1;
        }
        if (h.type == SHT_PROGBITS && (size_t)h.offset + h.size > size) {
            snprintf(err, errsz, "%s: section data out of bounds", path);
            return -1;
        }
        if (i == shstrndx && h.type == SHT_STRTAB) {
            shstrtab = buf + h.offset;
            shstrtab_size = h.size;
        }
    }

    /* Section names (bounds-checked against the shstrtab). */
    for (uint16_t i = 0; i < shnum; i++) {
        ZcSection *s = &o->secs[i];
        uint32_t n = le32(buf + shoff + (size_t)i * 64);
        if (n >= shstrtab_size) {
            s->name = xstrdup("", 0);
        } else {
            const char *np = (const char *)(shstrtab + n);
            size_t len = 0;
            while (n + len < shstrtab_size && np[len] != '\0') len++;
            s->name = xstrdup(np, len);
        }
        if (!s->name) {
            snprintf(err, errsz, "out of memory");
            return -1;
        }
        if (strcmp(s->name, ".text") == 0) o->sec_text = i;
        else if (strcmp(s->name, ".rodata") == 0) o->sec_rodata = i;
        else if (strcmp(s->name, ".data") == 0) o->sec_data = i;
        else if (strcmp(s->name, ".bss") == 0) o->sec_bss = i;
        else if (strcmp(s->name, ".symtab") == 0) o->sec_symtab = i;
    }

    /* Symbol tables. */
    for (uint16_t i = 0; i < shnum; i++) {
        ZcSection *s = &o->secs[i];
        if (s->type != SHT_SYMTAB) continue;
        if (s->link >= shnum || o->secs[s->link].type != SHT_STRTAB) {
            snprintf(err, errsz, "%s: symtab %u has no strtab", path, i);
            return -1;
        }
        const ZcSection *st = &o->secs[s->link];
        s->strtab = (const char *)(buf + st->offset);
        size_t entsize = 24;
        if (s->size % entsize != 0) {
            snprintf(err, errsz, "%s: bad symtab size", path);
            return -1;
        }
        size_t cnt = s->size / entsize;
        if (cnt > ZC_ELF_MAX_SYMBOLS) {
            snprintf(err, errsz, "%s: too many symbols (%zu)", path, cnt);
            return -1;
        }
        s->syms = (ZcSymbol *)calloc(cnt ? cnt : 1, sizeof(ZcSymbol));
        if (!s->syms) {
            snprintf(err, errsz, "out of memory");
            return -1;
        }
        s->sym_count = cnt;
        for (size_t j = 0; j < cnt; j++) {
            const uint8_t *p = buf + s->offset + j * entsize;
            ZcSym sym;
            sym.name = le32(p);
            sym.info = p[4];
            sym.other = p[5];
            sym.shndx = le16(p + 6);
            sym.value = le64(p + 8);
            sym.size = le64(p + 16);
            ZcSymbol *z = &s->syms[j];
            z->value = sym.value;
            z->size = sym.size;
            z->bind = sym.info >> 4;
            z->type = sym.info & 0xf;
            z->shndx = sym.shndx;
            if (sym.name >= st->size) {
                z->name = xstrdup("", 0);
            } else {
                const char *np = (const char *)(buf + st->offset + sym.name);
                size_t len = 0;
                while (sym.name + len < st->size && np[len] != '\0') len++;
                z->name = xstrdup(np, len);
            }
            if (!z->name) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
        }
    }

    /* RELA sections (bound to their target via sh_info). */
    for (uint16_t i = 0; i < shnum; i++) {
        ZcSection *s = &o->secs[i];
        if (s->type != SHT_RELA) continue;
        if (s->info >= shnum) {
            snprintf(err, errsz, "%s: bad reloc target section", path);
            return -1;
        }
        size_t cnt = s->size / 24;
        if (cnt > ZC_ELF_MAX_RELOCS) {
            snprintf(err, errsz, "%s: too many relocations (%zu)", path, cnt);
            return -1;
        }
        s->relocs = (ZcReloc *)calloc(cnt ? cnt : 1, sizeof(ZcReloc));
        if (!s->relocs) {
            snprintf(err, errsz, "out of memory");
            return -1;
        }
        s->reloc_count = cnt;
        for (size_t j = 0; j < cnt; j++) {
            const uint8_t *p = buf + s->offset + j * 24;
            ZcRela r;
            r.offset = le64(p);
            r.info = le64(p + 8);
            r.addend = (int64_t)le64(p + 16);
            s->relocs[j].offset = r.offset;
            s->relocs[j].sym = (uint32_t)(r.info >> 32);
            s->relocs[j].type = (uint32_t)(r.info & 0xffffffff);
            s->relocs[j].addend = r.addend;
        }
    }
    return 0;
}

int zc_elf_obj_load(const char *path, ZcElfObj *o, char *err, size_t errsz) {
    memset(o, 0, sizeof(*o));
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
    uint8_t *buf = (uint8_t *)malloc(sz ? (size_t)sz : 1);
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
    int rc = obj_load_core(buf, (size_t)sz, path, o, err, errsz);
    free(buf);
    return rc;
}

int zc_elf_obj_load_mem(const uint8_t *data, size_t size, const char *path,
                        ZcElfObj *o, char *err, size_t errsz) {
    memset(o, 0, sizeof(*o));
    return obj_load_core(data, size, path, o, err, errsz);
}

void zc_elf_obj_free(ZcElfObj *o) {
    if (!o) return;
    if (o->secs) {
        for (uint16_t i = 0; i < o->shnum; i++) {
            ZcSection *s = &o->secs[i];
            free(s->name);
            if (s->syms) {
                for (size_t j = 0; j < s->sym_count; j++) free(s->syms[j].name);
                free(s->syms);
            }
            free(s->relocs);
        }
        free(o->secs);
    }
    free(o->data);
    free(o->path);
    memset(o, 0, sizeof(*o));
}

ZcSection *zc_elf_find_section(ZcElfObj *o, const char *name) {
    for (uint16_t i = 0; i < o->shnum; i++) {
        if (strcmp(o->secs[i].name, name) == 0) return &o->secs[i];
    }
    return NULL;
}

const ZcSection *zc_elf_symtab(ZcElfObj *o) {
    return zc_elf_find_section(o, ".symtab");
}

const ZcSymbol *zc_elf_find_symbol(ZcElfObj *o, const char *name) {
    const ZcSection *st = zc_elf_symtab(o);
    if (!st) return NULL;
    for (size_t i = 0; i < st->sym_count; i++) {
        if (strcmp(st->syms[i].name, name) == 0) return &st->syms[i];
    }
    return NULL;
}
