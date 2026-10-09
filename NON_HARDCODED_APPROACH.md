# Non-Hardcoded Assembly Emission Approach

## Problem

When emitting assembly, we need to use the correct register names for each ISA. The challenge is that:

1. **Backend register info** (`BE(c)->regs->int_regs`) is indexed by **VREG number** (compiler-internal numbering)
2. **Hardware register numbers** (`BE(c)->sp_reg`, `BE(c)->fp_reg`) are **ISA-specific hardware encodings**

These two numbering schemes don't match!

## Example: x86-64

| Hardware Reg # | VREG # | Register Name |
|---------------|--------|---------------|
| 0 (X86_RAX) | 0 | rax |
| 1 (X86_RCX) | 2 | rcx |
| 2 (X86_RDX) | 3 | rdx |
| 3 (X86_RBX) | 1 | rbx |
| 4 (X86_RSP) | 14 | rsp |
| 5 (X86_RBP) | 15 | rbp |
| 6 (X86_RSI) | 4 | rsi |
| 7 (X86_RDI) | 5 | rdi |

If we use `BE(c)->regs->int_regs[BE(c)->sp_reg]` where `sp_reg = 4`, we get `int_regs[4] = "rsi"` instead of "rsp"!

## Solution: ISA-Specific Hardware Register Name Arrays

Each ISA backend provides a **hardware register name array** indexed by hardware register number:

### x86-64
```c
static const char *x86_64_hw_reg_names[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
// Index 4 = "rsp", Index 5 = "rbp" ✓
```

### RISC-V
```c
// int_regs is indexed by VREG, which matches hardware for RISC-V
static const char *sp = BE(c)->regs->int_regs[BE(c)->sp_reg];  // sp_reg = 2 → "sp" ✓
static const char *fp = BE(c)->regs->int_regs[BE(c)->fp_reg];  // fp_reg = 8 → "s0" ✓
```

### AArch64
```c
// x29 (FP) and x31 (SP) are not in int_regs array
// Handle specially with hardcoded names (acceptable for special registers)
fprintf(out, "\tstp x29, x30, [sp, #%zu]!\n", stack_size);
```

## Benefits

1. **No hardcoding**: Register names come from backend tables
2. **ISA-agnostic**: Generic emitter code works for all ISAs
3. **Maintainable**: Add new ISA by providing hardware register name array
4. **Correct**: Uses actual hardware register numbers, not VREG numbers

## Implementation Pattern

```c
static void isa_emit_prologue_asm(ZeroCCCompiler *c, size_t stack_size) {
    FILE *out = c->asm_out ? c->asm_out : stdout;
    
    // Get register names from backend (non-hardcoded!)
    const char *sp = get_hw_reg_name(BE(c)->sp_reg);
    const char *fp = get_hw_reg_name(BE(c)->fp_reg);
    
    // Emit ISA-specific prologue
    fprintf(out, "\tpush %s\n", fp);
    fprintf(out, "\tmov %s, %s\n", sp, fp);
    fprintf(out, "\tsub $%zu, %s\n", stack_size, sp);
}
```

## Summary

The key insight is that **VREG numbering ≠ hardware register numbering**. Each ISA backend must provide a mapping from hardware register numbers to register names, which the generic assembly emitter can then use without hardcoding.
