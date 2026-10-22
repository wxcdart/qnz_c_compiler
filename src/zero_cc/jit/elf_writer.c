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
    ctx->shstrtab_size = 1;  // First byte is always null
    ctx->output = NULL;
    ctx->output_size = 0;
    ctx->output_capacity = 0;
    ctx->has_error = false;

    // Allocate dynamic tables (symbols, relocations, symbol-name strtab)
    ctx->symbols = (ElfWriterSymbol *)calloc(ELF_INITIAL_SYMBOLS, sizeof(ElfWriterSymbol));
    ctx->symbol_capacity = ctx->symbols ? ELF_INITIAL_SYMBOLS : 0;
    ctx->strtab = (char *)calloc(ELF_MAX_STRING_LEN, 1);
    ctx->strtab_capacity = ctx->strtab ? ELF_MAX_STRING_LEN : 0;
    ctx->reloc_offsets = (uint64_t *)calloc(ELF_INITIAL_RELOCATIONS, sizeof(uint64_t));
    ctx->reloc_symbol_indices = (uint16_t *)calloc(ELF_INITIAL_RELOCATIONS, sizeof(uint16_t));
    ctx->reloc_types = (uint32_t *)calloc(ELF_INITIAL_RELOCATIONS, sizeof(uint32_t));
    ctx->reloc_addends = (int64_t *)calloc(ELF_INITIAL_RELOCATIONS, sizeof(int64_t));
    ctx->reloc_section_indices = (uint16_t *)calloc(ELF_INITIAL_RELOCATIONS, sizeof(uint16_t));
    ctx->reloc_capacity = (ctx->reloc_offsets && ctx->reloc_symbol_indices &&
                           ctx->reloc_types && ctx->reloc_addends &&
                           ctx->reloc_section_indices) ? ELF_INITIAL_RELOCATIONS : 0;
    if (!ctx->symbols || !ctx->strtab || ctx->reloc_capacity == 0) {
        set_error(ctx, "Out of memory initializing ELF writer");
        return false;
    }

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

    if (ctx->symbol_count >= ctx->symbol_capacity) {
        int new_cap = ctx->symbol_capacity ? ctx->symbol_capacity * 2 : ELF_INITIAL_SYMBOLS;
        ElfWriterSymbol *grown = (ElfWriterSymbol *)realloc(ctx->symbols, (size_t)new_cap * sizeof(ElfWriterSymbol));
        if (!grown) {
            set_error(ctx, "Out of memory growing symbol table");
            return false;
        }
        ctx->symbols = grown;
        ctx->symbol_capacity = new_cap;
    }

    // Add symbol name to strtab (growing the table if needed)
    int name_idx = add_string(ctx->strtab, &ctx->strtab_size, ctx->strtab_capacity, name);
    if (name_idx < 0) {
        size_t new_cap = ctx->strtab_capacity * 2;
        char *grown = (char *)realloc(ctx->strtab, new_cap);
        if (!grown) {
            set_error(ctx, "Out of memory growing string table");
            return false;
        }
        ctx->strtab = grown;
        ctx->strtab_capacity = new_cap;
        name_idx = add_string(ctx->strtab, &ctx->strtab_size, ctx->strtab_capacity, name);
        if (name_idx < 0) {
            set_error(ctx, "String table overflow");
            return false;
        }
    }

    ElfWriterSymbol* sym = &ctx->symbols[ctx->symbol_count++];
    sym->name_offset = (uint32_t)name_idx;
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
    if (!ctx) {
        return false;
    }
    
    if (ctx->relocation_count >= ctx->reloc_capacity) {
        int new_cap = ctx->reloc_capacity ? ctx->reloc_capacity * 2 : ELF_INITIAL_RELOCATIONS;
        uint64_t *o = (uint64_t *)realloc(ctx->reloc_offsets, (size_t)new_cap * sizeof(uint64_t));
        uint16_t *si = (uint16_t *)realloc(ctx->reloc_symbol_indices, (size_t)new_cap * sizeof(uint16_t));
        uint32_t *t = (uint32_t *)realloc(ctx->reloc_types, (size_t)new_cap * sizeof(uint32_t));
        int64_t *a = (int64_t *)realloc(ctx->reloc_addends, (size_t)new_cap * sizeof(int64_t));
        uint16_t *s = (uint16_t *)realloc(ctx->reloc_section_indices, (size_t)new_cap * sizeof(uint16_t));
        if (!o || !si || !t || !a || !s) {
            if (o) ctx->reloc_offsets = o;
            if (si) ctx->reloc_symbol_indices = si;
            if (t) ctx->reloc_types = t;
            if (a) ctx->reloc_addends = a;
            if (s) ctx->reloc_section_indices = s;
            set_error(ctx, "Out of memory growing relocation table");
            return false;
        }
        ctx->reloc_offsets = o;
        ctx->reloc_symbol_indices = si;
        ctx->reloc_types = t;
        ctx->reloc_addends = a;
        ctx->reloc_section_indices = s;
        ctx->reloc_capacity = new_cap;
    }
    
    ctx->reloc_offsets[ctx->relocation_count] = offset;
    ctx->reloc_symbol_indices[ctx->relocation_count] = symbol_idx;
    ctx->reloc_types[ctx->relocation_count] = rel_type;
    ctx->reloc_addends[ctx->relocation_count] = addend;
    ctx->reloc_section_indices[ctx->relocation_count] = section_idx;
    ctx->relocation_count++;
    
    return true;
}

