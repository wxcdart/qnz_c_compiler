# Debug Summary - AArch64 Accumulation Test Failure

## Current Status

**Issue**: AArch64 `test_accum.c` returns exit code 94 instead of expected 52 (42 + 10)

**Root Cause Identified**: `emit_callee_save_loads()` in `ast_lowering.c` was running for all ISAs but is x86-64 specific

**Fix Applied**: Made callee-saved handling ISA-conditional
```c
if (c->isa == ISA_X86_64) {
    emit_callee_save_loads(c);
}
```

**Current State**: Test still fails (94 instead of 52), but the `-8` offset load is gone from disassembly

## What I've Done

### 1. Fixed Callee-Saved Register Handling (COMPLETE)
- **File**: `src/zero_cc/parser/ast_lowering.c` (line 165-170)
- **Change**: Made `emit_callee_save_loads()` ISA-conditional
- **Result**: Removed incorrect `-8` offset load from AArch64 epilogue

### 2. Added IR Dump Functionality (PARTIALLY COMPLETE)
- **Files Modified**:
  - `src/zero_cc/types/symbols.h` - Added `dump_ir` flag
  - `src/zero_cc/parser/ir.c` - Added `ir_dump()` function
  - `src/zero_cc/parser/ir.h` - Added `ir_dump()` declaration
  - `src/zero_cc/parser/ast_lowering.c` - Added IR dump call
  - `src/main.c` - Set flag before compilation

- **Status**: IR dump flag not working (no output)
- **Issue**: Flag is set but `ir_dump()` not being called
- **Next Step**: Debug why flag isn't working

### 3. Created Analysis Tools (COMPLETE)
Created 5 Python analysis tools:
1. `tools/stack_frame_layout_analyzer.py` - Stack frame analysis
2. `tools/backend_interface_analyzer.py` - Backend interface verification
3. `tools/design_verification_suite.py` - Comprehensive design verification
4. `tools/ir_analyzer.py` - IR analysis with CFG, flag dependencies
5. `tools/flag_flow_analyzer.py` - Deep flag dependency analysis

### 4. Created Documentation (COMPLETE)
- `docs/AST_LOWERING_DESIGN_ANALYSIS.md` - Comprehensive analysis
- `docs/DEBUG_SUMMARY.md` - This file

## Disassembly Analysis

### Current AArch64 Output (test_accum)

```asm
<abs_val>:
   0:  sub sp, sp, #0x40
   4:  stp x29, x30, [sp]
   8:  mov x29, sp
   c:  stur x0, [x29, #-56]
  10:  ldur x0, [x29, #-56]
  14:  mov x3, #0x0
  18:  cmp x0, x3
  1c:  cset x0, lt
  20:  tst x0, x0
  24:  b.eq 38
  28:  ldur x0, [x29, #-56]
  2c:  neg x0, x0
  30:  b 44
  34:  b 38
  38:  ldur x3, [x29, #-56]  <- SAME OFFSET as parameter storage!
  3c:  mov x0, x3
  40:  b 44
  44:  ldp x29, x30, [sp]
  48:  add sp, sp, #0x40
  4c:  ret
```

**Key Observation**: The else branch at 0x38 loads from `[x29, #-56]`, which is the SAME offset where the parameter `x` was stored at 0xc. This looks correct.

**Problem**: The CSET at 0x1c might be returning the wrong value.

### CSET Encoding Check

```c
// From aarch64.c
static const uint32_t cset_encodings[] = {
    0x9A9FA7E0,  /* CC_LT: cset Xd, lt */
    0x9A9FC7E0,  /* CC_LE: cset Xd, le */
    ...
};
```

Encoding `0x9A9FA7E0 | dst`:
- `0x9A9FA7E0` = `cset x0, lt`

This should set x0 = 1 if x0 < x3 (after CMP), else x0 = 0.

**Question**: Is the condition code mapping correct?

## Potential Issues to Investigate

### 1. Condition Code Mapping
IR uses `CC_LT` (0) for "less than", but AArch64 CSET uses different condition codes:

