# IR Design Robustness Analysis

## Overview

This document analyzes the robustness of the current IR design after Phase 2B refactoring.

## Strengths ✓

### 1. ISA-Agnostic Comparison Operations
- **OP_CMP_BRANCH** replaces x86-specific SETcc opcodes
- Condition codes are ISA-neutral (CC_LT, CC_LE, CC_GT, CC_GE, CC_EQ, CC_NE)
- Backends can implement using ISA-specific instructions:
  - x86-64: `cmp` + `setcc`
  - AArch64: `cmp` + `cset`
  - RISC-V: `slt` + branch

### 2. Clean Backend Interface
- `emit_cmp_branch()` added to `isa_backend.h`
- NULL-safe: checks `if (BE(c)->emit_cmp_branch)` before calling
- Consistent with existing backend pattern

### 3. Instruction Structure Flexibility
```c
typedef struct __attribute__((packed)) {
    uint8_t  op;        /* Operation code (0-255) */
    uint8_t  temp_dest; /* Destination temp (0-255, 0xFF if none) */
    uint8_t  temp_src1; /* Source temp 1 (0-255, 0xFF if none) */
    uint8_t  temp_src2; /* Source temp 2 (0-255, 0xFF if none) */
    int16_t  imm;       /* Immediate value (-32768 to 32767) */
    int64_t  imm64;     /* 64-bit immediate value */
    uint16_t label_idx; /* Label index for jumps (0-65535) */
} Instr;
```

- 256 opcodes possible (currently ~51 used)
- 256 temporaries available
- Flexible field reuse (imm used for condition codes in CMP_BRANCH)

## Weaknesses/Issues ✗

### 1. Missing Backend Implementations

**AArch64 Backend:**
```c
// Missing in aarch64.c:
// .emit_cmp_branch = NULL,  // Not implemented!
```

**RISC-V Backend:**
```c
// Missing in riscv.c:
// .emit_cmp_branch = NULL,  // Not implemented!
```

**Risk:** If AArch64 or RISC-V backends are used with comparison operations, they will silently do nothing (NULL check prevents crash, but codegen is broken).

### 2. Implicit Flag Dependency

The x86_64 implementation uses flag state implicitly:
```c
static void x86_emit_cmp_branch(ZeroCCCompiler *c, int dst, int src1, int src2, int cond) {
    if (src1 != 0xFF || src2 != 0xFF) {
        x86_emit_cmp(c, src1, src2);  // Sets flags
    }
    // setcc reads flags - assumes flags are set!
    x86_emit_setcc(c, dst, setcc_codes[cond]);
}
```

**Problem:** This is fragile. If flags are clobbered between cmp and setcc (e.g., by register allocation spills), the result is wrong.

**Better approach:** Make flag dependency explicit in IR:
```c
// Option A: Two explicit instructions
ir_emit_cmp(src1, src2);
ir_emit_setcc_branch(dst, cond);  // Explicitly reads flags

// Option B: Single instruction with explicit flag register
ir_emit_cmp_branch(dst, src1, src2, cond);  // Flags are implicit result of cmp
```

### 3. Condition Code Encoding Limitation

Condition codes stored in `int16_t imm` field:
```c
.ir_emit_cmp_branch(result_temp, left_temp, right_temp, cond);
```

**Issue:** Only 6 condition codes defined, but `int16_t` allows -32768 to 32767.
- Wasted encoding space
- No validation that cond is in valid range (0-5)
- Backend must check bounds

**Better:** Use `uint8_t` for condition code field, or add dedicated field.

### 4. Inconsistent Flag Handling

Current design has multiple ways to set flags:
- `OP_CMP` - sets flags
- `OP_CMP_ZERO` - sets flags
- `OP_CMP_BRANCH` - sets flags AND reads them (x86 only)

**Problem:** No explicit flag register in IR. Flag state is implicit and architecture-dependent.

