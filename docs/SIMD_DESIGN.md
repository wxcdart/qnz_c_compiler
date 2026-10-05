# ZeroCC SIMD Vectorization Design

## Overview

This document outlines the design for adding SIMD vectorization support to ZeroCC, building on the existing scalar SSE infrastructure.

## Current Architecture Analysis

### What ZeroCC Already Has

| Component | Status | SIMD-Relevant |
|-----------|--------|---------------|
| **XMM registers** | ✅ xmm0-xmm15 | Can be reused for 128-bit vectors |
| **FP opcodes** | ✅ FADD, FMUL, etc. | Scalar only, need vector variants |
| **Register allocator** | ✅ VREG_F0-VREG_F7 | Can be extended for vector VREGs |
| **x86-64 backend** | ✅ SSE for FP | Can add SSE2 integer SIMD |
| **Optimization passes** | ✅ LICM, CSE | Can add vectorization pass |

### What's Missing

| Component | Needed For |
|-----------|------------|
| SIMD IR opcodes | Vector operations (VADD, VMUL, etc.) |
| Vector types | `__m128i`, `__m128` in IR |
| Vector register allocator | Track vector VREGs separately |
| Auto-vectorization pass | Detect loops for vectorization |
| SSE2 instruction emission | paddd, pmuludq, movdqa, etc. |

## Design Principles

### 1. Minimal IR Changes

Leverage existing scalar FP infrastructure:
- Reuse XMM registers (already allocated)
- Extend IR opcodes with vector variants
- Keep scalar and vector paths separate

### 2. SSE2 First, AVX Later

Start with 128-bit SSE2 (widely supported):
- 4 × int32, 2 × int64, 4 × float per register
- All x86-64 CPUs support SSE2 (since 2001)
- Add AVX (256-bit) as optimization later

### 3. Intrinsics-First Approach

Support GCC-style intrinsics first:
```c
__m128i v = _mm_set_epi32(a, b, c, d);
__m128i sum = _mm_add_epi32(v1, v2);
```

Then add auto-vectorization:
```c
// Auto-vectorized by compiler:
for (int i = 0; i < N; i++) {
    sum += arr[i];
}
```

## IR Design

### New Vector Types

Add to `src/zero_cc/types/types.h`:

```c
typedef enum {
    TYPE_INT32_VEC4,    // __m128i - 4 × int32
    TYPE_FLOAT_VEC4,    // __m128  - 4 × float
    TYPE_INT64_VEC2,    // __m128i - 2 × int64
    TYPE_DOUBLE_VEC2,   // __m128d - 2 × double
} VectorType;
```

### New Vector Opcodes

Add to `src/zero_cc/parser/ir.h`:

```c
/* Vector Load/Store */
OP_VLOAD,           /* Load vector from memory (movdqa) */
OP_VSTORE,          /* Store vector to memory (movdqa) */
OP_VLOAD_UNALIGNED, /* Load unaligned (movdqu) */

/* Vector Arithmetic */
OP_VADD_INT,        /* Vector add integers (paddd) */
OP_VSUB_INT,        /* Vector subtract (psubd) */
OP_VMUL_INT,        /* Vector multiply (pmuludq) */
OP_VADD_FLOAT,      /* Vector add floats (addps) */
OP_VMUL_FLOAT,      /* Vector multiply (mulps) */

/* Vector Shuffling */
OP_VSHUFFLE,        /* Shuffle vector elements (pshufd) */
OP_VPERM,           /* Permute vector elements (permute) */

/* Vector Conversions */
OP_VCVT_INT_TO_FLOAT,  /* Convert int to float (cvtdq2ps) */
OP_VCVT_FLOAT_TO_INT,  /* Convert float to int (cvtps2dq) */

/* Vector Reduction */
OP_VREDUCE_ADD,     /* Reduce vector to scalar (add horizontal) */
OP_VREDUCE_MUL,     /* Reduce vector to scalar (mul horizontal) */
```

### Instruction Format

Extend `Instr` struct in `ir.h`:

