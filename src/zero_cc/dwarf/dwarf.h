#ifndef ZEROCC_DWARF_H
#define ZEROCC_DWARF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "zero_cc/types/types.h"
#include "zero_cc/jit/elf_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DWARF 4 Tag constants */
#define DW_TAG_array_type            0x01
#define DW_TAG_formal_parameter     0x05
#define DW_TAG_member               0x0d
#define DW_TAG_pointer_type         0x0f
#define DW_TAG_compile_unit         0x11
#define DW_TAG_structure_type       0x13
#define DW_TAG_subprogram           0x2e
#define DW_TAG_variable             0x34
#define DW_TAG_base_type            0x24
#define DW_TAG_subrange_type        0x21
#define DW_TAG_union_type           0x17

/* DWARF 4 Children determination */
#define DW_CHILDREN_no              0x00
#define DW_CHILDREN_yes             0x01

/* DWARF 4 Attribute constants */
#define DW_AT_sibling               0x01
#define DW_AT_location              0x02
#define DW_AT_name                  0x03
#define DW_AT_byte_size             0x0b
#define DW_AT_stmt_list             0x10
#define DW_AT_low_pc                0x11
#define DW_AT_high_pc               0x12
#define DW_AT_language              0x13
#define DW_AT_comp_dir              0x1b
#define DW_AT_producer              0x25
#define DW_AT_prototyped            0x27
#define DW_AT_upper_bound           0x2f
#define DW_AT_data_member_location  0x38
#define DW_AT_decl_column           0x39
#define DW_AT_decl_file             0x3a
#define DW_AT_decl_line             0x3b
#define DW_AT_encoding              0x3e
#define DW_AT_external              0x3f
#define DW_AT_frame_base            0x40
#define DW_AT_type                  0x49

/* DWARF 4 Form constants */
#define DW_FORM_addr                0x01
#define DW_FORM_data2               0x05
#define DW_FORM_data4               0x06
#define DW_FORM_data8               0x07
#define DW_FORM_string              0x08
#define DW_FORM_data1               0x0b
#define DW_FORM_flag                0x0c
#define DW_FORM_sdata               0x0d
#define DW_FORM_strp                0x0e
#define DW_FORM_udata               0x0f
#define DW_FORM_ref4                0x13
#define DW_FORM_sec_offset          0x17
#define DW_FORM_exprloc             0x18
#define DW_FORM_flag_present        0x19

/* DWARF 4 Standard Opcode constants */
#define DW_LNS_copy                 1
#define DW_LNS_advance_pc           2
#define DW_LNS_advance_line         3
#define DW_LNS_set_file             4
#define DW_LNS_set_column           5
#define DW_LNS_negate_stmt          6
#define DW_LNS_set_basic_block      7
#define DW_LNS_const_add_pc         8
#define DW_LNS_fixed_advance_pc     9
#define DW_LNS_set_prologue_end     10
#define DW_LNS_set_epilogue_begin   11
#define DW_LNS_set_isa              12

/* DWARF 4 Extended Opcode constants */
#define DW_LNE_end_sequence         1
#define DW_LNE_set_address          2
#define DW_LNE_define_file          3
#define DW_LNE_set_discriminator    4

/* DWARF 4 Location Operations */
#define DW_OP_addr                  0x03
#define DW_OP_fbreg                 0x91
#define DW_OP_reg0                  0x50
#define DW_OP_reg6                  0x56
#define DW_OP_reg8                  0x58
#define DW_OP_reg29                 0x6d

/* DWARF 4 Base Type Encodings */
#define DW_ATE_address              0x01
#define DW_ATE_boolean              0x02
#define DW_ATE_complex_float        0x03
#define DW_ATE_float                0x04
#define DW_ATE_signed               0x05
#define DW_ATE_signed_char          0x06
#define DW_ATE_unsigned             0x07
#define DW_ATE_unsigned_char        0x08

/* DWARF 4 Languages */
#define DW_LANG_C89                 0x0001
#define DW_LANG_C                   0x0002
#define DW_LANG_C99                 0x000c
#define DW_LANG_C11                 0x001d

/* Line number state tracking */
void zero_cc_dwarf_add_line(ZeroCCCompiler *c, size_t text_offset,
                            uint32_t line, uint16_t col, bool is_stmt);
void zero_cc_dwarf_mark_prologue_end(ZeroCCCompiler *c);

/* Emit DWARF 4 sections (.debug_line, .debug_info, .debug_abbrev, .debug_str)
 * and relocations into the ELF writer context */
bool zero_cc_dwarf_emit(ZeroCCCompiler *c, ElfWriterCtx *elf_ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZEROCC_DWARF_H */
