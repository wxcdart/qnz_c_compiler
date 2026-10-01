# IR-Based Codegen Integration Status

## Current State

The IR-based codegen pipeline is in place but has integration issues that prevent full functionality.

### Working Features
- IR infrastructure (ir.h, ir_emit_ops.h, live_range.h, regalloc.h)
- IR emission from AST lowering (lower_expr, lower_stmt)
- Basic IR-to-machine code conversion (emit_ir_to_machine_code)
- Register allocator infrastructure (linear-scan)
- Frame pointer handling (0xFF marker for FP register)

### Known Issues

1. **DIV Instruction Clobber Handling**: The x86 DIV instruction clobbers rax and rdx, but the register allocator doesn't account for this. Temps assigned to rax/rdx that are live during DIV are incorrectly clobbered.

2. **Register Allocation**: The register allocator is assigning registers incorrectly for some cases, particularly when the frame pointer is involved.

3. **Instruction Ordering**: The current implementation mixes IR emission (deferred) with direct machine code emission (immediate), which can cause ordering issues.

## Fixes Needed

1. **Add Clobber Handling to Live Range Analysis**: The DIV/MOD instructions should be marked as clobbering rax and rdx, and the live range analysis should account for this.

2. **Fix Register Allocator**: The register allocator should avoid assigning rax and rdx to temps that are live during DIV instructions.

3. **Complete IR-Based Codegen**: Convert all direct machine code emission to IR-based emission to ensure consistent instruction ordering.

## Test Status

- `abi_sum_jit`: PASS
- `align`: PASS
- `ast_lowering`: PASS
- `string_len_jit`: PASS (array indexing with NODE_INDEX fixed)
- `string_literal_jit`: PASS (comparison operators fixed)
- All tests: PASS

## Recent Fixes

### NODE_INDEX Lowering (Array Indexing)
- Implemented proper array indexing `base[index]` using `OP_LOAD_BYTE` for char element access
- The x86-64 backend emits `movzx rax, byte [rax]` for zero-extended byte loads

### Comparison Operators Zero-Extension
- Fixed `sete`, `setne`, `setl`, `setle`, `setg`, `setge` instructions to zero-extend the result
- Added `movzx rax, al` after each setcc instruction to ensure the full 64-bit register contains 0 or 1

### Live Range Analysis for Conditional Jumps
- Updated `live_range_scan_ir` to properly record temp uses for conditional jumps (OP_JE, OP_JNE, etc.)
- This ensures condition temporaries remain live through branch evaluation

## Next Steps

1. Implement clobber handling in live range analysis
2. Fix register allocator to respect clobbered registers
3. Complete the IR-based codegen conversion
4. Add more comprehensive tests for arithmetic operations
