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
    ABBREV_SUBRANGE_TYPE,
    ABBREV_STRUCTURE_TYPE,
    ABBREV_STRUCTURE_TYPE_ANON,
    ABBREV_UNION_TYPE,
    ABBREV_UNION_TYPE_ANON,
    ABBREV_MEMBER,
    ABBREV_MEMBER_BITFIELD,
    ABBREV_ENUM_TYPE,
    ABBREV_ENUM_TYPE_ANON,
    ABBREV_ENUMERATOR,
    ABBREV_TYPEDEF
};

typedef struct {
    char key[160];
    uint32_t ref;
} DwarfTypeMapEntry;

typedef struct {
    DwarfTypeMapEntry *entries;
    size_t count;
    size_t capacity;
} DwarfTypeMap;

static void dwarf_type_map_init(DwarfTypeMap *map) {
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static uint32_t dwarf_type_map_find(DwarfTypeMap *map, const char *key) {
    if (!map || !key) return 0;
    for (size_t i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            return map->entries[i].ref;
        }
    }
    return 0;
}

static void dwarf_type_map_add(DwarfTypeMap *map, const char *key, uint32_t ref) {
    if (!map || !key) return;
    if (map->count >= map->capacity) {
        size_t new_cap = map->capacity ? map->capacity * 2 : 64;
        DwarfTypeMapEntry *new_entries = (DwarfTypeMapEntry *)realloc(map->entries, new_cap * sizeof(DwarfTypeMapEntry));
        if (!new_entries) return;
        map->entries = new_entries;
        map->capacity = new_cap;
    }
    snprintf(map->entries[map->count].key, sizeof(map->entries[map->count].key), "%s", key);
    map->entries[map->count].ref = ref;
    map->count++;
}

static void dwarf_type_map_free(DwarfTypeMap *map) {
    if (map && map->entries) {
        free(map->entries);
        map->entries = NULL;
    }
    if (map) {
        map->count = 0;
        map->capacity = 0;
    }
}

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
static void dwarf_generate_abbrev(ZeroCCCompiler *c, ZeroCCBuffer *out) {
    bool is_d5 = (c && c->dwarf_version >= 5);
    /* Abbrev 1: DW_TAG_compile_unit (children = yes) */
    dwarf_emit_uleb128(out, ABBREV_COMPILE_UNIT);
    dwarf_emit_uleb128(out, DW_TAG_compile_unit);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_producer);  dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_language);  dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, is_d5 ? DW_FORM_line_strp : DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_comp_dir);  dwarf_emit_uleb128(out, is_d5 ? DW_FORM_line_strp : DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_low_pc);    dwarf_emit_uleb128(out, DW_FORM_addr);
    dwarf_emit_uleb128(out, DW_AT_high_pc);   dwarf_emit_uleb128(out, DW_FORM_data8);
    dwarf_emit_uleb128(out, DW_AT_stmt_list); dwarf_emit_uleb128(out, DW_FORM_sec_offset);
    if (is_d5) {
        dwarf_emit_uleb128(out, DW_AT_ranges); dwarf_emit_uleb128(out, DW_FORM_sec_offset);
    }
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

    /* Abbrev 15: DW_TAG_structure_type (children = yes, named) */
    dwarf_emit_uleb128(out, ABBREV_STRUCTURE_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_structure_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 16: DW_TAG_structure_type_anon (children = yes, anon) */
    dwarf_emit_uleb128(out, ABBREV_STRUCTURE_TYPE_ANON);
    dwarf_emit_uleb128(out, DW_TAG_structure_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 17: DW_TAG_union_type (children = yes, named) */
    dwarf_emit_uleb128(out, ABBREV_UNION_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_union_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 18: DW_TAG_union_type_anon (children = yes, anon) */
    dwarf_emit_uleb128(out, ABBREV_UNION_TYPE_ANON);
    dwarf_emit_uleb128(out, DW_TAG_union_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 19: DW_TAG_member (children = no) */
    dwarf_emit_uleb128(out, ABBREV_MEMBER);
    dwarf_emit_uleb128(out, DW_TAG_member);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);                 dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_type);                 dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_data_member_location); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);                          dwarf_emit_uleb128(out, 0);

    /* Abbrev 20: DW_TAG_member_bitfield (children = no) */
    dwarf_emit_uleb128(out, ABBREV_MEMBER_BITFIELD);
    dwarf_emit_uleb128(out, DW_TAG_member);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);                 dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_type);                 dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, DW_AT_byte_size);            dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_bit_size);             dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_bit_offset);           dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_data_member_location); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, 0);                          dwarf_emit_uleb128(out, 0);

    /* Abbrev 21: DW_TAG_enumeration_type (children = yes, named) */
    dwarf_emit_uleb128(out, ABBREV_ENUM_TYPE);
    dwarf_emit_uleb128(out, DW_TAG_enumeration_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 22: DW_TAG_enumeration_type_anon (children = yes, anon) */
    dwarf_emit_uleb128(out, ABBREV_ENUM_TYPE_ANON);
    dwarf_emit_uleb128(out, DW_TAG_enumeration_type);
    dwarf_emit_u8(out, DW_CHILDREN_yes);
    dwarf_emit_uleb128(out, DW_AT_byte_size); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* Abbrev 23: DW_TAG_enumerator (children = no) */
    dwarf_emit_uleb128(out, ABBREV_ENUMERATOR);
    dwarf_emit_uleb128(out, DW_TAG_enumerator);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);        dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_const_value); dwarf_emit_uleb128(out, DW_FORM_sdata);
    dwarf_emit_uleb128(out, 0);                 dwarf_emit_uleb128(out, 0);

    /* Abbrev 24: DW_TAG_typedef (children = no) */
    dwarf_emit_uleb128(out, ABBREV_TYPEDEF);
    dwarf_emit_uleb128(out, DW_TAG_typedef);
    dwarf_emit_u8(out, DW_CHILDREN_no);
    dwarf_emit_uleb128(out, DW_AT_name);      dwarf_emit_uleb128(out, DW_FORM_strp);
    dwarf_emit_uleb128(out, DW_AT_decl_file); dwarf_emit_uleb128(out, DW_FORM_data1);
    dwarf_emit_uleb128(out, DW_AT_decl_line); dwarf_emit_uleb128(out, DW_FORM_udata);
    dwarf_emit_uleb128(out, DW_AT_type);      dwarf_emit_uleb128(out, DW_FORM_ref4);
    dwarf_emit_uleb128(out, 0);               dwarf_emit_uleb128(out, 0);

    /* End of table */
    dwarf_emit_u8(out, 0);
}