```c
typedef struct {
    Opcode op;
    int temp_dest;      /* Destination VREG (scalar or vector) */
    int temp_src1;      /* Source VREG 1 */
    int temp_src2;      /* Source VREG 2 */
    int64_t imm;        /* Immediate value */
    int label_idx;      /* Label index for jumps */
    
    /* NEW: Vector-specific fields */
    VectorType vec_type;    /* Vector type (TYPE_INT32_VEC4, etc.) */
    int vec_lane;           /* Lane index for scalar extraction */
} Instr;
```

## Register Allocator Design

### Vector VREGs

Extend `lowering_state.h`:

```c
typedef struct {
    int next_vreg;          /* Next scalar VREG */
    int next_vreg_vec;      /* Next vector VREG */
    int vreg_alloc[256];    /* Scalar VREG allocation */
    int vreg_vec_alloc[32]; /* Vector VREG allocation (xmm0-xmm31) */
    bool vreg_vec_used[16]; /* Track used XMM registers */
} RegAllocState;
```

### Allocation Rules

1. **Separate pools**: Scalar VREGs and vector VREGs tracked separately
2. **XMM reuse**: Vector VREGs use same XMM registers as scalar FP
3. **Spilling**: Vector spills use 16-byte aligned stack slots
4. **Callee-save**: XMM6-XMM15 are callee-saved (System V ABI)

## Backend Design

### SSE2 Instruction Emission

Add to `src/zero_cc/isa/x86_64.c`:

```c
/* Vector Load (movdqa) */
static void x86_emit_vload(ZeroCCCompiler *c, int dest_xmm, int base_reg, int offset) {
    /* REX.W + 0F 6F /r (movdqa xmm, m128) */
    me_emit_u8(c, 0x66);  /* SSE2 prefix */
    if (dest_xmm >= 8) me_emit_u8(c, 0x44);  /* REX.R for xmm8-xmm15 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x6F);
    me_emit_modrm(c, 3, dest_xmm, base_reg);
    me_emit_sib(c, 0, 4, base_reg);  /* [base + offset] */
    me_emit_offset(c, offset);
}

/* Vector Add Integers (paddd) */
static void x86_emit_vadd_int(ZeroCCCompiler *c, int dest_xmm, int src_xmm) {
    /* 0F FE /r (paddd xmm1, xmm2) */
    me_emit_u8(c, 0x66);  /* SSE2 prefix */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xFE);
    me_emit_modrm(c, 3, dest_xmm, src_xmm);
}

/* Vector Multiply (pmuludq) */
static void x86_emit_vmul_int(ZeroCCCompiler *c, int dest_xmm, int src_xmm) {
    /* 0F D4 /r (pmuludq xmm1, xmm2) */
    me_emit_u8(c, 0x66);  /* SSE2 prefix */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xD4);
    me_emit_modrm(c, 3, dest_xmm, src_xmm);
}
```

### Backend Function Pointers

Add to `isa_backend.h`:

```c
typedef struct {
    /* Existing functions */
    void (*emit_prologue)(ZeroCCCompiler *c, int frame_size);
    void (*emit_epilogue)(ZeroCCCompiler *c);
    
    /* NEW: Vector functions */
    void (*emit_vload)(ZeroCCCompiler *c, int dest_xmm, int base, int offset);
    void (*emit_vstore)(ZeroCCCompiler *c, int src_xmm, int base, int offset);
    void (*emit_vadd_int)(ZeroCCCompiler *c, int dest_xmm, int src_xmm);
    void (*emit_vsub_int)(ZeroCCCompiler *c, int dest_xmm, int src_xmm);
    void (*emit_vmul_int)(ZeroCCCompiler *c, int dest_xmm, int src_xmm);
} ISABackend;
```

## Optimization Pass Design

### Loop Vectorization Pass

Create `src/zero_cc/parser/opt_vectorize.c`:

```c
/* Detect vectorizable loops */
static bool loop_is_vectorizable(LoopInfo *loop) {
    /* Check for:
     * 1. Simple induction variable
     * 2. No control flow inside loop
     * 3. Memory accesses are sequential
     * 4. No function calls
     */
    
    if (loop_has_control_flow(loop)) return false;
    if (loop_has_function_calls(loop)) return false;
    if (!loop_has_sequential_memory(loop)) return false;
    
    return true;
}

/* Vectorize a loop */
static void vectorize_loop(ZeroCCCompiler *c, LoopInfo *loop) {
    /* 1. Replace scalar ops with vector ops
     * 2. Adjust loop bounds (N/4 for 4-wide vectors)
     * 3. Add reduction at end
     */
    
    for (int i = 0; i < loop->body_count; i++) {
        Instr *instr = ir_get(loop->body_indices[i]);
        
        switch (instr->op) {
            case OP_ADD:
                /* Replace with OP_VADD_INT */
                instr->op = OP_VADD_INT;
                instr->vec_type = TYPE_INT32_VEC4;
                break;
            case OP_MUL:
                /* Replace with OP_VMUL_INT */
                instr->op = OP_VMUL_INT;
                instr->vec_type = TYPE_INT32_VEC4;
                break;
            case OP_LOAD_32:
                /* Replace with OP_VLOAD */
                instr->op = OP_VLOAD;
                instr->vec_type = TYPE_INT32_VEC4;
                break;
            case OP_STORE_32:
                /* Replace with OP_VSTORE */
                instr->op = OP_VSTORE;
                instr->vec_type = TYPE_INT32_VEC4;
                break;
        }
    }
    
    /* Adjust loop count: N -> N/4 */
    adjust_loop_count(loop, 4);
    
    /* Add reduction pass */
    add_reduction_at_exit(c, loop);
}
```

### Integration with Pass Pipeline

Update `opt_pass.c`:

```c
static const OptPass g_opt_passes[] = {
    { "constant_fold", opt_pass_constant_fold, 1, 10 },
    { "dce", opt_pass_dce, 1, 30 },
    { "cse", opt_pass_cse, 1, 40 },
    { "licm", opt_pass_licm, 1, 50 },
    { "strength_reduction", opt_pass_strength_reduction, 1, 60 },
    { "branch_simplify", opt_pass_branch_simplify, 1, 70 },
    /* NEW: Vectorization (only at -O3) */
    { "vectorize", opt_pass_vectorize, 0, 80 },
};
```

## Implementation Phases

### Phase 1: SIMD IR Infrastructure (2 days)

- [ ] Add vector types to `types.h`
- [ ] Add vector opcodes to `ir.h`
- [ ] Extend `Instr` struct with `vec_type`
- [ ] Add vector instruction printers

### Phase 2: Backend Support (3 days)

- [ ] Add SSE2 instruction emission to `x86_64.c`
- [ ] Add vector backend functions to `isa_backend.h`
- [ ] Implement vector load/store
- [ ] Implement vector arithmetic (add, sub, mul)

### Phase 3: Register Allocator (2 days)

- [ ] Extend `RegAllocState` for vector VREGs
- [ ] Add vector register allocation
- [ ] Implement vector spilling (16-byte aligned)
- [ ] Handle XMM callee-save registers

### Phase 4: Auto-Vectorization (3 days)

- [ ] Create `opt_vectorize.c` pass
- [ ] Detect vectorizable loops
- [ ] Transform scalar loops to vector loops
- [ ] Add reduction at loop exit

### Phase 5: Testing & Tuning (2 days)

- [ ] Test with call_heavy.c
- [ ] Benchmark performance (target: 2-4x speedup)
- [ ] Fix edge cases (unaligned access, odd loop counts)
- [ ] Add more vector intrinsics

## Example: call_heavy.c Vectorization

### Before (Scalar)

```c
// ZeroCC IR (scalar):
for (int i = 0; i < 100; i++) {
    int t1 = i * i;      // OP_MUL
    int t2 = (i % 10);
    int t3 = t2 * t2;    // OP_MUL
    t3 = t3 * (i % 10);  // OP_MUL
    sum = sum + t1;      // OP_ADD
    sum = sum + t3;      // OP_ADD
}
```

### After (Vectorized)

