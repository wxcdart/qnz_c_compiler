#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zero_cc/dwarf/dwarf.h"
#include "zero_cc/core/buffers.h"

/* Abbreviation codes */
enum {
    ABBREV_NONE = 0,
    ABBREV_COMPILE_UNIT,
    ABBREV_BASE_TYPE,
    ABBREV_POINTER_TYPE,
    ABBREV_POINTER_TYPE_VOID,
    ABBREV_SUBPROGRAM,
    ABBREV_SUBPROGRAM_VOID,
    ABBREV_SUBPROGRAM_STATIC,
    ABBREV_SUBPROGRAM_STATIC_VOID,
    ABBREV_FORMAL_PARAM,
    ABBREV_VARIABLE,
    ABBREV_VARIABLE_GLOBAL,
    ABBREV_VARIABLE_STATIC_GLOBAL,
    ABBREV_ARRAY_TYPE,
    ABBREV_SUBRANGE_TYPE
};

typedef struct {
    uint16_t section_idx;
    uint64_t offset;
    const char *target_symbol;
    uint32_t rel_type;
    int64_t addend;
} DwarfReloc;

typedef struct {
    DwarfReloc *entries;
    size_t count;
    size_t capacity;
} DwarfRelocList;

static void dwarf_relocs_init(DwarfRelocList *list) {
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void dwarf_relocs_add(DwarfRelocList *list, uint16_t sec_idx, uint64_t offset,
                             const char *target_sym, uint32_t rel_type, int64_t addend) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 64;
        DwarfReloc *new_entries = (DwarfReloc *)realloc(list->entries, new_cap * sizeof(DwarfReloc));
        if (!new_entries) return;
        list->entries = new_entries;
        list->capacity = new_cap;
    }
    list->entries[list->count++] = (DwarfReloc){
        .section_idx = sec_idx,
        .offset = offset,
        .target_symbol = target_sym,
        .rel_type = rel_type,
        .addend = addend
    };
}

static void dwarf_relocs_free(DwarfRelocList *list) {
    if (list->entries) {
        free(list->entries);
        list->entries = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

/* Low-level emission helpers */
static inline void dwarf_emit_u8(ZeroCCBuffer *buf, uint8_t val) {
    zero_cc_buf_put_bytes(buf, &val, 1);
}

static inline void dwarf_emit_u16(ZeroCCBuffer *buf, uint16_t val) {
    uint8_t bytes[2] = { (uint8_t)val, (uint8_t)(val >> 8) };
    zero_cc_buf_put_bytes(buf, bytes, 2);
}

static inline void dwarf_emit_u32(ZeroCCBuffer *buf, uint32_t val) {
    uint8_t bytes[4] = {
        (uint8_t)val, (uint8_t)(val >> 8),
        (uint8_t)(val >> 16), (uint8_t)(val >> 24)
    };
    zero_cc_buf_put_bytes(buf, bytes, 4);
}

static inline void dwarf_emit_u64(ZeroCCBuffer *buf, uint64_t val) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(val >> (i * 8));
    zero_cc_buf_put_bytes(buf, bytes, 8);
}

static inline void dwarf_emit_uleb128(ZeroCCBuffer *buf, uint64_t val) {
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val != 0) byte |= 0x80;
        zero_cc_buf_put_bytes(buf, &byte, 1);
    } while (val != 0);
}

static inline void dwarf_emit_sleb128(ZeroCCBuffer *buf, int64_t val) {
    bool more = true;
    while (more) {
        uint8_t byte = (uint8_t)(val & 0x7F);
        val >>= 7;
        if ((val == 0 && (byte & 0x40) == 0) || (val == -1 && (byte & 0x40) != 0)) {
            more = false;
        } else {
            byte |= 0x80;
        }
        zero_cc_buf_put_bytes(buf, &byte, 1);
    }
}

static inline void dwarf_emit_string(ZeroCCBuffer *buf, const char *str) {
    if (!str) str = "";
    zero_cc_buf_put_bytes(buf, str, strlen(str) + 1);
}

static uint32_t dwarf_str_add(ZeroCCBuffer *str_buf, const char *str) {
    if (!str) str = "";
    size_t len = strlen(str) + 1;
    for (size_t i = 0; i + len <= str_buf->length; i++) {
        if (memcmp(str_buf->data + i, str, len) == 0) {
            return (uint32_t)i;
        }
    }
    uint32_t offset = (uint32_t)str_buf->length;
    zero_cc_buf_put_bytes(str_buf, str, len);
    return offset;
}

void zero_cc_dwarf_mark_prologue_end(ZeroCCCompiler *c) {
    if (c) c->dwarf_pending_prologue_end = true;
}

