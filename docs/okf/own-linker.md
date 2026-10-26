---
type: Concept
title: ZeroCC Own Linker (T2) - ELF Reader, ar Reader, Static ET_EXEC Output (x86-64, AArch64, RISC-V64)
description: ZeroCC's own static linker - the ELF64-LE object reader, the classic ar archive reader with greedy member extraction, symbol resolution, section layout at conventional x86-64/AArch64/RISC-V64 bases, supported relocation sets, and per-ISA generated _start stubs. Gate: make test-linker (21/21).
resource: linker
tags: [linker, elf, ar, aot, x86-64, aarch64, riscv64, relocations, zero-rt]
timestamp: 2026-10-26T02:54:02Z
generated: pi-agent
verified: engineering-lead
status: active
---

# ZeroCC Own Linker (T2)

`zerocc a.c b.c --zerocc-link -o app` links the per-TU objects with
ZeroCC's own linker instead of the system linker. The result is a
directly-executable **static ET_EXEC** (no libc, no crt files): two
`PT_LOAD` segments (RX, RW at the next page), a rebuilt
`.symtab`/`.strtab`/`.shstrtab`, and a linker-generated `_start` stub.

**Supported ISAs:** x86-64, **AArch64** (`--target aarch64`), and **RISC-V64** (`--target riscv64`). Inputs
must share one `e_machine` (mixed-architecture link is an error). The
per-ISA differences: text base (`0x401000` x86-64, `0x400000` AArch64, `0x10000` RISC-V64 —
standard static conventions), the relocation sets, and the `_start` stub
bytes. zero_rt Phase 0 is the first customer (x86-64; a cross zero_rt is
Phase 1, so `--zerocc-link` only auto-links `zero_rt.a` for x86-64).

This is the next step on the zero_rt critical path (T1 driver → **T2 own
linker** → T3 runtime): zero_rt Phase 0 is its first customer.

## Components (`src/zero_cc/linker/`)

| File | Role |
|---|---|
| `elf_reader.{h,c}` | Minimal ELF64-LE **ET_REL** parser: section headers (name-resolved), `.symtab` + strtab, `SHT_RELA` sections bound to their target via `sh_info`. Accepts `e_machine` `EM_X86_64`, `EM_AARCH64`, `EM_RISCV` (243) and records it; all copies are bounds-checked; little-endian loads are explicit. |
| `ar_reader.{h,c}` | Classic `ar` archive reader (GNU/BSD/LLVM ar): 60-byte member headers, long-name (`#1/NNN`) members, index-member skipping (`//`, `__.SYMDEF`, `Sym64 Table`). Members are returned as views into the archive image. |
| `linker.{h,c}` | `zero_cc_link_exec(out, objects, archives, err)`: the pipeline below. |

Driver integration (`src/main.c`): `--zerocc-link` (with `-o`) selects the
own linker; supports x86-64, aarch64, and riscv64 (rejected with a clear error otherwise).
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
5. **Layout**: `.text` at `0x401000` (x86-64), `0x400000` (AArch64), or `0x10000` (RISC-V64);
   `.rodata` packed 8-aligned after it, `.data` page-aligned (RW segment), `.bss` after
   `.data` (page-aligned when `.data` is empty). Input sections are
   merged **by name family** (`.text*`/`.rodata*`/`.data*`/`.bss*` —
   gcc-style `.data.rel.ro`, `.rodata.str1.1` fold in; `.eh_frame` is
   dropped for a static exec).
6. **Apply relocations** (per `e_machine`; all inputs must agree):
   - **x86-64**: `R_X86_64_{64, PC32, PLT32, 32, 32S, RELATIVE}`;
     `PLT32` resolves as `PC32` (a fully static link has no PLT stubs).
   - **AArch64**: `R_AARCH64_{ABS64, ABS32, ADR_PREL_PG_HI21,
     ADD_ABS_LO12_NC, CALL26}`. The PC-relative/absolute-field relocations
     patch **instruction fields** in place (the emitter emits zeroed imm
     fields): `ADR_PREL_PG_HI21` inverts `ADRP` (`imm21 = page(S+A) -
     page(P)`, encoded `imm21[20:2]` in bits [23:5], `imm21[1]` in bit 30,
     `imm21[0]` in bit 29 — a layout verified empirically against the
     cross toolchain, not the naive `<<5` form), `ADD_ABS_LO12_NC` writes
     imm12 to bits [21:10], `CALL26` writes `imm26 = (S+A-P)>>2` to bits
     [25:0] (branch-aligned, ±128 MB).
   - **RISC-V64**: `R_RISCV_64` (2), `R_RISCV_32` (1), `R_RISCV_JAL` (17, J-type `imm20 = (S+A-P)>>1`),
     `R_RISCV_CALL` / `R_RISCV_CALL_PLT` (18/19, 8-byte macro pair auipc+jalr),
     `R_RISCV_PCREL_HI20` (23, `(Δ + 0x800) >> 12` into bits [31:12]),
     `R_RISCV_PCREL_LO12_I` (24, `(Δ + A) & 0xFFF` into bits [31:20]),
     `R_RISCV_PCREL_LO12_S` (25, `(Δ + A) & 0xFFF` into bits [31:25] and [11:7]),
     and no-op handling for `R_RISCV_{NONE, ALIGN, RELAX}`.
     *PCREL Pairing Invariant*: `LO12` references a local marker symbol placed at the companion `HI20`
     `auipc`. The linker collects all `HI20` delta values in a first pass, then looks up the delta
     keyed by marker symbol address to patch `LO12`.
   Unknown types are a hard error. Section symbols resolve to the final
   section VMA (+ value); UND symbols must be resolved or the link fails
   with `undefined symbol`.
