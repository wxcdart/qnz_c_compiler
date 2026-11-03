---
name: runtime-function-integration
description: >-
  Integrate a host-compiled runtime function that ZeroCC-generated code calls at
  runtime (JIT dlsym path + AOT PLT path), and diagnose external-call failures:
  JIT crash on a far call target, AOT call jumping to 0/<link_error>. Use when
  adding a new host runtime helper callable from generated code, or when a call
  from generated code to an external symbol misbehaves in JIT or AOT mode.
---

# Runtime Function Integration (JIT + AOT)

Generated code calls host-compiled runtime helpers through **two independent
resolution paths — both must be wired and verified**:

- **JIT (`--run`)**: the code image is `mmap(NULL, …)`'d in-process; external
  names resolve via `dlsym(RTLD_DEFAULT, …)` (`zero_cc_resolve_symbol`).
- **AOT (`-o`)**: the `.o` gets `R_X86_64_PLT32` relocs; the host linker
  resolves them when the executable is linked.

> **Prefer inline IR ops over a host runtime when possible.** setjmp/longjmp
> started life as a host runtime (`zc_setjmp`/`zc_longjmp` in
> `src/zero_cc/jit/jmp_runtime.c`) and was later replaced by inline
> `OP_SETJMP`/`OP_LONGJMP` IR ops emitted by every ISA backend in both
> emitters (commit `adae9c4`; see `docs/okf/ir-and-regalloc.md` and
> `docs/okf/isa-backends.md`). A host runtime is the right tool when the
> operation is genuinely hard to emit inline (allocator, OS services); a
> 5-instruction state save/restore is not. Use this skill for the remaining
> host-runtime cases.

## Step 0 — Prove the runtime standalone first

Compile the runtime file with the **host** GCC and test it with a plain C
driver before touching zerocc:

```bash
gcc -O0 -fno-pie src/zero_cc/jit/<rt>.c /tmp/driver.c -o /tmp/drt && /tmp/drt
```

`-O0` matters when the runtime relies on a fixed frame-pointer layout
(`[rbp]` = saved RBP, `[rbp+8]` = return address, `[rbp+16]` = caller RSP).
If the function saves/restores state, **read all saved values into registers
before swapping RSP/RBP**, restore RSP/RBP before touching memory, and set
`RAX` before jumping to the saved return address.

## Step 1 — JIT path

1. **Build**: add the object to `Makefile` (link it into `build/bin/zerocc`).
2. **Export for dlsym**: the JIT resolves by symbol name, so the host linker
   must export it:
   ```make
   -Wl,--export-dynamic-symbol=<sym1> -Wl,--export-dynamic-symbol=<sym2>
   ```
   Verify: `nm -D build/bin/zerocc | grep <sym>`.
3. **Far-target calls**: the mmap'd image sits near libc (`0x7f…`) while the
   main executable sits far away (`0x5e…`) — often **>2GB**, beyond what a
   relative `call rel32` can encode. x86 `x86_emit_call`
   (`src/zero_cc/isa/x86_64.c`) handles this: external calls in JIT mode emit
   `mov r11, imm64; call *r11` and record an `is_abs_call` reloc (8-byte
   absolute patch, `src/zero_cc/jit/execution.h`). Do **not** route new
   external JIT calls back to relative `E8`.
   Symptom of the far-call bug: crash *inside* a `call` with a bogus target;
   gdb shows `#0 0x… in ??` with a corrupted frame.
4. If the generated code's behavior depends on the call being `noreturn` or
   on automatics surviving a non-local resume, encode that in the IR lowering
   (emit an infinite loop after a noreturn call; set `c->lowering` flags to
   force a frame pointer and disable mem2reg so automatics stay in stack
   slots). Remember `c->lowering` is reset per function in
   `ast_lowering.c`, so per-function flags do not leak.

## Step 2 — AOT path

1. **Link the runtime into the executable**: the AOT link line in `src/main.c`
   must include the runtime object for the target ISA.
2. **Register the function so the `.o` emits a PLT reloc**: the `.o` writer
   (`src/main.c`, symbol/reloc loops) only emits relocs for symbols present in
   the TU symbol table, built from `c->functions`. A call target that appears
   only as a reloc string resolves to symbol index 0 — the disassembly shows
   `call 0 <link_error>` and the executable jumps to NULL.
   Fix: in the lowering that emits the call, register the runtime function the
   same way implicit-declaration registration does
   (`src/zero_cc/ir/ast_lowering_call.c` pattern):
   ```c
   if (c->function_count < ZERO_CC_MAX_FUNCTIONS && !zero_cc_find_function(c, name)) {
       ZeroCCFunction *fn = &c->functions[c->function_count++];
       memset(fn, 0, sizeof(*fn));
       snprintf(fn->name, sizeof(fn->name), "%s", name);
       fn->text_offset = 0;
       fn->is_prototype = true;
   }
   ```
   Verify: `objdump -d <aot-exe> | grep -E "call.*<rt>|call.*link_error"`
   must show `call 0x… <sym>`, never `call 0 <link_error>`.

## Step 3 — Dual verification

For every new runtime call site, test **both** paths with the same inputs:

```bash
./build/bin/zerocc --run test.c                 # JIT
./build/bin/zerocc test.c -o /tmp/t && /tmp/t   # AOT
```

Then run the affected c-torture category tests in both modes, run the full
battery (see AGENTS.md §2), confirm `grep -rn "ZC_DBG" src/zero_cc/` is `0`
(never commit debug instrumentation), and only then unskip/ratchet per the
`c-torture-gate` skill. New IR ops (if any) need **both** x86-64 emitters:
the JIT raw-byte emitter and the AOT asm emitter, plus the other ISAs.

## Debugging Checklist

| Symptom | Likely cause |
|---|---|
| JIT: crash in `call` to bogus `0x7f…`/`0x5e…` address | relative call to a >2GB-far target → use the `is_abs_call` absolute-call path |
| JIT: `dlsym` returns NULL / resolve fails | missing `--export-dynamic-symbol` in the Makefile link line |
| AOT: `call 0 <link_error>` in objdump | function not registered in `c->functions` → no symbol in `.o` → reloc resolved to index 0 |
| AOT: linker "undefined reference" | runtime `.o` not on the AOT link line in `src/main.c` |
| Runtime corrupts the stack on resume | RSP/RBP modified before all saved values were read, or frame layout assumed without `-O0`/frame pointer |
| Works standalone but not through the compiler | the compiler's frame layout differs (no frame pointer) — force one via `c->lowering` flags |
