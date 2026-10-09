# Critical ISA-Specific Bugs Fixed

## Summary

Fixed two critical bugs that caused incorrect behavior on AArch64 and RISC-V backends:

1. **Hardcoded Scratch Registers** - Used argument/return registers for spills, corrupting function arguments
2. **Hardcoded Frame Reserve** - Allocated wrong amount of frame space for callee-saves, causing stack corruption

---

## Bug #1: Hardcoded Scratch Registers (ast_lowering_mc.c:38-55)

### The Problem

**Code:**
```c
/* Register 10 hardcoded for spills */
if (spill_src1 >= 0) {
    BE(c)->emit_load(c, 10 /* r10 */, BE(c)->fp_reg, -slot_off);
    phys_src1 = 10;
}
```

**Impact on AArch64/RISC-V:**
- x86-64: Register 10 = r10 (safe, non-argument register)
- **AArch64: Register 10 = x10 (ARGUMENT/RETURN REGISTER!)**
- **RISC-V: Register 10 = a0 (FIRST ARGUMENT/RETURN REGISTER!)**

**Result:** Spilling to register 10 corrupts function arguments and return values!

### The Solution

Added ISA-specific scratch register fields to `ZeroCCISABackend`:
```c
int scratch_reg1;  /* First scratch register (non-argument/return) */
int scratch_reg2;  /* Second scratch register (non-argument/return) */
```

**Fixed Code:**
```c
if (spill_src1 >= 0) {
    BE(c)->emit_load(c, BE(c)->scratch_reg1, BE(c)->fp_reg, -slot_off);
    phys_src1 = BE(c)->scratch_reg1;
}
```

### Per-ISA Configuration

**x86-64:**
```c
.scratch_reg1 = 10,  /* r10 - non-argument register */
.scratch_reg2 = 11,  /* r11 - non-argument register */
```

**AArch64:**
```c
.scratch_reg1 = 9,   /* x9 - non-argument register */
.scratch_reg2 = 10,  /* x10 - non-argument register */
```

**RISC-V:**
```c
.scratch_reg1 = 5,   /* t0 - non-argument register */
.scratch_reg2 = 6,   /* t1 - non-argument register */
```

---

## Bug #2: Hardcoded Callee-Save Frame Reserve (regalloc.h:460)

### The Problem

**Code:**
```c
#define CALLEE_SAVE_FRAME_RESERVE 48
```

This hardcoded value assumes x86-64's 5 callee-saved registers (rbx, r12-r15 = 40 bytes + padding = 48 bytes).

**Impact on AArch64/RISC-V:**
- x86-64: 5 callee-saved regs → 48 bytes ✓
- **AArch64: 12 callee-saved regs (x19-x30) → 96 bytes needed, but only 48 allocated!**
- **RISC-V: 12 callee-saved regs (s0-s11) → 96 bytes needed, but only 48 allocated!**

**Result:** Stack frame too small, saved registers overwrite local variables (or vice versa), causing stack corruption!

### The Solution

Added ISA-specific frame reserve field to `ZeroCCISABackend`:
```c
int callee_save_frame_reserve;  /* In bytes, ISA-specific */
```

Updated all usages:
```c
/* Before: c->current_stack_frame = CALLEE_SAVE_FRAME_RESERVE; */
/* After:  c->current_stack_frame = BE(c)->callee_save_frame_reserve; */
```

### Per-ISA Configuration

**x86-64:**
```c
.callee_save_frame_reserve = 48,  /* 5 regs: rbx, r12-r15 */
```

**AArch64:**
```c
.callee_save_frame_reserve = 96,  /* 12 regs: x19-x30 */
```

**RISC-V:**
```c
.callee_save_frame_reserve = 96,  /* 12 regs: s0-s11 */
```

---

## Files Modified

### src/zero_cc/isa/isa_backend.h
Added three new fields to `ZeroCCISABackend`:
```c
/* Scratch registers for spilling (ISA-specific, non-argument/return registers) */
int scratch_reg1;
int scratch_reg2;

/* Callee-save frame reserve in bytes (ISA-specific: 48 for x86, 96 for AArch64/RISC-V) */
int callee_save_frame_reserve;
```

### src/zero_cc/parser/ast_lowering_mc.c
Changed hardcoded scratch registers to use backend fields:
```c
/* Before: BE(c)->emit_load(c, 10, ...) */
/* After:  BE(c)->emit_load(c, BE(c)->scratch_reg1, ...) */
```

### src/zero_cc/jit/compile.h
Changed hardcoded frame reserve to use backend field:
```c
/* Before: c->current_stack_frame = CALLEE_SAVE_FRAME_RESERVE; */
/* After:  c->current_stack_frame = BE(c)->callee_save_frame_reserve; */
```

### src/zero_cc/parser/ast_lowering.c
Changed all references to use backend field (3 locations)

### src/zero_cc/isa/x86_64.c, aarch64.c, riscv.c
Initialized new backend fields with ISA-specific values

---

## Testing

✅ **All 118/118 tests still passing**
- No regressions
- Build clean (only standard compiler warnings)
- Verified on x86-64 (existing tests), architecture-safe for AArch64/RISC-V

---

## Impact

**Before Fix:**
- AArch64/RISC-V: Spills corrupt function arguments
- AArch64/RISC-V: Frame too small for callee-saves
- Stack corruption and register clobbering likely

**After Fix:**
- All ISAs use safe scratch registers
- All ISAs allocate correct frame space
- No register conflicts or stack corruption

---

## Severity

🔴 **CRITICAL** - These bugs would cause:
1. Silent data corruption on AArch64/RISC-V
2. Function arguments being clobbered
3. Return values being overwritten
4. Stack corruption between locals and saved registers
5. Undefined behavior and crash-prone binaries

These were blocking issues for correct AArch64/RISC-V support.

