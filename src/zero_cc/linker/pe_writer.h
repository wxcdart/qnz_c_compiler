/**
 * @file pe_writer.h
 * @brief PE/COFF (PE32+) Executable and Object File Writer for ZeroCC (Sprint 246)
 *
 * Provides native Windows PE32+ (x86-64) binary emission:
 * - Standalone PE32+ console executables (.exe) with MS-DOS stub, PE headers,
 *   sections (.text, .rdata, .data, .idata), and KERNEL32.dll import table.
 * - Relocatable COFF object files (.obj) with IMAGE_FILE_MACHINE_AMD64.
 */

#ifndef ZERO_CC_LINKER_PE_WRITER_H
#define ZERO_CC_LINKER_PE_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct ZeroCCCompiler;
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Standard PE/COFF Constants */
#define ZERO_CC_PE_DOS_MAGIC             0x5A4D /* "MZ" */
#define ZERO_CC_PE_SIGNATURE             0x00004550 /* "PE\0\0" */
#define ZERO_CC_IMAGE_FILE_MACHINE_AMD64 0x8664
#define ZERO_CC_PE32PLUS_MAGIC           0x020B
#define ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_GUI 2   /* Windows GUI Subsystem */
#define ZERO_CC_IMAGE_SUBSYSTEM_WINDOWS_CUI 3   /* Windows Console Subsystem */

/* Characteristics */
#define ZERO_CC_IMAGE_FILE_RELOCS_STRIPPED     0x0001
#define ZERO_CC_IMAGE_FILE_EXECUTABLE_IMAGE    0x0002
#define ZERO_CC_IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020
#define ZERO_CC_IMAGE_FILE_DLL                 0x2000

/* Data Directory Indices */
#define ZERO_CC_IMAGE_DIRECTORY_ENTRY_EXPORT   0
#define ZERO_CC_IMAGE_DIRECTORY_ENTRY_IMPORT   1
#define ZERO_CC_IMAGE_DIRECTORY_ENTRY_IAT      12

/* Section Characteristics */
#define ZERO_CC_IMAGE_SCN_CNT_CODE               0x00000020
#define ZERO_CC_IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define ZERO_CC_IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define ZERO_CC_IMAGE_SCN_MEM_EXECUTE            0x20000000
#define ZERO_CC_IMAGE_SCN_MEM_READ               0x40000000
#define ZERO_CC_IMAGE_SCN_MEM_WRITE              0x80000000

/* Section alignments */
#define ZERO_CC_PE_SECTION_ALIGNMENT 0x1000 /* 4 KB virtual page */
#define ZERO_CC_PE_FILE_ALIGNMENT    0x200  /* 512-byte disk sector */
#define ZERO_CC_PE_DEFAULT_IMAGE_BASE 0x140000000ULL

/* COFF Relocation Types (AMD64) */
#define ZERO_CC_IMAGE_REL_AMD64_ABSOLUTE 0x0000
#define ZERO_CC_IMAGE_REL_AMD64_ADDR64   0x0001
#define ZERO_CC_IMAGE_REL_AMD64_ADDR32   0x0002
#define ZERO_CC_IMAGE_REL_AMD64_ADDR32NB 0x0003
#define ZERO_CC_IMAGE_REL_AMD64_REL32    0x0004

/* COFF Storage Classes */
#define ZERO_CC_IMAGE_SYM_CLASS_EXTERNAL 2
#define ZERO_CC_IMAGE_SYM_CLASS_STATIC   3

#pragma pack(push, 1)

typedef struct {
    uint16_t e_magic;    /* 0x5A4D "MZ" */
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;   /* Offset to PE header */
} ZeroCCDosHeader;

typedef struct {
    uint16_t machine;
    uint16_t number_of_sections;
    uint32_t time_date_stamp;
    uint32_t pointer_to_symbol_table;
    uint32_t number_of_symbols;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
} ZeroCCCoffFileHeader;

typedef struct {
    uint32_t virtual_address;
    uint32_t size;
} ZeroCCDataDirectory;

