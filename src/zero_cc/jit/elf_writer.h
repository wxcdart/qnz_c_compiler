/**
 * @file elf_writer.h
 * @brief ELF64 Object File Generation for ZeroCC
 *
 * Generates ELF64 format object files for x86-64, AArch64, and RISC-V.
 * Supports .text, .data, .rodata, .symtab, .strtab, and .shstrtab sections.
 *
 * Usage:
 *   ElfWriterCtx ctx;
 *   elf_writer_init(&ctx, ELF_MACHINE_X86_64);
 *   elf_writer_add_section(&ctx, ".text", code, code_size);
 *   elf_writer_add_symbol(&ctx, "main", SYMBOL_GLOBAL, offset);
 *   elf_writer_write(&ctx, "output.o");
 *   elf_writer_cleanup(&ctx);
 */

#ifndef ZEROCC_ELF_WRITER_H
#define ZEROCC_ELF_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// ELF Constants
// ============================================================================

// ELF Magic Number
#define ELF_MAGIC_0 0x7F
#define ELF_MAGIC_1 'E'
#define ELF_MAGIC_2 'L'
#define ELF_MAGIC_3 'F'

// ELF Class (32/64-bit)
#define ELF_CLASS_32  1
#define ELF_CLASS_64  2

// ELF Data Encoding
#define ELF_DATA_LITTLE_ENDIAN  1
#define ELF_DATA_BIG_ENDIAN     2

// ELF Version
#define ELF_VERSION_CURRENT 1

// ELF OS/ABI
#define ELF_OSABI_NONE        0  // UNIX System V
#define ELF_OSABI_LINUX       3  // Linux

// ELF Machine Types
typedef enum {
    ELF_MACHINE_NONE      = 0,   // No machine
    ELF_MACHINE_X86_64    = 62,  // AMD x86-64
    ELF_MACHINE_AARCH64   = 183, // ARM AArch64
    ELF_MACHINE_RISCV64   = 243, // RISC-V 64-bit
    ELF_MACHINE_RISCV32   = 244, // RISC-V 32-bit
} ElfMachineType;

// RISC-V e_flags EF_RISCV_FLOAT_ABI values, per the RISC-V psABI (SOFT=0,
// SINGLE=0x2, DOUBLE=0x4). Named ZC_... to avoid colliding with the identical
// macros the system <elf.h> defines (which is in the include chain via
// core/utils.h). The system cross libc (musl/glibc) targets the double-float
// ABI (lp64d for rv64, ilp32d for rv32); a RISC-V object must declare the same
// or the linker rejects the link with "can't link soft-float modules with
// double-float modules". (Sprint 41)
enum {
    ZC_EF_RISCV_FLOAT_ABI_SOFT   = 0x0,
    ZC_EF_RISCV_FLOAT_ABI_SINGLE = 0x2,
    ZC_EF_RISCV_FLOAT_ABI_DOUBLE = 0x4,
};

// ELF Section Types
typedef enum {
    ELF_SHT_NULL          = 0,  // Undefined
    ELF_SHT_PROGBITS      = 1,  // Program information
    ELF_SHT_SYMTAB        = 2,  // Symbol table
    ELF_SHT_STRTAB        = 3,  // String table
    ELF_SHT_RELA          = 4,  // Relocation entries (addend)
    ELF_SHT_NOBITS        = 8,  // No space in file (.bss)
    ELF_SHT_REL           = 9,  // Relocation entries (no addend)
    ELF_SHT_DYNSYM        = 11, // Dynamic symbol table
} ElfSectionType;

// ELF Section Flags
typedef enum {
    ELF_SHF_WRITE     = (1 << 0),  // Writable
    ELF_SHF_ALLOC     = (1 << 1),  // Occupies memory during execution
    ELF_SHF_EXECINSTR = (1 << 2),  // Executable
} ElfSectionFlags;

