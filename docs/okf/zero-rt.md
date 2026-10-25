---
type: Concept
title: Zero_RT — ZeroCC's Own Freestanding Runtime (Phase 0: Linux x86-64)
description: The zero_rt/ runtime (heap, strings, exit, minimal stdio with Ryū float formatting) compiled by zerocc itself, auto-linked by --zerocc-link, and the zeroc_main_wrapper entry routing that makes atexit work on a plain return from main.
resource: zero-rt
tags: [zero-rt, runtime, freestanding, heap, stdio, ryu, zero-cc-link, phase-0]
timestamp: 2026-10-25T04:47:42Z
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

## Phase 0 Closeout (Sprint 49 — Resolved)

All Phase 0 `zero_rt` `printf` / `snprintf` / `sprintf` / `fprintf` formatting issues in `--zerocc-link` binaries are resolved:
- **ABI-correct `va_list`**: `va_list` is now a 5-pointer struct `{gp, gp_end, fp, fp_end, ovf}` across `<stdarg.h>` (`core/preprocessor.h`), prologue register-save area (`x86_64.c`, `x86_64_asm.c`, `aarch64.c`, `riscv.c`), and `NODE_VA_START`/`NODE_VA_ARG` lowering (`ast_lowering_expr.c`), supporting variadic `va_list` passing across multiple function calls (`printf` → `rt_stream_printf` → `vsnprintf_core`). Variadic functions keep their frame pointer (`omit_frame_pointer = false` in `ast_lowering.c`).
- **Format loop & Ryū `%e`/`%g` exponents (`zero_rt/core/stdio.c`)**: Removed the duplicate `fmt++` at the end of `vsnprintf_core` that skipped the character after every conversion specifier; fixed `ryu_parse` and `fmt_g`/`%e` precision passed to `d2exp_buffered`.
- **Unsigned integer division, modulo, and comparisons (`OP_DIV`/`OP_MOD`/`OP_COND_SET`)**: C99 usual arithmetic conversions (`zero_cc_binop_is_unsigned` in `ast_lowering_expr.c`) mark unsigned `/` and `%` (`ir_mark_last_unsigned(1)` → `xor %edx,%edx; divq` on x86-64, `udiv` on AArch64, `divu`/`remu` on RISC-V) and emit unsigned condition codes (`6..9` → `setb`/`setbe`/`seta`/`setae` on x86-64, `lo`/`ls`/`hi`/`hs` on AArch64, `sltu` on RISC-V) for `<`, `<=`, `>`, `>=`. This fixes `%llx`/`%llu`/`%llo` for values $\ge 2^{63}$ in `u64_chars` and `%f`/`%e`/`%g` for doubles $\ge 10^{15}$ in vendored Ryū (`d2fixed.c`, `d2s.c`).

## Related Concepts

- [ZeroCC Own Linker (T2)](./own-linker.md) — the linker zero_rt is linked with.
- [Multi-Translation-Unit Driver (T1)](./multi-tu.md)
- [Frontend, Type System & Semantic Analysis](./frontend-and-types.md) — string-literal array initializers (needed by runtime code).
- [Build, Test & Verification Guide](./build-and-verification.md)
