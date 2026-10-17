# Task 56: Pass 2 Machine Code Emission - Start Here

## 🎯 Objective

Complete the two-pass IR architecture by implementing all missing opcode handlers in Pass 2 (IR → Machine Code).

## 📊 Current State

```
✅ Circular dependencies: None
✅ Backend functions: All required functions present in isa_backend.h
✅ IR buffer: ir_emit, ir_get_buffer, ir_get_count, ir_clear all working
✅ Two-pass flow: AST → IR → MC connected
❌ Missing: 15 opcode handlers in ast_lowering_mc.c
```

## 📋 Missing Opcode Handlers (15 total)

### Critical Path (Do First)
1. **OP_RET** - Return from function
2. **OP_EPILOGUE** - Function epilogue (callee-save loads, leave, ret)
3. **OP_CALL_INDIRECT** - Call through register pointer

### Stack Operations (ISA-specific)
4. **OP_PUSH_RAX** - Push register to stack
5. **OP_POP_RAX** - Pop register from stack
6. **OP_PUSH_RBX** - Push register to stack
7. **OP_POP_RBX** - Pop register from stack

### Arithmetic Operations (ISA-specific)
8. **OP_ADD_IMM** - Add immediate to register
9. **OP_SUB_IMM** - Subtract immediate from register
10. **OP_INC** - Increment register
11. **OP_DEC** - Decrement register
12. **OP_SAR** - Arithmetic shift right

### Special Operations
13. **OP_STACK_ALIGN** - Align stack pointer

## 🗂️ Files to Modify

### Core Changes
1. `src/zero_cc/parser/ast_lowering_mc.c` - Add 15 opcode handlers
2. `src/zero_cc/isa/isa_backend.h` - Add 10 new function pointers

### Backend Changes (3 ISAs)
3. `src/zero_cc/isa/x86_64.c` - Implement 10 functions
4. `src/zero_cc/isa/aarch64.c` - Implement 10 functions
5. `src/zero_cc/isa/riscv.c` - Implement 10 functions (x2 for 32/64-bit)

## 🔧 Backend Functions to Add

Add to `isa_backend.h` after existing function pointers:

```c
/* Stack operations */
void (*emit_push_reg)(ZeroCCCompiler *c, int reg);
void (*emit_pop_reg)(ZeroCCCompiler *c, int reg);

/* Arithmetic operations */
void (*emit_add_imm)(ZeroCCCompiler *c, int dst, int32_t imm);
void (*emit_sub_imm)(ZeroCCCompiler *c, int dst, int32_t imm);
void (*emit_inc)(ZeroCCCompiler *c, int reg);
void (*emit_dec)(ZeroCCCompiler *c, int reg);
void (*emit_sar)(ZeroCCCompiler *c, int dst, int shift);

/* Control flow */
void (*emit_call_reg)(ZeroCCCompiler *c, int reg);
void (*emit_ret)(ZeroCCCompiler *c);

/* Special */
void (*emit_stack_align)(ZeroCCCompiler *c, uint8_t alignment);
```

## 📝 Implementation Order

### Phase 1: Core Infrastructure (2 hours)
- [ ] Add 10 new function pointers to `isa_backend.h`
- [ ] Add `ir_clear()` call at start of function compilation
- [ ] Verify: `make test` still passes

### Phase 2: Critical Handlers (1 hour)
- [ ] Add OP_RET handler in ast_lowering_mc.c
- [ ] Add OP_EPILOGUE handler in ast_lowering_mc.c
- [ ] Add OP_CALL_INDIRECT handler in ast_lowering_mc.c
- [ ] Verify: `make test` still passes

### Phase 3: Stack Handlers (2 hours)
- [ ] Add OP_PUSH_RAX, OP_POP_RAX handlers
- [ ] Add OP_PUSH_RBX, OP_POP_RBX handlers
- [ ] Implement for x86-64, aarch64, riscv
- [ ] Verify: `make test` still passes

### Phase 4: Arithmetic Handlers (2 hours)
- [ ] Add OP_ADD_IMM, OP_SUB_IMM handlers
- [ ] Add OP_INC, OP_DEC, OP_SAR handlers
- [ ] Implement for x86-64, aarch64, riscv
- [ ] Verify: `make test` still passes

### Phase 5: Special Handlers (1 hour)
- [ ] Add OP_STACK_ALIGN handler
- [ ] Implement for x86-64, aarch64, riscv
- [ ] Verify: `make test` still passes

### Phase 6: Full Testing (1 hour)
- [ ] Run full test suite: `make test`
- [ ] Run with ASan: `make asan`
- [ ] Run with UBSan: `make ubsan`
- [ ] Verify: All 118 tests pass

## 🧪 Testing Strategy

### Before Each Change
```bash
make clean && make test  # Baseline: 118/118 pass
```

### After Each Phase
```bash
make test  # Verify no regressions
```

### Final Verification
```bash
make clean && make test
make asan
make ubsan
python3 tools/task56_precheck.py  # Should show all PASS
```

## 📚 Reference Materials

| Resource | Location |
|----------|----------|
| Handler templates | `tools/task56_missing_handlers.txt` |
| Detailed checklist | `tools/task56_checklist.md` |
| Pre-check script | `tools/task56_precheck.py` |
| Dependency graph | `deps_graph.json` |
| ISA backend docs | `src/zero_cc/isa/isa_backend.h` |

## ⚠️ Common Pitfalls

1. **Don't forget to update all 3 ISAs** - x86-64, aarch64, riscv
2. **Test incrementally** - Run `make test` after each phase
3. **Check register allocation** - Use `regalloc_get_reg_for_temp()` for temps
4. **Handle missing backend functions** - Check `if (BE(c)->emit_xxx)` before calling
5. **Watch for signed vs unsigned** - immediates may need casting

## 🚀 Quick Start

```bash
# 1. Review current state
python3 tools/task56_precheck.py
python3 tools/task56_detailed_analysis.py

# 2. Read templates
cat tools/task56_missing_handlers.txt

# 3. Start with Phase 1
# Edit: src/zero_cc/isa/isa_backend.h
# Add: 10 new function pointers

# 4. Test
make test

# 5. Continue with Phase 2-6
```

## 📈 Success Criteria

- [ ] All 15 opcode handlers implemented
- [ ] All 10 backend functions implemented for 3 ISAs
- [ ] All 118 tests pass
- [ ] No ASan/UBSan errors
- [ ] `python3 tools/task56_precheck.py` shows all PASS
- [ ] Two-pass architecture complete

## 🎉 Completion

When done, Task 56 will be complete and the two-pass IR architecture will be fully functional, ready for Sprint 8 optimization work (Tasks 57-59).