// ELF Symbol Binding
typedef enum {
    ELF_STB_LOCAL  = 0,  // Local symbol
    ELF_STB_GLOBAL = 1,  // Global symbol
    ELF_STB_WEAK   = 2,  // Weak symbol
} ElfSymbolBinding;

// ELF Symbol Type
typedef enum {
    ELF_STT_NOTYPE  = 0,  // Not typed
    ELF_STT_OBJECT  = 1,  // Data object
    ELF_STT_FUNC    = 2,  // Function
    ELF_STT_SECTION = 3,  // Section symbol
    ELF_STT_FILE    = 4,  // Source file
} ElfSymbolType;

// ELF Symbol Visibility
typedef enum {
    ELF_STV_DEFAULT   = 0,
    ELF_STV_INTERNAL  = 1,
    ELF_STV_HIDDEN    = 2,
    ELF_STV_PROTECTED = 3,
} ElfSymbolVisibility;

// ============================================================================
// ELF Structures (ELF32)
// ============================================================================

// ELF32 Header (52 bytes)
typedef struct {
    uint8_t  e_ident[16];  // ELF identification
    uint16_t e_type;       // Object file type
    uint16_t e_machine;    // Machine type
    uint32_t e_version;    // Object file version
    uint32_t e_entry;      // Entry point address
    uint32_t e_phoff;      // Program header table offset
    uint32_t e_shoff;      // Section header table offset
    uint32_t e_flags;      // Processor-specific flags
    uint16_t e_ehsize;     // ELF header size
    uint16_t e_phentsize;  // Program header entry size
    uint16_t e_phnum;      // Number of program header entries
    uint16_t e_shentsize;  // Section header entry size
    uint16_t e_shnum;      // Number of section header entries
    uint16_t e_shstrndx;   // Section name string table index
} __attribute__((packed)) Elf32Header;

// ELF32 Section Header (40 bytes)
typedef struct {
    uint32_t sh_name;      // Section name (string table index)
    uint32_t sh_type;      // Section type
    uint32_t sh_flags;     // Section flags
    uint32_t sh_addr;      // Section virtual address
    uint32_t sh_offset;    // Section file offset
    uint32_t sh_size;      // Section size
    uint32_t sh_link;      // Link to another section
    uint32_t sh_info;      // Additional section information
    uint32_t sh_addralign; // Section alignment
    uint32_t sh_entsize;   // Entry size if fixed-size entries
} __attribute__((packed)) Elf32SectionHeader;

// ELF32 Symbol Table Entry (16 bytes)
typedef struct {
    uint32_t st_name;      // Symbol name (string table index)
    uint32_t st_value;     // Symbol value
    uint32_t st_size;      // Symbol size
    uint8_t  st_info;      // Symbol type and binding
    uint8_t  st_other;     // Symbol visibility
    uint16_t st_shndx;     // Section index
} __attribute__((packed)) Elf32Sym;

// ELF32 Relocation Entry (12 bytes)
typedef struct {
    uint32_t r_offset;     // Offset of relocation
    uint32_t r_info;       // Symbol index and relocation type
    int32_t  r_addend;     // Constant addend
} __attribute__((packed)) Elf32Rela;

// ============================================================================
// ELF Structures (ELF64)
// ============================================================================

// ELF64 Header (52 bytes)
typedef struct {
    uint8_t  e_ident[16];  // ELF identification
    uint16_t e_type;       // Object file type
    uint16_t e_machine;    // Machine type
    uint32_t e_version;    // Object file version
    uint64_t e_entry;      // Entry point address
    uint64_t e_phoff;      // Program header table offset
    uint64_t e_shoff;      // Section header table offset
    uint32_t e_flags;      // Processor-specific flags
    uint16_t e_ehsize;     // ELF header size
    uint16_t e_phentsize;  // Program header entry size
    uint16_t e_phnum;      // Number of program header entries
    uint16_t e_shentsize;  // Section header entry size
    uint16_t e_shnum;      // Number of section header entries
    uint16_t e_shstrndx;   // Section name string table index
} __attribute__((packed)) Elf64Header;

