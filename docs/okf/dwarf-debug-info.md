---
type: Architecture
title: DWARF 4 Debug Information Architecture (-g)
description: DWARF 4 debug information generation in ZeroCC, mapping machine code to source line/column and emitting function, variable, and type DIEs for GDB and LLDB.
resource: architecture/dwarf
tags: [dwarf, debug, elf, gdb, lldb]
timestamp: 2026-11-13T22:46:58Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# DWARF 4 Debug Information Architecture (`-g`)

ZeroCC implements standard **DWARF 4** source-level debugging enabled with `-g` (and disabled with `-g0`). When active, the compiler emits ELF debug sections (`.debug_line`, `.debug_info`, `.debug_abbrev`, and `.debug_str`) and populates corresponding relocation entries in the object file, enabling full source-level stepping, breakpoints, variable inspection, and backtraces in `gdb` and `lldb`.

## 1. Line Number Program (`.debug_line`)

The line table tracks machine code offsets throughout AST lowering and machine code emission:
- **Source tracking**: The AST lowering pass (`ast_lowering_stmt.c`, `ast_lowering_expr.c`) propagates line and column numbers from AST nodes onto each generated `Instr`.
- **Address-line mapping**: Machine code emission (`ast_lowering_mc.c`) calls `zero_cc_dwarf_add_line()` whenever machine code offset advances to record `(offset, line, col, is_stmt)`.
- **Directory and file tables**: The compiler splits the primary source filename into directory and base filename entries in the DWARF line header, respecting relative and absolute compilation paths.
- **Prologue end marker**: `DW_LNS_set_prologue_end` is emitted at the first instruction after the function prologue, ensuring breakpoints placed on function names land directly on the first executable statement after argument stores.
- **Line program opcodes**: Generates standard DWARF 4 opcodes (`DW_LNE_set_address`, `DW_LNS_advance_pc`, `DW_LNS_advance_line`, `DW_LNS_set_column`, `DW_LNS_copy`, special opcodes, and `DW_LNE_end_sequence`).

## 2. Debug Information Tree (`.debug_info` & `.debug_abbrev`)

The `.debug_info` section encodes the compilation unit DIE and its children using abbreviated encodings defined in `.debug_abbrev`:
- **Compilation Unit (`DW_TAG_compile_unit`)**: Attributes `DW_AT_producer` ("ZeroCC 0.1"), `DW_AT_language` (ANSI C99), `DW_AT_name` (source file), `DW_AT_comp_dir` (working directory), `DW_AT_low_pc` / `DW_AT_high_pc` (`.text` range), and `DW_AT_stmt_list` (offset 0 in `.debug_line`).
- **Base Types (`DW_TAG_base_type`)**: Standard primitive integer and floating-point types (`int`, `unsigned int`, `char`, `unsigned char`, `short`, `unsigned short`, `long int`, `long unsigned int`, `float`, `double`, `_Bool`) with appropriate byte size and encoding attributes.
- **Pointers & Arrays (`DW_TAG_pointer_type`, `DW_TAG_array_type`, `DW_TAG_subrange_type`)**: Pointer types referring to base types and array types with upper bound subranges.
- **Global & Static Variables (`DW_TAG_variable`)**: Emits `DW_AT_name`, `DW_AT_decl_file`, `DW_AT_decl_line`, `DW_AT_type`, and `DW_AT_location` (`DW_OP_addr <data_offset>`) with `.data` relocations.
- **Subprograms (`DW_TAG_subprogram`)**: Emitted for each non-prototype function definition:
  - `DW_AT_name`, `DW_AT_decl_file`, `DW_AT_decl_line`, `DW_AT_type`, `DW_AT_low_pc`, `DW_AT_high_pc`.
  - `DW_AT_frame_base`: Architecture-specific frame pointer register (`DW_OP_reg6` on x86-64, `DW_OP_reg29` on AArch64, `DW_OP_reg8` on RISC-V).
  - Formal Parameters (`DW_TAG_formal_parameter`): `DW_AT_location` via `DW_OP_fbreg: -offset`.
  - Local Variables (`DW_TAG_variable`): `DW_AT_location` via `DW_OP_fbreg: -offset`.

## 3. Frame-Pointer Preservation Invariant

To ensure reliable local variable and parameter evaluation across all optimization levels:
- When `-g` is enabled and `-fomit-frame-pointer` has not been explicitly requested (`c->omit_frame_pointer < 0`), frame pointer omission (`omit_frame_pointer`) is disabled.
- Functions retain their standard prologue (`push %rbp; mov %rsp, %rbp` on x86-64), ensuring `DW_OP_fbreg: -offset` resolves correctly during GDB inspection.
- Leaf functions with no stack allocations maintain their frame pointer under `-g` instead of omitting it.

## 4. Multi-ISA and ELF Writer Integration

- Section headers `.debug_line`, `.debug_info`, `.debug_abbrev`, and `.debug_str` are generated in `src/zero_cc/dwarf/dwarf.c` and integrated with `ElfWriterCtx` in `zero_cc_dwarf_emit()`.
- Relocations against `.text`, `.data`, `.debug_str`, and `.debug_abbrev` are emitted using the active target ISA's relocation codes (`R_X86_64_32/64`, `R_AARCH64_ABS32/64`, `R_RISCV_32/64`).
- Relocation section names follow the canonical standard `.rela.<sec>` (GNU ld and readelf compatible).

## 5. Verification

- **Readelf validation**: `readelf -wl` confirms line program decoding; `readelf -wi` verifies compilation unit and variable DIE hierarchy.
- **GDB regression suite**: Covered by automated driver tests in `tools/verify/driver_tests.py` testing breakpoints by function name, line stepping (`next`/`step`), variable inspection (`print`), backtraces (`bt`), and `-g0` omission.
