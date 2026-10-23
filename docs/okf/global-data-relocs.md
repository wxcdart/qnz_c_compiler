---
type: Concept
title: Global Variables, Data Section & Pointer Relocations
description: File-scope variable initializers (scalars, IEEE-754 floats, aggregate/designated init, char arrays from string literals), block-scope static locals (unsized arrays, pointer-to-string initializers, per-call stack mirror), the ZeroCCDataReloc mechanism, JIT data-page patching, and AOT .rela.<sec> emission.
resource: backend/global-data-relocs
tags: [globals, data-section, relocations, jit, aot, elf, initialization]
timestamp: 2026-10-23T04:25:11Z
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
- **String into char array**: `char g[4] = "abc";` — truncated to fit, NUL-terminated when room; `char g[] = "hi";` infers `array_len = strlen+1`; overlong strings truncate (no NUL). `unsigned char gu[] = "glob";` is the same (`is_char_array` covers `TYPE_INT8` **and** `TYPE_UINT8` — a pre-fix silent miscompile stored a rodata pointer instead of bytes).
- **Struct/union aggregates**: `struct S gs = {5, 1.5};` (positional) and `struct S gs = {.a = 7, .c = 'z'};` (designated, any order). Values are written into a zero-cleared `calloc(size, 1)` scratch buffer at true member offsets (padding-correct) and appended in one go. Member floatness is detected from the struct's `members[]` tags (`TYPE_FLOAT`/`TYPE_DOUBLE` — see [frontend-and-types.md](./frontend-and-types.md)). A **char-array member** initialized by a string literal (positional or designated, `struct S g = {7, "glob"};`) copies the string **bytes** into the member (bounded by the member's own size, never the whole object) instead of the pointer form — pre-fix this stored an 8-byte rodata address into the member (silent miscompile: `name[0]` held the low byte of the pointer). 2-D char arrays fill one row per string (`char m[2][4] = {"ab", "cd"};`). Local-scope forms (and the leaf-alignment rules) are documented in [frontend-and-types.md](./frontend-and-types.md) — *String-Literal Initialization of Char Arrays*.
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
- Constant expressions at file scope (`zero_cc_eval_const_expr`, `src/zero_cc/parser/const_eval.h`) support `TK_CHAR_LIT` (value in `num_val`) in addition to numerics, `true`/`false`, `NULL` and parentheses; a no-progress guard in the aggregate-init loop prevents hangs on unrecognized tokens. `zero_cc_eval_const_expr` also now parses **casts** `(type) expr` (e.g. `(void*)0`, `((int)5)`) so initializer constant-expressions like `static void *p = (void*)0;` evaluate cleanly instead of erroring.

## Block-Scope `static` Locals (Sprint 37)

Block-scope `static` variables (including **unsized arrays** inferred from their initializer, per C99 6.7.9) share the same data-section + relocation machinery as file-scope globals, plus a per-call stack mirror so all accesses stay frame-relative (fast, no global-symbol indirection).

- **Declaration** (`src/zero_cc/parser/ast_stmt.h`): a static local is registered via `zero_cc_static_local_register` (a global entry with `static_local=1` plus a reserved `.data` slot) and a **stack mirror slot** is reserved with `zero_cc_add_local`. The initializer is baked into `.data` immediately at parse time by `zero_cc_static_local_patch_init` (the AST is never retained for static locals).
- **Sizing the unsized array up front**: an unsized `T arr[] = {...}` must be sized **before** `zero_cc_add_local`/`zero_cc_static_local_register`, or it is reserved at the 8-byte minimum and the real (larger) object overflows into the neighbour's slot. `zero_cc_measure_array_init_at` walks the initializer tokens **without consuming them** and returns the object byte size + element count; the declaration code applies it to `decl_type` before deriving `decl_ti`/`sym`. This is what makes multiple static-local arrays in one function (e.g. zerocc's own `gpr_names[]`/`xmm_names[]` in `live_range.h`, `sys_paths[]` in `preprocessor.h`) coexist without corrupting each other.
- **Pointer-to-string elements**: `static const char *names[] = {"a","b",NULL};` writes each string to `.rodata` and records a `ZeroCCDataReloc` (kind 0) per pointer slot, so both the JIT data-page patcher and the AOT `.rela..data` emitter resolve the pointers. `NULL` (and other constant expressions) evaluate via `zero_cc_eval_const_expr`. Char-array string initializers (`static char s[] = "hi"`) and braced char/numeric lists are written as raw bytes.
- **The per-call mirror** (`src/zero_cc/ir/ast_lowering.c`, `static_local_mirror_in`/`_out`): at function entry the object is copied `.data` → frame, at every return the frame is copied back. Both directions copy **full 8-byte chunks** from data offset `O+off` to frame `fp + off - stack_offset` (i.e. element `i` lands at the same `fp - stack_offset + i*8` the body reads/writes). A prior bug stored chunk `off` at `fp - (stack_offset + off)` (wrong direction), scrambling multi-element arrays. The 8-byte-chunk copy is safe because the data slot and the frame slot are both allocated rounded-up-to-8.
- **Persistence**: the mirror-out runs on every return path (the epilogue helper `epilogue_for_return` is used by all return exits), so a `static` counter/array survives early `return` mid-function.
- **Variadic `va_arg`** (`src/zero_cc/core/preprocessor.h`): the built-in `stdarg.h` fallback previously redefined `va_start`/`va_arg`/`va_end` as **no-ops**, silently breaking variadic functions (`test/abi/variadic_fn`). It now maps them to `__builtin_va_start`/`__builtin_va_arg`/`__builtin_va_end`/`__builtin_va_copy` (all four are registered as object-like macros in the built-in macro table alongside `va_*`).

## Related Concepts

- [Multi-ISA Backends & Linker/JIT](./isa-backends.md)
- [Frontend, Type System & Semantic Analysis](./frontend-and-types.md)
- [Pipeline & Two-Pass Architecture](./architecture-pipeline.md)
- [Build & Verification](./build-and-verification.md)