// ELF64 Section Header (64 bytes)
typedef struct {
    uint32_t sh_name;      // Section name (string table index)
    uint32_t sh_type;      // Section type
    uint64_t sh_flags;     // Section flags
    uint64_t sh_addr;      // Section virtual address
    uint64_t sh_offset;    // Section file offset
    uint64_t sh_size;      // Section size
    uint32_t sh_link;      // Link to another section
    uint32_t sh_info;      // Additional section information
    uint64_t sh_addralign; // Section alignment
    uint64_t sh_entsize;   // Entry size if fixed-size entries
} __attribute__((packed)) Elf64SectionHeader;

// ELF64 Symbol Table Entry (24 bytes)
typedef struct {
    uint32_t st_name;      // Symbol name (string table index)
    uint8_t  st_info;      // Symbol type and binding
    uint8_t  st_other;     // Symbol visibility
    uint16_t st_shndx;     // Section index
    uint64_t st_value;     // Symbol value
    uint64_t st_size;      // Symbol size
} __attribute__((packed)) Elf64Sym;

// ELF64 Relocation Entry (24 bytes)
typedef struct {
    uint64_t r_offset;     // Offset of relocation
    uint64_t r_info;       // Symbol index and relocation type
    int64_t  r_addend;     // Constant addend
} __attribute__((packed)) Elf64Rela;

// ============================================================================
// Section Types
// ============================================================================

typedef struct {
    char name[32];
    uint8_t* data;
    size_t size;
    size_t capacity;
    ElfSectionType type;
    uint64_t flags;
    uint64_t align;
    uint64_t offset;  // File offset
    uint32_t sh_name; // Offset in shstrtab
} ElfWriterSection;

typedef enum {
    ELF_SECTION_TEXT,
    ELF_SECTION_DATA,
    ELF_SECTION_RODATA,
    ELF_SECTION_BSS,
    ELF_SECTION_SYMTAB,
    ELF_SECTION_STRTAB,
    ELF_SECTION_SHSTRTAB,
    ELF_SECTION_NULL,
    ELF_SECTION_COUNT,
} ElfSectionId;

// ============================================================================
// Symbol Types
// ============================================================================

typedef struct {
    uint32_t name_offset;   /* Offset into ctx->strtab (stable across realloc) */
    uint16_t section_idx;
    uint64_t value;
    uint64_t size;
    uint8_t info;
    uint8_t other;
} ElfWriterSymbol;

// ============================================================================

// ============================================================================
// ELF Writer Context
// ============================================================================

#define ELF_MAX_SECTIONS    32
#define ELF_INITIAL_SYMBOLS     256
#define ELF_INITIAL_RELOCATIONS 256
#define ELF_MAX_STRING_LEN  4096

typedef struct {
    // Configuration
    ElfMachineType machine;
    const char* output_path;

    // Section data
    ElfWriterSection sections[ELF_MAX_SECTIONS];
    int section_count;

    // Symbol table (dynamically growing)
    ElfWriterSymbol *symbols;
    int symbol_capacity;
    int symbol_count;

    // Relocation entries (dynamically growing)
    uint64_t *reloc_offsets;
    uint16_t *reloc_symbol_indices;
    uint32_t *reloc_types;
    int64_t *reloc_addends;
    uint16_t *reloc_section_indices;
    int reloc_capacity;
    int relocation_count;

    // String tables
    char *strtab;                        // Symbol names (dynamically growing)
    size_t strtab_capacity;
    size_t strtab_size;
    char shstrtab[ELF_MAX_STRING_LEN];    // Section names
    size_t shstrtab_size;

    // Output buffer
    uint8_t* output;
    size_t output_size;
    size_t output_capacity;

    // Error state
    bool has_error;
    char error_msg[256];
} ElfWriterCtx;

