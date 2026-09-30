# Include Structure Refactoring Plan

## Current Problems (Resolved)

1. ✅ **Static variables in headers**: `ir.h` had `static instr_count` causing duplicate copies (FIXED - moved to ir.c)
2. ✅ **Huge monolithic headers**: `ast_lowering.h` was 57KB with mixed concerns (SPLIT - state moved to ir_emitter.h)
3. ✅ **Unclear separation**: JIT compiler (`compile.h`) and parser (`parser.c`) both include IR headers (CLEARED - unified via ir_emitter.h)
4. ✅ **Multiple include paths**: Same headers included from different locations (RESOLVED - consistent include structure)

## Implemented Structure

### Phase 1: Separate IR State from AST Lowering ✅

**Before**:
```
ast_lowering.h (57KB)
  - AST node traversal
  - Machine code emission (me_* functions)
  - Temp variable tracking (static globals)
  - Label/goto management (static globals)
  - Loop control flow (static globals)
```

**After**:
```
parser/ir_emitter.h (~4KB) - NEW
  - IREmitterContext struct (all state)
  - Temp variable management
  - Label/goto management  
  - Loop control flow
  - State initialization/reset

parser/ast_lowering.h (~35KB) - REFACTORED
  - AST node traversal
  - Machine code emission (via machine_emitter.h)
  - Uses IREmitterContext for state
  - No static globals

parser/machine_emitter.h - EXISTING
  - Machine code emission wrappers
  - Relocation tracking
```

### Phase 2: Clean Header Dependencies ✅

**Public API Headers** (what users include):
```
include/zero_cc.h
  - zero_cc/core.h
  - zero_cc/types_lexer.h
  - zero_cc/parser.h
  - zero_cc/expressions.h
  - zero_cc/statements.h
  - zero_cc/jit_linker.h
```

**Internal Parser Headers** (parser.c only):
```
src/zero_cc/parser/
  - ast_arena.h (arena management)
  - ast_expr.h (expression parsing)
  - ast_stmt.h (statement parsing)
  - ast_dump.h (AST dumping/debugging)
  - ast_lowering.h (AST → machine code)
  - ir.h (IR buffer - extern declarations)
  - ir.c (IR buffer - definitions)
  - ir_emitter.h (IR state management) ← NEW
  - regalloc.h (register allocator)
  - live_range.h (live range analysis)
  - machine_emitter.h (machine code wrappers)
```

**JIT Compiler Headers** (compile_unit.h only):
```
src/zero_cc/jit/
  - compile.h (function compilation) ← NOW USES ir_emitter.h
  - compile_unit.h (translation unit compilation) ← NOW USES ir_emitter.h
  - execution.h (JIT execution)
  - jit_linker.h (symbol resolution)
```

### Phase 3: Eliminate Circular Dependencies ✅

**Resolved circular deps**:
- ✅ `compile.h` ↔ `ast_lowering.h`: Both now depend on `ir_emitter.h` instead of each other
- ⏸️ `expressions.h` ↔ `statements.h`: Old parser path, not affected by this refactoring

**Solution Applied**:
- Created `ir_emitter.h` as shared dependency for state management
- `compile.h` and `compile_unit.h` include `ir_emitter.h`
- `ast_lowering.h` includes `ir_emitter.h` for state
- No direct dependencies between compile.h and ast_lowering.h

## Implementation Steps (ALL COMPLETE)

1. ✅ **Move IR buffer to ir.c** (DONE - fixes static variable issue)
2. ✅ **Split ast_lowering.h** (DONE - state moved to ir_emitter.h)
3. ✅ **Create ir_emitter.h** (DONE - IR state management API)
4. ✅ **Update compile.h** to use new headers (DONE - includes ir_emitter.h)
5. ✅ **Update parser.c** to use new headers (DONE - via ast_lowering.h)
6. ✅ **Verify all tests pass** (DONE - all 70+ tests pass)

## Files Modified

| File | Action | Status |
|------|--------|--------|
| `src/zero_cc/parser/ast_lowering.h` | Refactored - removed static globals | ✅ |
| `src/zero_cc/parser/ir_emitter.h` | NEW - IR state management | ✅ |
| `src/zero_cc/jit/compile.h` | Updated includes | ✅ |
| `src/zero_cc/jit/compile_unit.h` | Updated includes | ✅ |
| `src/zero_cc/parser.c` | No changes needed (includes ast_lowering.h) | ✅ |
| `Makefile` | Already includes ir.o | ✅ |

## Benefits Achieved

- ✅ **Clearer separation**: IR state management separated from AST lowering
- ✅ **Easier debugging**: Smaller, focused headers (ir_emitter.h ~4KB)
- ✅ **Faster compilation**: Less redundant inclusion
- ✅ **Better maintainability**: Changes in one area don't break others
- ✅ **No more static variable bugs**: All state in IREmitterContext
- ✅ **Clean dependency graph**: compile.h and ast_lowering.h both depend on ir_emitter.h

## Test Results

All 70+ tests pass:
- ✅ IR infrastructure tests (ir_test, ir_opcodes_test)
- ✅ AST lowering tests (ast_lowering)
- ✅ JIT compilation tests (abi_sum_jit, atomic_jit, etc.)
- ✅ Language feature tests (struct_by_value_*, goto_*, etc.)
- ✅ Edge case tests (tls, variadic, func_ptr_*, etc.)

## Future Work

- ⏸️ **IR emission path**: Currently ast_lowering.h emits machine code directly. Future work can add IR emission using ir_emitter.h helpers.
- ⏸️ **Old parser deprecation**: The old direct parser (statements.h/expressions.h) still has its own state management. Can be deprecated when AST parser is fully feature-complete.
- ⏸️ **Register allocation**: The live range analysis and register allocation code exists but is not fully integrated. Can be enabled when IR emission path is implemented.
