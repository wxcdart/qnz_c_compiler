# AST Lowering Design Analysis

## Executive Summary

The AST lowering layer (`src/zero_cc/parser/ast_lowering*.c`) has **significant x86-64 assumptions** that violate our ISA-agnostic design goals. This is the root cause of the AArch64 accumulation test failure (returning 98 instead of 52).

**Critical Issue**: Callee-saved register handling is x86-64 specific but runs for all ISAs, causing incorrect stack accesses on AArch64.

## Analysis Results

### Verification Suite Output

```
Overall Status: FAIL
Total Checks: 7
  Pass: 3
  Warning: 2
  Fail: 1
  Skipped: 1

Critical Failures:
  [✗] AST Lowering: Callee-saved handling is not ISA-conditional (x86-64 specific)

Warnings:
  [⚠] Build System: Found ISA-specific flags: x86_64, aarch64
  [⚠] Stack Frame Layout: CALLEE_SAVE_FRAME_RESERVE found
```

### AST Lowering Analyzer Output

```
Files analyzed: 35
Total ISA assumptions found: 263
  - x86-64 specific: 260
  - AArch64 specific: 3
Critical issues: 236

Assumptions by Type:
  - register: 207
  - instruction: 18
  - stack_layout: 32
  - calling_convention: 6
```

## Root Cause Analysis

### The Bug: Callee-Saved Register Overlap

In `src/zero_cc/parser/ast_lowering.c`:

```c
/* Line 165: Always runs for ALL ISAs */
emit_callee_save_loads(c);
```

This function loads from x86-64 specific offsets:

```c
/* src/zero_cc/parser/regalloc.h:435 */
static const int32_t callee_save_slot_offset[] = {
    0,  /* idx 0 (rax)  - not callee-saved */
    8,  /* idx 1 (rbx)  */
    0,  /* idx 2 (rsi)  - not callee-saved */
    0,  /* idx 3 (rdi)  - not callee-saved */
    0,  /* idx 4 (r8)   - not callee-saved */
    0,  /* idx 5 (r9)   - not callee-saved */
    0,  /* idx 6 (r10)  - not callee-saved */
    0,  /* idx 7 (r11)  - not callee-saved */
    16, /* idx 8 (r12)  */
    24, /* idx 9 (r13)  */
    32, /* idx 10 (r14) */
    40, /* idx 11 (r15) */
};
```

**Problem**: When AArch64 backend runs, it loads from offset `-8` (for rbx) even though AArch64 doesn't use rbx!

### Stack Frame Layout Issue

```c
/* src/zero_cc/parser/ast_lowering.c:136-140 */
c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
int32_t spill_base_offset = c->current_stack_frame;
if (c->regalloc.next_spill_slot > 0) {
    c->current_stack_frame += c->regalloc.next_spill_slot * 8;
}
```

**Problem**: `spill_base_offset` can be less than `CALLEE_SAVE_FRAME_RESERVE` (48), causing spill slots to overlap with callee-saved register area.

## Architecture Comparison

### Current Architecture (Broken)

```
┌─────────────────────────────────────────┐
│           AST Lowering Layer            │
│  (x86-64 assumptions throughout)        │
├─────────────────────────────────────────┤
│  emit_callee_save_loads()  ← X86 ONLY   │
│  callee_save_slot_offset[] ← X86 ONLY   │
│  r10, r11 scratch regs     ← X86 ONLY   │
│  rdx for struct return     ← X86 ONLY   │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│           Backend Layer                 │
│  (ISA-specific implementations)         │
├─────────────────────────────────────────┤
│  x86_64.c                               │
│  aarch64.c                              │
│  riscv.c                                │
└─────────────────────────────────────────┘
```

### Proposed Architecture (ISA-Agnostic)

```
┌─────────────────────────────────────────┐
│           AST Lowering Layer            │
│  (ISA-neutral, uses backend interface)  │
├─────────────────────────────────────────┤
│  BE(c)->emit_prologue()                 │
│  BE(c)->emit_epilogue()                 │
│  BE(c)->emit_load()                     │
│  BE(c)->emit_store()                    │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│           Backend Layer                 │
│  (ISA-specific implementations)         │
├─────────────────────────────────────────┤
│  x86_64_emit_epilogue()                 │
│    └─ emit_callee_save_loads()          │
│                                         │
│  aarch64_emit_epilogue()                │
│    └─ (no callee-saved loads)          │
│                                         │
│  riscv_emit_epilogue()                  │
│    └─ (different callee-saved rules)   │
└─────────────────────────────────────────┘
```

