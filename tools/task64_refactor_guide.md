# Task 64: Target-Independent IR Canonicalization - Refactoring Guide

## Current Status ✅ Phase 1 Complete

### Completed
- [x] Added `emit_leave()` and `get_code_offset()` to ISA backend abstraction
- [x] Moved `me_emit_leave()` from IR layer to backend layer
- [x] Moved `me_get_offset()` from IR layer to backend layer
- [x] IR layer no longer includes `machine_emitter.h`
- [x] All 118 tests passing

### Remaining Work (Phase 2)

The analyzer shows "issues" but most are:
- ✅ **Correct backend abstraction usage** (`BE(c)->emit_*`)
- ✅ **Comments explaining ABI** (not actual code)
- ✅ **Register name tables** (needed for register allocation)

#### Actual ISA-Specific Code to Abstract

1. **Calling Convention Tables** (`ast_lowering_mc.c:404`)
   ```c
   static const int int_arg_regs[6] = {7, 6, 2, 1, 8, 9};  /* rdi, rsi, rdx, rcx, r8, r9 */
   ```
   **Action**: Move to `isa_backend.h` as function pointers or tables

2. **IR Opcode Comments** (`ir.h`)
   ```c
   OP_MOV_IMM,        /* mov rax, imm16 */
   OP_MOV_MEM_RAX,    /* mov [rbp - offset], rax */
   ```
   **Action**: Update comments to be ISA-agnostic (e.g., "move immediate to register")

3. **Register Clobber Tracking** (`live_range.h`)
   ```c
   bool *rax_clobbered;
   bool *rdx_clobbered;
   ```
   **Action**: These are OK - they track actual hardware registers for call clobbering

## Recommended Approach

### Option A: Backend Tables (Recommended)
Add to `isa_backend.h`:
```c
typedef struct {
    // ... existing fields ...
    const int *int_arg_regs;      // Array of int arg register indices
    int int_arg_reg_count;
    const int *fp_arg_regs;       // Array of FP arg register indices  
    int fp_arg_reg_count;
} ISARegisterConvention;
```

### Option B: Backend Functions
Add to `isa_backend.h`:
```c
int (*get_int_arg_reg)(ZeroCCCompiler *c, int arg_index);
int (*get_fp_arg_reg)(ZeroCCCompiler *c, int arg_index);
```

## Files to Modify

### Phase 2 Tasks

| File | Change | Effort |
|------|--------|--------|
| `isa_backend.h` | Add calling convention abstraction | 2 hours |
| `x86_64.c` | Implement x86-64 calling convention | 1 hour |
| `aarch64.c` | Implement AArch64 calling convention | 2 hours |
| `riscv.c` | Implement RISC-V calling convention | 2 hours |
| `ast_lowering_mc.c` | Use backend calling convention | 2 hours |

**Total Effort**: ~9 hours

## Verification

After Phase 2:
```bash
# Run detailed analyzer
python3 tools/task64_detailed_analyzer.py

# Should show only:
# - isa_backend_calls (correct usage)
# - Comments (acceptable)
# - Register tables (needed for regalloc)
```

## Next Steps After Task 64

1. **Sprint 7**: Cross-platform backends (AArch64, RISC-V, Windows PE/COFF)
2. **Sprint 8**: Two-pass IR & optimization pipeline
3. **Self-hosting**: Compile ZeroCC with itself
