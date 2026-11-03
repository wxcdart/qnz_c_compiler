---
name: zerocc-debugging
description: >-
  Triage ZeroCC miscompiles, JIT/AOT crashes, and link errors. Use when generated
  code gives wrong results, segfaults, or fails to link; when a test passes in one
  mode (-O level, JIT vs AOT, ISA) and fails in another; or when inspecting zerocc
  internals (IR, asm, relocations) to localize a codegen bug.
---

# ZeroCC Debugging Triage

## 1. Bisect the failure space first

ZeroCC has **independent code paths per (mode × optimization × ISA)** — a bug
usually lives in exactly one cell. Narrow the cell before reading code:

| Axis | How to vary it | Why it matters |
|---|---|---|
| Mode | `--run` (JIT) vs `-o bin` (AOT) | JIT raw-byte emitter and AOT asm emitter are **separate code paths** (see §3) |
| Opt level | `-O0 -O1 -O2 -O3` | `-O1+` enables mem2reg, copy-prop; `-O2` adds CSE/GVN/LICM; `-O3` strength reduction. `-O0`-correct/`-O1`-wrong ⇒ optimizer bug, not lowering |
| ISA | `--target=...` / `make test-isa-qemu` | Each ISA has its own emitters |

```bash
./build/bin/zerocc -O0 test.c -o /tmp/t0 && /tmp/t0     # baseline
for o in 1 2 3; do ./build/bin/zerocc -O$o test.c -o /tmp/to && /tmp/to; done
./build/bin/zerocc --run test.c                          # JIT
```

A/B against the host compiler is the ground truth for behavior:

```bash
gcc -std=c11 -O0 test.c -o /tmp/ref && /tmp/ref
```

## 2. Inspection tools

```bash
./build/bin/zerocc -dump-ir test.c            # IR before optimization (lowering bugs)
./build/bin/zerocc -S test.c -o /tmp/t.s      # AOT assembly (codegen inspection)
./build/bin/zerocc -c test.c -o /tmp/t.o      # object: readelf -r /tmp/t.o (relocs)
objdump -d /tmp/t.o                           # disassembly of AOT output
readelf -sW /tmp/t.o                          # symbol table (missing externs?)
```

- **AOT link/crash**: `objdump -d` the *executable*. A `call 0 <link_error>`
  means the callee was never registered in the TU symbol table — see the
  `runtime-function-integration` skill.
- **JIT crash** (`--run` segfaults): the crash is *inside the zerocc process*.
  Run under gdb:
  ```bash
  gdb -batch -ex 'set args --run test.c' -ex run -ex bt -ex 'x/3i $pc' \
      ./build/bin/zerocc
  ```
  A bogus `call` target with a corrupted frame usually means a far external
  call (>2GB) or an unpatched reloc — check the `is_abs_call` / reloc paths
  (`src/zero_cc/jit/execution.h`).
- **Wrong value, no crash**: dump IR at `-O0` (is the lowering right?), then
  the `-S` asm (is the emitter right?), then diff against `gcc -S -O0` output
  for the suspicious statement.

## 3. Known codegen traps (verify these before suspecting new code)

- **Two x86-64 emitters**: any new IR op must be lowered in *both* the JIT
  raw-byte emitter and the AOT asm emitter (plus each other ISA). A missing
  arm in one emitter is the classic "works in JIT, garbage in AOT".
- **`-O1+` fragility**: branchy compute/clamp/restore patterns can trip a
  latent LRA / redundant-move-elimination defect. If `-O0` works and `-O1`
  doesn't, reproduce with branch-free algebra to isolate; report the pattern,
  don't just "fix" the test.
- **IR operand rules**: `ir_emit_cmp` and `OP_XOR` take **temp IDs**, never
  raw immediates; `ir_emit_cond_set` consumes the flags of the *immediately
  preceding* `OP_CMP`; `ir_emit_je` tests zero — use `jne` when the guard
  should skip only if non-zero.
- **16-bit temp budget**: >65534 temps/function is a clean `REJECT`
  ("split the function"), not a bug.
- **Narrow sub-word extraction**: prefer spill + byte copy over shift temps.
- **Overflow checks**: do *not* set `Instr.width` on the `add`/`sub`/`mul`
  being checked — the width-extension pass clobbers the raw wide result the
  check must observe.
- **SysV ABI**: 9–16B struct values ride `VREG_0`/`VREG_2` (`rdx`/`x1`/`a1`);
  >16B struct args/returns use the hidden-pointer (`sret`) convention and must
  copy the full `size % 8` tail.
- **Static locals**: value accesses may use the stack mirror, but
  `&static_local` must lower to the data-section `__sl_N` global, not the
  mirror slot.
- **Inliner**: bodies with labels/goto/`&&label` are never inlined — if a
  label test misbehaves under inlining pressure, the guard is the first
  suspect, not the label mechanism.

## 4. Hygiene

- Temporary `ZC_DBG*` instrumentation must be removed before verification or
  committing: `grep -rn "ZC_DBG" src/zero_cc/ | wc -l` must be `0`.
- Never "fix" a test's expected output to make a miscompile disappear; fix
  the compiler or classify the test (see the `c-torture-gate` skill).
- Any fix must be re-verified in **both** modes and **all four** `-O` levels
  (`make test-opt` sweeps exactly this), plus the full battery before
  committing.
