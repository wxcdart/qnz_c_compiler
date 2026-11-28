/**
 * @file macho_writer.h
 * @brief macOS Mach-O 64-bit Executable, Dynamic Library and Object Writer for ZeroCC (Sprint 251)
 *
 * Provides native 64-bit Mach-O binary emission for Darwin / macOS (x86-64 and ARM64):
 * - Relocatable object files (.o / MH_OBJECT) with sections and symbol table
 * - Standalone executables (MH_EXECUTE) with __PAGEZERO, __TEXT, __DATA, __LINKEDIT,
 *   LC_LOAD_DYLINKER, LC_LOAD_DYLIB (libSystem.B.dylib), and LC_MAIN entry point
 * - Dynamic link libraries (.dylib / MH_DYLIB) with LC_ID_DYLIB and export table
 */

#ifndef ZERO_CC_LINKER_MACHO_WRITER_H
#define ZERO_CC_LINKER_MACHO_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct ZeroCCCompiler;
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Mach-O 64 Constants */
#define ZERO_CC_MH_MAGIC_64          0xFEEDFACFU
#define ZERO_CC_MH_CIGAM_64          0xCFFAEDFEU

/* CPU Architecture Types */
#define ZERO_CC_CPU_ARCH_ABI64       0x01000000U
#define ZERO_CC_CPU_TYPE_X86_64      (ZERO_CC_CPU_ARCH_ABI64 | 7U)
#define ZERO_CC_CPU_TYPE_ARM64       (ZERO_CC_CPU_ARCH_ABI64 | 12U)
#define ZERO_CC_CPU_SUBTYPE_X86_64_ALL 3U
#define ZERO_CC_CPU_SUBTYPE_ARM64_ALL  0U

/* File Types */
#define ZERO_CC_MH_OBJECT            0x1U
#define ZERO_CC_MH_EXECUTE           0x2U
#define ZERO_CC_MH_DYLIB             0x6U

/* Header Flags */
#define ZERO_CC_MH_NOUNDEFS          0x00000001U
#define ZERO_CC_MH_DYLDLINK          0x00000004U
#define ZERO_CC_MH_TWOLEVEL          0x00000080U
#define ZERO_CC_MH_SUBSECTIONS_VIA_SYMBOLS 0x00002000U
#define ZERO_CC_MH_PIE               0x00200000U

/* Load Command Types */
#define ZERO_CC_LC_SEGMENT_64        0x00000019U
#define ZERO_CC_LC_SYMTAB            0x00000002U
#define ZERO_CC_LC_DYSYMTAB          0x0000000BU
#define ZERO_CC_LC_LOAD_DYLIB        0x0000000CU
#define ZERO_CC_LC_ID_DYLIB          0x0000000DU
#define ZERO_CC_LC_LOAD_DYLINKER     0x0000000EU
#define ZERO_CC_LC_MAIN              0x80000028U

/* Section Types and Attributes */
#define ZERO_CC_MACHO_S_REGULAR                 0x00000000U
#define ZERO_CC_MACHO_S_ZEROFILL                0x00000001U
#define ZERO_CC_MACHO_S_CSTRING_LITERALS        0x00000002U
#define ZERO_CC_MACHO_S_NON_LAZY_SYMBOL_POINTERS 0x00000006U
#define ZERO_CC_MACHO_S_LAZY_SYMBOL_POINTERS     0x00000007U
#define ZERO_CC_MACHO_S_SYMBOL_STUBS             0x00000008U
#define ZERO_CC_MACHO_S_ATTR_SOME_INSTRUCTIONS  0x00000400U
#define ZERO_CC_MACHO_S_ATTR_PURE_INSTRUCTIONS  0x80000000U

/* Indirect Symbol Table Special Values */
#define ZERO_CC_INDIRECT_SYMBOL_LOCAL           0x80000000U
#define ZERO_CC_INDIRECT_SYMBOL_ABS             0x40000000U

/* VM Protections */
#define ZERO_CC_VM_PROT_NONE         0x0
#define ZERO_CC_VM_PROT_READ         0x1
#define ZERO_CC_VM_PROT_WRITE        0x2
#define ZERO_CC_VM_PROT_EXECUTE      0x4
#define ZERO_CC_VM_PROT_ALL          (ZERO_CC_VM_PROT_READ | ZERO_CC_VM_PROT_WRITE | ZERO_CC_VM_PROT_EXECUTE)

/* Symbol Types */
#define ZERO_CC_N_UNDF               0x00U
#define ZERO_CC_N_ABS                0x02U
#define ZERO_CC_N_SECT               0x0EU
#define ZERO_CC_N_EXT                0x01U