7. **Emit ET_EXEC**: ELF header (all `Half`/`Word`/`DWORD` field sizes; RISC-V sets `EF_RISCV_FLOAT_ABI_DOUBLE` (4)),
   two `PT_LOAD`s with `p_offset ≡ p_vaddr (mod 0x1000)`, section headers,
   and the string tables placed **after** the shdr table (a leading NUL in
   both `.strtab` and `.shstrtab` is mandatory). Output is `chmod 0755`.

## Entry point

If an input defines `_start` (a CRT object), it is the entry. Otherwise
the linker **generates a `_start` stub** at the front of `.text`
(per ISA):

- **x86-64** (16 B):
  ```
  xor %rbp,%rbp ; call main ; mov %eax,%edi ; mov $60,%eax ; syscall
  ```
  with the `call` displacement patched once the target's final VMA is known.
- **AArch64** (12 B + `nop` pad): x0 holds the return value across `bl`:
  ```
  mov x8, #94   ; exit_group
  bl  <target>
  svc #0
  ```
  `bl` (not `b`) is essential: it sets LR so the target's `ret` falls
  through to the `svc`.
- **RISC-V64** (20 B stub):
  ```
  auipc t1, %pcrel_hi(target)
  addi  t1, t1, %pcrel_lo(target)
  jalr  ra, t1, 0
  li    a7, 93    ; __NR_exit
  ecall
  ```
  `jalr ra, t1, 0` sets RA so `main`'s `ret` falls through to the `ecall` exit syscall sequence.

This is why a zeroc object whose `main` ends in `ret` can be an entry
point: the stub supplies the return address. (ZeroCC programs use the
no-argument `main`.) **Target routing:** the call goes to `main`, *except*
when the linked set defines zero_rt's `zeroc_main_wrapper` (its archive
member is force-extracted) — then the stub calls the wrapper, which runs
`main` and then `exit()`, so the atexit list fires on a plain `return`.
See [Zero_RT](./zero-rt.md).

## What the differential gate covers

`make test-linker` (`tools/linker_tests.py`, `test/linker/`), 21 cases:
- **x86-64 (14)**: functional cases (cross-TU call/int global/double,
  uninitialized globals, 3-file chain, `.rodata` string from TU2, a
  ~40-instruction fib program), each **differentially compared against a
  `gcc -no-pie` link of the same objects**; 2 archive cases (member
  extraction); 3 error cases (multiple definition, missing `main`,
  undefined symbol).
- **unsupported-ISA rejection (1)**: `--target riscv32` errors cleanly.
- **AArch64 (3)**: cross-TU call, `.data`-only globals+string case (adrp/add pairs),
  and archive case — each linked with `--target aarch64 --zerocc-link`, run under
  `qemu-aarch64-static`, and differentially compared against `musl-gcc-aarch64 -static`.
- **RISC-V64 (3)**: `rv64_hi` cross-TU call, `rv64_data` global data/pointer access
  (pcrel_hi20/lo12_i pairs), and `rv64_ar` archive case — each linked with
  `--target riscv64 --zerocc-link`, run under `qemu-riscv64-static`, and differentially
  compared against `musl-gcc-riscv64 -static`.

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

- **32-bit targets** (`--target riscv32` is not yet supported in the own linker;
  x86-64, aarch64, and riscv64 are fully supported).
- **zero_rt is x86-64 only** today, so `--zerocc-link` auto-links it only
  for x86-64 targets (a cross zero_rt is Phase 1).
- No dynamic symbols/PLT stubs, no `GOTPCRELX`/`GOT32` materialization,
  no interposition/weak-alias lattice — correct for a fully static link
  of zeroc objects (and zero_rt), which is the intended customer.

(The earlier cross-TU `int *p = &x;` limitation is closed in the same
sprint: the emitter now produces the correct object shape — a `.rela..data`
`R_X86_64_64` against the UND symbol, via a kind-2 by-name data reloc —
and all three paths (own linker, system linker, JIT merge) resolve it.
See `multi-tu.md`; the `cross_ptr` / `cross_ptr_gccobj` battery cases
cover both object shapes.)
