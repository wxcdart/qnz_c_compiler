---
type: Concept
title: Static TLS (_Thread_local) Support
description: _Thread_local / __thread static variables: the ZeroCCTLSBackend (declare/finalize), OP_TLS_LEA lowering, the initial-exec x86-64 idiom (mov %fs:0 + lea $var@tpoff with R_X86_64_TPOFF32), .tdata/.tbss ELF sections and symbols, and the JIT data-section fallback.
resource: backend/tls-static
tags: [tls, thread-local, tdata, tbss, tpoff, x86-64, elf, jit, aot, abi]
timestamp: 2026-10-24T21:17:08Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Static TLS (`_Thread_local`) Support

ZeroCC implements C11 `_Thread_local` (and the GNU `__thread` spelling) for
file-scope statics in both JIT (`--run`) and AOT (`-o` / `-c` / `-S`) modes,
using the **initial-exec** (static) TLS model on x86-64. Only integral and
pointer scalars up to 8 bytes are supported (arrays and larger aggregates are
rejected with a diagnostic).

## Frontend

- The tokenizer maps `_Thread_local`, `__thread` and `thread_local` to
  `TK_THREAD_LOCAL`.
- File-scope handling: `zero_cc_decl_thread_local()` in
  `src/zero_cc/jit/compile_unit.h` (reached from the top-level loop for both
  `is_thread_local` in the generic global branch and the direct
  `TK_THREAD_LOCAL` dispatch). It parses
  `_Thread_local [static] [extern] type [const/volatile] *... name [= const];`,
  evaluates constant initializers (float literals via
  `zero_cc_eval_const_expr_float`, everything else via
  `zero_cc_eval_const_expr`), and then:
  1. calls `c->tls_backend->declare(c, name, &type, init_val, has_init)`,
  2. registers the name as an ordinary `ZeroCCGlobalVar` (with
     `is_tls = true`) so the existing global machinery (patchers, symbol
     tables, `-S` emission, ELF writer) sees it,
  3. adds a semantic-scope symbol so expressions can use the name.

## TLS backend (`src/zero_cc/jit/tls_backend.c`)

`ZeroCCTLSBackend` (in `src/zero_cc/jit/backends.h`) has two callbacks:

- `declare(c, name, type, init_value, has_init) -> int`
  Records the variable in `c->tls_vars[]` (`ZeroCCTLSVar`: name/size/align/
  has_init/init) and in the legacy `c->thread_locals[]` table. Returns the
  in-block offset (JIT: the data-section offset it just appended; AOT: 0 as a
  placeholder until finalize).
- `finalize(c)` (called once at the end of the top-level loop in
  `zero_cc_compile_c99`) packs the block:
  - **AOT** (`!c->jit_mode`): `.tdata` = initialized variables in declaration
    order, each aligned to its type alignment; `.tbss` = uninitialized
    variables, aligned relative to the end of `.tdata`. Patches each
    `ZeroCCGlobalVar.data_offset` to its in-block offset, sets
    `c->tls_bss_size` / `c->tls_block_align`, and builds the packed `.tdata`
    image in `c->tls_data_section`.
  - **JIT** (`c->jit_mode`, set by the driver before compilation): there is no
    thread pointer to speak of, so each variable is appended to the ordinary
    data section (aligned, initial value or zeros written as raw bytes) and
    behaves like a plain global. Codegen therefore uses the normal
    `OP_MOV_GLOBAL` address load — no TLS op is emitted.

The backend rejects non-x86-64 targets at declaration time, so the TLS IR op
below is x86-64-only in practice.

## IR op: `OP_TLS_LEA`

- `ir_emit_tls_lea(dest, name)` (in `src/zero_cc/ir/ir.h`) emits
  `dest = address of TLS variable name`. `imm64` carries the name string.
- Expression lowering (`src/zero_cc/ir/ast_lowering_expr.c`) routes every
  global address through `ir_emit_global_addr(c, temp, gv)`, which picks
  `OP_TLS_LEA` when `gv->is_tls && !c->jit_mode` and `OP_MOV_GLOBAL`
  otherwise. Loads/stores of the value are the ordinary deref/width-aware
  load+store ops on the address, so narrow types (char/short) and floats work
  without TLS-specific codegen.

## x86-64 initial-exec emission (AOT)

`&tlsvar` is the canonical two-instruction GCC idiom, both halves in the
destination register so no scratch register is needed:

```asm
movq  %fs:0, dst          # thread pointer
leaq  $var@tpoff(dst), dst   # R_X86_64_TPOFF32 over the 32-bit disp
```

- Machine-code path: `case OP_TLS_LEA` in `src/zero_cc/ir/ast_lowering_mc.c`
  emits `64 48 8B 04 25 00 00 00 00` then `48|R 8D (0x80 | (d<<3) | d)`
  (+ SIB `0x24` when `d == 4`, i.e. `%rsp`) `disp32`, and records a
  `ZeroCCReloc` with `is_tls = true` over the `disp32`.
- `-S` path: `case OP_TLS_LEA` in `src/zero_cc/ir/asm_emitter.c` emits the
  same two lines (`movq %fs:0, %reg` / `leaq name@tpoff(%reg), %reg`).
- A single-instruction `lea $off(%fs), %reg` is **not** a valid encoding (GAS
  rejects it), and `addq $imm32, %reg` is RAX-only, so the mov+lea pair is
  the only scratch-free idiom.

## ELF / linker artifacts (AOT)

`main.c` (ELF writer) emits:

- `.tdata` — `SHT_PROGBITS`, `SHF_ALLOC|SHF_WRITE|SHF_TLS (0x400)`, alignment
  `c->tls_block_align`, contents `c->tls_data_section`.
- `.tbss` — `SHT_NOBITS`, same flags, size `c->tls_bss_size`.
- One object symbol per TLS variable: `.tdata` when `data_offset <
  tdata_len`, `.tbss` otherwise.
- `R_X86_64_TPOFF32` (0x17) relocations for `is_tls` relocations.

`SHF_TLS` is required on both sections or `ld` will not form a `PT_TLS`
segment. The block alignment is capped at 8 (the largest variable
alignment); keeping it small avoids inter-section padding confusion in the
linker's `S - G` arithmetic for TPOFF.

The `-S` path emits the sections as `.section .tdata,"awT",@progbits` (bytes
from the packed image, one labeled symbol per initialized variable) and
`.section .tbss,"aw",@nobits` (`.space` per variable), and TLS globals are
skipped in the ordinary `.data` emission.

## Verification

`test/jit/tls.c` exercises the matrix (tdata/tbss ordering, short/double/
long/int*, address-of + store-through-pointer) and is swept by
`make test-opt` (JIT + AOT at -O0..-O3). AOT verification links the zerocc
object with `gcc -no-pie` (dynamic and `-static`) or runs the full
`zerocc -o` pipeline; all four paths must print identical output.
