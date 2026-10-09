# Phase 2: Register Allocator Extensions - COMPLETE ✅

## Overview
Successfully implemented vector register allocator extensions for SIMD support across all three ISAs (x86-64, AArch64, RISC-V).

## Implementation Summary

### 1. Vector Register Tables (regalloc.h)
Added ISA-specific vector register tables:

- **x86-64**: 16 XMM registers (xmm0-xmm15)
  ```c
  static const char *vec_registers_x86[] = {
      "xmm0", "xmm1", ..., "xmm15"
  };
  #define VEC_COUNT_X86 16
  ```

- **AArch64**: 32 NEON V registers (v0-v31)
  ```c
  static const char *vec_registers_aarch64[] = {
      "v0", "v1", ..., "v31"
  };
  #define VEC_COUNT_AARCH64 32
  ```

- **RISC-V**: 32 RVV V registers (v0-v31)
  ```c
  static const char *vec_registers_riscv[] = {
      "v0", "v1", ..., "v31"
  };
  #define VEC_COUNT_RISCV 32
  ```

### 2. Vector Register Free Tracking (regalloc.h)
Added vector register allocation tracking:

```c
typedef struct RegAllocator {
    ...
    bool vec_free[32];  /* Vector registers (max 32 across all ISAs) */
    int next_vec_spill_slot;  /* Vector spill slots (16-byte aligned) */
    ...
} RegAllocator;
```

### 3. 16-Byte Aligned Vector Spill Slots (regalloc.h)
Implemented 16-byte aligned spill slot allocation for vector values:

```c
static void spill_interval(RegAllocator *ra, LiveInterval *interval) {
    if (interval->reg_class == REG_CLASS_VEC) {
        /* Align to 16 bytes (4 x 32-bit floats or 2 x 64-bit doubles) */
        interval->spill_slot = ra->next_vec_spill_slot;
        ra->next_vec_spill_slot += 16;  /* Each vector spill slot is 16 bytes */
    } else {
        interval->spill_slot = ra->next_spill_slot++;
    }
    interval->is_spilled = true;
}
```

### 4. Vector Register Allocation (regalloc.h)
Extended `alloc_reg()` to handle vector register class:

```c
else if (interval->reg_class == REG_CLASS_VEC) {
    /* Vector register allocation (ISA-specific count) */
    int vec_count = VEC_COUNT(ra);
    for (int i = 0; i < vec_count; i++) {
        if (ra->vec_free[i]) {
            ra->vec_free[i] = false;
            return i;
        }
    }
}
```

### 5. Vector Register Expiration (regalloc.h)
Updated `expire_old_intervals()` to free vector registers:

```c
if (active->reg_class == REG_CLASS_VEC) {
    ra->vec_free[active->assigned_reg] = true;
}
```

### 6. Vector Register Name Helpers (regalloc.h)
Added helper functions for vector register names:

```c
static const char *regalloc_get_vec_reg_name(ZeroCCISA isa, int reg_idx);
static int regalloc_get_vec_reg_count(ZeroCCISA isa);
```

### 7. Backend Interface Extensions (isa_backend.h)
Added `get_vec_reg_name` function pointer:

```c
const char *(*get_vec_reg_name)(int vec_reg_idx);  /* Get vector register name */
```

### 8. ISA-Specific Vector Register Name Functions
Implemented for all three ISAs:

- **x86_64.c**: `x86_get_vec_reg_name()` → "xmm0"-"xmm15"
- **aarch64.c**: `aarch64_get_vec_reg_name()` → "v0"-"v31"
- **riscv.c**: `riscv_get_vec_reg_name()` → "v0"-"v31"

### 9. Assembly Emitter Integration (asm_emitter.c)
Added `asm_get_vec_reg_name()` for vector register name generation:

```c
static const char *asm_get_vec_reg_name(ZeroCCCompiler *c, int vec_reg_idx) {
    if (BE(c)->get_vec_reg_name) {
        const char *name = BE(c)->get_vec_reg_name(vec_reg_idx);
        snprintf(buf, sizeof(buf), "%%%s", name);
    } else {
        /* Fallback: use ISA-specific naming */
        if (c->regalloc.target_isa == ISA_AARCH64) {
            snprintf(buf, sizeof(buf), "%%v%d", vec_reg_idx);
        } else if (c->regalloc.target_isa == ISA_RISCV64) {
            snprintf(buf, sizeof(buf), "%%v%d", vec_reg_idx);
        } else {
            snprintf(buf, sizeof(buf), "%%xmm%d", vec_reg_idx);
        }
    }
    return buf;
}
```

## Test Results

✅ **All 118/118 tests passing**

```
Results: 118/118 passed, 0 failed in 0.36s
```

## Files Modified

| File | Changes |
|------|---------|
| `src/zero_cc/parser/regalloc.h` | Vector register tables, free tracking, 16-byte aligned spill slots, allocation logic, helper functions |
| `src/zero_cc/isa/isa_backend.h` | Added `get_vec_reg_name` function pointer |
| `src/zero_cc/isa/x86_64.c` | `x86_get_vec_reg_name()` implementation |
| `src/zero_cc/isa/aarch64.c` | `aarch64_get_vec_reg_name()` implementation |
| `src/zero_cc/isa/riscv.c` | `riscv_get_vec_reg_name()` implementation |
| `src/zero_cc/parser/asm_emitter.c` | `asm_get_vec_reg_name()` for assembly emission |

