# Task 56 Triple Check Results

## ✅ Pre-Implementation Verification Complete

### Current State

| Check | Status |
|-------|--------|
| Tests | 118/118 passing ✅ |
| ir_clear() | Implemented ✅ |
| deps_graph.json | Updated ✅ |
| Documentation | Complete ✅ |

### Missing Items (Task 56 Work)

#### 1. Opcode Handlers (15 in ast_lowering_mc.c)

**Stack Operations:**
- OP_PUSH_RAX
- OP_POP_RAX
- OP_PUSH_RBX
- OP_POP_RBX

**Arithmetic Operations:**
- OP_ADD_IMM
- OP_SUB_IMM
- OP_INC
- OP_DEC
- OP_SAR

**Control Flow:**
- OP_CALL_INDIRECT
- OP_RET
- OP_EPILOGUE

**Special:**
- OP_STACK_ALIGN

#### 2. Backend Function Pointers (3 in isa_backend.h)

- `void (*emit_sar)(ZeroCCCompiler *c, int dst, int shift);`
- `void (*emit_call_reg)(ZeroCCCompiler *c, int reg);`
- `void (*emit_stack_align)(ZeroCCCompiler *c, uint8_t alignment);`

**Note:** Other functions (emit_push_reg, emit_pop_reg, emit_add_imm, etc.) are already defined in isa_backend.h and implemented in x86_64.c.

#### 3. Backend Implementations

| Function | x86_64.c | aarch64.c | riscv.c |
|----------|----------|-----------|---------|
| emit_push_reg | ✅ | ✅ | ✅ |
| emit_pop_reg | ✅ | ✅ | ✅ |
| emit_add_imm | ✅ | ❌ | ❌ |
| emit_sub_imm | ✅ | ❌ | ❌ |
| emit_inc | ✅ | ❌ | ❌ |
| emit_dec | ✅ | ❌ | ❌ |
| emit_sar | ❌ | ❌ | ❌ |
| emit_call_reg | ❌ | ❌ | ❌ |
| emit_stack_align | ❌ | ❌ | ❌ |
| emit_ret | ✅ | ✅ | ✅ |

#### 4. ir_clear() Call Location

**Should be added after `regalloc_destroy()` in ast_lowering.c:**

```c
/* Restore callee-saved registers before the epilogue leave/ret. */
emit_callee_save_loads(c);

/* Emit epilogue */
BE(c)->emit_leave(c);
BE(c)->emit_ret(c);
regalloc_destroy(&c->regalloc);

/* Add here: */
ir_clear();

/* text_offset was already set... */
```

### Implementation Priority

**Phase 1: Core Infrastructure (First)**
1. Add 3 missing backend function pointers to isa_backend.h
2. Add ir_clear() call in ast_lowering.c
3. Verify tests still pass

**Phase 2: Critical Handlers**
1. Add OP_RET, OP_EPILOGUE, OP_CALL_INDIRECT handlers
2. Implement emit_call_reg for all 3 ISAs
3. Verify tests still pass

**Phase 3: Stack Handlers**
1. Add OP_PUSH_RAX, OP_POP_RAX, OP_PUSH_RBX, OP_POP_RBX handlers
2. Verify tests still pass

**Phase 4: Arithmetic Handlers**
1. Add OP_ADD_IMM, OP_SUB_IMM, OP_INC, OP_DEC, OP_SAR handlers
2. Implement remaining backend functions for all 3 ISAs
3. Verify tests still pass

**Phase 5: Special Handlers**
1. Add OP_STACK_ALIGN handler
2. Implement emit_stack_align for all 3 ISAs
3. Verify tests still pass

### Files to Modify

| File | Changes |
|------|---------|
| `isa_backend.h` | Add 3 function pointers |
| `ast_lowering.c` | Add ir_clear() call |
| `ast_lowering_mc.c` | Add 15 opcode handlers |
| `x86_64.c` | Implement 3 functions |
| `aarch64.c` | Implement 7 functions |
| `riscv.c` | Implement 7 functions (x2 for 32/64-bit) |

### Verification Commands

```bash
# Before starting
python3 tools/task56_precheck.py

# After each phase
make test

# Final verification
make clean && make test
python3 tools/task56_precheck.py
```

### Risk Assessment

| Risk | Level | Mitigation |
|------|-------|------------|
| Breaking existing tests | Low | Incremental testing after each phase |
| Missing backend functions | Low | Templates ready in task56_missing_handlers.txt |
| ISA-specific issues | Medium | Test each ISA separately |
| Stack alignment issues | Medium | Use existing alignment logic as reference |

### Success Criteria

- [ ] All 15 opcode handlers implemented
- [ ] All 3 new backend function pointers added
- [ ] All 17 backend implementations complete (3 + 7 + 7)
- [ ] ir_clear() called between functions
- [ ] All 118 tests pass
- [ ] No ASan/UBSan errors
- [ ] python3 tools/task56_precheck.py shows all PASS

### Notes

1. **emit_ret** is already implemented in all ISAs - no new work needed
2. **emit_push_reg** and **emit_pop_reg** are already implemented in all ISAs
3. **emit_add_imm** and **emit_sub_imm** are already implemented in x86_64.c
4. The main missing work is:
   - 3 new backend function pointers
   - 15 opcode handlers
   - 17 backend implementations (x86_64: 3, aarch64: 7, riscv: 7)
5. ir_clear() needs to be called after each function compilation

### Conclusion

Task 56 is **READY TO START**. All pre-work is complete:
- ✅ Dependencies analyzed
- ✅ Templates prepared
- ✅ Documentation complete
- ✅ Tests passing
- ✅ Clear implementation path defined