/* Generate .debug_line section content */
static void dwarf_generate_line_table(ZeroCCCompiler *c,
                                      ZeroCCBuffer *out,
                                      ZeroCCBuffer *line_str_buf,
                                      uint16_t debug_line_idx,
                                      uint32_t rel_sec_type,
                                      uint32_t rel_addr_type,
                                      DwarfRelocList *relocs) {
    bool is_d5 = (c && c->dwarf_version >= 5);
    int addr_size = zc_target_ptr_size(c);
    size_t unit_length_pos = out->length;
    dwarf_emit_u32(out, 0);  /* unit_length placeholder */

    if (is_d5) {
        /* DWARF 5 Line Table Header */
        dwarf_emit_u16(out, 5);
        dwarf_emit_u8(out, (uint8_t)addr_size);
        dwarf_emit_u8(out, 0);  /* segment_selector_size */
    } else {
        /* Version 4 */
        dwarf_emit_u16(out, 4);
    }

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
    const char *comp_dir = c->comp_dir ? c->comp_dir : ".";
    const char *slash = strrchr(src_file, '/');
    char dir_buf[1024];
    const char *fname = src_file;
    const char *file_dir = comp_dir;
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
        file_dir = dir_buf;
        fname = slash + 1;
        dir_idx = 1;
    }

    if (is_d5) {
        /* Directory Table in DWARF 5:
         * directory_entry_format_count = 1
         * format: DW_LNCT_path (0x01), DW_FORM_line_strp (0x1f)
         * directories_count = 2:
         *   Entry 0: comp_dir (compilation directory)
         *   Entry 1: file_dir (source file directory) */
        dwarf_emit_u8(out, 1);
        dwarf_emit_uleb128(out, DW_LNCT_path);
        dwarf_emit_uleb128(out, DW_FORM_line_strp);
        dwarf_emit_uleb128(out, 2);
        uint32_t cdir_off = dwarf_str_add(line_str_buf, comp_dir);
        dwarf_relocs_add(relocs, debug_line_idx, out->length, ".debug_line_str", rel_sec_type, cdir_off);
        dwarf_emit_u32(out, cdir_off);
        uint32_t fdir_off = dwarf_str_add(line_str_buf, file_dir);
        dwarf_relocs_add(relocs, debug_line_idx, out->length, ".debug_line_str", rel_sec_type, fdir_off);
        dwarf_emit_u32(out, fdir_off);

        /* File Name Table in DWARF 5:
         * file_name_entry_format_count = 2
         * format: DW_LNCT_path (DW_FORM_line_strp), DW_LNCT_directory_index (DW_FORM_udata)
         * file_names_count = 2:
         *   Entry 0: primary CU source file (dir index 1)
         *   Entry 1: file 1 for DIE decl_file references (dir index 1) */
        dwarf_emit_u8(out, 2);
        dwarf_emit_uleb128(out, DW_LNCT_path);
        dwarf_emit_uleb128(out, DW_FORM_line_strp);
        dwarf_emit_uleb128(out, DW_LNCT_directory_index);
        dwarf_emit_uleb128(out, DW_FORM_udata);
        dwarf_emit_uleb128(out, 2);
        uint32_t file_off = dwarf_str_add(line_str_buf, fname);
        /* Entry 0 */
        dwarf_relocs_add(relocs, debug_line_idx, out->length, ".debug_line_str", rel_sec_type, file_off);
        dwarf_emit_u32(out, file_off);
        dwarf_emit_uleb128(out, 1); /* dir index 1 */
        /* Entry 1 */
        dwarf_relocs_add(relocs, debug_line_idx, out->length, ".debug_line_str", rel_sec_type, file_off);
        dwarf_emit_u32(out, file_off);
        dwarf_emit_uleb128(out, 1); /* dir index 1 */
    } else {
        if (dir_idx > 0) {
            dwarf_emit_string(out, dir_buf);
            dwarf_emit_u8(out, 0);  /* End of directories */
        } else {
            dwarf_emit_u8(out, 0);  /* Empty directory table */
        }

        /* File names table */
        dwarf_emit_string(out, fname);
        dwarf_emit_uleb128(out, dir_idx);  /* Directory index */
        dwarf_emit_uleb128(out, 0);        /* Time */
        dwarf_emit_uleb128(out, 0);        /* Size */
        dwarf_emit_u8(out, 0);             /* End of file names */
    }

    /* Patch header length */
    size_t header_len = out->length - header_start;
    *(uint32_t *)(out->data + header_length_pos) = (uint32_t)header_len;

    /* Line number program */
    uint64_t cur_addr = 0;
    int32_t cur_line = 1;
    uint32_t cur_col = 0;

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
    if (is_d5) {
        dwarf_emit_u8(out, DW_LNS_set_file);
        dwarf_emit_uleb128(out, 1);
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

/* Map ZeroCCType to a base type, pointer, array, struct, union, enum, or typedef DIE */
static uint32_t dwarf_get_type_ref(ZeroCCCompiler *c,
                                   ZeroCCBuffer *info,
                                   ZeroCCBuffer *str_buf,
                                   uint16_t debug_info_idx,
                                   uint32_t rel_sec_type,
                                   uint32_t rel_addr_type,
                                   DwarfRelocList *relocs,
                                   ZeroCCType type,
                                   uint32_t *base_refs,
                                   uint32_t void_ptr_ref,
                                   DwarfTypeMap *type_map) {
    if (type.is_long_double || type.is_float128) {
        return base_refs[13];
    }

    if (type.tag == TYPE_PTR) {
        if (!type.base || type.base->tag == TYPE_VOID) {
            return void_ptr_ref;
        }
        uint32_t pointee_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                                  rel_sec_type, rel_addr_type, relocs,
                                                  *type.base, base_refs, void_ptr_ref, type_map);
        char pkey[32];
        snprintf(pkey, sizeof(pkey), "ptr_%u", pointee_ref);
        uint32_t cached = dwarf_type_map_find(type_map, pkey);
        if (cached != 0) return cached;

        uint32_t ptr_ref = (uint32_t)info->length;
        dwarf_emit_uleb128(info, ABBREV_POINTER_TYPE);
        dwarf_emit_u8(info, (uint8_t)zc_target_ptr_size(c));
        dwarf_emit_u32(info, pointee_ref);
        dwarf_type_map_add(type_map, pkey, ptr_ref);
        return ptr_ref;
    }

    if (type.tag == TYPE_ARRAY && type.base) {
        uint32_t elem_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                               rel_sec_type, rel_addr_type, relocs,
                                               *type.base, base_refs, void_ptr_ref, type_map);
        char akey[64];
        snprintf(akey, sizeof(akey), "arr_%u_%zu", elem_ref, type.array_len);
        uint32_t cached = dwarf_type_map_find(type_map, akey);
        if (cached != 0) return cached;

        uint32_t arr_ref = (uint32_t)info->length;
        dwarf_emit_uleb128(info, ABBREV_ARRAY_TYPE);
        dwarf_emit_u32(info, elem_ref);
        dwarf_emit_uleb128(info, ABBREV_SUBRANGE_TYPE);
        uint64_t ubound = (type.array_len > 0) ? (uint64_t)(type.array_len - 1) : 0;
        dwarf_emit_uleb128(info, ubound);
        dwarf_emit_u8(info, 0);  /* End of array children */
        dwarf_type_map_add(type_map, akey, arr_ref);
        return arr_ref;
    }

    if (type.tag == TYPE_STRUCT || type.tag == TYPE_UNION) {
        bool is_union = (type.tag == TYPE_UNION);
        const char *tag_prefix = is_union ? "union" : "struct";
        char key[160];
        bool is_named = (type.struct_name[0] != '\0' &&
                         strncmp(type.struct_name, "__anon_", 7) != 0);
        if (is_named) {
            snprintf(key, sizeof(key), "%s %s", tag_prefix, type.struct_name);
            uint32_t existing = dwarf_type_map_find(type_map, key);
            if (existing != 0) return existing;
        } else {
            snprintf(key, sizeof(key), "anon_%s_%u", tag_prefix, (uint32_t)info->length);
        }

        ZeroCCType *st_def = is_named ? zero_cc_find_struct(c, type.struct_name) : NULL;
        const ZeroCCType *src = (st_def && st_def->member_count > 0) ? st_def : &type;

        uint32_t struct_ref = (uint32_t)info->length;
        dwarf_type_map_add(type_map, key, struct_ref);

        uint32_t abbrev = is_union ? (is_named ? ABBREV_UNION_TYPE : ABBREV_UNION_TYPE_ANON)
                                   : (is_named ? ABBREV_STRUCTURE_TYPE : ABBREV_STRUCTURE_TYPE_ANON);
        dwarf_emit_uleb128(info, abbrev);
        if (is_named) {
            uint32_t sname_off = dwarf_str_add(str_buf, type.struct_name);
            dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, sname_off);
            dwarf_emit_u32(info, sname_off);
        }
        dwarf_emit_uleb128(info, (uint64_t)src->size);

        if (src->members) {
            for (size_t mi = 0; mi < src->member_count; mi++) {
                ZeroCCStructMember *m = &src->members[mi];
                if (!m->name[0]) continue;

                uint32_t mname_off = dwarf_str_add(str_buf, m->name);
                ZeroCCType mtype;
                memset(&mtype, 0, sizeof(mtype));
                mtype.tag = m->tag;
                mtype.size = m->size;
                mtype.align = m->align;
                mtype.base = m->base;
                mtype.is_long_double = m->is_long_double;
                mtype.is_float128 = m->is_float128;
                if (m->tag == TYPE_ARRAY) {
                    mtype.array_len = (m->base && m->base->size > 0) ? (m->size / m->base->size) : m->size;
                }
                if ((m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) && m->base && m->base->struct_name[0]) {
                    snprintf(mtype.struct_name, sizeof(mtype.struct_name), "%s", m->base->struct_name);
                    mtype.members = m->base->members;
                    mtype.member_count = m->base->member_count;
                }

                uint32_t mtype_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                                        rel_sec_type, rel_addr_type, relocs,
                                                        mtype, base_refs, void_ptr_ref, type_map);

                if (m->is_bitfield) {
                    dwarf_emit_uleb128(info, ABBREV_MEMBER_BITFIELD);
                    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, mname_off);
                    dwarf_emit_u32(info, mname_off);
                    dwarf_emit_u32(info, mtype_ref);
                    dwarf_emit_uleb128(info, (uint64_t)m->size);
                    dwarf_emit_uleb128(info, (uint64_t)m->bit_width);
                    dwarf_emit_u8(info, m->bit_offset);
                    dwarf_emit_uleb128(info, (uint64_t)m->offset);
                } else {
                    dwarf_emit_uleb128(info, ABBREV_MEMBER);
                    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, mname_off);
                    dwarf_emit_u32(info, mname_off);
                    dwarf_emit_u32(info, mtype_ref);
                    dwarf_emit_uleb128(info, (uint64_t)m->offset);
                }
            }
        }

        dwarf_emit_u8(info, 0);  /* End of struct/union children */
        return struct_ref;
    }

    if (type.tag == TYPE_ENUM) {
        char key[160];
        bool is_named = (type.struct_name[0] != '\0' &&
                         strncmp(type.struct_name, "__anon_", 7) != 0);
        if (is_named) {
            snprintf(key, sizeof(key), "enum %s", type.struct_name);
            uint32_t existing = dwarf_type_map_find(type_map, key);
            if (existing != 0) return existing;
        } else {
            snprintf(key, sizeof(key), "anon_enum_%u", (uint32_t)info->length);
        }

        uint32_t enum_ref = (uint32_t)info->length;
        dwarf_type_map_add(type_map, key, enum_ref);

        uint32_t abbrev = is_named ? ABBREV_ENUM_TYPE : ABBREV_ENUM_TYPE_ANON;
        dwarf_emit_uleb128(info, abbrev);
        if (is_named) {
            uint32_t ename_off = dwarf_str_add(str_buf, type.struct_name);
            dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, ename_off);
            dwarf_emit_u32(info, ename_off);
        }
        dwarf_emit_u8(info, 4);             /* byte_size = 4 */
        dwarf_emit_u32(info, base_refs[0]); /* DW_AT_type -> int */

        if (c->enum_constants) {
            for (size_t ec = 0; ec < c->enum_const_capacity; ec++) {
                if (c->enum_constants[ec].name[0]) {
                    dwarf_emit_uleb128(info, ABBREV_ENUMERATOR);
                    uint32_t cname_off = dwarf_str_add(str_buf, c->enum_constants[ec].name);
                    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, cname_off);
                    dwarf_emit_u32(info, cname_off);
                    dwarf_emit_sleb128(info, c->enum_constants[ec].value);
                }
            }
        }

        dwarf_emit_u8(info, 0);  /* End of enum children */
        return enum_ref;
    }

    /* Base types */
    switch (type.tag) {
        case TYPE_INT32:   return base_refs[0];  /* int */
        case TYPE_UINT32:  return base_refs[1];  /* unsigned int */
        case TYPE_INT8:    return base_refs[2];  /* char */
        case TYPE_UINT8:   return base_refs[3];  /* unsigned char */
        case TYPE_INT16:   return base_refs[4];  /* short */
        case TYPE_UINT16:  return base_refs[5];  /* unsigned short */
        case TYPE_INT64:   return base_refs[6];  /* long */
        case TYPE_UINT64:  return base_refs[7];  /* unsigned long */
        case TYPE_FLOAT:   return base_refs[8];  /* float */
        case TYPE_DOUBLE:  return base_refs[9];  /* double */
        case TYPE_BOOL:    return base_refs[10]; /* bool */
        case TYPE_INT128:  return base_refs[11]; /* __int128 */
        case TYPE_UINT128: return base_refs[12]; /* unsigned __int128 */
        default:           return base_refs[0];  /* default to int */
    }
}

