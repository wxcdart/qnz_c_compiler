# VLA Stack Allocation Bug Analysis

## Summary

The Variable Length Array (VLA) test is failing due to incorrect stack offset calculation for VLA variables.

## Test Case

```c
int run_vla(int n) {
    int arr[n];
    for (int i = 0; i < n; i++) {
        arr[i] = i * 2;
    }
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;  // Expected: 20, Actual: 114
}
```

## Root Cause

The VLA pointer's `sym->stack_offset` is being calculated incorrectly.

### Expected Behavior
- `c->current_stack_frame` should be initialized to `CALLEE_SAVE_FRAME_RESERVE` (48) before AST lowering
- VLA pointer should be at offset 56 (48 + 8 for pointer)
- Store/Load should use offset -56 from frame pointer

### Actual Behavior
- VLA pointer is at offset 32 (shown as `[sp-4294967232]` = -32 in IR)
- Store uses offset -32, but Load uses offset -64
- Mismatch causes VLA data to be written to wrong location

## IR Dump

```
   9: VLA_ALLOC r23, r22
  10: MOV_MEM [sp-4294967232], r23  ; Store at -32
  ...
  22: LOAD r31, [fp-64]             ; Load from -64 (mismatch!)
```

## Disassembly

```
  48: mov    %rax,-0x40(%rbp)       ; Store at -64
  88: mov    -0x40(%rbp),%rbx       ; Load from -64
```

The disassembly shows the store is at -64, but the IR shows -32!

## Investigation

The issue appears to be that:
1. `sym->stack_offset` is calculated during AST lowering
2. `c->current_stack_frame` is initialized to 48 before AST lowering
3. But the VLA pointer's `sym->stack_offset` is 32, not 56

This suggests either:
- `c->current_stack_frame` is not being initialized correctly
- `sym->stack_offset` is being calculated at the wrong time
- There's a bug in how offsets are stored/retrieved

## Status

**Pre-existing bug**, unrelated to callee-saved register fix.

Requires separate investigation of:
1. Stack frame initialization timing
2. Symbol offset calculation
3. IR offset storage/retrieval

## Recommendation

Defer VLA bug fix until after Phase D (IR-based callee-save). The VLA issue is a deeper stack frame layout problem that needs comprehensive analysis.
