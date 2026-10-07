# AArch64 Backend Analysis & Refactoring Plan

## What Was Wrong

### 1. Debug Code Left In Production
```c
fprintf(stderr, "DEBUG: aarch64_emit_prologue stack_size=%zu\n", stack_size);
fprintf(stderr, "DEBUG: emit_leave called, offset=%zu\n", me_get_offset(c));
fprintf(stderr, "DEBUG: emit_store_64 src_reg=%d base_reg=%d\n", src_reg, base_reg);
```
**Problem:** Debug prints pollute stderr, slow down compilation, and indicate incomplete testing.

### 2. Broken Prologue Encoding
```c
/* WRONG: stp encoding is malformed */
arm64_emit_u32(c, 0xA9BD7BFD | ((stack_size / 16) << 15));

/* WRONG: sub encoding assumes stack_size < 4096 */
if (stack_size > 0 && stack_size < 4096) {
    arm64_emit_u32(c, 0xD10003FF | ((stack_size & 0xFFF) << 10));
}
```
**Problem:**
- `stp x29, x30, [sp, #-16]!` should be fixed encoding `0xA9BD7BFD`, not OR'd with stack_size
- Stack size > 4095 needs multi-instruction sequence (movk/movz to temp, then sub)
- No handling for frame sizes that exceed immediate encoding

### 3. Push/Pop Using Wrong Instructions
```c
/* Changed from stp/ldp to sub/str */
arm64_emit_u32(c, 0xD10023FF);  /* sub sp, sp, #8 */
arm64_emit_u32(c, 0xF90003E0 | (reg & 0x1F));  /* str xZ, [sp] */
```
**Problem:**
- AArch64 has no single-register push/pop; must use sub/str or stp/ldp
- Current encoding is incomplete (missing proper str encoding)
- Should use `stp xZ, xZ, [sp, #-16]!` for pairs, or `str xZ, [sp, #-8]!` for single

### 4. Manual Relocation Bookkeeping
```c
/* In lea_rip */
if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
    c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
    c->str_relocs[c->str_reloc_count].rodata_offset = offset;
    c->str_relocs[c->str_reloc_count].dst_reg = dst;
    c->str_reloc_count++;
}
```
**Problem:**
- Duplicates ELF writer's relocation logic
- Creates two separate relocation paths (c->relocs vs c->str_relocs)
- Should emit `adr dst, [pc, #offset]` and let ELF writer patch via standard relocation

### 5. Fragile Negative Offset Handling
```c
/* In load/store */
if (offset < 0 && offset >= -4096 && (offset % 8) == 0) {
    /* Use x10 as temporary register */
    arm64_emit_u32(c, 0xD1000000 | (imm12 << 10) | (base_reg << 5) | 10);
    arm64_emit_u32(c, 0xF9400000 | (10 << 5) | dst_reg);
}
```
**Problem:**
- Hardcodes x10 as temp register (clobbers caller-saved register)
- No register allocation coordination
- Should use IR-level address computation or backend scratch register pool

### 6. Multiple Encoding Attempts in Same Function
```c
static void aarch64_emit_not(...) {
    arm64_emit_u32(c, 0xAA1F03E0 | (src << 16) | dst);  /* First attempt */
    arm64_emit_u32(c, 0xCA000000 | (src << 16) | dst);  /* Second attempt */
    arm64_emit_u32(c, 0xF29FFFFF | (dst << 5) | dst);   /* Third attempt */
}
```
**Problem:**
- Emits 3 instructions instead of 1
- Wrong instruction (mvn/eor encoding incorrect)
- Should be: `mvn dst, src` = `0xAA1F03E0 | (src << 16) | dst`

### 7. Missing Backend Functions
Many functions from `isa_backend.h` were not implemented or were stubs:
- `emit_add_imm`, `emit_sub_imm` (needed for immediate arithmetic)
- `emit_inc`, `emit_dec` (needed for loop counters)
- `emit_shr`, `emit_sar` (shift operations)
- `emit_setl/le/g/ge/e/ne` (conditional set)
- `emit_vla_alloc` (variable-length array)
- `emit_atomic_*` (atomic operations)

## Refactoring Recommendations

### Phase 1: Clean Up Existing Code (1-2 days)

1. **Remove all debug prints**
   ```bash
   grep -n "fprintf(stderr" src/zero_cc/isa/aarch64.c
   ```

2. **Fix prologue/epilogue to AAPCS64 standard**
   ```c
   static void aarch64_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
       /* stp x29, x30, [sp, #-16]! */
       arm64_emit_u32(c, 0xA9BD7BFD);
       /* mov x29, sp */
       arm64_emit_u32(c, 0x910003FD);
       /* sub sp, sp, #stack_size (handle >4095 with multi-instruction) */
       if (stack_size <= 4095) {
           arm64_emit_u32(c, 0xD10003FF | ((stack_size / 4) << 10));
       } else {
           /* Emit movz/movk sequence to load stack_size into x16, then sub */
           /* ... */
       }
   }
   ```

3. **Fix push/pop to use proper encodings**
   ```c
   static void aarch64_emit_push_reg(ZeroCCCompiler *c, int reg) {
       /* str xZ, [sp, #-8]! */
       arm64_emit_u32(c, 0xF9BD03E0 | (reg & 0x1F));
   }
   
   static void aarch64_emit_pop_reg(ZeroCCCompiler *c, int reg) {
       /* ldr xZ, [sp], #8 */
       arm64_emit_u32(c, 0xF9C003E0 | (reg & 0x1F));
   }
   ```