void zero_cc_dwarf_add_line(ZeroCCCompiler *c, size_t text_offset,
                            uint32_t line, uint16_t col, bool is_stmt) {
    if (!c || line == 0) return;
    bool is_prologue_end = false;
    if (c->dwarf_pending_prologue_end) {
        is_prologue_end = true;
        c->dwarf_pending_prologue_end = false;
    }
    if (c->dwarf_line_count > 0) {
        ZeroCCDwarfLineEntry *last = &c->dwarf_lines[c->dwarf_line_count - 1];
        if (last->text_offset == text_offset) {
            last->line = line;
            last->col = col;
            last->is_stmt = is_stmt;
            if (is_prologue_end) last->is_prologue_end = true;
            return;
        }
        if (last->line == line && last->col == col && !is_prologue_end) {
            return;
        }
    }
    if (c->dwarf_line_count >= c->dwarf_line_capacity) {
        size_t new_cap = c->dwarf_line_capacity ? c->dwarf_line_capacity * 2 : 256;
        ZeroCCDwarfLineEntry *new_entries = (ZeroCCDwarfLineEntry *)realloc(
            c->dwarf_lines, new_cap * sizeof(ZeroCCDwarfLineEntry));
        if (!new_entries) return;
        c->dwarf_lines = new_entries;
        c->dwarf_line_capacity = new_cap;
    }
    c->dwarf_lines[c->dwarf_line_count++] = (ZeroCCDwarfLineEntry){
        .text_offset = text_offset,
        .line = line,
        .col = col,
        .is_stmt = is_stmt,
        .is_prologue_end = is_prologue_end
    };
}

/* Generate .debug_abbrev section content */
static void dwarf_generate_abbrev(ZeroCCBuffer *out) {
    /* Abbrev 1: DW_TAG_compile_unit (children = yes) */
    dwarf_emit_uleb128(out, ABBREV_COMPILE_UNIT);
    dwarf_emit_uleb128(out, DW_TAG_compile_unit);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_producer);  dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_language);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_comp_dir);  dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_low_pc);    dwarf_emit_uleb128(out, DW_FORM_addr);
    dwarf_emit_uleb128(out, DW_AT_high_pc);   dwarf_emit_uleb128(out, DW_FORM_data8);
    dwarf_emit_uleb128(out, DW_AT_stmt_list); dwarf_emit_uleb128(out, DW_FORM_sec_offset);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 2: DW_TAG_base_type (children = no) */
    dwarf_emit_uleb128(out, ABBREV_BASE_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_base_type);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_string);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_encoding);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 3: DW_TAG_pointer_type (children = no) */
    dwarf_emit_uleb128(out, ABBREV_POINTER_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_pointer_type);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 4: DW_TAG_pointer_type_void (children = no) */
    dwarf_emit_uleb128(out, ABBREV_POINTER_TYPE_VOID);
    dwarf_emit_uleb128(out, DW_TAG_pointer_type);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 5: DW_TAG_subprogram (children = yes, external, typed) */
    dwarf_emit_uleb128(out, ABBREV_SUBPROGRAM);
    dwarf_emit_uleb128(out, DW_TAG_subprogram);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_external);   dwarf_emit_uleb128(out, DW_FORM_flag_present);
    dwarf_emit_uleb128(out, DW_AT_name);       dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line);  dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);       dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_low_pc);     dwarf_emit_uleb128(out, DW_FORM_addr);
    dwarf_emit_uleb128(out, DW_AT_high_pc);    dwarf_emit_uleb128(out, DW_FORM_data8);
    dwarf_emit_uleb128(out, DW_AT_frame_base); dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);                dwarf_emit_uleb128(out, 0);

    /* Abbrev 6: DW_TAG_subprogram_void (children = yes, external, void) */
    dwarf_emit_uleb128(out, ABBREV_SUBPROGRAM_VOID);
    dwarf_emit_uleb128(out, DW_TAG_subprogram);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_external);   dwarf_emit_uleb128(out, DW_FORM_flag_present);
    dwarf_emit_uleb128(out, DW_AT_name);       dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line);  dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_low_pc);     dwarf_emit_uleb128(out, DW_FORM_addr);
    dwarf_emit_uleb128(out, DW_AT_high_pc);    dwarf_emit_uleb128(out, DW_FORM_data8);
    dwarf_emit_uleb128(out, DW_AT_frame_base); dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);                dwarf_emit_uleb128(out, 0);

    /* Abbrev 7: DW_TAG_subprogram_static (children = yes, static, typed) */
    dwarf_emit_uleb128(out, ABBREV_SUBPROGRAM_STATIC);
    dwarf_emit_uleb128(out, DW_TAG_subprogram);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_name);       dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line);  dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);       dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_low_pc);     dwarf_emit_uleb128(out, DW_FORM_addr);
    dwarf_emit_uleb128(out, DW_AT_high_pc);    dwarf_emit_uleb128(out, DW_FORM_data8);
    dwarf_emit_uleb128(out, DW_AT_frame_base); dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);                dwarf_emit_uleb128(out, 0);

    /* Abbrev 8: DW_TAG_subprogram_static_void (children = yes, static, void) */
    dwarf_emit_uleb128(out, ABBREV_SUBPROGRAM_STATIC_VOID);
    dwarf_emit_uleb128(out, DW_TAG_subprogram);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_name);       dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line);  dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_low_pc);     dwarf_emit_uleb128(out, DW_FORM_addr);
    dwarf_emit_uleb128(out, DW_AT_high_pc);    dwarf_emit_uleb128(out, DW_FORM_data8);
    dwarf_emit_uleb128(out, DW_AT_frame_base); dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);                dwarf_emit_uleb128(out, 0);

    /* Abbrev 9: DW_TAG_formal_parameter (children = no) */
    dwarf_emit_uleb128(out, ABBREV_FORMAL_PARAM);
    dwarf_emit_uleb128(out, DW_TAG_formal_parameter);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_location);  dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 10: DW_TAG_variable (children = no, local) */
    dwarf_emit_uleb128(out, ABBREV_VARIABLE);
    dwarf_emit_uleb128(out, DW_TAG_variable);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_location);  dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 11: DW_TAG_variable_global (children = no, global) */
    dwarf_emit_uleb128(out, ABBREV_VARIABLE_GLOBAL);
    dwarf_emit_uleb128(out, DW_TAG_variable);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_external);  dwarf_emit_uleb128(out, DW_FORM_flag_present);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_location);  dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 12: DW_TAG_variable_static_global (children = no, static global) */
    dwarf_emit_uleb128(out, ABBREV_VARIABLE_STATIC_GLOBAL);
    dwarf_emit_uleb128(out, DW_TAG_variable);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_location);  dwarf_emit_uleb128(out, DW_FORM_exprloc);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 13: DW_TAG_array_type (children = yes) */
    dwarf_emit_uleb128(out, ABBREV_ARRAY_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_array_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 14: DW_TAG_subrange_type (children = no) */
    dwarf_emit_uleb128(out, ABBREV_SUBRANGE_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_subrange_type);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_upper_bound); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);                 dwarf_emit_uleb128(out, 0);

    /* End of table */
    dwarf_emit_u8(out, 0);
}

