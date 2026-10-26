---
type: Concept
title: Zero_RT — ZeroCC's Own Freestanding Runtime (Phase 0: Linux x86-64)
description: The zero_rt/ runtime (heap, strings, exit, minimal stdio with Ryū float formatting) compiled by zerocc itself, auto-linked by --zerocc-link, and the zeroc_main_wrapper entry routing that makes atexit work on a plain return from main.
resource: zero-rt
tags: [zero-rt, runtime, freestanding, heap, stdio, ryu, zero-cc-link, phase-0]
timestamp: 2026-10-26T05:08:25Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Zero_RT — ZeroCC's Own Freestanding Runtime

**Phase 0 scope: Linux x86-64.** The goal is for ZeroCC programs to need no system libc/crt: the runtime is owned by the project, compiled **by zerocc itself**, and linked by zerocc's own linker ([own-linker.md](./own-linker.md)).

## Layout (`zero_rt/`)

- `rt_os.h` — OS-boundary declarations. Syscalls go through the `__zerocc_syscall(...)` IR builtin (see the `OP_SYSCALL` entry in [ir-and-regalloc.md](./ir-and-regalloc.md) / the Sprint 43 changelog), not inline asm.
- `linux/os.c` — syscall implementations: `read`, `write`, `brk`, `getpid`, `exit_group`.
- `core/heap.c` — `malloc`/`free`/`calloc`/`realloc` on a `brk`-backed first-fit allocator.
- `core/string.c` — `memcpy`/`memmove`/`memset`/`memcmp`, `strlen`/`strcpy`/`strcat`/etc.
- `core/exit.c` — `atexit`/`exit`/`abort` plus **`zeroc_main_wrapper`**: calls `main`, then `exit(status)`, so the atexit list runs on a plain `return` from `main`.
- `core/stdio.c` — unbuffered `printf`/`fprintf`/`sprintf`/`snprintf`/`puts`/`putchar`; float formatting via vendored **Ryū** (`third_party/vendor/ryu/`, Apache-2.0 OR Boost-1.0: `d2fixed_buffered` for `%f`, `d2exp_buffered` for `%e`/`%g`).

Virtual freestanding headers for the compiler's preprocessor (`core/preprocessor.h`) were extended for Phase 0: `inttypes.h` (stdint + PRI*/SCN* macros), `assert.h` (always-evaluated, `abort()` on failure), and `atexit` in `stdlib.h`.

## Build & Self-Compilation

`make all` builds `build/lib/zero_rt.a` **with zerocc itself** (`$(BUILD_DIR)/bin/zerocc -c zero_rt/... -O0`), then archives the objects with the system `ar` (the own linker reads classic archives; *writing* archives is a follow-up). The rule depends on `build/bin/zerocc`, so a compiler change rebuilds the runtime. Dead pre-Phase-0 Python tools live in `tools/archive/`.

## Auto-Linking (`--zerocc-link`)

In `compile_and_link` (`src/main.c`), when `--zerocc-link` is used the driver appends `build/lib/zero_rt.a` to the archive list — located via `$ZERO_RT_A` or `../lib/zero_rt.a` relative to the zerocc binary (`/proc/self/exe`). **`--no-zerocc-rt`** disables it (tests linking plain programs against the raw `_start` stub keep working).

## Linker Changes for zero_rt (`src/zero_cc/linker/linker.c`)

- **Entry routing**: when the linked set contains zero_rt's `zeroc_main_wrapper`, the generated `_start` stub (see [own-linker.md](./own-linker.md)) calls the **wrapper** instead of `main`. The wrapper member is **force-extracted** from the archive (`member_defines(RT_ENTRY_WRAPPER)`) so it is present even if nothing references it; without zero_rt the stub keeps calling `main` directly.
- **Per-round re-resolve**: `extract_archives` re-runs `resolve_symbols` after any round that extracted new members, so a newly extracted member's undefined references (e.g. `zeroc_main_wrapper` → `main`, `stdio` → `os.write`) are visible to *subsequent* archive scans. Pre-fix the greedy extraction stopped one member short on exactly this pattern.
- **`elf_reader.c`**: `shoff` is parsed as a full 64-bit `Elf64_Shdr` offset (the old 16-bit mask broke section-header bounds checks for objects whose section table sits above 64 KiB).

## Phase 0 & Phase 1 Closeout (Sprint 49 & Sprint 55 — Resolved)

All `zero_rt` `printf` / `snprintf` / `sprintf` / `fprintf` / heap / string capabilities are 100% verified across host (x86-64) and cross-architectures (AArch64 and RISC-V 64-bit):
- **Cross Runtime Archives (`build/lib/zero_rt_aarch64.a`, `build/lib/zero_rt_riscv64.a`)**: Built directly with zerocc self-compilation (`--target aarch64`, `--target riscv64`) and auto-linked under `--zerocc-link` matching `--target` or sniffing input object `e_machine`.
- **AArch64 Relocations & Stores**: Fixed `R_AARCH64_ABS64`/`ABS32` relocation stomping lower 32 bits of pointer targets in `linker.c`, corrected `R_AARCH64_ADR_PREL_PG_HI21` instruction mask (`0x60FFFFE0`), and eliminated broken `param_save` offset calculations in `aarch64.c`.
- **RISC-V 64-bit Immediates & Branches**: Added full 64-bit immediate materialization in `riscv.c` (`slli 32` + low 32 bits ORed), inverted B-type branch skip (+8) over 20-bit JAL trampolines for >4KB jumps, and mapped variadic float args to GP register streams per psABI.
- **Syscall Non-Leaf Frame Safety**: `OP_SYSCALL` marks lowering functions as non-leaf, preventing x86-64 red zone stack omission (`alloc_frame_size = 0`) from corrupting stack variables during syscall register push/pop sequences.
- **Linux Generic Syscalls**: Corrected `RT_SYS_BRK` to 214 and enabled `RT_HAS_BRK` for both AArch64 and RISC-V64. Fixed 64-bit heap allocation free-bit mask in `heap.c` and copy loops in `string.c`.

## Related Concepts

- [ZeroCC Own Linker (T2)](./own-linker.md) — the linker zero_rt is linked with.
- [Multi-Translation-Unit Driver (T1)](./multi-tu.md)
- [Frontend, Type System & Semantic Analysis](./frontend-and-types.md) — string-literal array initializers (needed by runtime code).
- [Build, Test & Verification Guide](./build-and-verification.md)