/* Distinct target sections that have relocations (order of first appearance) */
static int elf_reloc_distinct_sections(const ElfWriterCtx* ctx, uint16_t* out, int max) {
    int n = 0;
    for (int i = 0; i < ctx->relocation_count; i++) {
        uint16_t s = ctx->reloc_section_indices[i];
        int found = 0;
        for (int j = 0; j < n; j++) {
            if (out[j] == s) { found = 1; break; }
        }
        if (!found && n < max) out[n++] = s;
    }
    return n;
}

static int elf_reloc_count_for_section(const ElfWriterCtx* ctx, uint16_t section_idx) {
    int n = 0;
    for (int i = 0; i < ctx->relocation_count; i++) {
        if (ctx->reloc_section_indices[i] == section_idx) n++;
    }
    return n;
}


static size_t elf_writer_get_output_size_32(ElfWriterCtx* ctx) {
    if (!ctx) return 0;
    size_t offset = sizeof(Elf32Header);  // 52 bytes

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
        offset = (offset + 3) & ~3;
        offset += ctx->strtab_size;
    }

    // Symbol table (.symtab)
    offset = (offset + 3) & ~3;
    offset += (ctx->symbol_count + 1) * sizeof(Elf32Sym);

    // Relocation sections (.rela.<sec>)
    if (ctx->relocation_count > 0) {
        offset = (offset + 3) & ~3;
        offset += ctx->relocation_count * sizeof(Elf32Rela);
    }

    // Section name string table (.shstrtab)
    size_t shstrtab_size = ctx->shstrtab_size;
    shstrtab_size += strlen(".strtab") + 1;
    shstrtab_size += strlen(".symtab") + 1;
    shstrtab_size += strlen(".shstrtab") + 1;
    if (ctx->relocation_count > 0) {
        uint16_t ds[ELF_MAX_SECTIONS];
        int nd = elf_reloc_distinct_sections(ctx, ds, ELF_MAX_SECTIONS);
        for (int i = 0; i < nd; i++) {
            if (ds[i] >= 1 && ds[i] <= (uint16_t)ctx->section_count) {
                shstrtab_size += strlen(".rela.") + strlen(ctx->sections[ds[i] - 1].name) + 1;
            }
        }
    }
    shstrtab_size += strlen(".note.GNU-stack") + 1;

    offset = (offset + 3) & ~3;
    offset += shstrtab_size;

    int n_rela_groups = 0;
    if (ctx->relocation_count > 0) {
        uint16_t ds[ELF_MAX_SECTIONS];
        n_rela_groups = elf_reloc_distinct_sections(ctx, ds, ELF_MAX_SECTIONS);
    }
    int extra_sections = 5 + n_rela_groups;
    offset = (offset + 3) & ~3;
    offset += (ctx->section_count + extra_sections) * sizeof(Elf32SectionHeader);

    return offset;
}