/* Generate .debug_line section content */
static void dwarf_generate_line_table(ZeroCCCompiler *c,
                                      ZeroCCBuffer *out,
                                      uint16_t debug_line_idx,
                                      uint32_t rel_addr_type,
                                      DwarfRelocList *relocs) {
    size_t unit_length_pos = out->length;
    dwarf_emit_u32(out, 0);  /* unit_length placeholder */

    /* Version 4 */
    dwarf_emit_u16(out, 4);

    size_t header_length_pos = out->length;
    dwarf_emit_u32(out, 0);  /* header_length placeholder */
    size_t header_start = out->length;

    dwarf_emit_u8(out, 1);    /* minimum_instruction_length */
    dwarf_emit_u8(out, 1);    /* maximum_operations_per_instruction */
    dwarf_emit_u8(out, 1);    /* default_is_stmt */
    dwarf_emit_u8(out, (uint8_t)(int8_t)-5); /* line_base = -5 */
    dwarf_emit_u8(out, 14);   /* line_range = 14 */
    dwarf_emit_u8(out, 13);   /* opcode_base = 13 */

    /* Standard opcode argument counts (12 opcodes) */
    uint8_t std_opcodes[12] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    zero_cc_buf_put_bytes(out, std_opcodes, sizeof(std_opcodes));

    /* Source file and directory resolution */
    const char *src_file = c->source_filename ? c->source_filename : "source.c";
    const char *slash = strrchr(src_file, '/');
    char dir_buf[1024];
    uint32_t dir_idx = 0;

    if (slash) {
        size_t dirlen = (size_t)(slash - src_file);
        if (dirlen == 0) {
            dir_buf[0] = '/';
            dir_buf[1] = '\0';
        } else {
            if (dirlen >= sizeof(dir_buf)) dirlen = sizeof(dir_buf) - 1;
            memcpy(dir_buf, src_file, dirlen);
            dir_buf[dirlen] = '\0';
        }
        dwarf_emit_string(out, dir_buf);
        dwarf_emit_u8(out, 0);  /* End of directories */
        dir_idx = 1;
    } else {
        dwarf_emit_u8(out, 0);  /* Empty directory table */
        dir_idx = 0;
    }

    /* File names table */
    const char *fname = slash ? slash + 1 : src_file;
    dwarf_emit_string(out, fname);
    dwarf_emit_uleb128(out, dir_idx);  /* Directory index */
    dwarf_emit_uleb128(out, 0);        /* Time */
    dwarf_emit_uleb128(out, 0);        /* Size */
    dwarf_emit_u8(out, 0);             /* End of file names */

    /* Patch header length */
    size_t header_len = out->length - header_start;
    *(uint32_t *)(out->data + header_length_pos) = (uint32_t)header_len;

    /* Line number program */
    uint64_t cur_addr = 0;
    int32_t cur_line = 1;
    uint32_t cur_col = 0;

    int addr_size = (c->isa == ISA_RISCV32) ? 4 : 8;

    /* Set starting address with relocation to .text + 0 */
    dwarf_emit_u8(out, 0);
    dwarf_emit_uleb128(out, addr_size + 1);
    dwarf_emit_u8(out, DW_LNE_set_address);
    dwarf_relocs_add(relocs, debug_line_idx, out->length, ".text", rel_addr_type, 0);
    if (addr_size == 4) {
        dwarf_emit_u32(out, 0);
    } else {
        dwarf_emit_u64(out, 0);
    }

    for (size_t i = 0; i < c->dwarf_line_count; i++) {
        ZeroCCDwarfLineEntry *ent = &c->dwarf_lines[i];
        if (ent->text_offset < cur_addr) continue;

        int64_t addr_diff = (int64_t)(ent->text_offset - cur_addr);
        int64_t line_diff = (int64_t)ent->line - (int64_t)cur_line;

        if (ent->col > 0 && ent->col != cur_col) {
            dwarf_emit_u8(out, DW_LNS_set_column);
            dwarf_emit_uleb128(out, ent->col);
            cur_col = ent->col;
        }

        if (ent->is_prologue_end) {
            dwarf_emit_u8(out, DW_LNS_set_prologue_end);
        }

        /* Check if fits in special opcode */
        int opcode = (int)(line_diff - (-5)) + (14 * (int)addr_diff) + 13;
        if (line_diff >= -5 && line_diff <= 8 && opcode >= 13 && opcode <= 255) {
            dwarf_emit_u8(out, (uint8_t)opcode);
            cur_addr = ent->text_offset;
            cur_line = ent->line;
        } else {
            if (line_diff != 0) {
                dwarf_emit_u8(out, DW_LNS_advance_line);
                dwarf_emit_sleb128(out, line_diff);
                cur_line = ent->line;
            }
            if (addr_diff > 0) {
                dwarf_emit_u8(out, DW_LNS_advance_pc);
                dwarf_emit_uleb128(out, (uint64_t)addr_diff);
                cur_addr = ent->text_offset;
            }
            dwarf_emit_u8(out, DW_LNS_copy);
        }
    }

    /* Trailing PC advance if code exists past last line entry */
    if (c->text_section.length > cur_addr) {
        uint64_t rem = c->text_section.length - cur_addr;
        dwarf_emit_u8(out, DW_LNS_advance_pc);
        dwarf_emit_uleb128(out, rem);
        cur_addr = c->text_section.length;
    }

    /* End of sequence */
    dwarf_emit_u8(out, 0);
    dwarf_emit_uleb128(out, 1);
    dwarf_emit_u8(out, DW_LNE_end_sequence);

    /* Patch unit length */
    size_t unit_len = out->length - (unit_length_pos + 4);
    *(uint32_t *)(out->data + unit_length_pos) = (uint32_t)unit_len;
}