// ============================================================================
// API Functions
// ============================================================================

/**
 * @brief Initialize ELF writer context
 * @param ctx Writer context
 * @param machine Target machine type
 * @return true on success, false on error
 */
bool elf_writer_init(ElfWriterCtx* ctx, ElfMachineType machine);

/* Index (1-based) of an already-added section by name, or 0 if absent.
 * Use this instead of hardcoded section positions when adding symbols or
 * relocations: empty sections are not emitted, so positions shift. */
uint16_t elf_writer_section_index(const ElfWriterCtx* ctx, const char* name);

/**
 * @brief Add a section to the ELF file
 * @param ctx Writer context
 * @param name Section name (e.g., ".text", ".data")
 * @param data Section data
 * @param size Data size
 * @param type Section type
 * @param flags Section flags
 * @param align Section alignment
 * @return true on success, false on error
 */
bool elf_writer_add_section(ElfWriterCtx* ctx, const char* name,
                            const uint8_t* data, size_t size,
                            ElfSectionType type, uint64_t flags, uint64_t align);

/**
 * @brief Add a symbol to the symbol table
 * @param ctx Writer context
 * @param name Symbol name
 * @param binding Symbol binding (LOCAL, GLOBAL, WEAK)
 * @param type Symbol type (FUNC, OBJECT, etc.)
 * @param section_idx Section index
 * @param value Symbol offset within section
 * @param size Symbol size
 * @return true on success, false on error
 */
bool elf_writer_add_symbol(ElfWriterCtx* ctx, const char* name,
                           ElfSymbolBinding binding, ElfSymbolType type,
                           uint16_t section_idx, uint64_t value, uint64_t size);

/**
 * @brief Add a relocation entry
 * @param ctx Writer context
 * @param section_idx Section containing the relocation
 * @param offset Offset within section
 * @param symbol_idx Symbol index
 * @param rel_type Relocation type (machine-specific)
 * @param addend Addend value
 * @return true on success, false on error
 */
bool elf_writer_add_relocation(ElfWriterCtx* ctx, uint16_t section_idx,
                               uint64_t offset, uint16_t symbol_idx,
                               uint32_t rel_type, int64_t addend);

/**
 * @brief Write ELF file to disk
 * @param ctx Writer context
 * @param path Output file path
 * @return true on success, false on error
 */
bool elf_writer_write(ElfWriterCtx* ctx, const char* path);

/**
 * @brief Write ELF file to memory buffer
 * @param ctx Writer context
 * @param buffer Output buffer (must be pre-allocated)
 * @param buffer_size Buffer size
 * @param written Size of written data
 * @return true on success, false on error
 */
bool elf_writer_write_to_buffer(ElfWriterCtx* ctx, uint8_t* buffer,
                                size_t buffer_size, size_t* written);

/**
 * @brief Get required buffer size for ELF output
 * @param ctx Writer context
 * @return Required buffer size in bytes
 */
size_t elf_writer_get_output_size(ElfWriterCtx* ctx);

/**
 * @brief Clean up ELF writer context
 * @param ctx Writer context
 */
void elf_writer_cleanup(ElfWriterCtx* ctx);

/**
 * @brief Get last error message
 * @param ctx Writer context
 * @return Error message string
 */
const char* elf_writer_get_error(ElfWriterCtx* ctx);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Get machine type name
 * @param machine Machine type
 * @return Machine type name string
 */
const char* elf_machine_name(ElfMachineType machine);

/**
 * @brief Get relocation type for machine and symbol type
 * @param machine Target machine
 * @param sym_type Symbol type (FUNC, OBJECT)
 * @return Relocation type constant
 */
uint32_t elf_get_reloc_type(ElfMachineType machine, ElfSymbolType sym_type);

#ifdef __cplusplus
}
#endif

#endif // ZEROCC_ELF_WRITER_H
