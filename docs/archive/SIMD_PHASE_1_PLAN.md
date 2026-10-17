# SIMD Implementation - Phase 1: Backend Interface Extensions

## Status: READY TO START

### Why Phase 1 is Critical
Phase 1 establishes the foundation for all SIMD emission. Without this interface, phases 2-5 cannot be implemented.

### Phase 1 Detailed Tasks

#### Task 1.1: Vector Register Info (isa_backend.h)
```c
/* Add to ISARegInfo structure: */
const char *vec_regs[32];      /* Vector register names */
int num_vec_regs;              /* Count of vector registers */
```

**Per-ISA Values:**
- x86-64: 16 registers (xmm0-xmm15)
- AArch64: 32 registers (v0-v31)
- RISC-V: 32 registers (v0-v31)

#### Task 1.2: Vector Operation Function Pointers (isa_backend.h)
Add 27 function pointers to `ZeroCCISABackend`:

**Memory Operations (2):**
```c
void (*emit_vload_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, int element_count, FILE *out);
void (*emit_vstore_asm)(ZeroCCCompiler *c, int src, int base, int32_t offset, int element_count, FILE *out);
```

**Packed Single (4x float32) Arithmetic (4):**
```c
void (*emit_vadd_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsub_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmul_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdiv_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
```

**Packed Double (2x float64) Arithmetic (4):**
```c
void (*emit_vadd_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsub_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmul_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdiv_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
```

**Bitwise Operations (3):**
```c
void (*emit_vand_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vor_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vxor_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
```

**Scalar Float in Vector Registers (8):**
```c
void (*emit_vaddss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);  /* scalar float add */
void (*emit_vsubss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmulss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdivss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vaddsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);  /* scalar double add */
void (*emit_vsubsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmulsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdivsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
```

**Shuffle & Conversion (6):**
```c
void (*emit_vshuf_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int imm8, FILE *out);
void (*emit_vcvt_si2ss_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
void (*emit_vcvt_ss2si_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
void (*emit_vcvt_si2sd_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
void (*emit_vcvt_sd2si_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
```

**Comparison (2):**
```c
void (*emit_vcmp_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int predicate, FILE *out);
void (*emit_vcmp_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int predicate, FILE *out);
```

#### Task 1.3: Vector Register Class (regalloc.h)
```c
#define REG_CLASS_VEC 2  /* Vector registers (XMM/NEON/RVV) */
```

#### Task 1.4: Backend Implementations
For each ISA (x86_64.c, aarch64.c, riscv.c):

1. **Initialize vec_regs table:**
```c
/* In x86_64_backend.regs: */
.vec_regs = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
},
.num_vec_regs = 16,
```

2. **Initialize function pointers to stubs:**
```c
.emit_vload_asm = x86_emit_vload_asm_stub,
.emit_vstore_asm = x86_emit_vstore_asm_stub,
.emit_vadd_ps_asm = x86_emit_vadd_ps_asm_stub,
/* ... etc ... */
```

3. **Implement stub functions:**
```c
static void x86_emit_vload_asm_stub(ZeroCCCompiler *c, int dst, int base, 
                                    int32_t offset, int element_count, FILE *out) {
    /* TODO: Implement in Phase 3 */
    (void)c; (void)dst; (void)base; (void)offset; (void)element_count; (void)out;
}
```

### Implementation Order

**Day 1-2**: Update isa_backend.h
- Add vec_regs[] and num_vec_regs to ISARegInfo
- Add 27 function pointers to ZeroCCISABackend

**Day 2-3**: Update regalloc.h
- Add REG_CLASS_VEC definition

**Day 3-4**: Update x86_64.c, aarch64.c, riscv.c
- Add vector register tables
- Initialize function pointers
- Implement stub functions

**Day 5**: Testing & Verification
- Build should succeed
- All 118/118 tests should still pass
- No functional changes (stubs do nothing yet)

### Expected Output

After Phase 1:
- Backend interface supports vector operations
- All 3 ISAs have stub implementations
- Foundation ready for Phase 2
- Build passes, tests pass
- No new functionality yet

### Files Modified

1. `src/zero_cc/isa/isa_backend.h` - +40 lines (vec_regs, function pointers)
2. `src/zero_cc/parser/regalloc.h` - +1 line (REG_CLASS_VEC)
3. `src/zero_cc/isa/x86_64.c` - +60 lines (table, stubs)
4. `src/zero_cc/isa/aarch64.c` - +60 lines (table, stubs)
5. `src/zero_cc/isa/riscv.c` - +60 lines (table, stubs)

### Success Criteria

✅ isa_backend.h has vector register info and 27 function pointers
✅ All 3 backends initialized with vector register tables
✅ All 27 function pointers initialized (stubs)
✅ Build succeeds without errors
✅ All 118/118 tests pass
✅ No functional changes (stubs are empty)

### Risk Assessment

**Low Risk**: This phase is additive only
- No existing code changed (only extended)
- Stubs mean no functional impact
- Clean separation from existing functionality
- Easy to review: just tables and stubs

### Notes for Implementation

1. Use exact function pointer signatures as listed above
2. Keep stubs minimal (just parameter cast to void to avoid unused warnings)
3. Use consistent naming: `isa_emit_opcode_asm()` pattern
4. Document each function pointer with comment
5. Verify no compiler warnings after build

