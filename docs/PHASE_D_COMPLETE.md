# Phase D: IR-Based Callee-Save Operations - COMPLETE

## Summary

Phase D successfully implements IR-based callee-saved register preservation, making callee-save operations visible in IR dumps for better debugging and analysis.

## Changes Made

### 1. New IR Opcodes

Added two new ISA-neutral IR opcodes:

```c
OP_CALLEE_SAVE_STORES,  /* emit callee-saved register stores */
OP_CALLEE_SAVE_LOADS,   /* emit callee-saved register loads */
```

### 2. IR Helper Functions

Added helper functions in `src/zero_cc/parser/ir.h`:

```c
/* Emit callee-saved register stores (after register allocation) */
static inline void ir_emit_callee_save_stores(uint32_t callee_save_mask);

/* Emit callee-saved register loads (after register allocation) */
static inline void ir_emit_callee_save_loads(uint32_t callee_save_mask);
```

### 3. Backend Interface Changes

Updated `ISACallingConvention` in `src/zero_cc/isa/isa_backend.h`:

**Removed:**
```c
void (*emit_callee_save_stores)(ZeroCCCompiler *c);
void (*emit_callee_save_loads)(ZeroCCCompiler *c);
```

**Added:**
```c
const int32_t *callee_save_slot_offsets;  /* Slot offsets for each callee-saved GPR */
```

### 4. Backend Implementations

**x86-64 (`src/zero_cc/isa/x86_64.c`):**
- Removed `x86_64_emit_callee_save_stores/loads()` functions
- Added `callee_save_slot_offsets` array: `{8, 16, 24, 32, 40}` for `{rbx, r12, r13, r14, r15}`
- Updated `x86_64_callee_saved_gpr` to exclude rbp (handled by prologue/epilogue): `{3, 12, 13, 14, 15}`

**AArch64 (`src/zero_cc/isa/aarch64.c`):**
- Removed `aarch64_emit_callee_save_stores/loads()` functions
- Added `callee_save_slot_offsets` array: `{8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96}` for `{x19-x30}`

**RISC-V (`src/zero_cc/isa/riscv.c`):**
- Updated `riscv64_conv` and `riscv32_conv` to include `callee_save_slot_offsets`

### 5. IR-to-MC Lowering

Updated `src/zero_cc/parser/ast_lowering_mc.c`:

```c
case OP_CALLEE_SAVE_STORES:
    /* Generic implementation using backend's callee_saved_gpr array and slot offsets */
    if (BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->emit_store) {
        uint32_t mask = (uint32_t)instr->imm64;
        const int *callee_regs = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int reg = callee_regs[i];
            if ((mask >> reg) & 1u) {
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                BE(c)->emit_store(c, reg, BE(c)->fp_reg, -slot);
            }
        }
    }
    break;

case OP_CALLEE_SAVE_LOADS:
    /* Similar implementation for loads */
    break;
```

### 6. AST Lowering Integration

Updated `src/zero_cc/parser/ast_lowering.c`:

```c
/* Emit callee-save IR instructions after register allocation (Phase D: IR-based callee-save) */
uint32_t callee_save_mask = c->regalloc.used_callee_saved_gpr_mask;
if (callee_save_mask) {
    ir_emit_callee_save_stores(callee_save_mask);
    ir_emit_callee_save_loads(callee_save_mask);
}
```

### 7. IR Dump Support

Added display support in `src/zero_cc/parser/ir.c`:

```c
case OP_CALLEE_SAVE_STORES: fprintf(f, "CALLEE_SAVE_STORES mask=0x%x", (unsigned)instr->imm);
                            break;
case OP_CALLEE_SAVE_LOADS: fprintf(f, "CALLEE_SAVE_LOADS mask=0x%x", (unsigned)instr->imm);
                          break;
```

## Benefits

### 1. Better Debugging

Callee-save operations are now visible in IR dumps:

```
=== IR for function 'abs_val' ===
  18: CALLEE_SAVE_STORES mask=0x8 [flags_out=r0] [flags_in=r0]
  19: CALLEE_SAVE_LOADS mask=0x8 [flags_out=r0] [flags_in=r0]
```

### 2. ISA-Neutral Design

Callee-save logic is now handled generically in the IR-to-MC layer using backend-provided slot offsets, making it easier to add new ISAs.

### 3. Cleaner Backend Interface

Removed hook functions from `ISACallingConvention`, simplifying the backend interface.

### 4. Consistent Slot Management

Slot offsets are now defined per-ISA in the backend, ensuring correct stack layout for each target.

## Test Results

**JIT Tests:** 117/118 passing ✓
- Only pre-existing `vla_test` failure (unrelated VLA stack allocation bug)

**Verification:**
```bash
./build/bin/zerocc --target x86-64 -dump-ir test.c -run
# Shows CALLEE_SAVE_STORES/LOADS in IR output

objdump -d test.o | grep -A 20 "<function>:"
# Shows correct callee-save store/load instructions
```

## Files Modified

1. `src/zero_cc/parser/ir.h` - Added IR opcodes and helper functions
2. `src/zero_cc/parser/ir.c` - Added IR dump support
3. `src/zero_cc/parser/ast_lowering.c` - Emit IR callee-save instructions
4. `src/zero_cc/parser/ast_lowering_mc.c` - Lower IR callee-save to machine code
5. `src/zero_cc/isa/isa_backend.h` - Updated backend interface
6. `src/zero_cc/isa/x86_64.c` - Removed hook functions, added slot offsets
7. `src/zero_cc/isa/aarch64.c` - Removed hook functions, added slot offsets
8. `src/zero_cc/isa/riscv.c` - Added slot offsets

## Next Steps

1. ✅ **Phase D Complete** - IR-based callee-save operations implemented
2. ⏭️ **Proceed to RISC-V Backend Completion** - Implement `emit_cond_set()` and `emit_jcond()`
3. ⏭️ **Consider fixing vla_test** - Pre-existing VLA stack allocation bug
4. ⏭️ **Test cross-ISA comparison** with `tools/isa_diff.py`
5. ⏭️ **Proceed to SIMD vectorization** once multi-ISA support is complete

## Design Decisions

### Why IR-Based Callee-Save?

1. **Debuggability**: Callee-save operations visible in IR dumps
2. **Optimization**: Future DCE/CSE could potentially optimize unused callee-saved registers
3. **ISA-Neutrality**: Generic implementation in IR-to-MC layer
4. **Cleaner Architecture**: Removes ISA-specific hooks from backend interface

### Slot Offset Design

Slot offsets are indexed by array position in `callee_saved_gpr`, not by physical register number:

```c
// x86-64
callee_saved_gpr = {3, 12, 13, 14, 15}  // rbx, r12-r15
slot_offsets = {8, 16, 24, 32, 40}       // sequential slots

// AArch64
callee_saved_gpr = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30}
slot_offsets = {8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96}
```

This design ensures:
- Sequential slot allocation (no gaps)
- Easy to extend for new ISAs
- Clear mapping between register and slot

## Comparison with Previous Design

| Aspect | Previous (Hook-Based) | New (IR-Based) |
|--------|----------------------|----------------|
| Callee-save visibility | Hidden in backend | Visible in IR |
| Backend interface | 2 hook functions | 0 hook functions |
| Slot management | ISA-specific in hooks | ISA-specific in arrays |
| Debugging | Disassembly only | IR + disassembly |
| Extensibility | Add hooks for new ISA | Define slot offsets |

## Conclusion

Phase D successfully implements IR-based callee-saved register preservation, improving debuggability and maintaining ISA-neutral design. All JIT tests pass (except pre-existing VLA bug), and the implementation is ready for further optimization work.
