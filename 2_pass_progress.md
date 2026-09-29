# Two-Pass IR: Progress Summary

## Phase 1 Complete: Infrastructure ✅

### What We Built

**IR Buffer** (`src/zero_cc/parser/ir.h`):
- 64K instruction array (512KB, packed 8-byte instructions)
- O(1) emit and reset
- Overflow protection

**Integration**:
- Added to `compile_unit.h` (global init)
- Added to `compile.h` (per-function reset)

### Files Created/Modified

| File | Changes |
|------|---------|
| `src/zero_cc/parser/ir.h` | NEW: IR buffer infrastructure |
| `src/zero_cc/jit/compile_unit.h` | Include ir.h |
| `src/zero_cc/jit/compile.h` | Include ir.h, reset IR per function |
| `test/ir_test.c` | NEW: Unit tests for IR buffer |
| `2_pass_plan.md` | Updated with IR design |
| `2_pass_decision.md` | NEW: Final design decisions |
| `2_pass_considerations.md` | NEW: Edge cases and details |
| `circular_dependency_analysis.md` | NEW: Architecture analysis |

### Test Results

```
✅ IR unit tests pass
✅ All 71 existing tests pass
✅ No compiler warnings
```

### IR Structure (8 bytes packed)

```c
typedef struct __attribute__((packed)) {
    uint8_t  op;        /* 0-255 opcodes */
    uint8_t  temp_dest; /* 0-255 temps (0xFF if none) */
    uint8_t  temp_src1; /* 0-255 temps (0xFF if none) */
    uint8_t  temp_src2; /* 0-255 temps (0xFF if none) */
    int16_t  imm;       /* -32768 to 32767 */
    uint16_t label_idx; /* 0-65535 labels */
} Instr;
```

### Memory Usage

```
Total: 512KB (65,536 × 8 bytes)
Per function: 0 bytes (shared, reset between functions)
Overflow: Fails with clear error message
```

---

## Phase 2: Pass 1 (Instruction Selection) - Next

### What We Need

**Goal**: Replace direct x86-64 code emission with IR generation

**Current** (single-pass):
```c
// In ast_lowering.h
zero_cc_emit_u8(&c->text_section, 0x48);  // mov rax, imm
zero_cc_emit_u8(&c->text_section, 0xB8);
zero_cc_emit_u64(&c->text_section, value);
```

**Target** (two-pass):
```c
// Pass 1: Generate IR
Instr instr = {
    .op = OP_MOV_IMM,
    .temp_dest = temp_id,
    .temp_src1 = 0xFF,
    .temp_src2 = 0xFF,
    .imm = value,
    .label_idx = 0
};
ir_emit(instr);

// Pass 2: Emit machine code
// (after register allocation and optimization)
emit_machine_code();
```

### Implementation Plan

**Step 1: Define Opcodes** (50 opcodes)
- Data movement (MOV, LEA, PUSH, POP)
- Arithmetic (ADD, SUB, MUL, DIV, MOD)
- Logical (AND, OR, XOR, NOT, SHL, SHR)
- Comparison (CMP, TEST, SETcc)
- Control flow (JMP, JE, JNE, JL, JG)
- Function (CALL, RET, LEAVE)

**Step 2: Rename Current Code**
- `zero_cc_lower_ast()` → `lower_expr_pass1()`
- `zero_cc_emit_*()` → `ir_emit()`

**Step 3: Start with Simple Nodes**
- NODE_BINOP (add, sub, mul, div)
- NODE_UNOP (neg, not)
- NODE_IDENT (variable load)

**Step 4: Implement Pass 2**
- Convert IR to x86-64
- Use allocated registers
- Generate spill code

---

## Next Immediate Steps

1. **Define opcode enum** (50 opcodes)
2. **Implement emit functions** for each opcode
3. **Migrate NODE_BINOP** to two-pass
4. **Test with simple arithmetic** (a + b)
5. **Verify output matches** current implementation

---

## Timeline

| Phase | Duration | Status |
|-------|----------|--------|
| Phase 1: Infrastructure | 1 day | ✅ Complete |
| Phase 2: Pass 1 | 3-4 days | ⏳ Next |
| Phase 3: Register Alloc | 2 days | Planned |
| Phase 4: Pass 2 | 4-5 days | Planned |
| Phase 5: Optimization | 5-7 days | Planned |
| Phase 6: Testing | 3-5 days | Planned |

**Total: ~3 weeks**

---

## Key Decisions

✅ **64K instruction array** (not 128K, not vector)  
✅ **8-byte packed instructions** (not 16 bytes)  
✅ **Shared across functions** (not per-function)  
✅ **O(1) reset** (not malloc/free)  
✅ **Overflow check** (fail fast)  

---

## Success Criteria

- ✅ IR buffer works (unit tests pass)
- ✅ All existing tests pass (regression)
- ⏳ Pass 1 generates IR (next)
- ⏳ Pass 2 emits correct code (after)
- ⏳ Register allocation visible (after)
- ⏳ -O2 faster than -O0 (after)

---

## Notes

**Circular Dependencies**: Documented in `circular_dependency_analysis.md`  
**Edge Cases**: Documented in `2_pass_considerations.md`  
**Final Design**: Documented in `2_pass_decision.md`

**Ready for Phase 2!**