typedef struct {
    uint16_t magic;
    uint8_t  major_linker_version;
    uint8_t  minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t address_of_entry_point;
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_operating_system_version;
    uint16_t minor_operating_system_version;
    uint16_t major_image_version;
    uint16_t minor_image_version;
    uint16_t major_subsystem_version;
    uint16_t minor_subsystem_version;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t check_sum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t size_of_stack_reserve;
    uint64_t size_of_stack_commit;
    uint64_t size_of_heap_reserve;
    uint64_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t number_of_rva_and_sizes;
    ZeroCCDataDirectory data_directory[16];
} ZeroCCOptionalHeader64;

typedef struct {
    char     name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_linenumbers;
    uint16_t number_of_relocations;
    uint16_t number_of_linenumbers;
    uint32_t characteristics;
} ZeroCCSectionHeader;

typedef struct {
    uint32_t original_first_thunk; /* RVA to Import Lookup Table (ILT) */
    uint32_t time_date_stamp;
    uint32_t forwarder_chain;
    uint32_t name_rva;             /* RVA to DLL name string */
    uint32_t first_thunk;          /* RVA to Import Address Table (IAT) */
} ZeroCCImportDescriptor;

typedef struct {
    union {
        char short_name[8];
        struct {
            uint32_t zeroes;
            uint32_t offset;
        } long_name;
    } name;
    uint32_t value;
    int16_t  section_number;
    uint16_t type;
    uint8_t  storage_class;
    uint8_t  number_of_aux_symbols;
} ZeroCCCoffSymbol;

typedef struct {
    uint32_t virtual_address;
    uint32_t symbol_table_index;
    uint16_t type;
} ZeroCCCoffReloc;

typedef struct {
    uint32_t characteristics;
    uint32_t time_date_stamp;
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t name_rva;
    uint32_t ordinal_base;
    uint32_t number_of_functions;
    uint32_t number_of_names;
    uint32_t address_of_functions;
    uint32_t address_of_names;
    uint32_t address_of_name_ordinals;
} ZeroCCExportDirectory;

#pragma pack(pop)

typedef struct {
    bool is_dll;            /* Emit DLL instead of EXE */
    bool is_gui;            /* Windows GUI subsystem instead of CUI */
    const char *dll_name;   /* Basename for export table */
} ZeroCCPeOptions;

/**
 * @brief Emit a standalone Windows PE32+ executable (.exe).
 */
bool zero_cc_pe_write_exe(const char *filename,
                          const uint8_t *text, size_t text_size,
                          const uint8_t *rodata, size_t rodata_size,
                          const uint8_t *data, size_t data_size,
                          size_t bss_size,
                          uint32_t entry_text_offset);

/**
 * @brief Emit a standalone Windows PE32+ executable (.exe) directly from a ZeroCCCompiler.
 */
bool zero_cc_pe_write_exe_from_comp(const char *filename, ZeroCCCompiler *comp);

/**
 * @brief Emit a Windows PE32+ DLL (.dll) directly from a ZeroCCCompiler.
 */
bool zero_cc_pe_write_dll_from_comp(const char *filename, ZeroCCCompiler *comp);

/**
 * @brief Emit a Windows PE32+ executable or DLL with configurable options from a ZeroCCCompiler.
 */
bool zero_cc_pe_write_binary_from_comp(const char *filename,
                                       ZeroCCCompiler *comp,
                                       const ZeroCCPeOptions *opts);

/**
 * @brief Emit a relocatable COFF object file (.obj) for x86-64.
 */
bool zero_cc_pe_write_coff_obj(const char *filename,
                               const uint8_t *text, size_t text_size,
                               const uint8_t *rodata, size_t rodata_size,
                               const uint8_t *data, size_t data_size);

/**
 * @brief Emit a relocatable COFF object file (.obj) directly from a ZeroCCCompiler.
 */
bool zero_cc_pe_write_coff_obj_from_comp(const char *filename, ZeroCCCompiler *comp);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_LINKER_PE_WRITER_H */