```c
// ZeroCC IR (vectorized):
for (int i = 0; i < 25; i++) {  // 100/4 = 25 iterations
    // Load 4 values at once
    v_i = VLOAD(&i_array[i*4]);          // [i, i+1, i+2, i+3]
    
    // Compute squares in parallel
    v_t1 = VMUL_INT(v_i, v_i);           // [i², (i+1)², (i+2)², (i+3)²]
    
    // Accumulate
    v_sum = VADD_INT(v_sum, v_t1);       // Add 4 squares at once
}

// Reduce at end
sum = VREDUCE_ADD(v_sum);  // [a+b+c+d] → scalar
```

### Assembly Comparison

**Scalar (current ZeroCC):**
```asm
mov     eax, edi      ; load i
imul    eax, eax      ; i * i
add     edx, eax      ; sum += i²
inc     edi           ; i++
```

**Vectorized (with SIMD):**
```asm
movdqa  xmm0, [i_array + rdi*4]  ; load [i, i+1, i+2, i+3]
movdqa  xmm1, xmm0               ; copy
pmuludq xmm0, xmm1               ; [i², (i+1)², (i+2)², (i+3)²]
paddd   xmm2, xmm0               ; sum += 4 squares at once
add     rdi, 4                   ; i += 4
```

**Speedup: 4×** (4 iterations per loop)

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Alignment issues | High | Use movdqu for unaligned loads |
| Odd loop counts | Medium | Handle remainder iterations |
| Register pressure | Medium | Spill vectors to stack |
| Complex reductions | High | Start with simple sum/mul |

## Success Criteria

- [ ] call_heavy.c passes (exit code 17)
- [ ] 2-4× speedup on loop-heavy code
- [ ] All existing tests still pass (118/118)
- [ ] Vector intrinsics work (`_mm_add_epi32`, etc.)
- [ ] Auto-vectorization works at -O3

## References

- Intel SDM Volume 2: SSE2 Instruction Set
- GCC Vector Extensions: https://gcc.gnu.org/onlinedocs/gcc/Vector-Extensions.html
- LLVM Loop Vectorization: https://llvm.org/docs/LoopVectorization.html

---

*Last updated: 2024-10-05*
*Status: Design complete, ready for implementation*
*Target: Sprint 10 (Weeks 37-38)*

## Multi-ISA Support

### SIMD Across Architectures

| ISA | SIMD Extension | Vector Width | Status |
|-----|----------------|--------------|--------|
| **x86-64** | SSE2 | 128-bit | ✅ Primary target |
| **x86-64** | AVX | 256-bit | 📅 Future |
| **x86-64** | AVX-512 | 512-bit | 📅 Future |
| **AArch64** | NEON | 128-bit | 📅 Phase 2 |
| **AArch64** | SVE | 128-2048-bit | 📅 Future |
| **RISC-V** | RVV | 128-512-bit | 📅 Future |

### Design for Portability

The IR layer must be **ISA-agnostic**:

```c
/* IR opcodes are universal - same for all ISAs */
OP_VADD_INT,        /* Vector add integers */
OP_VMUL_INT,        /* Vector multiply */
OP_VLOAD,           /* Vector load from memory */
OP_VSTORE,          /* Vector store to memory */

/* Backend-specific implementation */
void (*emit_vadd_int)(ZeroCCCompiler *c, int dest_vec, int src_vec);
```

### AArch64 NEON Support

**Register Mapping:**
```c
/* XMM registers (x86-64) → Q registers (AArch64) */
xmm0 → q0,  xmm1 → q1,  ...,  xmm15 → q15
```

**Instruction Mapping:**
```c
/* x86-64 SSE2 → AArch64 NEON */
paddd   →   add q0, q1, q2          /* vector add */
pmuludq →   mul q0, q1, q2          /* vector multiply */
movdqa  →   ld1 {v0.4s}, [x0]       /* vector load */
movdqa  →   st1 {v0.4s}, [x0]       /* vector store */
```

