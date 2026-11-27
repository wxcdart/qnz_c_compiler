/**
 * @file coff_reader.h
 * @brief Windows PE/COFF Relocatable Object (.obj) Reader for ZeroCC (Sprint 259)
 *
 * Provides parsing for COFF relocatable object files:
 * - Header validation (IMAGE_FILE_MACHINE_AMD64 = 0x8664)
 * - Section headers (.text, .data, .rdata, .bss) and raw payload pointers
 * - COFF Symbol Table (IMAGE_SYMBOL) and String Table
 * - Section Relocation Table (IMAGE_REL_AMD64_ADDR64, REL32, ADDR32NB)
 */

#ifndef ZERO_CC_LINKER_COFF_READER_H
#define ZERO_CC_LINKER_COFF_READER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "zero_cc/linker/pe_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZERO_CC_COFF_MAX_SECTIONS 64
#define ZERO_CC_COFF_MAX_SYMBOLS  4096

typedef struct {
    uint32_t virtual_address;    /* Offset within section */
    uint32_t symbol_index;       /* Index in symbol table */
    uint16_t type;               /* Relocation type (e.g. ZERO_CC_IMAGE_REL_AMD64_*) */
} ZeroCCCoffRelocParsed;

typedef struct {
    char name[128];
    uint32_t value;              /* Offset in section or absolute */
    int16_t section_number;      /* 1-based section index (0 = UNDEF) */
    uint16_t type;               /* 0x20 for function */
    uint8_t storage_class;       /* EXTERNAL (2) or STATIC (3) */
} ZeroCCCoffSymParsed;

typedef struct {
    char name[16];               /* Section name (e.g. ".text", ".data", ".rdata", ".bss") */
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t characteristics;
    const uint8_t *data;         /* Pointer into file_data */
    ZeroCCCoffRelocParsed *relocs;
    size_t reloc_count;
} ZeroCCCoffSectParsed;

typedef struct {
    char name[256];              /* File path */
    uint8_t *file_data;
    size_t file_size;
    uint16_t machine;

    ZeroCCCoffSectParsed sections[ZERO_CC_COFF_MAX_SECTIONS];
    size_t section_count;

    ZeroCCCoffSymParsed symbols[ZERO_CC_COFF_MAX_SYMBOLS];
    size_t symbol_count;

    const char *strtab;
    uint32_t strsize;
} ZeroCCCoffParsedObject;

/**
 * @brief Parse COFF relocatable object (.obj) from memory buffer.
 */
bool zero_cc_coff_read_object_mem(const uint8_t *data, size_t size,
                                  const char *name, ZeroCCCoffParsedObject *obj,
                                  char *err, size_t err_size);

/**
 * @brief Parse COFF relocatable object (.obj) from a file path.
 */
bool zero_cc_coff_read_object_file(const char *path, ZeroCCCoffParsedObject *obj,
                                   char *err, size_t err_size);

/**
 * @brief Free parsed COFF object resources.
 */
void zero_cc_coff_parsed_object_free(ZeroCCCoffParsedObject *obj);

/**
 * @brief Find section by name in parsed COFF object.
 */
const ZeroCCCoffSectParsed *zero_cc_coff_find_section(const ZeroCCCoffParsedObject *obj,
                                                      const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_LINKER_COFF_READER_H */
