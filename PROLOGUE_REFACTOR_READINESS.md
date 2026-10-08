# Prologue/Epilogue Refactor - Readiness Checklist

## Status: ✅ READY TO REFACTOR

All tools are fixed and working. We can now safely refactor the prologue/epilogue.

---

## Tools Status

| Tool | Status | Tests |
|------|--------|-------|
| `prologue_validator.py` | ✅ Working | Validates assembly |
| `frame_layout_analyzer.py` | ✅ Working | Analyzes source code |
| `stack_alignment_checker.py` | ✅ Fixed | Validates alignment |
| `prologue_regression_suite.py` | ✅ Working | 11/11 tests passing |

---

## Test Results

### Existing Tests
```
$ make test
Results: 118/118 passed, 0 failed
```

### Regression Tests
```
$ python3 tools/prologue_regression_suite.py --run-all
✅ PASS no_stack_frame
✅ PASS small_frame
✅ PASS medium_frame
✅ PASS large_frame
✅ PASS nested_calls
✅ PASS many_args
✅ PASS int_return
✅ PASS void_return
✅ PASS empty_function
✅ PASS recursive_call
✅ PASS loop_with_calls

Test Summary: 11/11 passed
```

### Stack Alignment
```
$ python3 tools/stack_alignment_checker.py --asm-file /tmp/vla_standalone.s
Functions analyzed: 2
Call sites analyzed: 1
Issues found: 0
```

---

## What We're Refactoring

### Current Design (Fragile)
```
1. Emit OP_PROLOGUE IR instruction with placeholder frame size (0)
2. Run register allocation
3. Calculate actual frame size
4. PATCH the IR instruction with correct frame size
5. Emit assembly/machine code
```

**Problems**:
- Two-phase synchronization (placeholder → patch)
- Risk of forgetting to patch
- Hard to maintain
- Bug-prone (like the 8-byte vs 16-byte alignment issue)

### New Design (Clean)
```
1. Run register allocation
2. Calculate actual frame size
3. Emit prologue directly (no IR instruction, no patching)
4. Emit rest of function
```

**Benefits**:
- Single phase (no patching)
- Like epilogue (which already works this way)
- Easier to maintain
- Less bug-prone

---

## Refactor Plan

### Step 1: Remove OP_PROLOGUE from IR Emission
**File**: `src/zero_cc/parser/ast_lowering.c`

Remove code that emits `OP_PROLOGUE` instruction:
```c
// Remove this:
ir_emit_op(OP_PROLOGUE);
ir_emit_operand(0);  // Placeholder frame size
```

### Step 2: Calculate Frame Size After Register Allocation
**File**: `src/zero_cc/parser/ast_lowering.c`

Move frame size calculation to after register allocation:
```c
// After register allocation, calculate frame size:
int32_t frame_size = calculate_frame_size(c, spill_base_offset);
```

### Step 3: Emit Prologue Directly
**File**: `src/zero_cc/parser/asm_emitter.c` (for assembly)
**File**: `src/zero_cc/parser/ast_lowering_mc.c` (for machine code)

Emit prologue instructions directly:
```c
// Assembly emitter:
fprintf(out, "\tpush %%rbp\n");
fprintf(out, "\tmov %%rsp, %%rbp\n");
fprintf(out, "\tsub $%d, %%rsp\n", frame_size);

// Machine code emitter:
x86_emit_push(c, RBP);
x86_emit_mov(c, RSP, RBP);
x86_emit_sub(c, frame_size, RSP);
```

### Step 4: Remove OP_PROLOGUE Handling
**File**: `src/zero_cc/parser/asm_emitter.c`
**File**: `src/zero_cc/parser/ast_lowering_mc.c`

Remove cases that handle `OP_PROLOGUE` instruction:
```c
// Remove this:
case OP_PROLOGUE:
    // Old prologue emission code
    break;
```

---

## Validation Steps

### Before Refactor
```bash
# 1. Run baseline tests
make test
python3 tools/prologue_regression_suite.py --run-all

# 2. Generate reference assembly
./build/bin/zerocc -S -o /tmp/before.s /tmp/vla_standalone.c
```

### After Refactor
```bash
# 1. Run all tests
make test
python3 tools/prologue_regression_suite.py --run-all

# 2. Validate assembly
python3 tools/stack_alignment_checker.py --asm-file /tmp/before.s

# 3. Compare output
./build/bin/zerocc -S -o /tmp/after.s /tmp/vla_standalone.c
diff /tmp/before.s /tmp/after.s  # Should be identical (except prologue)
```

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Break existing tests | Low | 118/118 tests passing, regression suite |
| Frame size calculation error | Low | Same calculation, just moved |
| Assembly output mismatch | Low | Direct comparison with diff |
| Stack alignment issues | Low | Stack alignment checker |

**Overall Risk**: **LOW** - We have comprehensive testing in place

---

## Time Estimate

| Task | Effort |
|------|--------|
| Remove OP_PROLOGUE from IR emission | 30 min |
| Calculate frame size after register allocation | 30 min |
| Emit prologue directly (assembly) | 1 hour |
| Emit prologue directly (machine code) | 1 hour |
| Remove OP_PROLOGUE handling | 30 min |
| Testing and validation | 2 hours |
| **Total** | **~5-6 hours** |

---

## Files to Modify

1. `src/zero_cc/parser/ast_lowering.c` - Remove OP_PROLOGUE emission
2. `src/zero_cc/parser/asm_emitter.c` - Direct prologue emission
3. `src/zero_cc/parser/ast_lowering_mc.c` - Direct prologue emission
4. `src/zero_cc/parser/ir.h` - Optional: remove OP_PROLOGUE opcode

---

## Next Steps

1. ✅ Tools ready
2. ✅ Tests passing
3. ✅ Validation in place
4. ⏸️ **Ready to refactor**

**Recommendation**: Proceed with refactor now. All safety checks are in place.

---

## References

- [Prologue/Epilogue Analysis](docs/PROLOGUE_EPILOGUE_ANALYSIS.md)
- [Improvement Plan](docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md)
- [Debugging Tools](docs/PROLOGUE_DEBUGGING_TOOLS.md)