**Better:** Introduce explicit flag register:
```c
typedef enum {
    REG_FLAGS = 255,  // Special flag register
    // ... other registers
} Register;

// Explicit flag operations
ir_emit_cmp(src1, src2, REG_FLAGS);      // Sets flags
ir_emit_read_flags(dst, REG_FLAGS, cond); // Reads flags
```

### 5. OP_LNOT Implementation Issue

Current implementation:
```c
case TK_BANG:  /* Logical NOT: !operand */
    ir_emit_cmp_zero(operand_temp);
    ir_emit_cmp_branch(result_temp, 0xFF, 0xFF, CC_EQ);
    break;
```

**Problem:** Relies on implicit flag state from CMP_ZERO. If backend reorders instructions or spills registers, flags may be lost.

## Recommendations

### Short-term (Low Risk)

1. **Add NULL implementations for AArch64 and RISC-V**
   ```c
   // aarch64.c
   static void aarch64_emit_cmp_branch(ZeroCCCompiler *c, int dst, int src1, int src2, int cond) {
       // cmp src1, src2; cset dst, cond
       aarch64_emit_cmp(c, src1, src2);
       // cset encoding based on cond
   }
   
   // riscv.c
   static void riscv64_emit_cmp_branch(ZeroCCCompiler *c, int dst, int src1, int src2, int cond) {
       // slt/slti + branch encoding based on cond
   }
   ```

2. **Add validation in x86_64 backend**
   ```c
   if (cond < 0 || cond > 5) {
       fprintf(stderr, "Invalid condition code: %d\n", cond);
       abort();
   }
   ```

### Medium-term (Medium Risk)

3. **Split CMP_BRANCH into two instructions**
   ```c
   // IR layer
   ir_emit_cmp(src1, src2);           // Sets flags
   ir_emit_cond_set(dst, cond);       // Reads flags
   
   // Backend interface
   void (*emit_cond_set)(ZeroCCCompiler *c, int dst, int cond);
   ```

4. **Add flag register to IR**
   ```c
   typedef enum {
       OP_CMP = ...,       // Sets flags
       OP_TEST = ...,      // Sets flags  
       OP_SETCC = ...,     // Reads flags
       // ...
   } Opcode;
   ```

### Long-term (High Risk)

5. **Complete IR redesign for explicit flags**
   - Add flag register to instruction format
   - Make all flag operations explicit
   - Enable better optimization (CSE, DCE on flags)

## Test Coverage

### Current Tests
- ✓ 118 JIT tests pass
- ✓ Atomic operations (cmpxchg) work correctly
- ✓ Comparison operators (<, <=, >, >=, ==, !=) work
- ✓ Logical NOT (!) works

### Missing Tests
- ✗ AArch64 backend comparison (backend incomplete)
- ✗ RISC-V backend comparison (backend incomplete)
- ✗ Cross-ISA comparison verification

## Risk Assessment

| Issue | Risk Level | Impact | Fix Complexity |
|-------|------------|--------|----------------|
| Missing AArch64/RISC-V impl | HIGH | Backend broken | LOW (1 day) |
| Implicit flag dependency | MEDIUM | Subtle bugs | MEDIUM (3 days) |
| Condition code encoding | LOW | Validation issues | LOW (0.5 day) |
| Inconsistent flag handling | MEDIUM | Optimization limits | HIGH (1 week) |

## Conclusion

**Overall Assessment: MODERATELY ROBUST**

The IR design is **functional for x86-64** but has **critical gaps for multi-ISA support**:

1. **Immediate risk:** AArch64 and RISC-V backends will fail silently with comparisons
2. **Medium risk:** Implicit flag dependency makes optimization fragile
3. **Long-term risk:** No explicit flag register limits future optimizations

**Recommendation:** Implement AArch64/RISC-V `emit_cmp_branch()` before using those backends. Consider splitting CMP_BRANCH into CMP + COND_SET for better optimization opportunities.

---

*Generated: 2026-10-06*
*Phase 2B Complete*
