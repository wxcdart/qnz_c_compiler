# SIMD Architecture & Implementation Plan

## Current State Assessment

### ✅ Completed (IR Layer)
- **32 SIMD opcodes defined** (ir.h:100-132)
  - Scalar FP: VADDSS, VSUBSS, VMULSS, VDIVSS, VMOVSS, etc.
  - Packed Single (4x float32): VADDPS, VSUBPS, VMULPS, VDIVPS, etc.
  - Packed Double (2x float64): VADDPD, VSUBPD, VMULPD, VDIVPD, etc.
  - Bitwise: VAND, VOR, VXOR, VANDN
  - Shuffles: VSHUF
  - Conversions: VCVT_SI2SS, VCVT_SS2SI, VCVT_SI2SD, VCVT_SD2SI
  - Memory: VLOAD, VSTORE, VLOADSS, VLOADSD, VSTORESS, VSTORESD
  - Comparisons: VCMPSS, VCMPSD

- **IR emission helpers** in ir.h:ir_emit_vaddss(), ir_emit_vload(), etc.
- **IR printing** support for vector opcodes in ir.c

### ❌ Missing (Machine Code & Assembly)
- **ast_lowering_mc.c**: No lowering for OP_V* opcodes to machine code
- **asm_emitter.c**: No lowering for OP_V* opcodes to assembly text
- **isa_backend.h**: No function pointers for vector operations
- **x86_64_asm.c**: No SSE2/SSE3 instruction emission
- **aarch64_asm.c**: No NEON instruction emission
- **riscv_asm.c**: No RVV instruction emission

---

## Implementation Tasks

### Phase 1: Backend Interface Extensions (isa_backend.h)

#### 1.1 Vector Operation Function Pointers

Add to `ZeroCCISABackend`:

```c
/* Vector Memory Operations */
void (*emit_vload_asm)(ZeroCCCompiler *c, int dst_vreg, int base, int32_t offset, int element_count, FILE *out);
void (*emit_vstore_asm)(ZeroCCCompiler *c, int src_vreg, int base, int32_t offset, int element_count, FILE *out);

/* Vector Integer Arithmetic (packed 32-bit) */
void (*emit_vadd_epi32_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsub_epi32_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmul_epi32_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);

/* Vector Float Arithmetic (packed 32-bit single) */
void (*emit_vadd_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsub_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmul_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdiv_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);

/* Vector Float Arithmetic (packed 64-bit double) */
void (*emit_vadd_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsub_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmul_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdiv_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);

/* Vector Bitwise Operations */
void (*emit_vand_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vor_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vxor_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);

/* Vector Shuffle & Conversion */
void (*emit_vshuf_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int imm8, FILE *out);
void (*emit_vcvt_si2ss_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
void (*emit_vcvt_ss2si_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
void (*emit_vcvt_si2sd_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
void (*emit_vcvt_sd2si_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);

/* Vector Comparison */
void (*emit_vcmp_ps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int predicate, FILE *out);
void (*emit_vcmp_pd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int predicate, FILE *out);

/* Scalar Float in Vector Registers (SSE alignment) */
void (*emit_vaddss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsubss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmulss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdivss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vaddsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vsubsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vmulsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
void (*emit_vdivsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
```

#### 1.2 Vector Register Info

Extend `ISARegInfo`:
```c
const char *vec_regs[32];  /* Vector register names (xmm0-xmm15 on x86, v0-v31 on AArch64/RISC-V) */
int num_vec_regs;          /* Number of vector registers (16 on x86, 32 on AArch64/RISC-V) */
```

---

### Phase 2: Register Allocator Extensions (regalloc.h)

#### 2.1 Add Vector Register Class
```c
#define REG_CLASS_VEC 2  /* Vector registers (XMM/NEON/RVV) */
```

#### 2.2 Expand Interval Tracking
- Add `vec_intervals[]` array for vector register live ranges
- 16 registers on x86, 32 on AArch64/RISC-V
- 16-byte alignment requirement for vector spills

#### 2.3 Vector Spill Slots
- Allocate 16-byte aligned stack slots for vector values
- Track in `callee_save_frame_reserve` or separate `vec_spill_frame_reserve`

---

### Phase 3: ISA-Specific Vector Emission

#### 3.1 x86-64 SSE2 (x86_64_asm.c)

