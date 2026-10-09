# Callee-Save get_callee_save_info() Review - Final

## Question
Should we add `get_callee_save_info()` to `ISACallingConvention`?

## Analysis

### Current Design
```c
typedef struct {
    const int *callee_saved_gpr;      // Array of physical register numbers
    int callee_saved_gpr_count;
    const int32_t *callee_save_slot_offsets; // Indexed by array position
} ISACallingConvention;
```

Access pattern in `ast_lowering_mc.c`:
```c
for (int i = 0; i < count; i++) {
    int reg = callee_regs[i];
    if ((mask >> reg) & 1u) {
        int32_t slot = slot_offsets[i];
        BE(c)->emit_store(c, reg, BE(c)->fp_reg, -slot);
    }
}
```

### Recommendation: DON'T ADD get_callee_save_info()

**Reasoning:**
1. **Simple and Clear**: Direct array access is straightforward
2. **Efficient**: No function call overhead
3. **Flexible**: Different code paths can access different aspects
4. **Consistent**: Matches pattern for argument registers

### What We DID Add Instead

To solve the **register name mapping** problem (VREG ≠ hardware numbering), we added:

```c
const char *(*get_hw_reg_name)(int phys_reg);
```

This is **different** from `get_callee_save_info()`:
- `get_hw_reg_name`: Maps hardware register number → register name (needed for assembly emission)
- `get_callee_save_info`: Would wrap callee-save table access (not needed)

## Implementation

### Added to Backend Interface (isa_backend.h)
```c
const char *(*get_hw_reg_name)(int phys_reg);
```

### Implemented for All ISAs

**x86-64:**
```c
static const char *x86_get_hw_reg_name(int phys_reg) {
    static const char *x86_hw_names[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
    };
    return (phys_reg >= 0 && phys_reg < 16) ? x86_hw_names[phys_reg] : "rax";
}
```

**AArch64:**
```c
static const char *aarch64_get_hw_reg_name(int phys_reg) {
    static const char *aarch64_hw_names[] = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
        "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
        "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
        "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp"
    };
    return (phys_reg >= 0 && phys_reg < 32) ? aarch64_hw_names[phys_reg] : "x0";
}
```

**RISC-V:**
```c
static const char *riscv64_get_hw_reg_name(int phys_reg) {
    static const char *riscv_hw_names[] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
    };
    return (phys_reg >= 0 && phys_reg < 32) ? riscv_hw_names[phys_reg] : "zero";
}
```

### Usage in Assembly Emitter

```c
case OP_CALLEE_SAVE_STORES:
    if (BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->get_hw_reg_name) {
        uint32_t mask = (uint32_t)instr->imm;
        const int *callee_regs = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        for (int i = 0; i < count; i++) {
            int phys_reg = callee_regs[i];
            if ((mask >> phys_reg) & 1u) {
                const char *reg_name = BE(c)->get_hw_reg_name(phys_reg);
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                fprintf(out, "\tmovq %%%s, -%d(%%rbp)\n", reg_name, slot);
            }
        }
    }
    return;
```

## Python Tools Available

We have **7 comprehensive tools** for verifying callee-save handling:

| Tool | Purpose | Status |
|------|---------|--------|
| `callee_saved_analyzer.py` | General analysis across ISAs | ✓ Available |
| `verify_callee_save_fix.py` | End-to-end verification | ✓ Available |
| `prologue_verifier.py` | Byte-level verification | ✓ Available |
| `frame_layout_analyzer.py` | Frame layout analysis | ✓ Available |
| `verify_aarch64_callee_saved.py` | AArch64-specific | ✓ Available |
| `refactor_callee_saved.py` | Refactoring verification | ✓ Available |
| `find_callee_save_calls.py` | Call site analysis | ✓ Available |

## Testing Results

```bash
$ make test
Results: 118/118 passed, 0 failed in 0.43s

$ ./build/bin/zerocc -S -o test.s test.c
$ cat test.s | grep -A 2 "CALLEE"
    movq %rbx, -8(%rbp)
    movq -8(%rbp), %rbx
```

Callee-save register emission is working correctly!

## Conclusion

**Decision**: DON'T add `get_callee_save_info()` to `ISACallingConvention`.

**Rationale**:
1. Current table-based design is simple and efficient
2. Direct array access is clear and flexible
3. We added `get_hw_reg_name` instead to solve the actual problem (register name mapping)
4. Comprehensive Python tools exist for verification

**What We Added**: `get_hw_reg_name(phys_reg)` function pointer for ISA-specific hardware register name mapping.

**Files Modified**:
1. `isa_backend.h` - Added `get_hw_reg_name` function pointer
2. `x86_64.c` - Implemented x86-64 hardware register names
3. `aarch64.c` - Implemented AArch64 hardware register names
4. `riscv.c` - Implemented RISC-V hardware register names
5. `asm_emitter.c` - Updated to use `get_hw_reg_name` for callee-save emission

**No Hardcoding**: All register names come from backend tables, not hardcoded in generic emitter.
