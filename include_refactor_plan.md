# Include Structure Refactoring Plan

## Current Problems

1. **Static variables in headers**: `ir.h` had `static instr_count` causing duplicate copies (FIXED)
2. **Huge monolithic headers**: `ast_lowering.h` is 57KB - does both AST lowering AND IR emission
3. **Unclear separation**: JIT compiler (`compile.h`) and parser (`parser.c`) both include IR headers
4. **Multiple include paths**: Same headers included from different locations

## Proposed Structure

### Phase 1: Separate IR Emission from AST Lowering

**Current**:
```
ast_lowering.h (57KB)
  - AST node traversal
  - Machine code emission (zero_cc_emit_u8, etc.)
  - IR emission (ir_emit, etc.)
  - Temp variable tracking
```

**Split into**:
```
parser/ast_lowering.h (core AST lowering)
  - AST node traversal
  - Calls to emit_interface for code generation

parser/ir_emitter.h (NEW - ~2KB)
  - IR instruction emission
  - Temp variable management
  - Label management

parser/machine_emitter.h (NEW - ~2KB)  
  - Machine code emission wrappers
  - Relocation tracking
```

### Phase 2: Clean Header Dependencies

**Public API Headers** (what users include):
```
include/zero_cc.h
  - zero_cc/core.h
  - zero_cc/types.h
  - zero_cc/compiler.h (compile_c99, execute_jit, etc.)
```

**Internal Parser Headers** (parser.c only):
```
src/zero_cc/parser/
  - ast_arena.h (arena management)
  - ast_expr.h (expression parsing)
  - ast_stmt.h (statement parsing)
  - ast_lowering.h (AST → IR/machine code)
  - ir.h (IR buffer - extern declarations)
  - ir.c (IR buffer - definitions)
  - ir_emitter.h (IR emission API)
  - regalloc.h (register allocator)
  - live_range.h (live range analysis)
```

**JIT Compiler Headers** (compile_unit.h only):
```
src/zero_cc/jit/
  - compile.h (function compilation)
  - compile_unit.h (translation unit compilation)
  - execution.h (JIT execution)
  - jit_linker.h (symbol resolution)
```

### Phase 3: Eliminate Circular Dependencies

**Current circular deps**:
- `expressions.h` ↔ `statements.h` (both include parser.h)
- `compile.h` ↔ `ast_lowering.h` (both need IR/regalloc)

**Solution**:
- Use forward declarations where possible
- Split headers into "types" vs "functions"
- Move implementation to .c files where possible

## Implementation Steps

1. ✅ **Move IR buffer to ir.c** (DONE - fixes static variable issue)
2. ⏸️ **Split ast_lowering.h** (next task)
3. ⏸️ **Create ir_emitter.h** (separate IR emission API)
4. ⏸️ **Update compile.h** to use new headers
5. ⏸️ **Update parser.c** to use new headers
6. ⏸️ **Verify all tests pass**

## Benefits

- **Clearer separation**: Parser vs JIT compiler
- **Easier debugging**: Smaller, focused headers
- **Faster compilation**: Less redundant inclusion
- **Better maintainability**: Changes in one area don't break others
- **No more static variable bugs**: All globals in .c files

## Files to Modify

| File | Action |
|------|--------|
| `src/zero_cc/parser/ast_lowering.h` | Split into smaller headers |
| `src/zero_cc/parser/ir_emitter.h` | NEW - IR emission API |
| `src/zero_cc/parser/machine_emitter.h` | NEW - Machine code emission |
| `src/zero_cc/jit/compile.h` | Update includes |
| `src/zero_cc/parser.c` | Update includes |
| `Makefile` | Add ir.o (already done) |

## Risk Assessment

- **Low risk**: IR buffer already fixed
- **Medium risk**: Splitting ast_lowering.h (57KB file)
- **Mitigation**: Do it incrementally, test after each step
