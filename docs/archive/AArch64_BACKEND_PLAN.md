# AArch64 Backend Implementation Plan

## Overview

This document outlines the plan for implementing the AArch64 backend for the zero_cc C compiler.

## Prerequisites ✓

- ✓ IR is ISA-agnostic (no x86-specific opcodes)
- ✓ Explicit flag tracking (flags_in/flags_out)
- ✓ OP_CMP + OP_COND_SET (not combined CMP_BRANCH)
- ✓ OP_JCOND for conditional jumps
- ✓ 118 JIT tests passing on x86-64

## Implementation Phases

### Phase 1: Core Comparison Operations (1-2 days)

**Critical for correctness - implement first:**

1. **emit_cmp**
   ```c
   static void aarch64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
       // cmp src1, src2 → subs wzr, src1, src2
       aarch64_emit(c, "subs wzr, %w[src1], %w[src2]", src1, src2);
   }
   ```

2. **emit_cond_set**
   ```c
   static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
       static const char *cond_names[] = {"mi", "le", "gt", "ge", "eq", "ne"};
       aarch64_emit(c, "cset %w[dst], %s", dst, cond_names[cond]);
   }
   ```

3. **emit_jcond**
   ```c
   static void aarch64_emit_jcond(ZeroCCCompiler *c, int cond, uint16_t label_idx) {
       static const char *cond_names[] = {"mi", "le", "gt", "ge", "eq", "ne"};
       aarch64_emit(c, "b.%s L_%d", cond_names[cond], label_idx);
   }
   ```

**Test:** Run all 118 JIT tests (will fail on non-x86, but verifies codegen)

### Phase 2: Data Movement (0.5 days)

1. **emit_mov_imm** - MOVZ/MOVK sequence for 64-bit immediates
2. **emit_mov** - Register move (mov wdst, wsrc)
3. **emit_push** - sub sp, sp, #8; str wsrc, [sp]
4. **emit_pop** - ldr wdst, [sp]; add sp, sp, #8

### Phase 3: Arithmetic (1 day)

1. **emit_add** - add wdst, wsrc1, wsrc2
2. **emit_add_imm** - add wdst, wsrc, #imm
3. **emit_sub** - sub wdst, wsrc1, wsrc2
4. **emit_sub_imm** - sub wdst, wsrc, #imm
5. **emit_mul** - mul wdst, wsrc1, wsrc2
6. **emit_div** - sdiv wdst, wsrc1, wsrc2
7. **emit_mod** - sdiv + msub sequence

### Phase 4: Logical (0.5 days)

1. **emit_and** - and wdst, wsrc1, wsrc2
2. **emit_or** - orr wdst, wsrc1, wsrc2
3. **emit_xor** - eor wdst, wsrc1, wsrc2
4. **emit_shl** - lsl wdst, wsrc1, wsrc2
5. **emit_shr** - lsr wdst, wsrc1, wsrc2
6. **emit_sar** - asr wdst, wsrc1, wsrc2

### Phase 5: Function Operations (0.5 days)

1. **emit_call** - bl function
2. **emit_ret** - ret
3. **emit_prologue** - stp x29, x30, [sp, #-16]!; mov x29, sp
4. **emit_epilogue** - ldp x29, x30, [sp], #16; ret

### Phase 6: Memory Operations (0.5 days)

1. **emit_load** - ldr wdst, [sp, #offset]
2. **emit_store** - str wsrc, [sp, #offset]
3. **emit_lea** - add wdst, sp, #offset

### Phase 7: Floating Point (1 day)

1. **emit_fmov_imm** - ldr sdst, [rodata]
2. **emit_fadd** - fadd sdst, ssrc1, ssrc2
3. **emit_fsub** - fsub sdst, ssrc1, ssrc2
4. **emit_fmul** - fmul sdst, ssrc1, ssrc2
5. **emit_fdiv** - fdiv sdst, ssrc1, ssrc2

### Phase 8: ELF Output (1-2 days)

1. ELF header (EM_AARCH64 = 183)
2. Section headers (.text, .data, .rodata)
3. Symbol table (.symtab, .strtab)
4. Relocations (R_AARCH64_ADR_PREL_PG_HI21, R_AARCH64_CALL26)

## Condition Code Mapping

| IR Code | Value | AArch64 | Description |
|---------|-------|---------|-------------|
| CC_LT | 0 | mi | Signed less than (N!=V) |
| CC_LE | 1 | le | Signed less or equal |
| CC_GT | 2 | gt | Signed greater than (Z==0 && N==V) |
| CC_GE | 3 | ge | Signed greater or equal (N==V) |
| CC_EQ | 4 | eq | Equal (Z==1) |
| CC_NE | 5 | ne | Not equal (Z==0) |

## Test Plan

### Unit Tests
1. Comparison operators (<, <=, >, >=, ==, !=)
2. Logical NOT (!)
3. If-else statements
4. While loops
5. For loops

### Integration Tests
- Run all 118 JIT tests
- Compare output with x86-64
- Verify ELF format with readelf

## Tools

### Helper Tool
```bash
python3 tools/aarch64_backend_helper.py --all
```

### Cross-ISA Comparison
```bash
python3 tools/isa_diff.py test.c --isas x86_64 aarch64
```

## Timeline

| Phase | Description | Duration |
|-------|-------------|----------|
| 1 | Core Comparison | 1-2 days |
| 2 | Data Movement | 0.5 days |
| 3 | Arithmetic | 1 day |
| 4 | Logical | 0.5 days |
| 5 | Function | 0.5 days |
| 6 | Memory | 0.5 days |
| 7 | Floating Point | 1 day |
| 8 | ELF Output | 1-2 days |
| **Total** | | **6-8 days** |

## Risk Assessment

| Risk | Level | Mitigation |
|------|-------|------------|
| Condition code errors | MEDIUM | Test each comparison operator |
| ELF relocations | HIGH | Use readelf to verify |
| Stack alignment | MEDIUM | Follow AAPCS64 |
| Calling convention | HIGH | Follow AAPCS64 strictly |

## Success Criteria

- ✓ All 118 JIT tests pass on AArch64
- ✓ ELF output is valid (readelf -h shows EM_AARCH64)
- ✓ Code generation matches expected patterns
- ✓ No segmentation faults

## References

- [AArch64 Architecture Reference Manual](https://developer.arm.com/documentation/ddi0596/)
- [AAPCS64 Calling Convention](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
- [LLVM AArch64 Backend](https://github.com/llvm/llvm-project/tree/main/llvm/lib/Target/AArch64)

---

*Created: 2026-10-06*
*Status: Ready to implement*