/* Map ZeroCCType to a base type DIE or pointer type DIE */
static uint32_t dwarf_get_type_ref(ZeroCCBuffer *info, ZeroCCCompiler *c,
                                   ZeroCCType type, uint32_t *base_refs,
                                   uint32_t void_ptr_ref) {
    if (type.tag == TYPE_PTR) {
        if (!type.base || type.base->tag == TYPE_VOID) {
            return void_ptr_ref;
        }
        uint32_t pointee_ref = dwarf_get_type_ref(info, c, *type.base, base_refs, void_ptr_ref);
        uint32_t ptr_ref = (uint32_t)info->length;
        dwarf_emit_uleb128(info, ABBREV_POINTER_TYPE);
        dwarf_emit_u8(info, (c->isa == ISA_RISCV32) ? 4 : 8);
        dwarf_emit_u32(info, pointee_ref);
        return ptr_ref;
    }
    if (type.tag == TYPE_ARRAY && type.base) {
        uint32_t elem_ref = dwarf_get_type_ref(info, c, *type.base, base_refs, void_ptr_ref);
        uint32_t arr_ref = (uint32_t)info->length;
        dwarf_emit_uleb128(info, ABBREV_ARRAY_TYPE);
        dwarf_emit_u32(info, elem_ref);
        dwarf_emit_uleb128(info, ABBREV_SUBRANGE_TYPE);
        uint64_t ubound = (type.array_len > 0) ? (uint64_t)(type.array_len - 1) : 0;
        dwarf_emit_uleb128(info, ubound);
        dwarf_emit_u8(info, 0);  /* End of array children */
        return arr_ref;
    }

    /* Base types */
    switch (type.tag) {
        case TYPE_INT32:  return base_refs[0];  /* int */
        case TYPE_UINT32: return base_refs[1];  /* unsigned int */
        case TYPE_INT8:   return base_refs[2];  /* char */
        case TYPE_UINT8:  return base_refs[3];  /* unsigned char */
        case TYPE_INT16:  return base_refs[4];  /* short */
        case TYPE_UINT16: return base_refs[5];  /* unsigned short */
        case TYPE_INT64:  return base_refs[6];  /* long */
        case TYPE_UINT64: return base_refs[7];  /* unsigned long */
        case TYPE_FLOAT:  return base_refs[8];  /* float */
        case TYPE_DOUBLE: return base_refs[9];  /* double */
        case TYPE_BOOL:   return base_refs[10]; /* bool */
        default:          return base_refs[0];  /* default to int */
    }
}

