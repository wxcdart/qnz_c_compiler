# AST Lowering Refactoring Complete

## Summary

Successfully refactored callee-saved register handling from the ISA-agnostic AST lowering layer to the x86-64 backend layer.

## What Changed

### Files Modified

| File | Changes |
|------|---------|
| `src/zero_cc/isa/isa_backend.h` | Added `emit_callee_save_stores/loads` hooks to `ISACallingConvention` struct |
| `src/zero_cc/isa/x86_64.c` | Added x86-64 specific data arrays and implementation functions |
| `src/zero_cc/isa/aarch64.c` | Set hooks to NULL (AArch64 handles differently) |
| `src/zero_cc/isa/riscv.c` | Set hooks to NULL for both riscv64 and riscv32 |
| `src/zero_cc/parser/ast_lowering.c` | Removed `emit_callee_save_loads()` function, updated call to use hook |
| `src/zero_cc/parser/ast_lowering_mc.c` | Updated call to use hook instead of direct function call |
| `src/zero_cc/parser/ast_lowering_internal.h` | Removed function declarations |
| `src/zero_cc/parser/regalloc.h` | Removed `callee_save_slot_offset[]` and `callee_save_pool_to_vreg[]` arrays |

### Data Moved

**From `regalloc.h` to `x86_64.c`:**
```c
// x86-64 specific slot offsets
static const int32_t x86_64_callee_save_slot_offset[] = {
    /* idx 1 (rbx)  */ 8,
    /* idx 8 (r12)  */ 16,
    /* idx 9 (r13)  */ 24,
    /* idx 10 (r14) */ 32,
    /* idx 11 (r15) */ 40,
};

// Physical VREG mapping
static const int x86_64_callee_save_pool_to_vreg[] = {
    0, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
```

### Functions Moved

**From `ast_lowering.c` to `x86_64.c`:**
```c
// Store callee-saved registers to stack (in prologue)
static void x86_64_emit_callee_save_stores(ZeroCCCompiler *c);

// Load callee-saved registers from stack (in epilogue)
static void x86_64_emit_callee_save_loads(ZeroCCCompiler *c);
```

## Architecture

### Before
```
ast_lowering.c (ISA-agnostic)
    ├── emit_callee_save_stores()  ← x86-64 specific!
    └── emit_callee_save_loads()   ← x86-64 specific!
    
    Calls:
    - if (c->isa == ISA_X86_64) emit_callee_save_stores()
    - if (c->isa == ISA_X86_64) emit_callee_save_loads()
```

### After
```
isa_backend.h (interface)
    └── ISACallingConvention {
            emit_callee_save_stores,  // hook
            emit_callee_save_loads    // hook
        }

x86_64.c (x86-64 backend)
    ├── x86_64_callee_save_slot_offset[]
    ├── x86_64_callee_save_pool_to_vreg[]
    ├── x86_64_emit_callee_save_stores()
    └── x86_64_emit_callee_save_loads()
    
    x86_64_conv = {
        .emit_callee_save_stores = x86_64_emit_callee_save_stores,
        .emit_callee_save_loads = x86_64_emit_callee_save_loads,
    }

aarch64.c / riscv.c (other backends)
    .emit_callee_save_stores = NULL,  // not needed
    .emit_callee_save_loads = NULL,   // not needed

ast_lowering.c (ISA-agnostic)
    Calls:
    - if (BE(c)->conv->emit_callee_save_stores) BE(c)->conv->emit_callee_save_stores(c)
    - if (BE(c)->conv->emit_callee_save_loads) BE(c)->conv->emit_callee_save_loads(c)
```

## Benefits

1. **Clean Separation**: ISA-specific code is now in the backend layer
2. **Extensibility**: Adding new ISAs is easier - just implement hooks or leave NULL
3. **Maintainability**: All x86-64 specific data and logic in one place
4. **Type Safety**: Hooks are part of the calling convention interface

## Testing

- ✅ All 118 JIT tests pass
- ✅ x86-64 backend still works correctly
- ✅ AArch64 backend not affected (hooks are NULL)
- ✅ RISC-V backend not affected (hooks are NULL)

## Tools Created

### `tools/refactor_callee_saved.py`
Python assistant for the refactoring process:
- `--analyze` - Analyze current state of x86-64 specific code
- `--generate-patches` - Generate patch files for the refactoring
- `--verify` - Verify refactoring was applied correctly

### `tools/callee_saved_analyzer.py`
Analyzer for callee-saved register handling across ISAs.

## Next Steps

The AST lowering layer is now more ISA-agnostic. Future work:
1. Complete RISC-V backend implementation
2. Test cross-ISA comparison
3. Proceed to SIMD vectorization

## Verification

```bash
# Run verification tool
python3 tools/refactor_callee_saved.py --verify

# Run all tests
./tools/test_runner.py

# Expected output: 118/118 passed
```