**AArch64 Backend Implementation:**
```c
/* AArch64 vector add (add q0, q1, q2) */
static void aarch64_emit_vadd_int(ZeroCCCompiler *c, int dest_q, int src_q) {
    /* 0F 4C 00 00 (add v0.4s, v1.4s, v2.4s) */
    me_emit_u8(c, 0x4E);
    me_emit_u8(c, 0x2C);
    me_emit_u8(c, 0x00 | dest_q);
    me_emit_u8(c, 0x20 | src_q);
}
```

### RISC-V RVV Support

**Register Mapping:**
```c
/* XMM registers (x86-64) → V registers (RISC-V) */
xmm0 → v0,  xmm1 → v1,  ...,  xmm15 → v15
```

**Instruction Mapping:**
```c
/* x86-64 SSE2 → RISC-V RVV */
paddd   →   vadd.vv v0, v1, v2      /* vector add */
pmuludq →   vmul.vv v0, v1, v2      /* vector multiply */
movdqa  →   vlw.v v0, (a0)          /* vector load */
movdqa  →   vsw.v v0, (a0)          /* vector store */
```

**RISC-V Backend Implementation:**
```c
/* RISC-V vector add (vadd.vv v0, v1, v2) */
static void riscv_emit_vadd_int(ZeroCCCompiler *c, int dest_v, int src_v) {
    /* 0x47 (vadd.vv opcode) */
    me_emit_u8(c, 0x47);
    me_emit_u8(c, 0x00 | dest_v);
    me_emit_u8(c, 0x00 | src_v);
}
```

### ISA-Agnostic Vector Types

```c
/* Vector types are ISA-agnostic */
typedef enum {
    TYPE_INT32_VEC4,    /* 4 × int32 - works on all ISAs */
    TYPE_FLOAT_VEC4,    /* 4 × float - works on all ISAs */
    TYPE_INT64_VEC2,    /* 2 × int64 - works on all ISAs */
    TYPE_DOUBLE_VEC2,   /* 2 × double - works on all ISAs */
} VectorType;

/* Backend specifies actual implementation */
typedef struct {
    int vector_width;       /* 128, 256, 512 bits */
    int max_lanes;          /* Max vector lanes */
    bool has_vector_regs;   /* Does ISA support vectors? */
} ISAVectorSupport;
```

### Implementation Strategy

#### Phase 1: x86-64 SSE2 (Sprint 10)
- [ ] Implement SSE2 backend
- [ ] Test with call_heavy.c
- [ ] Benchmark performance

#### Phase 2: AArch64 NEON (Future)
- [ ] Add NEON backend functions
- [ ] Map XMM → Q registers
- [ ] Translate SSE2 → NEON instructions
- [ ] Test on AArch64 hardware/QEMU

#### Phase 3: RISC-V RVV (Future)
- [ ] Add RVV backend functions
- [ ] Map XMM → V registers
- [ ] Translate SSE2 → RVV instructions
- [ ] Test on RISC-V hardware/QEMU

### Backend Abstraction

