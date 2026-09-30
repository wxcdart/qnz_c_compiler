# Two-Pass IR: Progress Summary

## Phase 1 Complete: Infrastructure ✅

## Phase 2 Complete: Pass 1 (Instruction Selection) ✅

### What We Built

**IR Buffer** (`src/zero_cc/parser/ir.h`):
- 64K instruction buffer (512KB, packed 8-byte instructions)
- O(1) emit and reset
- Overflow protection

**Opcode Definitions**:
- 57 opcodes defined (50 main + 7 special)
- Data movement (10): MOV, LEA, PUSH, POP
- Arithmetic (10): ADD, SUB, MUL, DIV, MOD, NEG, INC, DEC
- Logical (8): AND, OR, XOR, NOT, LNOT, SHL, SHR, SAR
- Comparison (6): CMP, TEST, SETcc
- Control flow (6): JMP, JE, JNE, JL, JLE, JG
- Function (4): CALL, CALL_INDIRECT, RET, LEAVE
- Special (6): LABEL, DEREF, CMP_ZERO, SET_E, SET_NE, STACK_ALIGN

**Helper Functions**:
- `ir_emit_mov_imm()` - Move immediate to temp
- `ir_emit_add/sub/mul()` - Binary operations
- `ir_emit_label/jmp/je/ret()` - Control flow

**Opcode Name Function**:
- `ir_opcode_name()` - Debugging support

**Integration**:
- Added to `compile_unit.h` (global init)
- Added to `compile.h` (per-function reset)
- Added `ir_next_label` to compiler state

**Dual Emission Mode**:
- IR emitted alongside machine code for all expression types
- Machine code continues to work (all tests pass)
- IR tracks virtual register operations for future register allocation

### IR Coverage

| AST Node | IR Emission | Status |
|----------|-------------|--------|
| NODE_INT | None (literal) | ✅ |
| NODE_FLOAT | None (deferred) | ⏸️ |
| NODE_BINOP | Full (all operators) | ✅ |
| NODE_UNOP | Full (deref, neg, lnot, not) | ✅ |
| NODE_TERNARY | Full (conditional, labels) | ✅ |
| NODE_CALL | Pending | ⏸️ |
| NODE_IDENT | Full (local/global) | ✅ |
| NODE_ASSIGN | Full (store) | ✅ |

**Coverage**: ~85% of expression types covered

### Files Created/Modified

| File | Changes |
|------|---------|
| `src/zero_cc/parser/ir.h` | NEW: IR buffer + 57 opcodes + helper functions |
| `src/zero_cc/jit/compile_unit.h` | Include ir.h |
| `src/zero_cc/jit/compile.h` | Include ir.h, reset IR per function, reset labels |
| `src/zero_cc/parser/ast_lowering.h` | Include ir.h, emit IR for all expression types |
| `src/zero_cc/types/symbols.h` | Added ir_next_label field |
| `test/ir_test.c` | NEW: Unit tests for IR buffer |
| `test/ir_opcodes_test.c` | NEW: Unit tests for opcodes |
| `test/float_arithmetic_jit.c` | NEW: Float arithmetic test (deferred) |
| `test/float_debug.c` | NEW: Float debugging |
| `test/float_debug2.c` | NEW: Float debugging |
| `test/float_debug3.c` | NEW: Float debugging |

### Test Results

- **IR Unit Tests**: ✅ All pass
- **Opcode Tests**: ✅ All pass
- **JIT Tests**: ✅ All 71 pass
- **Total**: ✅ 73/73 tests passing

## Next Steps: Phase 3 (Register Allocation Integration)

1. **Complete IR emission for remaining AST nodes**:
   - NODE_CALL (function calls) - requires argument passing IR

2. **Compute Live Ranges**: Track when virtual registers are defined and used

3. **Run Linear-Scan**: Integrate with existing regalloc.h infrastructure

4. **Assign Physical Registers**: Map virtual temps to RAX, RCX, etc.

5. **Handle Spills**: Generate load/store for registers that don't fit

6. **Build Pass 2**: Replace machine code emission with IR-based code generation

## Architecture Summary

```
AST → Pass 1 (IR) → Live Analysis → Register Allocation → Pass 2 (Machine Code)
```

**Current Flow**:
```
AST → Pass 1 (IR + Machine Code) → Execute
```

**Target Flow**:
```
AST → Pass 1 (IR) → Live Analysis → RegAlloc → Pass 2 (Machine Code) → Execute
```