static bool elf_writer_write_to_buffer_32(ElfWriterCtx* ctx, uint8_t* buffer,
                                         size_t buffer_size, size_t* written) {
    if (!ctx || !buffer) {
        return false;
    }

    size_t required = elf_writer_get_output_size_32(ctx);
    if (buffer_size < required) {
        set_error(ctx, "Buffer too small");
        return false;
    }

    memset(buffer, 0, buffer_size);
    size_t offset = 0;

    Elf32Header* ehdr = (Elf32Header*)(buffer + offset);
    offset += sizeof(Elf32Header);

    ehdr->e_ident[0] = ELF_MAGIC_0;
    ehdr->e_ident[1] = ELF_MAGIC_1;
    ehdr->e_ident[2] = ELF_MAGIC_2;
    ehdr->e_ident[3] = ELF_MAGIC_3;
    ehdr->e_ident[4] = ELF_CLASS_32;
    ehdr->e_ident[5] = ELF_DATA_LITTLE_ENDIAN;
    ehdr->e_ident[6] = ELF_VERSION_CURRENT;
    ehdr->e_ident[7] = ELF_OSABI_NONE;
    memset(ehdr->e_ident + 8, 0, 8);

    ehdr->e_type = 1;  // ET_REL
    ehdr->e_machine = 243;  // EM_RISCV
    ehdr->e_version = ELF_VERSION_CURRENT;
    ehdr->e_entry = 0;
    ehdr->e_phoff = 0;
    ehdr->e_flags = ZC_EF_RISCV_FLOAT_ABI_SOFT;
    ehdr->e_ehsize = sizeof(Elf32Header);
    ehdr->e_phentsize = 0;
    ehdr->e_phnum = 0;
    ehdr->e_shentsize = sizeof(Elf32SectionHeader);
    ehdr->e_shnum = 0;
    ehdr->e_shstrndx = 0;

    size_t strtab_offset = 0;
    size_t symtab_offset = 0;
    size_t shstrtab_offset = 0;

    for (int i = 0; i < ctx->section_count; i++) {
        ElfWriterSection* sec = &ctx->sections[i];
        if (sec->align > 1) {
            size_t aligned = (offset + sec->align - 1) & ~(sec->align - 1);
            offset = aligned;
        }
        sec->offset = offset;
        memcpy(buffer + offset, sec->data, sec->size);
        offset += sec->size;
    }

    if (ctx->strtab_size > 0) {
        offset = (offset + 3) & ~3;
        strtab_offset = offset;
        memcpy(buffer + offset, ctx->strtab, ctx->strtab_size);
        offset += ctx->strtab_size;
    } else {
        strtab_offset = 0;
    }

    offset = (offset + 3) & ~3;
    symtab_offset = offset;
    Elf32Sym* symtab = (Elf32Sym*)(buffer + offset);
    memset(symtab, 0, sizeof(Elf32Sym));
    symtab++;

    uint16_t *sym_remap = (uint16_t *)calloc((size_t)ctx->symbol_count + 2, sizeof(uint16_t));
    if (!sym_remap) {
        set_error(ctx, "Out of memory building symbol remap");
        return false;
    }
    uint16_t cur_symtab_idx = 1;

    for (int i = 0; i < ctx->symbol_count; i++) {
        ElfWriterSymbol* sym = &ctx->symbols[i];
        int binding = (sym->info >> 4) & 0xF;
        if (binding == ELF_STB_LOCAL) {
            symtab->st_name = (uint32_t)sym->name_offset;
            symtab->st_info = sym->info;
            symtab->st_other = sym->other;
            symtab->st_shndx = (uint16_t)sym->section_idx;
            symtab->st_value = (uint32_t)sym->value;
            symtab->st_size = (uint32_t)sym->size;
            symtab++;
            sym_remap[i + 1] = cur_symtab_idx;
            cur_symtab_idx++;
        }
    }

    for (int i = 0; i < ctx->symbol_count; i++) {
        ElfWriterSymbol* sym = &ctx->symbols[i];
        int binding = (sym->info >> 4) & 0xF;
        if (binding != ELF_STB_LOCAL) {
            symtab->st_name = (uint32_t)sym->name_offset;
            symtab->st_info = sym->info;
            symtab->st_other = sym->other;
            symtab->st_shndx = (uint16_t)sym->section_idx;
            symtab->st_value = (uint32_t)sym->value;
            symtab->st_size = (uint32_t)sym->size;
            symtab++;
            sym_remap[i + 1] = cur_symtab_idx;
            cur_symtab_idx++;
        }
    }
    offset += (ctx->symbol_count + 1) * sizeof(Elf32Sym);

    uint16_t rela_groups[ELF_MAX_SECTIONS] = {0};
    size_t rela_group_offsets[ELF_MAX_SECTIONS] = {0};
    size_t rela_group_sizes[ELF_MAX_SECTIONS] = {0};
    size_t rela_name_idx[ELF_MAX_SECTIONS] = {0};
    int n_rela_groups = 0;
    size_t rela_text_offset = 0;
    if (ctx->relocation_count > 0) {
        offset = (offset + 3) & ~3;
        rela_text_offset = offset;
        n_rela_groups = elf_reloc_distinct_sections(ctx, rela_groups, ELF_MAX_SECTIONS);
        for (int g = 0; g < n_rela_groups; g++) {
            rela_group_offsets[g] = offset;
            int cnt = elf_reloc_count_for_section(ctx, rela_groups[g]);
            rela_group_sizes[g] = (size_t)cnt * sizeof(Elf32Rela);
            Elf32Rela* rela = (Elf32Rela*)(buffer + offset);
            for (int i = 0; i < ctx->relocation_count; i++) {
                if (ctx->reloc_section_indices[i] != rela_groups[g]) continue;
                rela->r_offset = (uint32_t)ctx->reloc_offsets[i];
                uint16_t raw_sym_idx = ctx->reloc_symbol_indices[i];
                uint16_t mapped_sym_idx = (sym_remap[raw_sym_idx] != 0)
                                              ? sym_remap[raw_sym_idx]
                                              : raw_sym_idx;
                rela->r_info = ((uint32_t)mapped_sym_idx << 8) | ((uint32_t)ctx->reloc_types[i] & 0xFF);
                rela->r_addend = (int32_t)ctx->reloc_addends[i];
                rela++;
            }
            offset += rela_group_sizes[g];
        }
    }

    free(sym_remap);

    size_t strtab_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".strtab");
    size_t symtab_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".symtab");
    size_t shstrtab_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".shstrtab");

    if (ctx->relocation_count > 0) {
        for (int g = 0; g < n_rela_groups; g++) {
            char rela_nm[48];
            if (rela_groups[g] >= 1 && rela_groups[g] <= (uint16_t)ctx->section_count) {
                snprintf(rela_nm, sizeof(rela_nm), ".rela.%s", ctx->sections[rela_groups[g] - 1].name);
            } else {
                snprintf(rela_nm, sizeof(rela_nm), ".rela.%u", rela_groups[g]);
            }
            rela_name_idx[g] = ctx->shstrtab_size;
            add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), rela_nm);
        }
    }

    size_t note_gnu_stack_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".note.GNU-stack");

    offset = (offset + 3) & ~3;
    shstrtab_offset = offset;
    memcpy(buffer + offset, ctx->shstrtab, ctx->shstrtab_size);
    offset += ctx->shstrtab_size;

    size_t section_headers_offset = (offset + 3) & ~3;
    Elf32SectionHeader* shdr = (Elf32SectionHeader*)(buffer + section_headers_offset);
    memset(shdr, 0, sizeof(Elf32SectionHeader));
    shdr++;

    for (int i = 0; i < ctx->section_count; i++) {
        ElfWriterSection* sec = &ctx->sections[i];
        shdr->sh_name = sec->sh_name;
        shdr->sh_type = sec->type;
        shdr->sh_flags = (uint32_t)sec->flags;
        shdr->sh_addr = 0;
        shdr->sh_offset = (uint32_t)sec->offset;
        shdr->sh_size = (uint32_t)sec->size;
        shdr->sh_link = 0;
        shdr->sh_info = 0;
        shdr->sh_addralign = (uint32_t)sec->align;
        shdr->sh_entsize = 0;
        shdr++;
    }

    // .strtab
    shdr->sh_name = (uint32_t)strtab_name_idx;
    shdr->sh_type = ELF_SHT_STRTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = (uint32_t)strtab_offset;
    shdr->sh_size = (uint32_t)ctx->strtab_size;
    shdr->sh_link = 0;
    shdr->sh_info = 0;
    shdr->sh_addralign = 4;
    shdr->sh_entsize = 0;
    shdr++;

    // .symtab
    shdr->sh_name = (uint32_t)symtab_name_idx;
    shdr->sh_type = ELF_SHT_SYMTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = (uint32_t)symtab_offset;
    shdr->sh_size = (uint32_t)((ctx->symbol_count + 1) * sizeof(Elf32Sym));
    shdr->sh_link = (uint32_t)(ctx->section_count + 1);
    {
        int local_count = 1;
        for (int i = 0; i < ctx->symbol_count; i++) {
            int binding = (ctx->symbols[i].info >> 4) & 0xF;
            if (binding == ELF_STB_LOCAL) local_count++;
        }
        shdr->sh_info = (uint32_t)local_count;
    }
    shdr->sh_addralign = 4;
    shdr->sh_entsize = sizeof(Elf32Sym);
    shdr++;

    // .rela.<sec>
    for (int g = 0; g < n_rela_groups; g++) {
        shdr->sh_name = (uint32_t)rela_name_idx[g];
        shdr->sh_type = ELF_SHT_RELA;
        shdr->sh_flags = 0;
        shdr->sh_addr = 0;
        shdr->sh_offset = (uint32_t)rela_group_offsets[g];
        shdr->sh_size = (uint32_t)rela_group_sizes[g];
        shdr->sh_link = ctx->section_count + 2;
        shdr->sh_info = rela_groups[g];
        shdr->sh_addralign = 4;
        shdr->sh_entsize = sizeof(Elf32Rela);
        shdr++;
    }

    // .note.GNU-stack
    size_t note_gnu_stack_offset = 0;
    if (ctx->relocation_count > 0) {
        note_gnu_stack_offset = rela_text_offset + ctx->relocation_count * sizeof(Elf32Rela);
    } else {
        note_gnu_stack_offset = symtab_offset + (ctx->symbol_count + 1) * sizeof(Elf32Sym);
    }
    note_gnu_stack_offset = (note_gnu_stack_offset + 3) & ~3;

    shdr->sh_name = (uint32_t)note_gnu_stack_name_idx;
    shdr->sh_type = ELF_SHT_PROGBITS;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = (uint32_t)note_gnu_stack_offset;
    shdr->sh_size = 0;
    shdr->sh_link = 0;
    shdr->sh_info = 0;
    shdr->sh_addralign = 1;
    shdr->sh_entsize = 0;
    shdr++;

    // .shstrtab
    shdr->sh_name = (uint32_t)shstrtab_name_idx;
    shdr->sh_type = ELF_SHT_STRTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = (uint32_t)shstrtab_offset;
    shdr->sh_size = (uint32_t)ctx->shstrtab_size;
    shdr->sh_link = 0;
    shdr->sh_info = 0;
    shdr->sh_addralign = 4;
    shdr->sh_entsize = 0;
    shdr++;

    int extra_sections = 4 + n_rela_groups;
    offset = section_headers_offset + (ctx->section_count + extra_sections + 1) * sizeof(Elf32SectionHeader);

    ehdr = (Elf32Header*)buffer;
    ehdr->e_shoff = (uint32_t)section_headers_offset;
    ehdr->e_shnum = (uint16_t)(ctx->section_count + extra_sections + 1);
    ehdr->e_shstrndx = (uint16_t)(ctx->section_count + extra_sections);

    if (written) {
        *written = offset;
    }

    return true;
}