```c
/* isa_backend.h - Vector support abstraction */
typedef struct {
    /* Existing backend functions */
    void (*emit_prologue)(ZeroCCCompiler *c, int frame_size);
    void (*emit_epilogue)(ZeroCCCompiler *c);
    
    /* Vector functions (NEW) */
    void (*emit_vload)(ZeroCCCompiler *c, int dest_vec, int base, int offset);
    void (*emit_vstore)(ZeroCCCompiler *c, int src_vec, int base, int offset);
    void (*emit_vadd_int)(ZeroCCCompiler *c, int dest_vec, int src_vec);
    void (*emit_vsub_int)(ZeroCCCompiler *c, int dest_vec, int src_vec);
    void (*emit_vmul_int)(ZeroCCCompiler *c, int dest_vec, int src_vec);
    
    /* Vector support info */
    ISAVectorSupport vector_support;
} ISABackend;

/* x86_64.c */
static const ISABackend x86_64_backend = {
    .emit_prologue = x86_emit_prologue,
    .emit_epilogue = x86_emit_epilogue,
    .emit_vload = x86_emit_vload,
    .emit_vstore = x86_emit_vstore,
    .emit_vadd_int = x86_emit_vadd_int,
    .emit_vmul_int = x86_emit_vmul_int,
    .vector_support = {
        .vector_width = 128,
        .max_lanes = 4,
        .has_vector_regs = true,
    },
};

/* aarch64.c */
static const ISABackend aarch64_backend = {
    .emit_prologue = aarch64_emit_prologue,
    .emit_epilogue = aarch64_emit_epilogue,
    .emit_vload = aarch64_emit_vload,
    .emit_vstore = aarch64_emit_vstore,
    .emit_vadd_int = aarch64_emit_vadd_int,
    .emit_vmul_int = aarch64_emit_vmul_int,
    .vector_support = {
        .vector_width = 128,
        .max_lanes = 4,
        .has_vector_regs = true,
    },
};

/* riscv.c */
static const ISABackend riscv64_backend = {
    .emit_prologue = riscv64_emit_prologue,
    .emit_epilogue = riscv64_emit_epilogue,
    .emit_vload = riscv64_emit_vload,
    .emit_vstore = riscv64_emit_vstore,
    .emit_vadd_int = riscv64_emit_vadd_int,
    .emit_vmul_int = riscv64_emit_vmul_int,
    .vector_support = {
        .vector_width = 128,  /* Configurable at runtime */
        .max_lanes = 4,
        .has_vector_regs = true,  /* Requires RVV extension */
    },
};
```

### Fallback for Non-SIMD ISAs

If an ISA doesn't support SIMD:

```c
/* opt_vectorize.c - Check if vectorization is supported */
static bool can_vectorize(ZeroCCCompiler *c) {
    return BE(c)->vector_support.has_vector_regs;
}

/* If no SIMD support, skip vectorization pass */
static void opt_pass_vectorize(ZeroCCCompiler *c) {
    if (!can_vectorize(c)) {
        return;  /* Skip vectorization */
    }
    /* ... vectorization logic ... */
}
```

### Stack Allocation Multi-ISA Support

Stack allocation works similarly across all ISAs:

| Aspect | x86-64 | AArch64 | RISC-V |
|--------|--------|---------|--------|
| **Stack pointer** | RSP | SP | SP |
| **Frame pointer** | RBP | FP/X29 | FP/X8 |
| **Alignment** | 16-byte | 16-byte | 16-byte |
| **Red zone** | 128 bytes | None | None |
| **Page probing** | OS-dependent | OS-dependent | OS-dependent |

**Backend-specific prologue:**
```c
/* x86_64.c */
static void x86_emit_prologue(ZeroCCCompiler *c, int frame_size) {
    me_emit_u8(c, 0x55);        /* push rbp */
    me_emit_u8(c, 0x48);        /* REX.W */
    me_emit_u8(c, 0x89);        /* mov rbp, rsp */
    me_emit_u8(c, 0xE5);
    /* sub rsp, frame_size */
}

/* aarch64.c */
static void aarch64_emit_prologue(ZeroCCCompiler *c, int frame_size) {
    /* stp x29, x30, [sp, #-16]! */
    /* mov x29, sp */
    /* sub sp, sp, #frame_size */
}

/* riscv.c */
static void riscv64_emit_prologue(ZeroCCCompiler *c, int frame_size) {
    /* addi sp, sp, -frame_size */
    /* sd ra, 0(sp) */
    /* sd s0, 8(sp) */
    /* addi s0, sp, frame_size */
}
```

### Conclusion

**Yes, the design works for AArch64 and RISC-V!**

Key points:
1. **IR layer is ISA-agnostic** - Same opcodes for all ISAs
2. **Backend implementations differ** - Each ISA has its own instruction emission
3. **SSE2 → NEON → RVV mapping** - Similar vector operations, different encodings
4. **Fallback support** - Non-SIMD ISAs skip vectorization pass
5. **Stack allocation is portable** - Same concept, different prologue code

**Implementation order:**
1. Sprint 10: x86-64 SSE2 (primary target)
2. Future: AArch64 NEON (128-bit, similar to SSE2)
3. Future: RISC-V RVV (scalable vectors, more complex)
