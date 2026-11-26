/**
 * @file macho_reader.h
 * @brief macOS Mach-O 64-bit Relocatable Object (MH_OBJECT) Reader for ZeroCC (Sprint 255)
 *
 * Provides parsing for Mach-O 64-bit relocatable object files (.o):
 * - Mach-O header validation (magic 0xfeedfacf, CPU type, MH_OBJECT)
 * - Load commands: LC_SEGMENT_64 (sections __text, __cstring, __data, __bss)
 * - Symbol table (LC_SYMTAB): nlist_64 symbols and string table
 * - Section relocation entries (ZeroCCMachORelocation)
 */

#ifndef ZERO_CC_LINKER_MACHO_READER_H
#define ZERO_CC_LINKER_MACHO_READER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "zero_cc/linker/macho_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZERO_CC_MACHO_MAX_SECTIONS 64
#define ZERO_CC_MACHO_MAX_SYMBOLS  4096

typedef struct {
    int32_t address;     /* Offset within the section */
    uint32_t symbolnum;  /* Symbol index (if is_extern) or 1-based section ordinal */
    bool is_pcrel;       /* PC-relative relocation */
    uint8_t length;      /* 0: 1-byte, 1: 2-byte, 2: 4-byte, 3: 8-byte */
    bool is_extern;      /* 1 = symbolnum is symbol index, 0 = section ordinal */
    uint8_t type;        /* Relocation type */
} ZeroCCMachoRelocParsed;

typedef struct {
    char name[128];
    uint8_t type;        /* N_TYPE field: N_UNDF, N_SECT, N_ABS, etc. */
    bool is_extern;      /* N_EXT flag */
    uint8_t sect;        /* 1-based section index (NO_SECT = 0) */
    uint16_t desc;
    uint64_t value;      /* Offset in section or address */
} ZeroCCMachoSymParsed;

typedef struct {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;      /* Power of 2 (e.g. 4 => 16 bytes) */
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    const uint8_t *data; /* Pointer into file_data */
    ZeroCCMachoRelocParsed *relocs;
    size_t reloc_count;
} ZeroCCMachoSectParsed;

typedef struct {
    char name[256];      /* File path or TU identifier */
    uint8_t *file_data;
    size_t file_size;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;

    ZeroCCMachoSectParsed sections[ZERO_CC_MACHO_MAX_SECTIONS];
    size_t section_count;

    ZeroCCMachoSymParsed symbols[ZERO_CC_MACHO_MAX_SYMBOLS];
    size_t symbol_count;

    const char *strtab;
    uint32_t strsize;
} ZeroCCMachoParsedObject;

/**
 * @brief Parse Mach-O 64-bit object from memory buffer.
 */
bool zero_cc_macho_read_object_mem(const uint8_t *data, size_t size,
                                   const char *name, ZeroCCMachoParsedObject *obj,
                                   char *err, size_t err_size);

/**
 * @brief Parse Mach-O 64-bit object from a file path.
 */
bool zero_cc_macho_read_object_file(const char *path, ZeroCCMachoParsedObject *obj,
                                    char *err, size_t err_size);

/**
 * @brief Free resources allocated during Mach-O object parsing.
 */
void zero_cc_macho_parsed_object_free(ZeroCCMachoParsedObject *obj);

/**
 * @brief Find a section in a parsed Mach-O object by section name and segment name.
 */
const ZeroCCMachoSectParsed *zero_cc_macho_find_section(const ZeroCCMachoParsedObject *obj,
                                                        const char *segname,
                                                        const char *sectname);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_LINKER_MACHO_READER_H */