| IR Condition | AArch64 CSET | AArch64 B.COND |
|-------------|--------------|----------------|
| CC_LT (0)   | lt           | lt             |
| CC_LE (1)   | le           | le             |
| CC_GT (2)   | gt           | gt             |
| CC_GE (3)   | ge           | ge             |
| CC_EQ (4)   | eq           | eq             |
| CC_NE (5)   | ne           | ne             |

**Check**: Are these mappings correct?

### 2. CMP Instruction
```asm
18:  cmp x0, x3
```

This sets flags based on `x0 - x3`. If x0 = -42 and x3 = 0:
- Result: -42 (negative)
- Flags: N=1, Z=0, C=1, V=0
- Condition "lt" (signed less than): N != V → 1 != 0 → TRUE

So CSET should set x0 = 1. This looks correct.

### 3. Test Instruction
```asm
20:  tst x0, x0
24:  b.eq 38
```

This tests if x0 = 0. If x0 = 1 (from CSET), then:
- Result: 1 (non-zero)
- Flags: Z=0
- Condition "eq" (equal to zero): Z = 0 → FALSE

So branch should NOT be taken, and we should go to the "then" branch at 0x28. This looks correct.

### 4. Accumulation Logic
```asm
<main>:
  70:  ldur x0, [x29, #-64]  ; load -42
  74:  ...
  7c:  bl abs_val            ; call abs_val(-42)
  80:  ldur x3, [x29, #-56]  ; load accumulator (0)
  84:  add x0, x3, x0        ; accumulator + result
  88:  stur x0, [x29, #-56]  ; store back
  ...
  a4:  ldur x3, [x29, #-56]  ; load accumulator (42)
  a8:  add x0, x3, x0        ; accumulator + abs_val(10)
  ac:  stur x0, [x29, #-56]  ; store back (52)
  b0:  ldur x0, [x29, #-56]  ; load result (52)
  b4:  b b8                  ; return
```

**Expected**: x0 = 52 at return
**Actual**: Exit code 94

**Question**: Where does 94 come from?

94 in binary: 0x5E = 0101 1110
52 in binary: 0x34 = 0011 0100

These are quite different. Something is definitely wrong.

## Next Steps

### Immediate (Debug IR Dump)
1. Fix IR dump to work
2. Dump IR for test_accum
3. Analyze IR for bugs

### Alternative Debug Approach
If IR dump doesn't work, use QEMU debugging:
```bash
QEMU_STRACE=1 qemu-aarch64-static /tmp/test_accum_static
```

Or use GDB:
```bash
gdb -q /tmp/test_accum_static
(gdb) target remote | qemu-aarch64-static -g 1234 /tmp/test_accum_static
```

### Hypothesis: CSET Condition Code
The most likely issue is the CSET condition code mapping. Let me verify:

```c
// From aarch64.c
static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    static const uint32_t cset_encodings[] = {
        0x9A9FA7E0,  /* CC_LT */
        0x9A9FC7E0,  /* CC_LE */
        ...
    };
    arm64_emit_u32(c, cset_encodings[cond] | (dst & 0x1F));
}
```

**Check**: What is the value of `cond` passed to this function?

## Files Modified

### Critical Fixes
- `src/zero_cc/parser/ast_lowering.c` - Made callee-saved handling ISA-conditional

### Debug Tools (Not Yet Working)
- `src/zero_cc/types/symbols.h` - Added dump_ir flag
- `src/zero_cc/parser/ir.c` - Added ir_dump()
- `src/zero_cc/parser/ir.h` - Added ir_dump() declaration
- `src/zero_cc/parser/ast_lowering.c` - Added IR dump call
- `src/main.c` - Set dump_ir flag

### Analysis Tools
- `tools/stack_frame_layout_analyzer.py`
- `tools/backend_interface_analyzer.py`
- `tools/design_verification_suite.py`
- `tools/ir_analyzer.py`
- `tools/flag_flow_analyzer.py`

### Documentation
- `docs/AST_LOWERING_DESIGN_ANALYSIS.md`
- `docs/DEBUG_SUMMARY.md`

## Test Status

- ✅ All 118 JIT tests pass
- ✅ Simple AArch64 cases work (`abs_val(-42)=42`)
- ❌ Accumulation test fails on AArch64 (94 vs 52)
- ✅ x86-64 works correctly

## Priority

**HIGH** - Blocks AArch64 backend completion