### Phase 2: Integration with ELF Writer (2-3 days)

4. **Remove manual relocation bookkeeping**
   - Delete `c->str_relocs` array from compiler struct
   - Use standard `c->relocs` array for all relocations
   - Emit `adr dst, [pc, #rodata_offset]` and record `R_AARCH64_ADR_PREL_LO21`

5. **Coordinate with ELF writer**
   - Check `src/zero_cc/jit/elf_writer.c` for AArch64 relocation handling
   - Ensure all relocation types are implemented:
     - `R_AARCH64_CALL26` (283) - function calls
     - `R_AARCH64_ADR_PREL_LO21` (274) - PC-relative data access
     - `R_AARCH64_ADR_PREL_PG_HI21` - page-relative (for large code)

### Phase 3: Register Allocation Integration (3-4 days)

6. **Implement scratch register pool**
   ```c
   typedef struct {
       int scratch_int_reg;  /* x10 or x11 for temp address computation */
       int scratch_fp_reg;   /* v16 or v17 for temp FP operations */
       bool scratch_in_use;
   } AArch64BackendState;
   ```

7. **Fix negative offset handling**
   - Use scratch register pool instead of hardcoded x10
   - Track scratch register usage across instruction emission
   - Save/restore if needed for callee-saved registers

8. **Implement callee-saved register preservation**
   - Track which callee-saved registers (x19-x28, x29) are used
   - Emit prologue stores and epilogue loads
   - Match x86-64 pattern from `compile.h`

### Phase 4: Complete Backend Functions (5-7 days)

9. **Implement missing opcode handlers** (reference x86_64.c)
   - Arithmetic immediates: `emit_add_imm`, `emit_sub_imm`
   - Increment/decrement: `emit_inc`, `emit_dec`
   - Shift operations: `emit_shl`, `emit_shr`, `emit_sar`
   - Conditional set: `emit_setl`, `emit_setle`, `emit_setg`, `emit_setge`, `emit_sete`, `emit_setne`
   - Logical not: `emit_lnot`
   - VLA allocation: `emit_vla_alloc`
   - Atomic operations: `emit_atomic_cmpxchg`, `emit_atomic_xadd`, `emit_atomic_xchg`

10. **Implement ABI functions**
    - `emit_param_save_int` - save incoming int args to stack
    - `emit_param_save_fp` - save incoming float args to stack
    - `emit_variadic_spill` - spill variadic register args
    - `emit_variadic_al_setup` - set up %al for variadic calls

11. **Test with QEMU**
    ```bash
    # Compile test
    ./build/bin/zerocc -c test/aot/arithmetic.c -o arithmetic.o --target aarch64
    
    # Link with cross-compiler
    aarch64-linux-gnu-gcc -o arithmetic arithmetic.o
    
    # Execute with QEMU
    qemu-aarch64-static arithmetic
    ```

## Architecture Issues to Fix

### Issue 1: Dual Relocation Systems
```
Current: c->relocs (for calls) + c->str_relocs (for data)
Problem: Inconsistent, hard to maintain
Fix: Single c->relocs array with relocation type field
```

### Issue 2: Hardcoded Scratch Registers
```
Current: Always use x10 for temp address computation
Problem: Clobbers caller-saved register without tracking
Fix: Backend state with scratch register pool, coordinated with regalloc
```

### Issue 3: Stack Size Encoding Limit
```
Current: Only supports stack_size < 4096
Problem: Breaks for large functions
Fix: Multi-instruction sequence for large immediates
```

### Issue 4: Debug Code in Production
```
Current: fprintf(stderr, "DEBUG: ...")
Problem: Pollutes output, indicates incomplete testing
Fix: Remove all debug prints, use proper test coverage
```

## Recommended Testing Strategy

1. **Unit tests for instruction encoding**
   - Test each emit_* function produces correct bytes
   - Use golden values from ARM Architecture Reference Manual

2. **Integration tests with QEMU**
   - Compile simple C functions to AArch64
   - Link with aarch64-linux-gnu-gcc
   - Execute with qemu-aarch64-static
   - Compare output with x86-64 JIT results

3. **Differential testing**
   - Use `tools/cross_isa_comparator.py`
   - Compare x86-64 and AArch64 codegen for same C source
   - Verify both produce correct results

## Timeline Estimate

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| Phase 1: Cleanup | 1-2 days | Debug-free, correct prologue |
| Phase 2: ELF Integration | 2-3 days | Working relocations |
| Phase 3: Regalloc Integration | 3-4 days | Scratch register pool |
| Phase 4: Complete Backend | 5-7 days | All opcode handlers |
| Testing | 2-3 days | QEMU validation |
| **Total** | **13-19 days** | **Production-ready AArch64** |

## Priority Order

1. **Critical**: Fix prologue/epilogue (currently broken)
2. **Critical**: Remove debug prints
3. **Critical**: Fix relocation system
4. **High**: Implement missing opcode handlers
5. **Medium**: Scratch register coordination
6. **Low**: Optimization (large stack frames, etc.)