size_t elf_writer_get_output_size(ElfWriterCtx* ctx) {
    if (!ctx) {
        return 0;
    }
    if (ctx->machine == ELF_MACHINE_RISCV32) {
        return elf_writer_get_output_size_32(ctx);
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

    // Relocation sections (.rela.<sec>) - one per distinct target section
    if (ctx->relocation_count > 0) {
        offset = (offset + 7) & ~7;  // Align to 8 bytes
        offset += ctx->relocation_count * sizeof(Elf64Rela);
    }

    // Section name string table (.shstrtab)
    // Add space for .strtab, .symtab, .shstrtab, .rela.<sec>, .note.GNU-stack section names that will be added during write
    size_t shstrtab_size = ctx->shstrtab_size;
    shstrtab_size += strlen(".strtab") + 1;
    shstrtab_size += strlen(".symtab") + 1;
    shstrtab_size += strlen(".shstrtab") + 1;
    if (ctx->relocation_count > 0) {
        uint16_t ds[ELF_MAX_SECTIONS];
        int nd = elf_reloc_distinct_sections(ctx, ds, ELF_MAX_SECTIONS);
        for (int i = 0; i < nd; i++) {
            /* ds[i] is an ELF section index (1-based over ctx->sections) */
            if (ds[i] >= 1 && ds[i] <= (uint16_t)ctx->section_count) {
                shstrtab_size += strlen(".rela.") + strlen(ctx->sections[ds[i] - 1].name) + 1;
            }
        }
    }
    shstrtab_size += strlen(".note.GNU-stack") + 1;
    
    offset = (offset + 7) & ~7;  // Align to 8 bytes
    offset += shstrtab_size;

    // Section headers: (section_count + 5 + n_rela_groups) * 64 bytes
    // (+5 for null, .strtab, .symtab, .note.GNU-stack, .shstrtab, +1 per .rela.<sec>)
    int n_rela_groups = 0;
    if (ctx->relocation_count > 0) {
        uint16_t ds[ELF_MAX_SECTIONS];
        n_rela_groups = elf_reloc_distinct_sections(ctx, ds, ELF_MAX_SECTIONS);
    }
    int extra_sections = 5 + n_rela_groups;
    offset += (ctx->section_count + extra_sections) * sizeof(Elf64SectionHeader);

    return offset;
}

bool elf_writer_write_to_buffer(ElfWriterCtx* ctx, uint8_t* buffer,
                                size_t buffer_size, size_t* written) {
    if (!ctx || !buffer) {
        return false;
    }
    if (ctx->machine == ELF_MACHINE_RISCV32) {
        return elf_writer_write_to_buffer_32(ctx, buffer, buffer_size, written);
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
    // RISC-V must declare the floating-point ABI. The system cross libc targets
    // double-float (lp64d/ilp32d); leaving e_flags=0 (soft-float) makes the
    // linker reject the link. See EF_RISCV_FLOAT_ABI_* in elf_writer.h. (Sprint 41)
    if (ctx->machine == ELF_MACHINE_RISCV64 || ctx->machine == ELF_MACHINE_RISCV32) {
        ehdr->e_flags = ZC_EF_RISCV_FLOAT_ABI_DOUBLE;
    } else {
        ehdr->e_flags = 0;
    }

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

    // Add symbols - local symbols first, then global symbols
    uint16_t *sym_remap = (uint16_t *)calloc((size_t)ctx->symbol_count + 2, sizeof(uint16_t));
    if (!sym_remap) {
        set_error(ctx, "Out of memory building symbol remap");
        return false;
    }
    uint16_t cur_symtab_idx = 1;
    
    // First pass: add local symbols
    for (int i = 0; i < ctx->symbol_count; i++) {
        ElfWriterSymbol* sym = &ctx->symbols[i];
        int binding = (sym->info >> 4) & 0xF;
        if (binding == ELF_STB_LOCAL) {
            symtab->st_name = sym->name_offset;
            symtab->st_info = sym->info;
            symtab->st_other = sym->other;
            symtab->st_shndx = sym->section_idx;
            symtab->st_value = sym->value;
            symtab->st_size = sym->size;
            symtab++;
            sym_remap[i + 1] = cur_symtab_idx;
            cur_symtab_idx++;
        }
    }

    // Second pass: add global symbols
    for (int i = 0; i < ctx->symbol_count; i++) {
        ElfWriterSymbol* sym = &ctx->symbols[i];
        int binding = (sym->info >> 4) & 0xF;
        if (binding != ELF_STB_LOCAL) {
            symtab->st_name = sym->name_offset;
            symtab->st_info = sym->info;
            symtab->st_other = sym->other;
            symtab->st_shndx = sym->section_idx;
            symtab->st_value = sym->value;
            symtab->st_size = sym->size;
            symtab++;
            sym_remap[i + 1] = cur_symtab_idx;
            cur_symtab_idx++;
        }
    }
    offset += (ctx->symbol_count + 1) * sizeof(Elf64Sym);

    // Relocation sections (.rela.<sec>) - one per distinct target section
    uint16_t rela_groups[ELF_MAX_SECTIONS] = {0};
    size_t rela_group_offsets[ELF_MAX_SECTIONS] = {0};
    size_t rela_group_sizes[ELF_MAX_SECTIONS] = {0};
    size_t rela_name_idx[ELF_MAX_SECTIONS] = {0};
    int n_rela_groups = 0;
    size_t rela_text_offset = 0;
    if (ctx->relocation_count > 0) {
        offset = (offset + 7) & ~7;  // Align to 8 bytes
        rela_text_offset = offset;
        n_rela_groups = elf_reloc_distinct_sections(ctx, rela_groups, ELF_MAX_SECTIONS);
        for (int g = 0; g < n_rela_groups; g++) {
            rela_group_offsets[g] = offset;
            int cnt = elf_reloc_count_for_section(ctx, rela_groups[g]);
            rela_group_sizes[g] = (size_t)cnt * sizeof(Elf64Rela);
            Elf64Rela* rela = (Elf64Rela*)(buffer + offset);
            for (int i = 0; i < ctx->relocation_count; i++) {
                if (ctx->reloc_section_indices[i] != rela_groups[g]) continue;
                rela->r_offset = ctx->reloc_offsets[i];
                uint16_t raw_sym_idx = ctx->reloc_symbol_indices[i];
                uint16_t mapped_sym_idx = (sym_remap[raw_sym_idx] != 0)
                                              ? sym_remap[raw_sym_idx]
                                              : raw_sym_idx;
                // r_info = (symbol_index << 32) | relocation_type
                rela->r_info = ((uint64_t)mapped_sym_idx << 32) | ctx->reloc_types[i];
                rela->r_addend = ctx->reloc_addends[i];
                rela++;
            }
            offset += rela_group_sizes[g];
        }
    }

    // Section name string table (.shstrtab)
    free(sym_remap);  // last use of the remap table
    // First, add the section names for .strtab, .symtab, .shstrtab
    size_t strtab_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".strtab");
    size_t symtab_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".symtab");
    size_t shstrtab_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".shstrtab");
    
    // Add .rela.<sec> section names if we have relocations
    if (ctx->relocation_count > 0) {
        for (int g = 0; g < n_rela_groups; g++) {
            char rela_nm[48];
            if (rela_groups[g] >= 1 && rela_groups[g] <= (uint16_t)ctx->section_count) {
                snprintf(rela_nm, sizeof(rela_nm), ".rela.%s", ctx->sections[rela_groups[g] - 1].name);
            } else {
                snprintf(rela_nm, sizeof(rela_nm), ".rela.%u", rela_groups[g]);
            }
            rela_name_idx[g] = ctx->shstrtab_size;
            add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), rela_nm);
        }
    }
    
    // Add .note.GNU-stack section name
    size_t note_gnu_stack_name_idx = ctx->shstrtab_size;
    add_string(ctx->shstrtab, &ctx->shstrtab_size, sizeof(ctx->shstrtab), ".note.GNU-stack");
    
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
    shdr->sh_name = (uint32_t)strtab_name_idx;
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
    shdr->sh_name = (uint32_t)symtab_name_idx;
    shdr->sh_type = ELF_SHT_SYMTAB;
    shdr->sh_flags = 0;
    shdr->sh_addr = 0;
    shdr->sh_offset = symtab_offset;
    shdr->sh_size = (ctx->symbol_count + 1) * sizeof(Elf64Sym);
    shdr->sh_link = (uint32_t)(ctx->section_count + 1);  // Index of .strtab
    // sh_info = index of first global symbol (count local symbols, starting from 1 for null symbol)
    {
        int local_count = 1;  // Start with 1 for null symbol
        for (int i = 0; i < ctx->symbol_count; i++) {
            int binding = (ctx->symbols[i].info >> 4) & 0xF;
            if (binding == ELF_STB_LOCAL) local_count++;
        }
        shdr->sh_info = (uint32_t)local_count;
    }
    shdr->sh_addralign = 8;
    shdr->sh_entsize = sizeof(Elf64Sym);
    shdr++;

    // .rela.<sec> section headers (one per distinct relocated section)
    for (int g = 0; g < n_rela_groups; g++) {
        shdr->sh_name = (uint32_t)rela_name_idx[g];
        shdr->sh_type = ELF_SHT_RELA;
        shdr->sh_flags = 0;  /* Relocation sections are not loaded into memory */
        shdr->sh_addr = 0;
        shdr->sh_offset = rela_group_offsets[g];
        shdr->sh_size = rela_group_sizes[g];
        shdr->sh_link = ctx->section_count + 2;  // Index of .symtab (null + data + .strtab + .symtab)
        shdr->sh_info = rela_groups[g];  // Index of the section these relocations apply to
        shdr->sh_addralign = 8;
        shdr->sh_entsize = sizeof(Elf64Rela);
        shdr++;
    }

    // .note.GNU-stack section (signals non-executable stack)
    size_t note_gnu_stack_offset = 0;
    if (ctx->relocation_count > 0) {
        note_gnu_stack_offset = rela_text_offset + ctx->relocation_count * sizeof(Elf64Rela);
    } else {
        /* Calculate from symtab if no relocations */
        note_gnu_stack_offset = symtab_offset + (ctx->symbol_count + 1) * sizeof(Elf64Sym);
    }
    note_gnu_stack_offset = (note_gnu_stack_offset + 7) & ~7;  // Align to 8 bytes

    // .note.GNU-stack section header (signals non-executable stack)
    shdr->sh_name = (uint32_t)note_gnu_stack_name_idx;
    shdr->sh_type = ELF_SHT_PROGBITS;
    shdr->sh_flags = 0;  /* Not allocated, no special flags */
    shdr->sh_addr = 0;
    shdr->sh_offset = note_gnu_stack_offset;
    shdr->sh_size = 0;  /* Empty section */
    shdr->sh_link = 0;
    shdr->sh_info = 0;
    shdr->sh_addralign = 1;
    shdr->sh_entsize = 0;
    shdr++;

    // .shstrtab section header
    shdr->sh_name = (uint32_t)shstrtab_name_idx;
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
    // Total: null + data sections + .strtab + .symtab + (.rela.<sec> x n) + .note.GNU-stack + .shstrtab
    int extra_sections = 4 + n_rela_groups;  // .strtab, .symtab, .rela.<sec> groups, .note.GNU-stack, .shstrtab
    offset = section_headers_offset + (ctx->section_count + extra_sections + 1) * sizeof(Elf64SectionHeader);

    // ========================================================================
    // Update ELF Header
    // ========================================================================
    ehdr = (Elf64Header*)buffer;
    ehdr->e_shoff = section_headers_offset;
    ehdr->e_shnum = ctx->section_count + extra_sections + 1;  // +1 for null section
    ehdr->e_shstrndx = ctx->section_count + extra_sections;  // Index of .shstrtab

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

    free(ctx->symbols);
    ctx->symbols = NULL;
    free(ctx->strtab);
    ctx->strtab = NULL;
    free(ctx->reloc_offsets);
    ctx->reloc_symbol_indices = NULL;
    free(ctx->reloc_symbol_indices);
    free(ctx->reloc_types);
    free(ctx->reloc_addends);
    free(ctx->reloc_section_indices);
    ctx->reloc_offsets = NULL;
    ctx->reloc_symbol_indices = NULL;
    ctx->reloc_types = NULL;
    ctx->reloc_addends = NULL;
    ctx->reloc_section_indices = NULL;
    ctx->symbol_capacity = 0;
    ctx->strtab_capacity = 0;
    ctx->reloc_capacity = 0;

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