**Vector Load/Store:**
```c
static void x86_emit_vload_asm(ZeroCCCompiler *c, int dst_vreg, int base, int32_t offset, int element_count, FILE *out) {
    /* element_count == 4 → movaps (16 bytes)
       element_count == 2 → movapd (16 bytes)
       element_count == 1 → movss (4 bytes, scalar) */
    const char *xmm_dst = BE(c)->vec_regs[dst_vreg];  /* xmm0, xmm1, etc. */
    fprintf(out, "\tmovaps\t%d(%%%s), %%%s\n", offset, base_reg, xmm_dst);
}
```

**Vector Arithmetic (Packed Single):**
```c
static void x86_emit_vadd_ps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    /* ADDPS: add packed single-precision (4x 32-bit floats) */
    fprintf(out, "\taddps\t%%%s, %%%s\n", BE(c)->vec_regs[src2], BE(c)->vec_regs[dst]);
}
```

**Vector Arithmetic (Packed Double):**
```c
static void x86_emit_vadd_pd_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    /* ADDPD: add packed double-precision (2x 64-bit floats) */
    fprintf(out, "\taddpd\t%%%s, %%%s\n", BE(c)->vec_regs[src2], BE(c)->vec_regs[dst]);
}
```

**Key x86-64 Instructions:**
- Load/Store: movaps, movapd, movss, movsd
- Arithmetic: addps, subps, mulps, divps, addpd, subpd, mulpd, divpd
- Bitwise: andps, orps, xorps, andpd, orpd, xorpd
- Shuffle: shufps, shufpd
- Conversion: cvtsi2ss, cvtss2si, cvtsi2sd, cvtsd2si
- Compare: cmpps, cmppd

---

#### 3.2 AArch64 NEON (aarch64_asm.c)

**Vector Load/Store:**
```c
static void aarch64_emit_vload_asm(ZeroCCCompiler *c, int dst_vreg, int base, int32_t offset, int element_count, FILE *out) {
    /* element_count == 4 → ld1 {v_dst.4s}
       element_count == 2 → ld1 {v_dst.2d} */
    const char *v_dst = BE(c)->vec_regs[dst_vreg];  /* v0, v1, etc. */
    fprintf(out, "\tld1\t{%s.4s}, [x%d, #%d]\n", v_dst, base, offset);
}
```

**Vector Arithmetic:**
```c
static void aarch64_emit_vadd_ps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    /* FADD: floating-point add (4x 32-bit) */
    fprintf(out, "\tfadd\t%s.4s, %s.4s, %s.4s\n", 
            BE(c)->vec_regs[dst], BE(c)->vec_regs[src1], BE(c)->vec_regs[src2]);
}
```

**Key AArch64 Instructions:**
- Load/Store: ld1, st1
- Arithmetic: fadd, fsub, fmul, fdiv (with .4s, .2d, .4h, etc. lane widths)
- Bitwise: and, orr, eor
- Shuffle: ext, zip1, zip2
- Conversion: fcvtzs, scvtf

---

#### 3.3 RISC-V RVV (riscv_asm.c)

**Vector Configuration & Load/Store:**
```c
static void riscv_emit_vload_asm(ZeroCCCompiler *c, int dst_vreg, int base, int32_t offset, int element_count, FILE *out) {
    /* Set vector length: vsetvli t0, x0, e32, m1, ta, ma (for 32-bit elements) */
    fprintf(out, "\tvsetvli\tt0, x0, e32, m1, ta, ma\n");
    /* Load: vle32.v v_dst, (base + offset) */
    fprintf(out, "\tvle32.v\t%s, %d(x%d)\n", BE(c)->vec_regs[dst_vreg], offset, base);
}
```

**Vector Arithmetic:**
```c
static void riscv_emit_vadd_ps_asm(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out) {
    /* Vector add (register-register) */
    fprintf(out, "\tvfadd.vv\t%s, %s, %s\n", 
            BE(c)->vec_regs[dst], BE(c)->vec_regs[src1], BE(c)->vec_regs[src2]);
}
```

**Key RISC-V Instructions:**
- Configuration: vsetvli
- Load/Store: vle32.v, vle64.v, vse32.v, vse64.v
- Arithmetic: vfadd.vv, vfsub.vv, vfmul.vv, vfdiv.vv
- Bitwise: vand.vv, vor.vv, vxor.vv
- Conversion: vfcvt.x.f.v, vfcvt.f.x.v

---

### Phase 4: Assembly Emitter Integration (asm_emitter.c)

Add cases in `asm_emit_instruction()` for all 32 SIMD opcodes:

```c
case OP_VADDSS:
    if (BE(c)->emit_vaddss_asm) {
        int phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
        int phys_src1 = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
        int phys_src2 = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src2);
        BE(c)->emit_vaddss_asm(c, phys_dst, phys_src1, phys_src2, out);
    }
    return;
    
case OP_VLOAD:
    if (BE(c)->emit_vload_asm) {
        int phys_dst = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_dest);
        int phys_base = regalloc_get_reg_for_temp(&c->regalloc, instr->temp_src1);
        int32_t offset = (int32_t)instr->imm64;
        int element_count = (int)(instr->imm64 >> 32);  /* Store element count in upper 32 bits */
        BE(c)->emit_vload_asm(c, phys_dst, phys_base, offset, element_count, out);
    }
    return;

/* ... repeat for all 30 other vector opcodes ... */
```

---

### Phase 5: Auto-Vectorization Pass (opt_vectorize.c)

#### 5.1 Loop Pattern Recognition

Detect vectorizable loops:
```
for (int i = 0; i < N; i++) {
    result[i] = a[i] + b[i];  // Vectorizable: parallel load, add, store
}
```

#### 5.2 Loop Unrolling & Substitution

1. **Detect**: Countable loop with constant step=1, array accesses
2. **Unroll by vector width**: Step 4 for float32 (SSE), step 2 for float64
3. **Substitute IR**:
   ```
   /* Original loop iteration (scalar) */
   i_load = ir_emit_load(result_base, fp, -(i*8));
   a_load = ir_emit_load(a_base, fp, -(i*8));
   b_load = ir_emit_load(b_base, fp, -(i*8));
   sum = ir_emit_fadd(i_load, a_load, b_load);
   ir_emit_store(sum, result_base, -(i*8));
   
   /* Vectorized iteration (4x parallel) */
   vec_a = ir_emit_vload(a_base, fp, -(i*8), 4);  /* Load 4 floats */
   vec_b = ir_emit_vload(b_base, fp, -(i*8), 4);  /* Load 4 floats */
   vec_sum = ir_emit_vaddps(vec_a, vec_b);        /* Add 4 in parallel */
   ir_emit_vstore(vec_sum, result_base, -(i*8), 4);
   ```
4. **Unroll by 4**: `for (i += 4)`
5. **Tail loop**: Scalar loop for remaining iterations

#### 5.3 Integration with opt_run_passes()

```c
/* In compile phase, after regalloc */
if (enable_vectorization) {
    opt_run_vectorization_pass(c);
}
```

---

## Implementation Sequence

### Week 1: Phase 1-2 (Backend Interface & Register Allocator)
- **Days 1-2**: Add 25+ function pointers to isa_backend.h
- **Days 2-3**: Extend regalloc.h for vector register class and spill slots
- **Days 3-5**: Update all 3 ISA backends to initialize new pointers (stubs)
- **Verification**: All 118/118 tests still passing

### Week 2: Phase 3 (ISA-Specific Emission)
- **Days 6-7**: Implement x86-64 SSE2 (20+ functions in x86_64_asm.c)
- **Days 8-9**: Implement AArch64 NEON (20+ functions in aarch64_asm.c)
- **Days 9-10**: Implement RISC-V RVV (20+ functions in riscv_asm.c)
- **Verification**: Unit tests for each ISA

### Week 3: Phase 4-5 (Integration & Vectorization Pass)
- **Days 11-12**: Add 32 cases to asm_emitter.c
- **Days 13-15**: Implement auto-vectorization pass (opt_vectorize.c)
- **Verification**: End-to-end tests with vla_test.c

---

## Success Criteria

✅ **Completion Metrics:**
- All 32 SIMD opcodes have functional assembly emission on x86-64, AArch64, RISC-V
- vla_test.c loops vectorized (4x speedup on x86-64 SSE)
- All 118/118 tests still passing
- No regressions in scalar code paths
- Assembly output validates with `as` and `ld`

---

## Testing Strategy

### Unit Tests
- One test per SIMD opcode per ISA (32 x 3 = 96 tests)
- Verify correct register allocation and spill handling
- Validate assembly syntax for each ISA

### Integration Tests
- Vectorized loop execution matching scalar results
- Tail handling (remainder iterations)
- Nested loops with SIMD
- Mixed scalar/vector code

### Performance Tests
- vla_test.c vectorized loops: 4x+ speedup on x86-64 SSE
- Memory bandwidth utilization
- Register pressure comparison

---

## Risk Mitigation

1. **Regression Risk**: Keep scalar paths unchanged, only add vector paths
2. **ISA Complexity**: Start with x86-64 SSE (simpler), then AArch64 NEON, then RISC-V RVV
3. **Register Pressure**: Spill handling critical - extensive testing with 16 vector registers
4. **Alignment**: Vector spill slots must be 16-byte aligned to prevent undefined behavior

