# zero_rt — ZeroCC's Self-Contained Cross-Platform Runtime (T3 Plan)

**Status**: PLAN (drafted Sprint 25+, post C5/C6 closure)
**Related**: `docs/fragility-analysis.md` (T2/T3 tiers), `docs/okf/isa-backends.md` (backend vtable pattern), `docs/okf/abi-sysv-x86_64.md`

---

## 1. Purpose

`zero_rt` is a minimal C runtime **owned by ZeroCC**, with **no dependency on musl or any platform C library**, that lets ZeroCC-linked programs run self-contained on:

- **Linux** (ELF, `syscall` instruction)
- **Windows** (PE/COFF, kernel32.dll)
- **macOS** (Mach-O, Mach `syscall` instruction)

It is the *default floor* runtime. musl (vendored at `third_party/vendor/musl/musl-1.2.6`, pristine) remains available as an **optional full-stdio library on Linux** (`-lmusl`-style) for programs needing `fopen`/`scanf`/locale.

## 2. Rationale

musl cannot serve as a cross-platform runtime because two layers are hard-wired to Linux:

1. **Syscall boundary** — `arch/x86_64/syscall_arch.h` emits a raw `syscall` instruction with the Linux ABI (number in `rax`, args in `rdi/rsi/rdx/r10/r8/r9`). That instruction enters ring 0; only a Linux kernel answers it. A user-mode API like kernel32.dll *cannot* interpose.
2. **Binary format** — musl links ELF; Windows loads PE/COFF, macOS loads Mach-O.

Therefore the Windows/macOS "libc" must be a thin, ZeroCC-owned OS-boundary layer — and generalizing that layer to all platforms (instead of musl-on-Linux / zero_rt-elsewhere) gives one runtime core, three thin backends, and no fork of a 100k-LOC codebase.

**Design precedent**: the same shape as the existing `ZeroCCISABackend` vtable — portable core + per-target backends — applied to the OS boundary instead of the ISA.

## 3. Architecture

```
src/zero_cc/rt/
  core/        # portable C, no direct OS calls
  #   heap.c     first-fit free-list over a growable arena (~150 lines)
  #   stdio.c    puts / printf subset; integer formatting trivial
  #   dtoa       float formatting for %f/%e/%g (vendored, see §5)
  #   string.c   memcpy / memset / strlen / strcmp / strcpy
  #   exit.c     exit(), atexit list
  posix/       # Linux + macOS: ONE shared __syscallN() inline-asm file
  #             (register order identical: selector rax, args rdi/rsi/rdx/r10/r8/r9)
  #             + per-OS syscall-number tables
  windows/     # kernel32 wrappers: WriteFile, GetStdHandle,
  #             HeapCreate/HeapAlloc/HeapFree, ExitProcess
  crt0/        # per-format entry: elf_x86_64.s, macho_x86_64.s, pe_x86_64.s
```

### OS boundary (the entire per-OS surface)

| Op | Linux | macOS | Windows |
|---|---|---|---|
| write | `syscall 1` | `syscall 0x200000EB` | `WriteFile(GetStdHandle(-11))` |
| exit | `syscall 60` | `syscall 0x2000005D` | `ExitProcess` |
| grow heap | `brk` / `mmap` | `brk` / `mmap` | `HeapAlloc` / `VirtualAlloc` |
| entry stack | `[argc][argv…][envp…]` | same layout | `PROCESS_PARAMETERS` block |

## 4. Platform cost table (asymmetric — the planning insight)

| Target | Compiler ABI work | T2 linker work | zero_rt OS layer |
|---|---|---|---|
| **Linux** | none (done) | ELF writer (planned) | trivial number table |
| **macOS** | **~none** — Apple x86-64 follows SysV AMD64 (arg regs, 16-byte alignment, sret rules); arm64 follows AAPCS like Linux. *Verify red-zone allowance against Apple's 64-bit Intel ABI guide before Phase 2.* | **Mach-O writer** (new format: `__TEXT,__text`, `LC_SEGMENT_64`, `LC_ENTRY_POINT`) | Mach number table (asm file shared with Linux) |
| **Windows** | **the big one** — arg regs `rcx/rdx/r8/r9`, **no red zone**, `rbx`/`rbp` freely allocatable → new ABI vtable in the compiler | **PE/COFF writer** (new format + import tables) | kernel32 wrappers |

Consequences:

1. The Linux/macOS split inside zero_rt is nearly free (shared `__syscallN` asm; only number tables differ).
2. **Windows compiler-ABI work must be tracked as its own item**, not smuggled inside "runtime" scope.

## 5. The one real algorithm: float formatting

`%f`/`%e`/`%g` require a correct (ideally shortest-round-trip) double→decimal converter. Do not hand-roll it:

- **Vendor Ryū** (MIT, single file, correct shortest round-trip) under `third_party/vendor/` — same practice as bearssl/quickjs/musl.
- musl's own `vfprintf` is entangled with musl internals (locale, `mbstate_t`) — do **not** lift it.

## 6. Scope discipline

**In**: heap (`malloc`/`free`), minimal stdio (`puts`, `printf` subset incl. `%d %x %s %p %f %g`), string builtins, `exit`/`atexit`, optionally `open/read/write/close` (cheap on the syscall layer).

