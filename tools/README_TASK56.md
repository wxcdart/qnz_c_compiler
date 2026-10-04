# Task 56: Pass 2 Machine Code Emission - Pre-Work Summary

## Current State Analysis

### ✅ Ready
- **Circular Dependencies**: None found
- **Backend Functions**: All 3 ISAs (x86-64, aarch64, riscv) have required functions
- **IR Buffer Integration**: `ir_emit`, `ir_get_buffer`, `ir_get_count`, `ir_clear` all present
- **Two-Pass Infrastructure**: `emit_ir_to_machine_code()` exists and is called from ast_lowering.c

### ❌ Missing
- **15 Opcode Handlers** in Pass 2 (ast_lowering_mc.c):
  - Stack ops: OP_PUSH_RAX, OP_POP_RAX, OP_PUSH_RBX, OP_POP_RBX
  - Arithmetic: OP_ADD_IMM, OP_SUB_IMM, OP_INC, OP_DEC, OP_SAR
  - Control Flow: OP_RET, OP_EPILOGUE, OP_CALL_INDIRECT
  - Special: OP_STACK_ALIGN

## Workspace Tools Created

| Tool | Purpose |
|------|---------|
| `task56_precheck.py` | Automated readiness check |
| `task56_detailed_analysis.py` | Deep dive into current state |
| `task56_workspace_setup.py` | Creates missing infrastructure |
| `task56_checklist.md` | Detailed task breakdown |
| `task56_missing_handlers.txt` | Handler code templates |

## Recommended Workflow

### Day 1: Infrastructure & Missing Handlers
```bash
# 1. Review current state
python3 tools/task56_detailed_analysis.py

# 2. Add missing opcode handlers (15 handlers)
# Use templates from tools/task56_missing_handlers.txt

# 3. Add backend function declarations to isa_backend.h
# Add: emit_push_reg, emit_pop_reg, emit_add_imm, etc.

# 4. Test
make test
```

### Day 2: Backend Implementation
```bash
# Implement missing functions for all 3 ISAs:
# x86_64.c, aarch64.c, riscv.c

# Test after each ISA
make test
```

### Day 3: Integration & Testing
```bash
# Full test suite
make test
make asan
make ubsan

# Verify two-pass flow
python3 tools/task56_precheck.py
```

## Estimated Effort
- **Infrastructure**: 2 hours
- **Missing Handlers**: 4 hours  
- **Backend Implementation**: 12 hours (4 hours per ISA)
- **Testing & Integration**: 4 hours
- **Total**: ~3 days

## Files to Modify

### Core Changes
1. `src/zero_cc/parser/ast_lowering_mc.c` - Add 15 opcode handlers
2. `src/zero_cc/isa/isa_backend.h` - Add 10 new function pointers

### Backend Changes
3. `src/zero_cc/isa/x86_64.c` - Implement 10 functions
4. `src/zero_cc/isa/aarch64.c` - Implement 10 functions
5. `src/zero_cc/isa/riscv.c` - Implement 10 functions (x2 for 32/64-bit)

## Success Criteria
- [ ] All 118 tests pass
- [ ] No compiler warnings
- [ ] No ASan/UBSan errors
- [ ] Two-pass flow verified

## Notes
- The "Two-Pass Flow" check in precheck.py is a false positive - functions are in ast_lowering.c, not parser.c
- Some missing handlers (PUSH/POP) may be ISA-specific and can use backend function pointers
- Focus on critical path first: RET, EPILOGUE, CALL_INDIRECT
