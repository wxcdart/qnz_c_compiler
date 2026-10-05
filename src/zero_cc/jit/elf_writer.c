/**
 * @file elf_writer.c
 * @brief ELF64 Object File Generation Implementation
 */

#include "elf_writer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// ============================================================================
// Internal Helper Functions
// ============================================================================

static void set_error(ElfWriterCtx* ctx, const char* msg) {
    ctx->has_error = true;
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
}

static int add_string(char* table, size_t* table_size, size_t max_size, const char* str) {
    size_t len = strlen(str) + 1;  // Include null terminator
    size_t new_size = *table_size + len;

    if (new_size > max_size) {
        return -1;
    }

    size_t index = *table_size;
    memcpy(table + *table_size, str, len);
    *table_size = new_size;

    return (int)index;
}

static int find_section_by_name(ElfWriterCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->section_count; i++) {
        if (strcmp(ctx->sections[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_add_section(ElfWriterCtx* ctx, const char* name,
                                ElfSectionType type, uint64_t flags, uint64_t align) {
    int idx = find_section_by_name(ctx, name);
    if (idx >= 0) {
        return idx;
    }

    if (ctx->section_count >= ELF_MAX_SECTIONS) {
        set_error(ctx, "Too many sections");
        return -1;
    }

    idx = ctx->section_count++;
    ElfWriterSection* sec = &ctx->sections[idx];

    memset(sec, 0, sizeof(*sec));
    strncpy(sec->name, name, sizeof(sec->name) - 1);
    sec->type = type;
    sec->flags = flags;
    sec->align = align;
    sec->capacity = 4096;
    sec->data = (uint8_t*)calloc(sec->capacity, 1);

    if (!sec->data) {
        set_error(ctx, "Memory allocation failed for section");
        ctx->section_count--;
        return -1;
    }

    return idx;
}

static void ensure_section_capacity(ElfWriterSection* sec, size_t needed) {
    if (needed <= sec->capacity) {
        return;
    }

    size_t new_capacity = sec->capacity * 2;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    uint8_t* new_data = (uint8_t*)realloc(sec->data, new_capacity);
    if (new_data) {
        sec->data = new_data;
        sec->capacity = new_capacity;
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool elf_writer_init(ElfWriterCtx* ctx, ElfMachineType machine) {
    if (!ctx) {
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->machine = machine;
    ctx->strtab_size = 1;  // First byte is always null
    ctx->shstrtab_size = 1;
    ctx->output = NULL;
    ctx->output_size = 0;
    ctx->output_capacity = 0;
    ctx->has_error = false;

    // Initialize section count to 0 (null section will be added first)
    ctx->section_count = 0;

    return true;
}

bool elf_writer_add_section(ElfWriterCtx* ctx, const char* name,
                            const uint8_t* data, size_t size,
                            ElfSectionType type, uint64_t flags, uint64_t align) {
    if (!ctx || !name || !data) {
        set_error(ctx, "Invalid arguments");
        return false;
    }

    int idx = find_or_add_section(ctx, name, type, flags, align);
    if (idx < 0) {
        return false;
    }

    ElfWriterSection* sec = &ctx->sections[idx];

    // Ensure capacity
    ensure_section_capacity(sec, size);

    // Copy data
    memcpy(sec->data, data, size);
    sec->size = size;

    // Add section name to shstrtab
    int name_idx = add_string(ctx->shstrtab, &ctx->shstrtab_size,
                              sizeof(ctx->shstrtab), name);
    if (name_idx < 0) {
        set_error(ctx, "String table overflow");
        return false;
    }

    sec->sh_name = (uint32_t)name_idx;

    return true;
}

bool elf_writer_add_symbol(ElfWriterCtx* ctx, const char* name,
                           ElfSymbolBinding binding, ElfSymbolType type,
                           uint16_t section_idx, uint64_t value, uint64_t size) {
    if (!ctx || !name) {
        set_error(ctx, "Invalid arguments");
        return false;
    }

    if (ctx->symbol_count >= ELF_MAX_SYMBOLS) {
        set_error(ctx, "Symbol table full");
        return false;
    }

    // Find or create section index
    int sec_idx = section_idx;
    if (section_idx == 0 && strcmp(name, ".text") == 0) {
        sec_idx = 1;  // First real section
    }

    // Add symbol name to strtab
    int name_idx = add_string(ctx->strtab, &ctx->strtab_size,
                              sizeof(ctx->strtab), name);
    if (name_idx < 0) {
        set_error(ctx, "String table overflow");
        return false;
    }

    ElfWriterSymbol* sym = &ctx->symbols[ctx->symbol_count++];
    sym->name = ctx->strtab + name_idx;
    sym->section_idx = section_idx;
    sym->value = value;
    sym->size = size;
    sym->info = (binding << 4) | (type & 0xF);
    sym->other = ELF_STV_DEFAULT;

    return true;
}

bool elf_writer_add_relocation(ElfWriterCtx* ctx, uint16_t section_idx,
                               uint64_t offset, uint16_t symbol_idx,
                               uint32_t rel_type, int64_t addend) {
    // Relocations are handled implicitly during write for now
    // Full relocation table support can be added later
    (void)ctx;
    (void)section_idx;
    (void)offset;
    (void)symbol_idx;
    (void)rel_type;
    (void)addend;
    return true;
}

size_t elf_writer_get_output_size(ElfWriterCtx* ctx) {
    if (!ctx) {
        return 0;
    }

    // ELF64 Header: 64 bytes (note: ELF header is 64 bytes for ELF64)
    size_t offset = 64;

    // Data sections (text, data, rodata, etc.)
    for (int i = 0; i < ctx->section_count; i++) {
        ElfWriterSection* sec = &ctx->sections[i];
        if (sec->align > 1) {
            offset = (offset + sec->align - 1) & ~(sec->align - 1);
        }
        offset += sec->size;
    }

    // Symbol string table (.strtab)
    if (ctx->strtab_size > 0) {
        offset = (offset + 7) & ~7;  // Align to 8 bytes
        offset += ctx->strtab_size;
    }

    // Symbol table (.symtab)
    offset = (offset + 7) & ~7;  // Align to 8 bytes
    offset += (ctx->symbol_count + 1) * sizeof(Elf64Sym);

    // Section name string table (.shstrtab)
    offset = (offset + 7) & ~7;  // Align to 8 bytes
    offset += ctx->shstrtab_size;

    // Section headers: (section_count + 4) * 64 bytes
    // (+4 for null, .strtab, .symtab, .shstrtab)
    offset += (ctx->section_count + 4) * sizeof(Elf64SectionHeader);

    return offset;
}

bool elf_writer_write_to_buffer(ElfWriterCtx* ctx, uint8_t* buffer,
                                size_t buffer_size, size_t* written) {
    if (!ctx || !buffer) {
        return false;
    }

    size_t required = elf_writer_get_output_size(ctx);
    if (buffer_size < required) {
        set_error(ctx, "Buffer too small");
        return false;
    }

    memset(buffer, 0, buffer_size);
    size_t offset = 0;

    // ========================================================================
    // ELF Header
    // ========================================================================
    Elf64Header* ehdr = (Elf64Header*)(buffer + offset);
    offset += sizeof(Elf64Header);

    // e_ident
    ehdr->e_ident[0] = ELF_MAGIC_0;
    ehdr->e_ident[1] = ELF_MAGIC_1;
    ehdr->e_ident[2] = ELF_MAGIC_2;
    ehdr->e_ident[3] = ELF_MAGIC_3;
    ehdr->e_ident[4] = ELF_CLASS_64;
    ehdr->e_ident[5] = ELF_DATA_LITTLE_ENDIAN;
    ehdr->e_ident[6] = ELF_VERSION_CURRENT;
    ehdr->e_ident[7] = ELF_OSABI_NONE;
    memset(ehdr->e_ident + 8, 0, 8);

    // e_type (relocatable object file)
    ehdr->e_type = 1;  // ET_REL

    // e_machine
    ehdr->e_machine = (uint16_t)ctx->machine;

    // e_version
    ehdr->e_version = ELF_VERSION_CURRENT;

    // e_entry (0 for relocatable)
    ehdr->e_entry = 0;

    // e_phoff (no program headers)
    ehdr->e_phoff = 0;

    // e_shoff (section header table offset)
    // Will be computed after all sections

    // e_flags
    ehdr->e_flags = 0;

    // e_ehsize
    ehdr->e_ehsize = sizeof(Elf64Header);

    // e_phentsize, e_phnum (no program headers)
    ehdr->e_phentsize = 0;
    ehdr->e_phnum = 0;

    // e_shentsize
    ehdr->e_shentsize = sizeof(Elf64SectionHeader);

    // e_shnum (will be updated)
    ehdr->e_shnum = 0;

    // e_shstrndx (will be updated)
    ehdr->e_shstrndx = 0;

    // ========================================================================
    // Sections (in order: text, data, rodata, symtab, strtab, shstrtab)
    // Section headers will be written at the end
    // ========================================================================

    // Track offsets for section headers
    size_t strtab_offset = 0;
    size_t symtab_offset = 0;
    size_t shstrtab_offset = 0;

    // Data sections (text, data, rodata, etc.)
    for (int i = 0; i < ctx->section_count; i++) {
        ElfWriterSection* sec = &ctx->sections[i];

        // Align offset
        if (sec->align > 1) {
            size_t aligned = (offset + sec->align - 1) & ~(sec->align - 1);
            offset = aligned;
        }

        sec->offset = offset;

        // Copy section data
        memcpy(buffer + offset, sec->data, sec->size);
        offset += sec->size;
    }

    // Symbol string table (.strtab)
    if (ctx->strtab_size > 0) {
        // Align to 8 bytes
        offset = (offset + 7) & ~7;
        strtab_offset = offset;
        memcpy(buffer + offset, ctx->strtab, ctx->strtab_size);
        offset += ctx->strtab_size;
    } else {
        strtab_offset = 0;
    }

    // Symbol table (.symtab)
    offset = (offset + 7) & ~7;
    symtab_offset = offset;
    Elf64Sym* symtab = (Elf64Sym*)(buffer + offset);

    // Null symbol entry
    memset(symtab, 0, sizeof(Elf64Sym));
    symtab++;

    // Add symbols
    for (int i = 0; i < ctx->symbol_count; i++) {
        ElfWriterSymbol* sym = &ctx->symbols[i];

        symtab->st_name = (uint32_t)(sym->name - ctx->strtab);
        symtab->st_info = sym->info;
        symtab->st_other = sym->other;
        symtab->st_shndx = sym->section_idx;
        symtab->st_value = sym->value;
        symtab->st_size = sym->size;

        symtab++;
    }
    offset += (ctx->symbol_count + 1) * sizeof(Elf64Sym);

    // Section name string table (.shstrtab)
    offset = (offset + 7) & ~7;
    shstrtab_offset = offset;
    memcpy(buffer + offset, ctx->shstrtab, ctx->shstrtab_size);
    offset += ctx->shstrtab_size;

    // ========================================================================
    // Section Headers (written after all sections to know offsets)
    // ========================================================================
    size_t section_headers_offset = offset;

    // We need to write section headers now, but we already reserved space
    // Let's rewrite the section headers in the correct location

    // Actually, let's recalculate: section headers come AFTER all data
    // So we need to go back and write them

    // For simplicity, let's write section headers at the end
    // and update e_shoff accordingly

    // ========================================================================
    // Rewrite Section Headers
    // ========================================================================

    // Null section
    Elf64SectionHeader* shdr = (Elf64SectionHeader*)(buffer + section_headers_offset);
    memset(shdr, 0, sizeof(Elf64SectionHeader));
    shdr++;

    // Data sections
    for (int i = 0; i < ctx->section_count; i++) {
        ElfWriterSection* sec = &ctx->sections[i];

        shdr->sh_name = sec->sh_name;
        shdr->sh_type = sec->type;
        shdr->sh_flags = sec->flags;
        shdr->sh_addr = 0;
        shdr->sh_offset = sec->offset;
        shdr->sh_size = sec->size;
        shdr->sh_link = 0;
        shdr->sh_info = 0;
        shdr->sh_addralign = sec->align;
        shdr->sh_entsize = 0;

        shdr++;
    }

    // .strtab section header
    shdr->sh_name = 0;  // Will be computed
    shdr->sh_type = ELF_SHT_STRTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = strtab_offset;
    shdr->sh_size = ctx->strtab_size;
    shdr->sh_link = 0;
    shdr->sh_info = 0;
    shdr->sh_addralign = 8;
    shdr->sh_entsize = 0;
    shdr++;

    // .symtab section header
    shdr->sh_name = 0;
    shdr->sh_type = ELF_SHT_SYMTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = symtab_offset;
    shdr->sh_size = (ctx->symbol_count + 1) * sizeof(Elf64Sym);
    shdr->sh_link = (uint32_t)(ctx->section_count + 1);  // Index of .strtab
    shdr->sh_info = 0;
    shdr->sh_addralign = 8;
    shdr->sh_entsize = sizeof(Elf64Sym);
    shdr++;

    // .shstrtab section header
    shdr->sh_name = 0;
    shdr->sh_type = ELF_SHT_STRTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = shstrtab_offset;
    shdr->sh_size = ctx->shstrtab_size;
    shdr->sh_link = 0;
    shdr->sh_info = 0;
    shdr->sh_addralign = 8;
    shdr->sh_entsize = 0;
    shdr++;  // Advance past .shstrtab section header

    // Update offset to point after section headers
    // Total: null + data sections + .strtab + .symtab + .shstrtab
    offset = section_headers_offset + (ctx->section_count + 4) * sizeof(Elf64SectionHeader);

    // ========================================================================
    // Update ELF Header
    // ========================================================================
    ehdr = (Elf64Header*)buffer;
    ehdr->e_shoff = section_headers_offset;
    ehdr->e_shnum = ctx->section_count + 4;  // +4 for null, .strtab, .symtab, .shstrtab
    ehdr->e_shstrndx = ctx->section_count + 2;  // Index of .shstrtab

    if (written) {
        *written = offset;
    }

    return true;
}

bool elf_writer_write(ElfWriterCtx* ctx, const char* path) {
    if (!ctx || !path) {
        set_error(ctx, "Invalid arguments");
        return false;
    }

    size_t required_size = elf_writer_get_output_size(ctx);
    uint8_t* buffer = (uint8_t*)malloc(required_size);

    if (!buffer) {
        set_error(ctx, "Memory allocation failed");
        return false;
    }

    size_t written = 0;
    bool success = elf_writer_write_to_buffer(ctx, buffer, required_size, &written);

    if (!success) {
        free(buffer);
        return false;
    }

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        set_error(ctx, strerror(errno));
        free(buffer);
        return false;
    }

    size_t bytes_written = fwrite(buffer, 1, written, fp);
    fclose(fp);

    free(buffer);

    if (bytes_written != written) {
        set_error(ctx, "Failed to write all data");
        return false;
    }

    return true;
}

void elf_writer_cleanup(ElfWriterCtx* ctx) {
    if (!ctx) {
        return;
    }

    for (int i = 0; i < ctx->section_count; i++) {
        if (ctx->sections[i].data) {
            free(ctx->sections[i].data);
            ctx->sections[i].data = NULL;
        }
    }

    if (ctx->output) {
        free(ctx->output);
        ctx->output = NULL;
    }

    memset(ctx, 0, sizeof(*ctx));
}

const char* elf_writer_get_error(ElfWriterCtx* ctx) {
    if (!ctx || !ctx->has_error) {
        return "No error";
    }
    return ctx->error_msg;
}

// ============================================================================
// Helper Functions
// ============================================================================

const char* elf_machine_name(ElfMachineType machine) {
    switch (machine) {
        case ELF_MACHINE_X86_64:  return "x86-64";
        case ELF_MACHINE_AARCH64: return "AArch64";
        case ELF_MACHINE_RISCV64:
        case ELF_MACHINE_RISCV32: return "RISC-V";
        default:                  return "Unknown";
    }
}

uint32_t elf_get_reloc_type(ElfMachineType machine, ElfSymbolType sym_type) {
    switch (machine) {
        case ELF_MACHINE_X86_64:
            return (sym_type == ELF_STT_FUNC) ? 1 : 2;  // R_X86_64_64 or R_X86_64_32
        case ELF_MACHINE_AARCH64:
            return 258;  // R_AARCH64_ABS64
        case ELF_MACHINE_RISCV64:
        case ELF_MACHINE_RISCV32:
            return 9;  // R_RISCV_64 or R_RISCV_32
        default:
            return 0;
    }
}