/* Frame base register operation by architecture */
static uint8_t dwarf_frame_base_op(const ZeroCCCompiler *c) {
    return (uint8_t)(0x50 + zc_target_dwarf_fp_reg(c));
}

/* Generate .debug_rnglists section content (DWARF 5) */
static void dwarf_generate_rnglists(ZeroCCCompiler *c,
                                    ZeroCCBuffer *out,
                                    uint16_t debug_rnglists_idx,
                                    uint32_t rel_addr_type,
                                    DwarfRelocList *relocs) {
    int addr_size = zc_target_ptr_size(c);
    size_t unit_length_pos = out->length;
    dwarf_emit_u32(out, 0); /* unit_length placeholder */
    dwarf_emit_u16(out, 5); /* version 5 */
    dwarf_emit_u8(out, (uint8_t)addr_size);
    dwarf_emit_u8(out, 0);  /* segment_selector_size */
    dwarf_emit_u32(out, 0); /* offset_entry_count */

    /* Range entry 1: DW_RLE_start_length (.text) */
    dwarf_emit_u8(out, DW_RLE_start_length);
    dwarf_relocs_add(relocs, debug_rnglists_idx, out->length, ".text", rel_addr_type, 0);
    if (addr_size == 4) {
        dwarf_emit_u32(out, 0);
    } else {
        dwarf_emit_u64(out, 0);
    }
    size_t text_sz = c->text_section.length ? c->text_section.length : 0x10;
    dwarf_emit_uleb128(out, (uint64_t)text_sz);

    /* End of list */
    dwarf_emit_u8(out, DW_RLE_end_of_list);

    /* Patch unit length */
    size_t ulen = out->length - unit_length_pos - 4;
    *(uint32_t *)(out->data + unit_length_pos) = (uint32_t)ulen;
}

