---
type: Concept
title: Zero_RT — ZeroCC's Own Freestanding Runtime (Linux x86-64 + AArch64/RISC-V64 cross archives)
description: The zero_rt/ runtime (heap, strings, exit, minimal stdio with Ryū float formatting) compiled by zerocc itself, auto-linked by --zerocc-link, and the zeroc_main_wrapper entry routing that makes atexit work on a plain return from main.
resource: zero-rt
tags: [zero-rt, runtime, freestanding, heap, stdio, ryu, zero-cc-link, phase-0]
timestamp: 2026-11-05T02:19:30Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Zero_RT — ZeroCC's Own Freestanding Runtime

**Phase 0 scope: Linux x86-64** (Sprint 49), extended in Sprint 55 to cross-architecture archives for AArch64 and RISC-V64 (see the closeout section below). The goal is for ZeroCC programs to need no system libc/crt: the runtime is owned by the project, compiled **by zerocc itself**, and linked by zerocc's own linker ([own-linker.md](./own-linker.md)).

## Layout (`zero_rt/`)

- `rt_os.h` — OS-boundary declarations. Syscalls go through the `__zerocc_syscall(...)` IR builtin (`OP_SYSCALL`, see [ir-and-regalloc.md](./ir-and-regalloc.md) and [isa-backends.md](./isa-backends.md)), which is implemented across all **4 target ISAs** (`x86_64` `syscall`, `aarch64` `svc #0`, `riscv64`/`riscv32` `ecall`) in both raw machine-code and GAS `.s` assembly emitters (Sprint 89), not inline asm.
- `linux/os.c` — syscall implementations: `read`, `write`, `brk`, `getpid`, `exit_group`.
- `core/heap.c` — `malloc`/`free`/`calloc`/`realloc` on a `brk`-backed first-fit allocator.
- `core/string.c` — `memcpy`/`memmove`/`memset`/`memcmp`, `strlen`/`strcpy`/`strcat`/etc.
- `core/exit.c` — `atexit`/`exit`/`abort` plus **`zeroc_main_wrapper`**: calls `main`, then `exit(status)`, so the atexit list runs on a plain `return` from `main`.
- `core/stdio.c` — unbuffered `printf`/`fprintf`/`sprintf`/`snprintf`/`puts`/`putchar`; float formatting via vendored **Ryū** (`third_party/vendor/ryu/`, Apache-2.0 OR Boost-1.0: `d2fixed_buffered` for `%f`, `d2exp_buffered` for `%e`/`%g`).

Virtual freestanding headers in [`src/zero_cc/core/virtual_headers.h`](../../src/zero_cc/core/virtual_headers.h) (extracted from `core/preprocessor.h` in Sprint 80, with preprocessor expansion in [`src/zero_cc/core/preprocessor.c`](../../src/zero_cc/core/preprocessor.c)) provide `inttypes.h` (`stdint.h` + `PRI*`/`SCN*` macros), `assert.h` (always-evaluated, `abort()` on failure), `atexit` in `stdlib.h`, `immintrin.h` (Sprint 89), and full `<stdio.h>` stream declarations including `freopen`, `feof`, `ferror`, `clearerr`, and `ungetc` (Sprint 106). Non-local jumps (`setjmp`/`longjmp`, `__builtin_setjmp`/`__builtin_longjmp`) are lowered directly to inline multi-ISA `OP_SETJMP`/`OP_LONGJMP` instructions across all 4 ISAs (Sprint 88) without requiring any host libc helper.

## Build & Self-Compilation

`make all` builds `build/lib/zero_rt.a` **with zerocc itself** (`$(BUILD_DIR)/bin/zerocc -c zero_rt/... -O0`), then archives the objects with the system `ar` (the own linker reads classic archives; *writing* archives is a follow-up). Since Sprint 55 the same rule also builds the cross archives `build/lib/zero_rt_aarch64.a` and `build/lib/zero_rt_riscv64.a` (zerocc `--target aarch64` / `--target riscv64` self-compilation). The rules depend on `build/bin/zerocc`, so a compiler change rebuilds the runtime. Dead pre-Phase-0 Python tools live in `tools/archive/`.

## Auto-Linking (`--zerocc-link`)

