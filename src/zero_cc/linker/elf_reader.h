#ifndef ZERO_CC_LINKER_ELF_READER_H
#define ZERO_CC_LINKER_ELF_READER_H
/*
 * elf_reader.h - Minimal ELF64 little-endian object (ET_REL) reader for the
 * ZeroCC own-linker (T2).
 *
 * Parses the parts the linker needs: section headers + section name
 * resolution, the symbol table (with its strtab), and SHT_RELA relocation
 * sections (bound to their target section via sh_info). Everything is
 * copied into malloc'd buffers; zc_elf_obj_free releases them.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ZC_ELF_MAX_SECTIONS 512
#define ZC_ELF_MAX_SYMBOLS 16384
#define ZC_ELF_MAX_RELOCS  32768

/* x86-64 relocation types (the set the linker resolves; see linker.c) */
enum {
    ZC_R_X86_64_NONE = 0,
    ZC_R_X86_64_64 = 1,
    ZC_R_X86_64_PC32 = 2,
    ZC_R_X86_64_GOT32 = 3,
    ZC_R_X86_64_PLT32 = 4,
    ZC_R_X86_64_RELATIVE = 8,
    ZC_R_X86_64_GOTPCRELX = 9,
    ZC_R_X86_64_32 = 10,
    ZC_R_X86_64_32S = 11
};

typedef struct {
    uint64_t offset;      /* offset within the target section */
    uint32_t sym;         /* index into the object's symtab */
    uint32_t type;        /* ZC_R_X86_64_* */
    int64_t addend;
} ZcReloc;

typedef struct {
    uint64_t value;       /* offset within st_shndx's section (0 for UND) */
    uint64_t size;
    uint8_t bind;         /* STB_LOCAL/GLOBAL/WEAK */
    uint8_t type;         /* STT_NOTYPE/OBJECT/FUNC/SECTION */
    uint16_t shndx;       /* SHN_UNDEF/ABS/COMMON or a section index */
    char *name;           /* copied, NUL-terminated */
} ZcSymbol;

typedef struct {
    char *name;           /* copied, NUL-terminated */
    uint16_t type;        /* SHT_NULL/PROGBITS/STRTAB/SYMTAB/RELA/NOBITS */
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;      /* file offset (valid for PROGBITS) */
    uint64_t size;
    uint32_t link;        /* linked-to section (strtab for symtab) */
    uint32_t info;        /* for RELA/SYMTAB: target section / local syms */
    uint64_t addralign;
    /* SHT_RELA: relocations against section `info` */
    ZcReloc *relocs;
    size_t reloc_count;
    /* SHT_SYMTAB */
    ZcSymbol *syms;
    size_t sym_count;
    const char *strtab;   /* into the owning obj's strtab section data */
} ZcSection;

typedef struct {
    /* Raw file image (ELF header validated) */
    uint8_t *data;
    size_t size;
    char *path;           /* for diagnostics */

    uint16_t shnum;
    ZcSection *secs;      /* [shnum] */

    /* Convenience lookups (indices into secs) */
    int sec_text;
    int sec_rodata;
    int sec_data;
    int sec_bss;
    int sec_symtab;
} ZcElfObj;

/* Parse an ELF64-LE ET_REL file. Returns 0 on success, -1 with a message in
 * err. The object owns all returned memory. */
int zc_elf_obj_load(const char *path, ZcElfObj *out, char *err, size_t errsz);

/* Load from memory (ar members). data is copied. */
int zc_elf_obj_load_mem(const uint8_t *data, size_t size, const char *path,
                        ZcElfObj *out, char *err, size_t errsz);

void zc_elf_obj_free(ZcElfObj *o);

/* Section accessors (NULL when absent) */
ZcSection *zc_elf_find_section(ZcElfObj *o, const char *name);
const ZcSection *zc_elf_symtab(ZcElfObj *o);
const ZcSymbol *zc_elf_find_symbol(ZcElfObj *o, const char *name);

#endif /* ZERO_CC_LINKER_ELF_READER_H */