/* Generate .debug_info section content */
static void dwarf_generate_info(ZeroCCCompiler *c,
                                ZeroCCBuffer *info,
                                ZeroCCBuffer *str_buf,
                                ZeroCCBuffer *line_str_buf,
                                uint16_t debug_info_idx,
                                uint32_t rel_sec_type,
                                uint32_t rel_addr_type,
                                DwarfRelocList *relocs) {
    size_t unit_length_pos = info->length;
    dwarf_emit_u32(info, 0);  /* unit_length placeholder */

    bool is_d5 = (c && c->dwarf_version >= 5);
    int addr_size = zc_target_ptr_size(c);

    if (is_d5) {
        /* DWARF 5 Compilation Unit Header */
        dwarf_emit_u16(info, 5);
        dwarf_emit_u8(info, DW_UT_compile);
        dwarf_emit_u8(info, (uint8_t)addr_size);
        /* debug_abbrev_offset (relocation against .debug_abbrev + 0) */
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_abbrev", rel_sec_type, 0);
        dwarf_emit_u32(info, 0);
    } else {
        /* Version 4 */
        dwarf_emit_u16(info, 4);
        /* debug_abbrev_offset (relocation against .debug_abbrev + 0) */
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_abbrev", rel_sec_type, 0);
        dwarf_emit_u32(info, 0);
        /* Address size */
        dwarf_emit_u8(info, (uint8_t)addr_size);
    }

    /* String offsets */
    uint32_t prod_off = dwarf_str_add(str_buf, "ZeroCC 0.1");
    const char *src_file = c->source_filename ? c->source_filename : "source.c";
    const char *comp_dir = c->comp_dir ? c->comp_dir : ".";

    uint32_t name_off = is_d5 ? dwarf_str_add(line_str_buf, src_file) : dwarf_str_add(str_buf, src_file);
    uint32_t dir_off = is_d5 ? dwarf_str_add(line_str_buf, comp_dir) : dwarf_str_add(str_buf, comp_dir);

    /* Compile Unit DIE (Abbrev 1) */
    dwarf_emit_uleb128(info, ABBREV_COMPILE_UNIT);
    /* DW_AT_producer */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, prod_off);
    dwarf_emit_u32(info, prod_off);
    /* DW_AT_language (C99) */
    dwarf_emit_u8(info, DW_LANG_C99);
    /* DW_AT_name */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, is_d5 ? ".debug_line_str" : ".debug_str", rel_sec_type, name_off);
    dwarf_emit_u32(info, name_off);
    /* DW_AT_comp_dir */
    dwarf_relocs_add(relocs, debug_info_idx, info->length, is_d5 ? ".debug_line_str" : ".debug_str", rel_sec_type, dir_off);
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
    if (is_d5) {
        /* DW_AT_ranges (offset 12 in .debug_rnglists) */
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_rnglists", rel_sec_type, 12);
        dwarf_emit_u32(info, 12);
    }

    /* Base types */
    enum { NUM_BASE = 14 };
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
        { "_Bool", 1, DW_ATE_boolean },
        { "__int128", 16, DW_ATE_signed },
        { "unsigned __int128", 16, DW_ATE_unsigned },
        { "long double", 16, DW_ATE_float }
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
    dwarf_emit_u8(info, (uint8_t)zc_target_ptr_size(c));

    DwarfTypeMap type_map;
    dwarf_type_map_init(&type_map);

    /* Typedefs */
    for (size_t ti = 0; ti < c->typedef_count; ti++) {
        ZeroCCTypedef *td = &c->typedefs[ti];
        if (!td->alias[0]) continue;
        char key[160];
        snprintf(key, sizeof(key), "typedef %s", td->alias);
        if (dwarf_type_map_find(&type_map, key) != 0) continue;

        uint32_t target_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                                 rel_sec_type, rel_addr_type, relocs,
                                                 td->type, base_refs, void_ptr_ref, &type_map);
        uint32_t td_ref = (uint32_t)info->length;
        dwarf_type_map_add(&type_map, key, td_ref);

        dwarf_emit_uleb128(info, ABBREV_TYPEDEF);
        uint32_t aname_off = dwarf_str_add(str_buf, td->alias);
        dwarf_relocs_add(relocs, debug_info_idx, info->length, ".debug_str", rel_sec_type, aname_off);
        dwarf_emit_u32(info, aname_off);
        dwarf_emit_u8(info, 1);      /* decl_file */
        dwarf_emit_uleb128(info, 1); /* decl_line */
        dwarf_emit_u32(info, target_ref);
    }

    /* Named structs & unions */
    for (size_t si = 0; si < c->struct_count; si++) {
        ZeroCCStructDef *sd = &c->structs[si];
        if (!sd->name[0] || strncmp(sd->name, "__anon_", 7) == 0) continue;
        char key[160];
        snprintf(key, sizeof(key), "%s %s", (sd->type.tag == TYPE_UNION) ? "union" : "struct", sd->name);
        if (dwarf_type_map_find(&type_map, key) != 0) continue;
        dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                           rel_sec_type, rel_addr_type, relocs,
                           sd->type, base_refs, void_ptr_ref, &type_map);
    }

    /* Global variables */
    for (size_t gi = 0; gi < c->global_var_count; gi++) {
        ZeroCCGlobalVar *gv = &c->global_vars[gi];
        if (!gv->name[0] || gv->alias_target[0] || gv->is_tls || gv->lto_eliminated) continue;

        uint32_t gv_name_off = dwarf_str_add(str_buf, gv->name);
        uint32_t type_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                               rel_sec_type, rel_addr_type, relocs,
                                               gv->type, base_refs, void_ptr_ref, &type_map);

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

    uint8_t fb_op = dwarf_frame_base_op(c);

    /* Functions */
    for (size_t fi = 0; fi < c->function_count; fi++) {
        ZeroCCFunction *fn = &c->functions[fi];
        if (!fn->name[0] || fn->is_prototype || fn->is_external || fn->is_object || fn->lto_eliminated) {
            continue;
        }

        uint32_t fn_name_off = dwarf_str_add(str_buf, fn->name);
        bool is_void = (fn->return_type.tag == TYPE_VOID);
        uint32_t ret_type_ref = 0;
        if (!is_void) {
            ret_type_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                              rel_sec_type, rel_addr_type, relocs,
                                              fn->return_type, base_refs, void_ptr_ref, &type_map);
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
            uint32_t var_type_ref = dwarf_get_type_ref(c, info, str_buf, debug_info_idx,
                                                       rel_sec_type, rel_addr_type, relocs,
                                                       var->type, base_refs, void_ptr_ref, &type_map);

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

    dwarf_type_map_free(&type_map);

    /* Patch unit length */
    size_t unit_len = info->length - (unit_length_pos + 4);
    *(uint32_t *)(info->data + unit_length_pos) = (uint32_t)unit_len;
}