/* Frame base register operation by architecture */
static uint8_t dwarf_frame_base_op(ZeroCCISA isa) {
    switch (isa) {
        case ISA_X86_64:  return DW_OP_reg6;   /* %rbp */
        case ISA_AARCH64: return DW_OP_reg29;  /* x29 */
        case ISA_RISCV64:
        case ISA_RISCV32: return DW_OP_reg8;   /* s0 / fp */
        default:          return DW_OP_reg6;
    }
}

/* Generate .debug_info section content */
static void dwarf_generate_info(ZeroCCCompiler *c,
                                ZeroCCBuffer *info,
                                ZeroCCBuffer *str_buf,
                                uint16_t debug_info_idx,
                                uint32_t rel_sec_type,
                                uint32_t rel_addr_type,
                                DwarfRelocList *relocs) {
    size_t unit_length_pos = info->length;
    dwarf_emit_u32(info, 0);  /* unit_length placeholder */

    /* Version 4 */
    dwarf_emit_u16(info, 4);

    /* debug_abbrev_offset (relocation against .debug_abbrev + 0) */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_abbrev", rel_sec_type, 0);
    dwarf_emit_u32(info, 0);

    /* Address size */
    int addr_size = (c->isa == ISA_RISCV32) ? 4 : 8;
    dwarf_emit_u8(info, (uint8_t)addr_size);

    /* String offsets */
    uint32_t prod_off = dwarf_str_add(str_buf, "ZeroCC 0.1");
    const char *src_file = c->source_filename ? c->source_filename : "source.c";
    uint32_t name_off = dwarf_str_add(str_buf, src_file);
    const char *comp_dir = c->comp_dir ? c->comp_dir : ".";
    uint32_t dir_off = dwarf_str_add(str_buf, comp_dir);

    /* Compile Unit DIE (Abbrev 1) */
    dwarf_emit_uleb128(info, ABBREV_COMPILE_UNIT);
    /* DW_AT_producer */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, prod_off);
    dwarf_emit_u32(info, prod_off);
    /* DW_AT_language (C99) */
    dwarf_emit_u8(info, DW_LANG_C99);
    /* DW_AT_name */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, name_off);
    dwarf_emit_u32(info, name_off);
    /* DW_AT_comp_dir */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, dir_off);
    dwarf_emit_u32(info, dir_off);
    /* DW_AT_low_pc (.text + 0) */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".text", rel_addr_type, 0);
    if (addr_size == 4) {
        dwarf_emit_u32(info, 0);
    } else {
        dwarf_emit_u64(info, 0);
    }
    /* DW_AT_high_pc (size of .text) */
    dwarf_emit_u64(info, c->text_section.length);
    /* DW_AT_stmt_list (.debug_line + 0) */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_line", rel_sec_type, 0);
    dwarf_emit_u32(info, 0);

    /* Base types */
    enum { NUM_BASE = 11 };
    uint32_t base_refs[NUM_BASE];

    struct {
        const char *name;
        uint8_t size;
        uint8_t encoding;
    } base_defs[NUM_BASE] = {
        { "int", 4, DW_ATE_signed },
        { "unsigned int", 4, DW_ATE_unsigned },
        { "char", 1, DW_ATE_signed_char },
        { "unsigned char", 1, DW_ATE_unsigned_char },
        { "short", 2, DW_ATE_signed },
        { "unsigned short", 2, DW_ATE_unsigned },
        { "long int", 8, DW_ATE_signed },
        { "long unsigned int", 8, DW_ATE_unsigned },
        { "float", 4, DW_ATE_float },
        { "double", 8, DW_ATE_float },
        { "_Bool", 1, DW_ATE_boolean }
    };

    for (int b = 0; b < NUM_BASE; b++) {
        base_refs[b] = (uint32_t)info->length;
        dwarf_emit_uleb128(info, ABBREV_BASE_TYPE);
        dwarf_emit_string(info, base_defs[b].name);
        dwarf_emit_u8(info, base_defs[b].size);
        dwarf_emit_u8(info, base_defs[b].encoding);
    }

    /* Void pointer type */
    uint32_t void_ptr_ref = (uint32_t)info->length;
    dwarf_emit_uleb128(info, ABBREV_POINTER_TYPE_VOID);
    dwarf_emit_u8(info, (c->isa == ISA_RISCV32) ? 4 : 8);

    /* Global variables */
    for (size_t gi = 0; gi < c->global_var_count; gi++) {
        ZeroCCGlobalVar *gv = &c->global_vars[gi];
        if (!gv->name[0] || gv->alias_target[0] || gv->is_tls) continue;

        uint32_t gv_name_off = dwarf_str_add(str_buf, gv->name);
        uint32_t type_ref = dwarf_get_type_ref(info, c, gv->type, base_refs, void_ptr_ref);

        if (gv->is_static) {
            dwarf_emit_uleb128(info, ABBREV_VARIABLE_STATIC_GLOBAL);
        } else {
            dwarf_emit_uleb128(info, ABBREV_VARIABLE_GLOBAL);
        }
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, gv_name_off);
        dwarf_emit_u32(info, gv_name_off);
        dwarf_emit_u8(info, 1);    /* decl_file */
        dwarf_emit_uleb128(info, 1); /* decl_line */
        dwarf_emit_u32(info, type_ref);

        /* Location: DW_OP_addr + offset in .data or .bss */
        const char *sec_sym = gv->is_bss ? ".bss" : ".data";
        dwarf_emit_uleb128(info, (uint64_t)(1 + addr_size)); /* exprloc length */
        dwarf_emit_u8(info, DW_OP_addr);
        dwarf_relocs_add(relocs, debug_info_idx, info->length, sec_sym, rel_addr_type, gv->data_offset);
        if (addr_size == 4) {
            dwarf_emit_u32(info, (uint32_t)gv->data_offset);
        } else {
            dwarf_emit_u64(info, gv->data_offset);
        }
    }

    uint8_t fb_op = dwarf_frame_base_op(c->isa);

    /* Functions */
    for (size_t fi = 0; fi < c->function_count; fi++) {
        ZeroCCFunction *fn = &c->functions[fi];
        if (!fn->name[0] || fn->is_prototype || fn->is_external || fn->is_object) {
            continue;
        }

        uint32_t fn_name_off = dwarf_str_add(str_buf, fn->name);
        bool is_void = (fn->return_type.tag == TYPE_VOID);
        uint32_t ret_type_ref = 0;
        if (!is_void) {
            ret_type_ref = dwarf_get_type_ref(info, c, fn->return_type, base_refs, void_ptr_ref);
        }

        uint32_t abbrev;
        if (fn->is_static) {
            abbrev = is_void ? ABBREV_SUBPROGRAM_STATIC_VOID : ABBREV_SUBPROGRAM_STATIC;
        } else {
            abbrev = is_void ? ABBREV_SUBPROGRAM_VOID : ABBREV_SUBPROGRAM;
        }

        dwarf_emit_uleb128(info, abbrev);
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, fn_name_off);
        dwarf_emit_u32(info, fn_name_off);
        dwarf_emit_u8(info, 1);  /* decl_file */
        dwarf_emit_uleb128(info, fn->line ? fn->line : 1);
        if (!is_void) {
            dwarf_emit_u32(info, ret_type_ref);
        }
        /* DW_AT_low_pc */
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".text", rel_addr_type, fn->text_offset);
        if (addr_size == 4) {
            dwarf_emit_u32(info, (uint32_t)fn->text_offset);
        } else {
            dwarf_emit_u64(info, fn->text_offset);
        }
        /* DW_AT_high_pc */
        dwarf_emit_u64(info, fn->text_size);

        /* DW_AT_frame_base: DW_OP_reg* (1 byte) */
        dwarf_emit_uleb128(info, 1);  /* 1 byte expression */
        dwarf_emit_u8(info, fb_op);

        /* Parameters and local variables */
        for (size_t vi = 0; vi < fn->dwarf_var_count; vi++) {
            ZeroCCDwarfVar *var = &fn->dwarf_vars[vi];
            if (!var->name[0] || var->name[0] == '.') continue;

            uint32_t var_name_off = dwarf_str_add(str_buf, var->name);
            uint32_t var_type_ref = dwarf_get_type_ref(info, c, var->type, base_refs, void_ptr_ref);

            if (var->is_param) {
                dwarf_emit_uleb128(info, ABBREV_FORMAL_PARAM);
            } else {
                dwarf_emit_uleb128(info, ABBREV_VARIABLE);
            }
            dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, var_name_off);
            dwarf_emit_u32(info, var_name_off);
            dwarf_emit_u8(info, 1);  /* decl_file */
            dwarf_emit_uleb128(info, var->line ? var->line : (fn->line ? fn->line : 1));
            dwarf_emit_u32(info, var_type_ref);

            if (var->is_static_local) {
                /* Static local in .data */
                dwarf_emit_uleb128(info, (uint64_t)(1 + addr_size));
                dwarf_emit_u8(info, DW_OP_addr);
                dwarf_relocs_add(relocs, debug_info_idx, info->length, ".data", rel_addr_type, var->static_data_offset);
                if (addr_size == 4) {
                    dwarf_emit_u32(info, (uint32_t)var->static_data_offset);
                } else {
                    dwarf_emit_u64(info, var->static_data_offset);
                }
            } else {
                /* Local variable / parameter at RBP - stack_offset */
                int64_t fb_disp = -(int64_t)var->stack_offset;
                /* Measure SLEB128 length */
                uint8_t sleb_buf[16];
                int sleb_len = 0;
                int64_t sv = fb_disp;
                bool more = true;
                while (more) {
                    uint8_t b = (uint8_t)(sv & 0x7F);
                    sv >>= 7;
                    if ((sv == 0 && (b & 0x40) == 0) || (sv == -1 && (b & 0x40) != 0)) {
                        more = false;
                    } else {
                        b |= 0x80;
                    }
                    sleb_buf[sleb_len++] = b;
                }
                dwarf_emit_uleb128(info, (uint64_t)(1 + sleb_len));
                dwarf_emit_u8(info, DW_OP_fbreg);
                zero_cc_buf_put_bytes(info, sleb_buf, (size_t)sleb_len);
            }
        }

        /* End of function children */
        dwarf_emit_u8(info, 0);
    }

    /* End of CU children */
    dwarf_emit_u8(info, 0);

    /* Patch unit length */
    size_t unit_len = info->length - (unit_length_pos + 4);
    *(uint32_t *)(info->data + unit_length_pos) = (uint32_t)unit_len;
}

