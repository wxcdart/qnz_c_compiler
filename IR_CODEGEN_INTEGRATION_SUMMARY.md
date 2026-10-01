# IR-Based Codegen Integration Summary

## Work Completed

### 1. Register Allocator Integration ✅
- Added `regalloc_get_reg_for_temp()` function to retrieve physical register for a temp ID
- Added `regalloc_idx_to_vreg()` mapping function to convert register allocator indices to VREG numbers
- Register allocator now properly assigns registers to temps based on live ranges

### 2. IR Infrastructure ✅
- `ir.h`: IR instruction format with `imm64` field for 64-bit immediates
- `ir_emit_ops.h`: 30+ IR emission functions (mov, load, store, add, sub, mul, div, etc.)
- `live_range.h`: Live range analysis for temp variables
- `regalloc.h`: Linear-scan register allocator with spilling support

### 3. Pipeline Structure ✅
- `zero_cc_lower_ast()`: Full pipeline: AST → IR → Live Range → RegAlloc → Machine Code
- `emit_ir_to_machine_code()`: Converts IR to machine code with allocated registers

### 4. Partial IR-Based Lowering ✅
- Converted `lower_expr()` to emit IR for simple cases (NODE_INT, NODE_BOOL, NODE_NULLPTR)
- Binary operations (ADD, SUB, MUL) use IR emission
- Return statements ensure result is in rax

## Issues Encountered

### 1. Instruction Ordering Problem
When converting to IR-based emission, the order of machine code generation became incorrect:
- Return statement emits jump to epilogue BEFORE expression code
- This causes the function to return before executing the expression

**Root Cause**: Mixing IR emission (deferred) with direct machine code emission (immediate) in the return statement handler.

**Solution**: Either:
a) Convert all statement handlers to use IR emission (including jumps)
b) Keep direct machine code emission for control flow

### 2. Prologue/Epilogue Duplication
The prologue was being emitted twice:
- Once by original code (for function definition)
- Once by IR pipeline code

**Solution**: Removed duplicate prologue emission from `zero_cc_lower_ast()`.

## Current Status

### What Works ✅
- All 100+ tests pass with direct machine code approach
- Register allocator correctly assigns registers to temps
- IR emission works for simple expressions

### What Needs Work ⚠️
- Full IR-based lowering requires converting all ~255 direct machine code call sites
- Control flow (jumps, returns) needs IR-based emission or careful ordering
- Statement handlers need to be updated to work with IR-based expression lowering

## Recommended Approach

### Option A: Hybrid Approach (Recommended)
Keep direct machine code emission for now, but gradually convert simple expressions to IR-based emission:
1. Convert leaf expressions (NODE_INT, NODE_BOOL, NODE_NULLPTR) to IR
2. Test each conversion
3. Gradually expand to more complex expressions
4. Eventually convert control flow

**Pros**: Lower risk, incremental progress, tests pass throughout
**Cons**: Mixed codebase during transition

### Option B: Complete Rewrite
Convert all ~255 call sites to IR-based emission at once:
1. Convert all expression handlers to IR emission
2. Convert all statement handlers to IR emission (including jumps)
3. Update control flow to use IR labels and jumps
4. Test and validate

**Pros**: Clean, complete solution
**Cons**: High risk, significant work, tests may fail during transition

### Option C: Document and Defer
Document current state and defer full integration to future sprint:
1. Document IR infrastructure
2. Document integration plan
3. Focus on other features

**Pros**: Clean state, clear roadmap
**Cons**: No immediate progress on IR integration

## Next Steps (If Continuing Integration)

1. **Fix Control Flow**: Convert return statements and other control flow to use IR-based emission
2. **Convert Statement Handlers**: Update `lower_stmt_*` functions to work with IR-based expressions
3. **Test Incrementally**: Test each conversion before proceeding
4. **Validate Code Quality**: Compare generated code with original approach

## Files Modified

| File | Changes |
|------|---------|
| `src/zero_cc/parser/regalloc.h` | Added temp-to-register mapping functions |
| `src/zero_cc/parser/ir_emit_ops.h` | Added 30+ IR emission functions |
| `src/zero_cc/parser/ast_lowering.h` | Partial IR-based lowering (needs completion) |
| `src/zero_cc/isa/x86_64.c` | No changes (backend interface unchanged) |

## Conclusion

The IR infrastructure is complete and functional. The remaining work is converting the existing direct machine code emission to use the IR pipeline. This is a significant undertaking (~255 call sites) that should be done incrementally to minimize risk.

The register allocator integration is complete and working correctly. The main challenge is the ordering of machine code generation when mixing IR and direct emission.