In `compile_and_link` (`src/main.c`), when `--zerocc-link` is used the driver appends the matching zero_rt archive to the archive list: `zero_rt.a` for x86-64, `zero_rt_aarch64.a` for AArch64, `zero_rt_riscv64.a` for RISC-V64 (Sprint 55) — located via `$ZERO_RT_A` / `$ZERO_RT_AARCH64_A` / `$ZERO_RT_RISCV64_A` or `../lib/<name>` relative to the zerocc binary (`/proc/self/exe`). **`--no-zerocc-rt`** disables it (tests linking plain programs against the raw `_start` stub keep working).

## Linker Changes for zero_rt (`src/zero_cc/linker/linker.c`)

- **Entry routing**: when the linked set contains zero_rt's `zeroc_main_wrapper`, the generated `_start` stub (see [own-linker.md](./own-linker.md)) calls the **wrapper** instead of `main`. The wrapper member is **force-extracted** from the archive (`member_defines(RT_ENTRY_WRAPPER)`) so it is present even if nothing references it; without zero_rt the stub keeps calling `main` directly.
- **Per-round re-resolve**: `extract_archives` re-runs `resolve_symbols` after any round that extracted new members, so a newly extracted member's undefined references (e.g. `zeroc_main_wrapper` → `main`, `stdio` → `os.write`) are visible to *subsequent* archive scans. Pre-fix the greedy extraction stopped one member short on exactly this pattern.
- **`elf_reader.c`**: `shoff` is parsed as a full 64-bit `Elf64_Shdr` offset (the old 16-bit mask broke section-header bounds checks for objects whose section table sits above 64 KiB).

## Phase 0 & Phase 1 Closeout (Sprint 49 & Sprint 55 — Resolved)

All `zero_rt` `printf` / `snprintf` / `sprintf` / `fprintf` / heap / string capabilities are 100% verified across host (x86-64) and cross-architectures (AArch64 and RISC-V 64-bit):
- **Cross Runtime Archives (`build/lib/zero_rt_aarch64.a`, `build/lib/zero_rt_riscv64.a`)**: Built directly with zerocc self-compilation (`--target aarch64`, `--target riscv64`) and auto-linked under `--zerocc-link` matching `--target` or sniffing input object `e_machine`.
- **AArch64 Relocations & Stores**: Fixed `R_AARCH64_ABS64`/`ABS32` relocation stomping lower 32 bits of pointer targets in `linker.c`, corrected `R_AARCH64_ADR_PREL_PG_HI21` instruction mask (`0x60FFFFE0`), and eliminated broken `param_save` offset calculations in `aarch64.c`.
- **RISC-V 64-bit Immediates & Branches**: Added full 64-bit immediate materialization in `riscv.c` (`slli 32` + low 32 bits ORed), inverted B-type branch skip (+8) over 20-bit JAL trampolines for >4KB jumps, and mapped variadic float args to GP register streams per psABI.
- **Syscall Non-Leaf Frame Safety & 4-ISA QEMU Gate (Sprint 89)**: `OP_SYSCALL` marks lowering functions as non-leaf, preventing x86-64 red zone stack omission (`alloc_frame_size = 0`) from corrupting stack variables during syscall register push/pop sequences; `aarch64_emit_syscall_asm` maintains 16-byte stack alignment (`112`-byte register save area) across `svc #0`; and [`test/isa_qemu/13_syscall.c`](../../test/isa_qemu/13_syscall.c) differential-tests `__zerocc_syscall` (`SYS_getpid`, `SYS_write`, `SYS_brk`) across all 4 ISAs (`x86_64`, `aarch64`, `riscv64`, `riscv32`) in both raw binary and `-S` assembly modes under `make test-isa-qemu`.
- **Linux Generic Syscalls**: Corrected `RT_SYS_BRK` to 214 and enabled `RT_HAS_BRK` for both AArch64 and RISC-V64. Fixed 64-bit heap allocation free-bit mask in `heap.c` and copy loops in `string.c`.

## Related Concepts

- [ZeroCC Own Linker (T2)](./own-linker.md) — the linker zero_rt is linked with.
- [Multi-Translation-Unit Driver (T1)](./multi-tu.md)
- [Frontend, Type System & Semantic Analysis](./frontend-and-types.md) — string-literal array initializers (needed by runtime code).
- [Build, Test & Verification Guide](./build-and-verification.md)
