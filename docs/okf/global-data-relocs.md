---
type: Concept
title: Global Variables, Data Section & Pointer Relocations
description: File-scope variable initializers (scalars, IEEE-754 floats, aggregate/designated init, char arrays from string literals), the ZeroCCDataReloc mechanism, JIT data-page patching, and AOT .rela.<sec> emission.
resource: backend/global-data-relocs
tags: [globals, data-section, relocations, jit, aot, elf, initialization]
timestamp: 2026-10-17T20:23:56Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Global Variables, Data Section & Pointer Relocations

ZeroCC supports file-scope variable declarations in both JIT (`--run`) and AOT (`-o`, `-c`) modes. The global variable mini-parser lives in `src/zero_cc/jit/compile_unit.h` (`zero_cc_compile_c99` top-level loop) and is entered for any declaration that is not a function definition or a struct/enum/typedef definition.

## Supported Initializers (file scope)

- **Scalar integers**: `int g = 42;` — little-endian bytes via `zero_cc_emit_scalar_bytes`.
- **Float/double scalars**: `float gf = 1.5f;` / `double gd = 3.14;` — emitted as **IEEE-754 bit patterns** (`zero_cc_eval_const_expr_float` handles literals, char literals, unary +/- and parentheses).
- **Multi-declarators**: `int a, b = 5, c = 10;` — each declarator is registered separately (`add_global_var` in `src/zero_cc/types/global_vars.h`).
- **Arrays**: `int ga[4] = {1,2,3,4};`, zero-init gaps `{1,0,0,4}`, partial init, and `double garr[3] = {...}` (float element bits). The registered `ZeroCCType` uses `zero_cc_type_array` so `gv->type.tag == TYPE_ARRAY` (array-to-pointer decay on use).
- **String into char array**: `char g[4] = "abc";` — truncated to fit, NUL-terminated when room; `char g[] = "hi";` infers `array_len = strlen+1`; overlong strings truncate (no NUL).
- **Struct/union aggregates**: `struct S gs = {5, 1.5};` (positional) and `struct S gs = {.a = 7, .c = 'z'};` (designated, any order). Values are written into a zero-cleared `calloc(size, 1)` scratch buffer at true member offsets (padding-correct) and appended in one go. Member floatness is detected from the struct's `members[]` tags (`TYPE_FLOAT`/`TYPE_DOUBLE` — see [frontend-and-types.md](./frontend-and-types.md)).
- **Pointer to string literal**: `const char *g = "abcd";` — string is appended to `.rodata`; 8 zero bytes go into `.data` plus a `ZeroCCDataReloc` (kind 0).
- **Address of another global**: `int *p = &x;` — 8 zero bytes plus a `ZeroCCDataReloc` (kind 1). The referenced global must be declared earlier (C rule).
- **`const`/`volatile`/`extern`/`static`** qualifiers are accepted; `static` maps to `ELF_STB_LOCAL`.

## ZeroCCDataReloc (`src/zero_cc/types/type_system.h`)

```c
typedef struct {
    size_t data_patch_offset;   /* byte offset within .data of the 8-byte pointer */
    int    target_kind;         /* 0 = .rodata + rodata_offset, 1 = .data + target_data_offset */
    size_t rodata_offset;
    size_t target_data_offset;
    int    target_global_idx;   /* index into global_vars[] when kind == 1 */
} ZeroCCDataReloc;
```

Stored in `c->data_relocs[ZERO_CC_MAX_DATA_RELOCS]` / `c->data_reloc_count` (`src/zero_cc/types/symbols.h`).

### JIT Patching (`src/zero_cc/jit/execution.h`)

JIT memory layout is `[text][rodata]` on the code page (made RX after copy) and `.data` on a separate RW page at `data_offset = code_page_sz`. After the data `memcpy`, each `ZeroCCDataReloc` is patched with the **absolute 8-byte address** `(char*)mem + data_offset + target_data_offset` (kind 1) or `(char*)mem + text_section.length + rodata_offset` (kind 0). The data page remains writable, so patching happens in place before the RX transition.

### AOT ELF Object (`src/main.c` `compile_object`, `src/zero_cc/jit/elf_writer.{h,c}`)

- **Kind 0** (rodata): `R_X86_64_64` at `.data + data_patch_offset` against the `.rodata` section symbol with addend `rodata_offset`.
- **Kind 1** (`&global`): `R_X86_64_64` against the target global's `STT_OBJECT` symbol (value = its `data_offset`), addend 0.
- The ELF writer supports **multiple relocation sections**: `elf_writer_add_relocation(ctx, section_idx, ...)` records the target section per entry; at write time entries are grouped into one `.rela.<sec>` section per distinct target (`.rela..text`, `.rela..data`, ...), each with `sh_info` = its target section index. The section-index convention is ELF 1-based over `ctx->sections[]` (index 0 is the null section).
- The `-o` executable path links the object with host `gcc`, so the linker resolves these relocs; the `-S` assembly path does **not** emit `.data` (known gap — `-S` + gcc cannot link global data yet).

## Companion Conventions

- `OP_MOV_GLOBAL` (address of a global in code) emits `mov rdst, imm64(0)` + an 8-byte text reloc; the JIT patcher resolves it via `find_global_var` to the absolute data address (see [isa-backends.md](./isa-backends.md)).
- Reading a global float/double uses `ir_emit_vloadss`/`ir_emit_vloadsd` (not a 64-bit int deref); struct member loads detect floatness from semantic `TypeInfo` or, as fallback, the base struct's `members[]` tags **matched by offset** (`aux.alt`), never by name — the `s.str` union slot of `NODE_MEMBER` is overwritten by semantic analysis (see [frontend-and-types.md](./frontend-and-types.md)).
- Constant expressions at file scope (`zero_cc_eval_const_expr`, `src/zero_cc/parser/const_eval.h`) support `TK_CHAR_LIT` (value in `num_val`) in addition to numerics, `true`/`false`, `NULL` and parentheses; a no-progress guard in the aggregate-init loop prevents hangs on unrecognized tokens.

## Related Concepts

- [Multi-ISA Backends & Linker/JIT](./isa-backends.md)
- [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
- [Pipeline & Two-Pass Architecture](./architecture-pipeline.md)
- [Build & Verification](./build-and-verification.md)