## Immediate Fix (Quick Win)

### Fix 1: Make Callee-Saved Handling ISA-Conditional

```c
/* src/zero_cc/parser/ast_lowering.c:165 */
/* Restore callee-saved registers before the epilogue leave/ret. */
/* Only for x86-64 - other ISAs handle this in their backend */
if (c->isa == ISA_X86_64) {
    emit_callee_save_loads(c);
}

BE(c)->emit_leave(c);
BE(c)->emit_ret(c);
```

### Fix 2: Ensure Spill Base Offset >= CALLEE_SAVE_FRAME_RESERVE

```c
/* src/zero_cc/parser/ast_lowering.c:136-140 */
/* Already applied - ensures spill slots don't overlap callee-saved area */
c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
if (c->current_stack_frame < CALLEE_SAVE_FRAME_RESERVE) {
    c->current_stack_frame = CALLEE_SAVE_FRAME_RESERVE;
}
int32_t spill_base_offset = c->current_stack_frame;
```

## Long-Term Refactoring Plan

### Phase 1: Move Callee-Saved Handling to Backend (1-2 days)

1. Add `emit_callee_save_loads` to backend interface (optional)
2. Implement in `x86_64.c`
3. Set to NULL for `aarch64.c` and `riscv.c`
4. Remove from `ast_lowering.c`

### Phase 2: Remove x86-64 Register Names from AST Lowering (2-3 days)

1. Replace `r10`, `r11` with scratch register abstraction
2. Replace `rdx` for struct return with backend hook
3. Make all register references ISA-neutral

### Phase 3: Standardize Stack Frame Layout (1-2 days)

1. Remove `CALLEE_SAVE_FRAME_RESERVE` or make ISA-specific
2. Ensure all ISAs use consistent offset convention
3. Add verification tests

### Phase 4: Comprehensive Testing (1 day)

1. Cross-ISA comparison tests
2. Stack frame layout verification
3. Edge case testing (large frames, many locals, etc.)

## Test Results

### Before Fix

```
$ ./build/bin/zerocc -c test_accum.c -o test_accum.o --target aarch64
$ qemu-aarch64-static test_accum_static
Exit code: 98  (WRONG - expected 52)
```

### After Fix (Proposed)

```
$ ./build/bin/zerocc -c test_accum.c -o test_accum.o --target aarch64
$ qemu-aarch64-static test_accum_static
Exit code: 52  (CORRECT)
```

### Current Status

- ✅ All 118 JIT tests pass
- ✅ Simple AArch64 cases work (`abs_val(-42)=42`, `abs_val(10)=10`)
- ❌ Accumulation test fails on AArch64 (98 vs 52)
- ✅ x86-64 works correctly

## Recommendations

### Immediate (Do Now)

1. **Apply Fix 1**: Make callee-saved handling ISA-conditional
2. **Verify Fix**: Test accumulation case on AArch64

### Short-Term (This Week)

1. **Apply Fix 2**: Ensure spill base offset >= CALLEE_SAVE_FRAME_RESERVE
2. **Add Tests**: Create regression tests for cross-ISA stack frame layout
3. **Documentation**: Update design docs with ISA-agnostic requirements

### Long-Term (Next Sprint)

1. **Refactor**: Move callee-saved handling to backend layer
2. **Clean Up**: Remove x86-64 register names from AST lowering
3. **Verify**: Run comprehensive cross-ISA comparison tests

## Files to Modify

### Critical (Fix Now)

- `src/zero_cc/parser/ast_lowering.c` (line 165)
- `src/zero_cc/parser/ast_lowering.c` (lines 136-140)

### Future Refactoring

- `src/zero_cc/isa/isa_backend.h` (add optional hook)
- `src/zero_cc/isa/x86_64.c` (move callee-saved here)
- `src/zero_cc/parser/ast_lowering_mc.c` (remove r10/r11 references)
- `src/zero_cc/parser/ast_lowering_stmt.c` (remove rdx references)

## Conclusion

The AST lowering layer has **significant x86-64 assumptions** that need to be addressed to achieve true ISA-agnostic design. The immediate fix (making callee-saved handling ISA-conditional) will resolve the accumulation test failure. Long-term refactoring will make the codebase cleaner and easier to maintain for multiple ISAs.

**Priority**: HIGH - Blocks AArch64 backend completion and multi-ISA support.