bool zero_cc_dwarf_emit(ZeroCCCompiler *c, ElfWriterCtx *elf_ctx) {
    if (!c || !elf_ctx || !c->debug_info) return true;

    ZeroCCBuffer line_buf;
    zero_cc_buf_init(&line_buf, 1024);

    ZeroCCBuffer abbrev_buf;
    zero_cc_buf_init(&abbrev_buf, 512);

    ZeroCCBuffer str_buf;
    zero_cc_buf_init(&str_buf, 1024);

    ZeroCCBuffer info_buf;
    zero_cc_buf_init(&info_buf, 2048);

    DwarfRelocList relocs;
    dwarf_relocs_init(&relocs);

    /* Determine relocation types based on target ISA */
    uint32_t rel_sec_type;   /* 32-bit reloc for DWARF section references */
    uint32_t rel_addr_type;  /* Address reloc for .text/.data references */

    switch (c->isa) {
        case ISA_X86_64:
            rel_sec_type = 10;   /* R_X86_64_32 */
            rel_addr_type = 1;   /* R_X86_64_64 */
            break;
        case ISA_AARCH64:
            rel_sec_type = 258;  /* R_AARCH64_ABS32 */
            rel_addr_type = 257; /* R_AARCH64_ABS64 */
            break;
        case ISA_RISCV64:
            rel_sec_type = 1;    /* R_RISCV_32 */
            rel_addr_type = 2;   /* R_RISCV_64 */
            break;
        case ISA_RISCV32:
            rel_sec_type = 1;    /* R_RISCV_32 */
            rel_addr_type = 1;   /* R_RISCV_32 */
            break;
        default:
            rel_sec_type = 10;
            rel_addr_type = 1;
            break;
    }

    /* 1. Generate .debug_abbrev */
    dwarf_generate_abbrev(&abbrev_buf);

    /* 2. Add sections to ELF writer first to obtain section indices */
    elf_writer_add_section(elf_ctx, ".debug_abbrev",
                           (const uint8_t *)abbrev_buf.data, abbrev_buf.length,
                           ELF_SHT_PROGBITS, 0, 1);
    uint16_t debug_abbrev_idx = elf_writer_section_index(elf_ctx, ".debug_abbrev");

    /* Placeholder additions for the remaining debug sections to get indices */
    elf_writer_add_section(elf_ctx, ".debug_line",
                           (const uint8_t *)"", 1,
                           ELF_SHT_PROGBITS, 0, 1);
    uint16_t debug_line_idx = elf_writer_section_index(elf_ctx, ".debug_line");

    elf_writer_add_section(elf_ctx, ".debug_str",
                           (const uint8_t *)"", 1,
                           ELF_SHT_PROGBITS, 0, 1);
    uint16_t debug_str_idx = elf_writer_section_index(elf_ctx, ".debug_str");

    elf_writer_add_section(elf_ctx, ".debug_info",
                           (const uint8_t *)"", 1,
                           ELF_SHT_PROGBITS, 0, 1);
    uint16_t debug_info_idx = elf_writer_section_index(elf_ctx, ".debug_info");

    /* 3. Generate .debug_line */
    dwarf_generate_line_table(c, &line_buf, debug_line_idx, rel_addr_type, &relocs);

    /* 4. Generate .debug_info and .debug_str */
    dwarf_generate_info(c, &info_buf, &str_buf, debug_info_idx, rel_sec_type, rel_addr_type, &relocs);

    /* 5. Update section payloads with generated buffers */
    if (debug_line_idx > 0 && debug_line_idx <= (uint16_t)elf_ctx->section_count) {
        ElfWriterSection *sec = &elf_ctx->sections[debug_line_idx - 1];
        if (sec->capacity < line_buf.length) {
            uint8_t *nd = (uint8_t *)realloc(sec->data, line_buf.length);
            if (nd) { sec->data = nd; sec->capacity = line_buf.length; }
        }
        if (sec->data) {
            memcpy(sec->data, line_buf.data, line_buf.length);
            sec->size = line_buf.length;
        }
    }
    if (debug_str_idx > 0 && debug_str_idx <= (uint16_t)elf_ctx->section_count) {
        ElfWriterSection *sec = &elf_ctx->sections[debug_str_idx - 1];
        if (sec->capacity < str_buf.length) {
            uint8_t *nd = (uint8_t *)realloc(sec->data, str_buf.length);
            if (nd) { sec->data = nd; sec->capacity = str_buf.length; }
        }
        if (sec->data) {
            memcpy(sec->data, str_buf.data, str_buf.length);
            sec->size = str_buf.length;
        }
    }
    if (debug_info_idx > 0 && debug_info_idx <= (uint16_t)elf_ctx->section_count) {
        ElfWriterSection *sec = &elf_ctx->sections[debug_info_idx - 1];
        if (sec->capacity < info_buf.length) {
            uint8_t *nd = (uint8_t *)realloc(sec->data, info_buf.length);
            if (nd) { sec->data = nd; sec->capacity = info_buf.length; }
        }
        if (sec->data) {
            memcpy(sec->data, info_buf.data, info_buf.length);
            sec->size = info_buf.length;
        }
    }

    /* 6. Add section symbols for the debug sections */
    elf_writer_add_symbol(elf_ctx, ".debug_line", ELF_STB_LOCAL, ELF_STT_SECTION, debug_line_idx, 0, 0);
    elf_writer_add_symbol(elf_ctx, ".debug_str", ELF_STB_LOCAL, ELF_STT_SECTION, debug_str_idx, 0, 0);
    elf_writer_add_symbol(elf_ctx, ".debug_abbrev", ELF_STB_LOCAL, ELF_STT_SECTION, debug_abbrev_idx, 0, 0);
    elf_writer_add_symbol(elf_ctx, ".debug_info", ELF_STB_LOCAL, ELF_STT_SECTION, debug_info_idx, 0, 0);

    /* 7. Emit relocations into elf_ctx */
    for (size_t r = 0; r < relocs.count; r++) {
        DwarfReloc *dr = &relocs.entries[r];
        int sym_idx = 0;
        for (int j = 0; j < elf_ctx->symbol_count; j++) {
            if (strcmp(elf_ctx->strtab + elf_ctx->symbols[j].name_offset, dr->target_symbol) == 0) {
                sym_idx = j + 1;
                break;
            }
        }
        if (sym_idx == 0) {
            uint16_t target_sidx = elf_writer_section_index(elf_ctx, dr->target_symbol);
            elf_writer_add_symbol(elf_ctx, dr->target_symbol, ELF_STB_LOCAL, ELF_STT_SECTION, target_sidx, 0, 0);
            sym_idx = elf_ctx->symbol_count;
        }
        elf_writer_add_relocation(elf_ctx, dr->section_idx, dr->offset,
                                  (uint16_t)sym_idx, dr->rel_type, dr->addend);
    }

    /* Cleanup */
    dwarf_relocs_free(&relocs);
    zero_cc_buf_free(&line_buf);
    zero_cc_buf_free(&abbrev_buf);
    zero_cc_buf_free(&str_buf);
    zero_cc_buf_free(&info_buf);

    return true;
}
