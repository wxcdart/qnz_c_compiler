---
type: Concept
title: ZeroCC Own Linker (T2) - ELF Reader, ar Reader, Static ET_EXEC Output
description: ZeroCC's own static linker - the ELF64-LE object reader, the classic ar archive reader with greedy member extraction, symbol resolution, section layout at the conventional x86-64 base, the supported relocation set, and the generated _start stub. Gate: make test-linker (14/14).
resource: linker
tags: [linker, elf, ar, aot, x86-64, relocations, zero-rt]
timestamp: 2026-10-23T01:03:56Z
generated: pi-agent
verified: engineering-lead
status: active
---

# ZeroCC Own Linker (T2)

`zerocc a.c b.c --zerocc-link -o app` links the per-TU objects with
ZeroCC's own linker instead of the system linker. The result is a
directly-executable **static ET_EXEC** (no libc, no crt files): two
`PT_LOAD` segments (RX at `0x401000`, RW at the next page), a rebuilt
`.symtab`/`.strtab`/`.shstrtab`, and a linker-generated `_start` stub.

This is the next step on the zero_rt critical path (T1 driver → **T2 own
linker** → T3 runtime): zero_rt Phase 0 is its first customer.

## Components (`src/zero_cc/linker/`)

| File | Role |
|---|---|
| `elf_reader.{h,c}` | Minimal ELF64-LE **ET_REL** parser: section headers (name-resolved), `.symtab` + strtab, `SHT_RELA` sections bound to their target via `sh_info`. All copies are bounds-checked; little-endian loads are explicit. |
| `ar_reader.{h,c}` | Classic `ar` archive reader (GNU/BSD/LLVM ar): 60-byte member headers, long-name (`#1/NNN`) members, index-member skipping (`//`, `__.SYMDEF`, `Sym64 Table`). Members are returned as views into the archive image. |
| `linker.{h,c}` | `zero_cc_link_exec(out, objects, archives, err)`: the pipeline below. |

Driver integration (`src/main.c`): `--zerocc-link` (with `-o`) selects the
own linker; x86-64 only for now (rejected with a clear error otherwise).
Inputs may be `.c` (compiled to a temp `.o`), prebuilt `.o` (passed
through), or `.a` (archive extraction).

## Pipeline

1. **Load** all `.o` inputs.
2. **Resolve symbols** (first pass): builds the global name table including
   UND entries; strong/weak rules, `SHN_COMMON` merging (max size,
   materialized into `.data`), section symbols bound to their output
   section; duplicate **strong** definitions are a multiple-definition
   error.
3. **Extract archives** (greedy to fixpoint, the classic archive
   algorithm): a member is pulled in when it defines a symbol currently
   undefined in the loaded set; rescan rounds handle members whose
   definitions make new symbols undefined. Extraction markers use the
   stable member-data pointers.
4. **Re-resolve** (idempotent; an `(obj, sym)` pair already registered is
   skipped) so the duplicate-definition check also covers archive members.
5. **Layout**: `.text` at `0x401000` (16-aligned), `.rodata` packed
   8-aligned after it, `.data` page-aligned (RW segment), `.bss` after
   `.data` (page-aligned when `.data` is empty). Input sections are
   merged **by name family** (`.text*`/`.rodata*`/`.data*`/`.bss*` —
   gcc-style `.data.rel.ro`, `.rodata.str1.1` fold in; `.eh_frame` is
   dropped for a static exec).
6. **Apply relocations**: `R_X86_64_{64, PC32, PLT32, 32, 32S, RELATIVE}`;
   `PLT32` resolves as `PC32` (a fully static link has no PLT stubs).
   Unknown types are a hard error. Section symbols resolve to the final
   section VMA (+ value); UND symbols must be resolved or the link fails
   with `undefined symbol`.
7. **Emit ET_EXEC**: ELF header (all `Half`/`Word`/`DWORD` field sizes),
   two `PT_LOAD`s with `p_offset ≡ p_vaddr (mod 0x1000)`, section headers,
   and the string tables placed **after** the shdr table (a leading NUL in
   both `.strtab` and `.shstrtab` is mandatory). Output is `chmod 0755`.

## Entry point

If an input defines `_start` (a CRT object), it is the entry. Otherwise
the linker **generates a 16-byte `_start` stub** at the front of `.text`:

```
xor %rbp,%rbp ; call main ; mov %eax,%edi ; mov $60,%eax ; syscall
```

with the `call` displacement patched once `main`'s final VMA is known.
This is why a zeroc object whose `main` ends in `ret` can be an entry
point: the stub supplies the return address. (ZeroCC programs use the
no-argument `main`.)

## What the differential gate covers

`make test-linker` (`tools/linker_tests.py`, `test/linker/`): 8 functional
cases (cross-TU call/int global/double, uninitialized globals, 3-file
chain, `.rodata` string from TU2, a ~40-instruction fib program), each
**differentially compared against a `gcc -no-pie` link of the same
objects**; 2 archive cases (member extraction); 3 error cases
(multiple definition, missing `main`, undefined symbol); 1
non-x86-64 rejection. `test/linker/%` is excluded from the `make test`
glob (multi-file inputs, not standalone programs).

## Latent emitter bug found by T2 (fixed)

The first no-`.text` object (a globals-only TU, e.g. `int target = 7;`)
exposed a real bug in the object **emitter**: `src/main.c` hardcoded
section indices (`.text`=1, `.rodata`=2, `.data`=2-or-3), which drift when
an earlier section is absent — the symbol `shndx` then pointed at
`.strtab` and **both** zerocc's own linker and the system linker
misresolved the global (segfault; ld bound it `ABS`). Fixed by
`elf_writer_section_index(ctx, name)` (1-based index of an already-added
section, 0 if absent) replacing every hardcoded position. Invariant:
**call sites that add symbols or relocations must resolve the section
index by name after the sections are added — never assume a position.**

## Known limitations (documented, not bugs)

- **x86-64 objects only** for now (the aarch64/riscv relocation sets are
  the next step; the QEMU gate already covers the per-ISA emitters).
- No dynamic symbols/PLT stubs, no `GOTPCRELX`/`GOT32` materialization,
  no interposition/weak-alias lattice — correct for a fully static link
  of zeroc objects (and zero_rt), which is the intended customer.

(The earlier cross-TU `int *p = &x;` limitation is closed in the same
sprint: the emitter now produces the correct object shape — a `.rela..data`
`R_X86_64_64` against the UND symbol, via a kind-2 by-name data reloc —
and all three paths (own linker, system linker, JIT merge) resolve it.
See `multi-tu.md`; the `cross_ptr` / `cross_ptr_gccobj` battery cases
cover both object shapes.)