**Out** (until explicitly requested): threads, signals, locale, full `<stdio.h>`/`<stdlib.h>`, math (`libm`), TLS.

Acceptance criterion for "self-contained": a C99 program using `printf`/`malloc`/strings/`exit` links and runs with **no external library at all** (Windows: kernel32 only, which every PE imports anyway).

## 7. Phasing

### Phase 0 — Linux (rides on T2)

zero_rt-Linux is the **first real customer of the T2 ELF linker**: it validates crt0 + heap + printf + archive/member resolution end-to-end on the platform we develop on, with no new binary formats.

- Deliverables: `crt0/elf_x86_64.s`, `core/*`, `posix/linux` table; T2 links `app.o + zero_rt` into a standalone ELF.
- **Acceptance**: `zerocc -o app app.c` (zero_rt linked) runs; `ldd app` → not dynamic (or zero_rt objects only); round-trip battery vs gcc (int + float printf, malloc/free stress, strings).

### Phase 1 — Windows (T3-Win)

- Deliverables: PE/COFF writer in T2 (import tables, `PROCESS_PARAMETERS` entry), **Win64 ABI vtable in the compiler** (arg regs, no red zone, `rbx`/`rbp` allocatable), `crt0/pe_x86_64.s`, `windows/*` kernel32 wrappers.
- **Acceptance**: a `.exe` runs on Windows (or WSL-imported); ABI conformance battery (arg passing, sret >16B, shadow space, leaf-function stack discipline without red zone).
- Escape hatch (later, cheap): `-lucrtbase` — the import-table machinery is per-DLL-identical, so "full C99 stdlib" becomes a link flag, not an architecture change.

### Phase 2 — macOS

- Deliverables: Mach-O writer in T2, Mach syscall table, `crt0/macho_x86_64.s`; compiler side nearly free.
- **Risk (validate first)**: a raw-syscall Mach-O that imports **no dylibs at all** (no libSystem) is legal but unorthodox. The minimal smoke test (empty `main` → `exit(0)` via Mach syscall) is the first Phase-2 task, before any further investment.
- Secondary risk: arm64 minimum page/segment alignment is 16KB vs 4KB on x86-64 — linker alignment knobs.

## 8. Verification (all phases)

- Round-trip battery per phase: ZeroCC-compile → zero_rt-link → run → compare exit codes/outputs against host gcc.
- Existing suite stays green: `make test` (140/140), `python3 tools/sret_verifier.py --verify-all` (6/6), `python3 tools/okf.py lint && python3 tools/okf.py index`.
- New OKF page `docs/okf/zero-rt.md` lands when Phase 0 ships (concept: OS-boundary vtable, phase status, vendored dtoa).

## 9. Related known gaps found during C5/C6 verification (Sprint 25+)

1. ~~**AOT `-S` never emits `.data`**~~ — **resolved in `b0975fc`**: `-S` output now emits `.data`/`.rodata` global sections; objects with globals link and run.
2. ~~**`CVTSI2SD` (int→double) unimplemented in the asm emitters**~~ — **resolved in `b0975fc`** (`CVTSI2SD`/`CVTTSD2SI` across all three ISAs).
3. **`-S` float constant/comparison/arithmetic width (Sprint 27, extended Sprint 28)**: float constants are stored as 8-byte doubles in `.rodata`, but the x86 asm emitter loads them with 4-byte `movss` (reads the low 4 bytes → 0.0), float `!=` emits integer `cmpq` (needs `ucomisd` + `setcc`), and FP arithmetic emits 2-operand `addsd %src2,%dst` / `subsd` (only correct when dst pre-loads src1 — the JIT MC path handles the general 3-operand case). Repro: `float a=2.0f+0.25f; if(a!=2.25f) return 1;` → rc=1 via `-S`, rc=0 via `-o`. The `-o` ELF path is unaffected. **RESOLVED in Sprint 30** (3-operand FP aliasing, `emit_fp_cmp_asm`, `movsd` constants, `VLOADSS`/`VSTORESS` width, `OP_CMP_ZERO`; plus C12 `ucomisd` flag mapping and C13 REX-before-prefix in the MC emitters).
4. ~~**C8 (Sprint 27, not `-S`-specific)**~~: float function-call result clobbered by FP arithmetic in the same expression — **RESOLVED in Sprint 28** (materialized FP returns + XMM spans-call spill + no-steal guards + class-aware FP spill + XMM/VEC pool cross-marking; see `docs/fragility-analysis.md` §C8).
5. ~~**C9 / C10 (Sprint 28, pre-existing)**~~: inline int→float cast dropped in call-argument position; float local as call argument inside a loop — **RESOLVED in Sprint 29** (C9 via the `NODE_CAST` float-target branch; C10 was C11 in disguise). See `docs/fragility-analysis.md` §C9/§C10/§C11.

## 10. Open questions

- Ryū vendoring (recommended) vs a minimal fixed-precision dtoa — decision at Phase 0.
- macOS: confirm Apple x86-64 red-zone status and whether a dylib-free Mach-O loads under both x86-64 and arm64 (SIP/Gatekeeper are irrelevant for locally built binaries; loader behavior is the question).
- Windows: exact kernel32 import set for the Phase-1 minimum (write/exit/heap) — confirm no implicit ucrt dependency from crt0.