bool zero_cc_dwarf_emit(ZeroCCCompiler *c, ElfWriterCtx *elf_ctx) {
    if (!c || !elf_ctx || !c->debug_info) return true;

    bool is_d5 = (c && c->dwarf_version >= 5);

    ZeroCCBuffer line_buf;
    zero_cc_buf_init(&line_buf, 1024);

    ZeroCCBuffer abbrev_buf;
    zero_cc_buf_init(&abbrev_buf, 512);

    ZeroCCBuffer str_buf;
    zero_cc_buf_init(&str_buf, 1024);

    ZeroCCBuffer line_str_buf;
    zero_cc_buf_init(&line_str_buf, 512);

    ZeroCCBuffer rnglists_buf;
    zero_cc_buf_init(&rnglists_buf, 256);

    ZeroCCBuffer info_buf;
    zero_cc_buf_init(&info_buf, 2048);

    DwarfRelocList relocs;
    dwarf_relocs_init(&relocs);

    /* Determine relocation types based on target ISA traits */
    const TargetTraits *traits = zc_target_traits(c);
    uint32_t rel_sec_type = traits->dwarf_sec_reloc;   /* 32-bit reloc for DWARF section references */
    uint32_t rel_addr_type = traits->dwarf_addr_reloc; /* Address reloc for .text/.data references */

    /* 1. Generate .debug_abbrev */
    dwarf_generate_abbrev(c, &abbrev_buf);

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

    uint16_t debug_line_str_idx = 0;
    uint16_t debug_rnglists_idx = 0;
    if (is_d5) {
        elf_writer_add_section(elf_ctx, ".debug_line_str",
                               (const uint8_t *)"", 1,
                               ELF_SHT_PROGBITS, 0, 1);
        debug_line_str_idx = elf_writer_section_index(elf_ctx, ".debug_line_str");

        elf_writer_add_section(elf_ctx, ".debug_rnglists",
                               (const uint8_t *)"", 1,
                               ELF_SHT_PROGBITS, 0, 1);
        debug_rnglists_idx = elf_writer_section_index(elf_ctx, ".debug_rnglists");
    }

    elf_writer_add_section(elf_ctx, ".debug_info",
                           (const uint8_t *)"", 1,
                           ELF_SHT_PROGBITS, 0, 1);
    uint16_t debug_info_idx = elf_writer_section_index(elf_ctx, ".debug_info");

    /* 3. Generate .debug_line */
    dwarf_generate_line_table(c, &line_buf, is_d5 ? &line_str_buf : &str_buf,
                              debug_line_idx, rel_sec_type, rel_addr_type, &relocs);

    /* 4. Generate .debug_rnglists and .debug_info */
    if (is_d5) {
        dwarf_generate_rnglists(c, &rnglists_buf, debug_rnglists_idx, rel_addr_type, &relocs);
    }
    dwarf_generate_info(c, &info_buf, &str_buf, &line_str_buf,
                        debug_info_idx, rel_sec_type, rel_addr_type, &relocs);

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
    if (is_d5 && debug_line_str_idx > 0 && debug_line_str_idx <= (uint16_t)elf_ctx->section_count) {
        ElfWriterSection *sec = &elf_ctx->sections[debug_line_str_idx - 1];
        if (sec->capacity < line_str_buf.length) {
            uint8_t *nd = (uint8_t *)realloc(sec->data, line_str_buf.length);
            if (nd) { sec->data = nd; sec->capacity = line_str_buf.length; }
        }
        if (sec->data) {
            memcpy(sec->data, line_str_buf.data, line_str_buf.length);
            sec->size = line_str_buf.length;
        }
    }
    if (is_d5 && debug_rnglists_idx > 0 && debug_rnglists_idx <= (uint16_t)elf_ctx->section_count) {
        ElfWriterSection *sec = &elf_ctx->sections[debug_rnglists_idx - 1];
        if (sec->capacity < rnglists_buf.length) {
            uint8_t *nd = (uint8_t *)realloc(sec->data, rnglists_buf.length);
            if (nd) { sec->data = nd; sec->capacity = rnglists_buf.length; }
        }
        if (sec->data) {
            memcpy(sec->data, rnglists_buf.data, rnglists_buf.length);
            sec->size = rnglists_buf.length;
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
    if (is_d5) {
        elf_writer_add_symbol(elf_ctx, ".debug_line_str", ELF_STB_LOCAL, ELF_STT_SECTION, debug_line_str_idx, 0, 0);
        elf_writer_add_symbol(elf_ctx, ".debug_rnglists", ELF_STB_LOCAL, ELF_STT_SECTION, debug_rnglists_idx, 0, 0);
    }

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
    zero_cc_buf_free(&line_str_buf);
    zero_cc_buf_free(&rnglists_buf);
    zero_cc_buf_free(&info_buf);

    return true;
}
