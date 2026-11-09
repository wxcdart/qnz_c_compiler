---
type: Concept
title: ZeroCC Own Linker (T2) - ELF Reader, ar Reader, Static ET_EXEC/PIE Output (x86-64, AArch64, RISC-V64)
description: ZeroCC's own static linker - the ELF64-LE object reader, the classic ar archive reader with greedy member extraction, symbol resolution, section layout at conventional x86-64/AArch64/RISC-V64 bases, the x86-64 far-rel32 detour-thunk pool, the static-PIE (ET_DYN) self-relocating link, supported relocation sets, and per-ISA generated _start stubs. Gate: make test-linker (21/21) + test/linker/far_thunk.c + test/linker/pie_test.c.
resource: linker
tags: [linker, elf, ar, aot, x86-64, aarch64, riscv64, relocations, zero-rt, static-pie, thunks]
timestamp: 2026-11-09T19:44:53Z
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
bytes. `--zerocc-link` auto-links the matching freestanding [Zero_RT](./zero-rt.md) archive
(`zero_rt.a` for x86-64, `zero_rt_aarch64.a` for AArch64, `zero_rt_riscv64.a` for RISC-V64;
pass `--no-zerocc-rt` to link against the raw `_start` stub alone).

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
`--zerocc-link-pie` selects the **static-PIE** variant (x86-64 only; see
below). Inputs may be `.c` (compiled to a temp `.o`), prebuilt `.o` (passed
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
     **Far rel32 detour thunks**: a `PC32`/`PLT32` whose final delta
     exceeds ±2 GB used to be silently truncated to `int32`. Now a thunk
     pool (14 B per slot, reserved at the end of `.text` during layout) is
     used: for an out-of-range **`call` (`e8`) or `jmp` (`e9`)** site the
     site is re-aimed at a pool slot holding the detour sequence
     `ff 25 00 00 00 00` + 8-byte absolute target (target = `S+A+4`, the
     instruction after the far call); near sites are patched directly as
     before. A far **rip-relative data reference** is a hard error
     (`rip-relative data reference`) — executing a thunk is not a value
     source. Unclaimed pool slots stay zero and are never executed. The
     `test/linker/far_thunk.c` driver pins both behaviors with synthetic
     objects (~3 GB away target in a 4 GB `.bss`).
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

## Static PIE (x86-64, `--zerocc-link-pie`)

`zero_cc_link_exec_pie` (CLI: `--zerocc-link-pie`) emits a **static
PIE** — an `ET_DYN` image with no `.dynamic` section that the kernel
may place at an arbitrary base (ASLR), instead of the fixed-base
`ET_EXEC`. x86-64 only, and the inputs must **not** define their own
`_start` (the linker's self-relocating stub is the relocation engine).

Mechanism, all in `src/zero_cc/linker/linker.c`:

1. **Layout from base 0**: sections are laid out starting at VMA 0
   (`.text` first, 16-aligned); a **`.rela.dyn` region** (8-aligned,
   24 B per slot) is reserved immediately after the RX content — one
   slot per `R_X86_64_64`, plus one per `PC32`/`PLT32` as a bound (a
   far rel32 thunk's 8-byte absolute target is itself a RELATIVE
   target). The RW sections are pushed past it.
2. **Relocation translation**: every `R_X86_64_64` becomes an
   `R_X86_64_RELATIVE` entry `{r_offset = site, r_addend = S+A}` — the
   site keeps its zero placeholder and the value is only known once the
   kernel has chosen the load base. `R_X86_64_{32, 32S}` are a hard
   error (32-bit absolutes cannot be expressed in a PIE image); the
   rel32/thunk logic is unchanged (near direct, far detour thunk).
3. **Emit**: `e_type = ET_DYN`, `e_entry = 0` (the stub), the first
   `PT_LOAD` is emitted **`PF_R|PF_W|PF_X`** (the kernel maps it
   `rwxp`), and a `SHT_RELA` `.rela.dyn` section header + content are
   added inside the RX segment.
4. **Self-relocating `_start` stub** (95 B, x86-64; assembled and
   verified, then embedded as a byte array with four patched fields —
   RX length, `.rela.dyn` rip-relative disp32, entry count, target
   call rel32):
   ```
   lea rdi,[rip-7]         ; rdi = image base
   mov r9, RX_LEN          ; mprotect length (link-time immediate)
   lea rsi,[.rela.dyn]
   mov ecx, N
   loop: mov rax,[rsi]; mov rbx,[rsi+16];
         lea r8,[rdi+rbx]; mov [rdi+rax],r8   ; site = base + addend
         add rsi,24; loop
   mprotect(base, RX_LEN, RX)   ; drop W, keep X (page stays executable)
   mov rdi,[rsp]; mov rsi,[rsp+8]  ; argc/argv (kernel-provided)
   xor rbp,rbp
   call <target>           ; main or zeroc_main_wrapper
   mov edi,eax; mov eax,60; syscall
   ```
   The RWX first segment is the key: the stub **executes from the very
   pages it rewrites**, so they must be writable *and* executable while
   the loop runs (a plain RX segment faults on the write; an RW
   re-protection faults on the next instruction fetch — Linux W^X).
   The absolute write (`site = base + addend`, not `site += addend`)
   is required because the placeholder sites are zero, not the addend.

The `test/linker/pie_test.c` driver compiles a two-TU program (static
global, static string, function pointer into the other TU) with
`zerocc -c`, links it both ways, and requires: the EXEC build prints
the reference output, the PIE build is a real `ET_DYN` carrying a
non-empty `SHT_RELA` `.rela.dyn`, and prints byte-identical output —
which only happens if every absolute reference (including the function
pointer the call goes through) was fixed up at load time.

## What the differential gate covers

`make test-linker` (`tools/linker_tests.py`, `test/linker/`), 21 cases, plus two self-contained
driver tests built and run by the normal test suite (`test/linker/far_thunk.c`
for the far-rel32 thunk pool, `test/linker/pie_test.c` for the static-PIE
link):
- **x86-64 (14)**: functional cases (cross-TU call/int global/double,
  uninitialized globals, 3-file chain, `.rodata` string from TU2, a
  ~40-instruction fib program), each **differentially compared against a
  `gcc -no-pie` link of the same objects**; 2 archive cases (member
  extraction); 3 error cases (multiple definition, missing `main`,
  undefined symbol).
- **AArch64 (3)**: cross-TU call, `.data`-only globals+string case (adrp/add pairs),
  and archive case — each linked with `--target aarch64 --zerocc-link`, run under
  `qemu-aarch64-static`, and differentially compared against `musl-gcc-aarch64 -static`.
- **RISC-V64 (3)**: `rv64_hi` cross-TU call, `rv64_data` global data/pointer access
  (pcrel_hi20/lo12_i pairs), and `rv64_ar` archive case — each linked with
  `--target riscv64 --zerocc-link`, run under `qemu-riscv64-static`, and differentially
  compared against `musl-gcc-riscv64 -static`.
- **RISC-V32 (3)**: `rv32_hi` cross-TU call, `rv32_data` global data/pointer access
  (pcrel_hi20/lo12_i pairs), and `rv32_ar` archive case — each linked with
  `--target riscv32 --zerocc-link`, run under `qemu-riscv32-static`, and differentially
  compared against `musl-gcc-riscv32 -static`.

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
This invariant is also relied on by `__attribute__((section("name")))` on
global objects (Sprint 107) and functions (Sprint 108) in `compile_object_single`
([`src/main.c`](../../src/main.c)), which emit dedicated `PROGBITS`/`NOBITS`/`EXECINSTR`
sections and bind each symbol's `shndx` via `elf_writer_section_index`.

## Known limitations (documented, not bugs)

- **zero_rt cross targets (Sprint 55, Sprint 143)**: `--zerocc-link` auto-links the per-target runtime archive (`zero_rt.a` x86-64, `zero_rt_aarch64.a`, `zero_rt_riscv64.a`, `zero_rt_riscv32.a`), built by `make all` via zerocc self-compilation.
- No dynamic symbols/PLT stubs, no `GOTPCRELX`/`GOT32` materialization,
  no interposition/weak-alias lattice — correct for a fully static link
  of zeroc objects (and zero_rt), which is the intended customer.

(The earlier cross-TU `int *p = &x;` limitation is closed in the same
sprint: the emitter now produces the correct object shape — a `.rela..data`
`R_X86_64_64` against the UND symbol, via a kind-2 by-name data reloc —
and all three paths (own linker, system linker, JIT merge) resolve it.
See [multi-tu.md](./multi-tu.md); the `cross_ptr` / `cross_ptr_gccobj` battery cases
cover both object shapes.)