/* X86-64 Relocation Types */
#define ZERO_CC_X86_64_RELOC_UNSIGNED 0U
#define ZERO_CC_X86_64_RELOC_SIGNED   1U
#define ZERO_CC_X86_64_RELOC_BRANCH   2U
#define ZERO_CC_X86_64_RELOC_GOT_LOAD 3U

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} ZeroCCMachHeader64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} ZeroCCSegmentCommand64;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} ZeroCCSection64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} ZeroCCSymtabCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} ZeroCCDysymtabCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dylib_name_offset;
    uint32_t dylib_timestamp;
    uint32_t dylib_current_version;
    uint32_t dylib_compatibility_version;
} ZeroCCDylibCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dylinker_name_offset;
} ZeroCCDylinkerCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} ZeroCCEntryPointCommand;

typedef struct {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} ZeroCCNlist64;

typedef struct {
    int32_t  r_address;
    uint32_t r_info;
} ZeroCCMachORelocation;

#pragma pack(pop)

typedef struct {
    bool is_dylib;            /* Emit dynamic library (.dylib) instead of executable */
    const char *dylib_name;   /* Install name for LC_ID_DYLIB (default: filename or @rpath/name) */
    uint32_t cputype;         /* CPU_TYPE_X86_64 or CPU_TYPE_ARM64 */
} ZeroCCMachoOptions;

/**
 * @brief Emit a standalone macOS Mach-O 64-bit executable.
 */
bool zero_cc_macho_write_exe(const char *filename,
                             const uint8_t *text, size_t text_size,
                             const uint8_t *rodata, size_t rodata_size,
                             const uint8_t *data, size_t data_size,
                             size_t bss_size,
                             uint32_t entry_offset);

/**
 * @brief Emit a standalone macOS Mach-O 64-bit executable with target CPU architecture.
 */
bool zero_cc_macho_write_exe_arch(const char *filename,
                                  const uint8_t *text, size_t text_size,
                                  const uint8_t *rodata, size_t rodata_size,
                                  const uint8_t *data, size_t data_size,
                                  size_t bss_size,
                                  uint32_t entry_offset,
                                  uint32_t cputype);

/**
 * @brief Emit a standalone macOS Mach-O 64-bit executable with dynamic library imports and stubs.
 */
bool zero_cc_macho_write_exe_arch_ext(const char *filename,
                                      const uint8_t *text, size_t text_size,
                                      const uint8_t *rodata, size_t rodata_size,
                                      const uint8_t *data, size_t data_size,
                                      size_t bss_size,
                                      uint32_t entry_offset,
                                      uint32_t cputype,
                                      const char **import_names, uint32_t import_count,
                                      const char **extra_dylibs, uint32_t extra_dylib_count);

/**
 * @brief Emit a standalone macOS Mach-O 64-bit executable directly from a ZeroCCCompiler.
 */
bool zero_cc_macho_write_exe_from_comp(const char *filename, ZeroCCCompiler *comp);

/**
 * @brief Emit a macOS Mach-O 64-bit dynamic library (.dylib) directly from a ZeroCCCompiler.
 */
bool zero_cc_macho_write_dylib_from_comp(const char *filename, ZeroCCCompiler *comp);

/**
 * @brief Emit a macOS Mach-O 64-bit binary with configurable options from a ZeroCCCompiler.
 */
bool zero_cc_macho_write_binary_from_comp(const char *filename,
                                         ZeroCCCompiler *comp,
                                         const ZeroCCMachoOptions *opts);

/**
 * @brief Emit a relocatable macOS Mach-O 64-bit object file (.o).
 */
bool zero_cc_macho_write_obj(const char *filename,
                             const uint8_t *text, size_t text_size,
                             const uint8_t *rodata, size_t rodata_size,
                             const uint8_t *data, size_t data_size,
                             size_t bss_size);

/**
 * @brief Emit a relocatable macOS Mach-O 64-bit object file (.o) with target CPU architecture.
 */
bool zero_cc_macho_write_obj_arch(const char *filename,
                                  const uint8_t *text, size_t text_size,
                                  const uint8_t *rodata, size_t rodata_size,
                                  const uint8_t *data, size_t data_size,
                                  size_t bss_size,
                                  uint32_t cputype);

/**
 * @brief Emit a relocatable macOS Mach-O 64-bit object file (.o) directly from a ZeroCCCompiler.
 */
bool zero_cc_macho_write_obj_from_comp(const char *filename, ZeroCCCompiler *comp);

/**
 * @brief Link multiple Mach-O 64-bit object files (.o) into a standalone executable or dylib.
 */
bool zero_cc_macho_link_objects(const char *filename,
                                const char **obj_files,
                                size_t obj_count,
                                const ZeroCCMachoOptions *opts);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_LINKER_MACHO_WRITER_H */