## Key Features

### 1. ISA-Agnostic Vector Register Management
- Single register allocator supports all three ISAs
- ISA-specific register counts (16 for x86-64, 32 for AArch64/RISC-V)
- Automatic register name generation based on target ISA

### 2. 16-Byte Alignment for Vector Spill Slots
- Critical for SSE/NEON/RVV performance
- Prevents alignment faults on strict architectures
- Separate spill slot counter for vector values

### 3. Type-Safe Register Class Handling
- `REG_CLASS_VEC` for vector registers
- Distinct from `REG_CLASS_XMM` (scalar FP)
- Proper live interval tracking per class

### 4. Architecture-Safe Allocation
- No hardcoded register numbers
- Backend-provided register names
- ISA-specific register count macros

## Alignment with SIMD Roadmap

### Phase 1: Backend Interface Extensions ✅
- Added `vec_regs[]` and `num_vec_regs` to ISARegInfo
- Added 27 vector operation function pointers
- **Status**: Complete

### Phase 2: Register Allocator Extensions ✅ (THIS PHASE)
- Added vector live range tracking
- Implemented 16-byte aligned vector spill slots
- Vector register allocation logic
- **Status**: Complete

### Phase 3: SIMD ISA-Specific Vector Emission ⏳
- Implement x86-64 SSE2 instructions
- Implement AArch64 NEON instructions
- Implement RISC-V RVV instructions
- **Status**: Ready to start

### Phase 4: Assembly Emitter Integration ⏳
- Add 32 opcode cases to asm_emitter.c
- Integrate with regalloc for physical register mapping
- **Status**: Ready (infrastructure in place)

### Phase 5: SIMD Auto-Vectorization Pass ⏳
- Loop pattern recognition and unrolling
- IR substitution (scalar → vector)
- Tail loop handling
- **Status**: Ready (tools available)

## Next Steps

### Immediate: Phase 3 - ISA-Specific Vector Emission
1. Implement x86_64_asm.c vector instruction emission:
   - `x86_emit_vmovaps()` - Move aligned packed single
   - `x86_emit_vaddps()` - Add packed single
   - `x86_emit_vsubps()` - Subtract packed single
   - ... (27 total instructions)

2. Implement aarch64_asm.c vector instruction emission:
   - `aarch64_emit_ld1()` - Load single structure
   - `aarch64_emit_fadd()` - Floating point add
   - `aarch64_emit_fsub()` - Floating point subtract
   - ... (25 total instructions)

3. Implement riscv_asm.c vector instruction emission:
   - `riscv_emit_vle32()` - Load element 32-bit
   - `riscv_emit_vfadd()` - Vector FP add
   - `riscv_emit_vfsub()` - Vector FP subtract
   - ... (15 total instructions)

### Testing Strategy
1. Use `vector_register_debugger.py` to debug allocation
2. Use `vector_instruction_validator.py` to validate output
3. Use `vector_coverage_tracker.py` to track implementation
4. Run existing test suite (118 tests) after each ISA

## Technical Details

### Vector Spill Slot Layout
```
Stack Frame:
┌─────────────────────┐
│ Function arguments  │
├─────────────────────┤
│ Scalar spill slots  │ (next_spill_slot * 4 bytes)
├─────────────────────┤
│ Vector spill slots  │ (next_vec_spill_slot bytes, 16-byte aligned)
│                     │
│   [0-15]   vreg 0   │
│   [16-31]  vreg 1   │
│   [32-47]  vreg 2   │
│   ...               │
├─────────────────────┤
│ Local variables     │
├─────────────────────┤
│ Callee-save area    │ (ISA-specific: 48/96 bytes)
└─────────────────────┘
```

### Register Allocation Algorithm
1. Compute live intervals for all temporaries (including vector temps)
2. Sort intervals by start position
3. For each interval:
   - Expire old intervals (end <= current position)
   - Try to allocate vector register if `reg_class == REG_CLASS_VEC`
   - If no free register, spill longest-ending active interval
   - Allocate register to current interval
4. Generate spill/reload code for spilled intervals

### 16-Byte Alignment Rationale
- SSE: 128-bit (16-byte) vectors require 16-byte alignment
- NEON: 128-bit (16-byte) vectors require 16-byte alignment
- RVV: Variable width, but 16-byte alignment is safe minimum
- Misaligned access can cause:
  - Performance penalty (split cache lines)
  - Alignment faults (on some architectures)
  - Incorrect results (with certain instructions)

## Conclusion

✅ **Phase 2 Complete**

The register allocator now fully supports vector registers with:
- ISA-specific register counts
- 16-byte aligned spill slots
- Proper live range tracking
- Backend-provided register names
- All 118 tests passing

Ready to proceed with Phase 3: ISA-Specific Vector Emission.
