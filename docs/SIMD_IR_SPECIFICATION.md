# SIMD IR Specification for ZeroCC

## Overview

This document defines the SIMD IR opcodes for ZeroCC's SIMD vectorization support.
These opcodes extend the existing IR to support XMM register operations for
vectorized computations.

## Design Goals

1. **ISA-neutral**: IR opcodes should be architecture-independent
2. **Simple**: Minimal opcode set covering common vectorization patterns
3. **Extensible**: Easy to add new SIMD operations
4. **Compatible**: Works with existing IR infrastructure

## SIMD Opcodes

### Arithmetic Operations

| Opcode | Description | Operands | Machine Instruction |
|--------|-------------|----------|---------------------|
| `OP_VADDSS` | Scalar single-precision add | dest, src1, src2 | `addss xmm, xmm/m32` |
| `OP_VSUBSS` | Scalar single-precision sub | dest, src1, src2 | `subss xmm, xmm/m32` |
| `OP_VMULSS` | Scalar single-precision mul | dest, src1, src2 | `mulss xmm, xmm/m32` |
| `OP_VDIVSS` | Scalar single-precision div | dest, src1, src2 | `divss xmm, xmm/m32` |
| `OP_VADDSD` | Scalar double-precision add | dest, src1, src2 | `addsd xmm, xmm/m64` |
| `OP_VSUBSD` | Scalar double-precision sub | dest, src1, src2 | `subsd xmm, xmm/m64` |
| `OP_VMULSD` | Scalar double-precision mul | dest, src1, src2 | `mulsd xmm, xmm/m64` |
| `OP_VDIVSD` | Scalar double-precision div | dest, src1, src2 | `divsd xmm, xmm/m64` |

### Packed Arithmetic (Future)

| Opcode | Description | Operands | Machine Instruction |
|--------|-------------|----------|---------------------|
| `OP_VADDPD` | Packed double-precision add | dest, src1, src2 | `addpd xmm, xmm/m128` |
| `OP_VADDPS` | Packed single-precision add | dest, src1, src2 | `addps xmm, xmm/m128` |
| `OP_VSUBPD` | Packed double-precision sub | dest, src1, src2 | `subpd xmm, xmm/m128` |
| `OP_VSUBPS` | Packed single-precision sub | dest, src1, src2 | `subps xmm, xmm/m128` |
| `OP_VMULPD` | Packed double-precision mul | dest, src1, src2 | `mulpd xmm, xmm/m128` |
| `OP_VMULPS` | Packed single-precision mul | dest, src1, src2 | `mulps xmm, xmm/m128` |
| `OP_VDIVPD` | Packed double-precision div | dest, src1, src2 | `divpd xmm, xmm/m128` |
| `OP_VDIVPS` | Packed single-precision div | dest, src1, src2 | `divps xmm, xmm/m128` |

### Data Movement

| Opcode | Description | Operands | Machine Instruction |
|--------|-------------|----------|---------------------|
| `OP_VMOV` | Vector move | dest, src | `movaps xmm, xmm` |
| `OP_VMOVSS` | Scalar single-precision move | dest, src | `movss xmm, xmm/m32` |
| `OP_VMOVSD` | Scalar double-precision move | dest, src | `movsd xmm, xmm/m64` |
| `OP_VLOAD` | Vector load from memory | dest, addr | `movaps xmm, [mem]` |
| `OP_VSTORE` | Vector store to memory | addr, src | `movaps [mem], xmm` |
| `OP_VLOADSS` | Load scalar single | dest, addr | `movss xmm, [mem]` |
| `OP_VLOADSD` | Load scalar double | dest, addr | `movsd xmm, [mem]` |
| `OP_VSTORESS` | Store scalar single | addr, src | `movss [mem], xmm` |
| `OP_VSTORESD` | Store scalar double | addr, src | `movsd [mem], xmm` |

### Conversions

| Opcode | Description | Operands | Machine Instruction |
|--------|-------------|----------|---------------------|
| `OP_VCVT_SI2SS` | Scalar int to single | dest, src | `cvtsi2ss xmm, r32` |
| `OP_VCVT_SS2SI` | Scalar single to int | dest, src | `cvtss2si r32, xmm` |
| `OP_VCVT_SI2SD` | Scalar int to double | dest, src | `cvtsi2sd xmm, r32` |
| `OP_VCVT_SD2SI` | Scalar double to int | dest, src | `cvttsd2si r32, xmm` |

### Comparison

| Opcode | Description | Operands | Machine Instruction |
|--------|-------------|----------|---------------------|
| `OP_VCMPSS` | Scalar single comparison | src1, src2, cond | `ucomiss xmm, xmm/m32` |
| `OP_VCMPSD` | Scalar double comparison | src1, src2, cond | `ucomisd xmm, xmm/m64` |

### Bitwise

| Opcode | Description | Operands | Machine Instruction |
|--------|-------------|----------|---------------------|
| `OP_VAND` | Vector bitwise AND | dest, src1, src2 | `andps xmm, xmm/m128` |
| `OP_VOR` | Vector bitwise OR | dest, src1, src2 | `orps xmm, xmm/m128` |
| `OP_VXOR` | Vector bitwise XOR | dest, src1, src2 | `xorps xmm, xmm/m128` |

## IR Instruction Format

All SIMD instructions follow the standard IR instruction format:

```c
typedef struct {
    Opcode op;           /* Opcode */
    uint8_t temp_dest;   /* Destination temp (0-255, 0xFF if none) */
    uint8_t temp_src1;   /* Source temp 1 (0-255, 0xFF if none) */
    uint8_t temp_src2;   /* Source temp 2 (0-255, 0xFF if none) */
    uint8_t cond;        /* Condition code (for comparisons) */
    uint8_t flags_out;   /* Flags written (0xFF if none) */
    uint8_t flags_in;    /* Flags read (0xFF if none) */
    int16_t imm;         /* Immediate value */
    int64_t imm64;       /* 64-bit immediate/pointer */
    int label_idx;       /* Label index (for jumps) */
} Instr;
```

### SIMD Operand Conventions

- **temp_dest**: XMM register number (0-15 for xmm0-xmm15)
- **temp_src1**: XMM register or memory address
- **temp_src2**: XMM register or immediate
- **cond**: Comparison condition (for OP_VCMPSS/OP_VCMPSD)

## IR Emission Functions

```c
/* Scalar single-precision operations */
static inline void ir_emit_vaddss(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vsubss(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vmulss(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vdivss(uint8_t dest, uint8_t src1, uint8_t src2);

/* Scalar double-precision operations */
static inline void ir_emit_vaddsd(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vsubsd(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vmulsd(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vdivsd(uint8_t dest, uint8_t src1, uint8_t src2);

/* Data movement */
static inline void ir_emit_vmov(uint8_t dest, uint8_t src);
static inline void ir_emit_vmovss(uint8_t dest, uint8_t src);
static inline void ir_emit_vmovsd(uint8_t dest, uint8_t src);
static inline void ir_emit_vload(uint8_t dest, uint8_t addr);
static inline void ir_emit_vstore(uint8_t addr, uint8_t src);
static inline void ir_emit_vloadss(uint8_t dest, uint8_t addr);
static inline void ir_emit_vloadsd(uint8_t dest, uint8_t addr);
static inline void ir_emit_vstoress(uint8_t addr, uint8_t src);
static inline void ir_emit_vstoresd(uint8_t addr, uint8_t src);

/* Conversions */
static inline void ir_emit_vcvtsi2ss(uint8_t dest, uint8_t src);
static inline void ir_emit_vcvttss2si(uint8_t dest, uint8_t src);
static inline void ir_emit_vcvtsi2sd(uint8_t dest, uint8_t src);
static inline void ir_emit_vcvttsd2si(uint8_t dest, uint8_t src);

/* Comparison */
static inline void ir_emit_vcmpss(uint8_t src1, uint8_t src2, uint8_t cond);
static inline void ir_emit_vcmpsd(uint8_t src1, uint8_t src2, uint8_t cond);

/* Bitwise */
static inline void ir_emit_vand(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vor(uint8_t dest, uint8_t src1, uint8_t src2);
static inline void ir_emit_vxor(uint8_t dest, uint8_t src1, uint8_t src2);
```

## Helper Function

```c
/* Check if opcode is a SIMD operation */
static inline bool ir_opcode_is_simd(Opcode op) {
    switch (op) {
        case OP_VADDSS: case OP_VSUBSS: case OP_VMULSS: case OP_VDIVSS:
        case OP_VADDSD: case OP_VSUBSD: case OP_VMULSD: case OP_VDIVSD:
        case OP_VMOV: case OP_VMOVSS: case OP_VMOVSD:
        case OP_VLOAD: case OP_VSTORE:
        case OP_VLOADSS: case OP_VLOADSD:
        case OP_VSTORESS: case OP_VSTORESD:
        case OP_VCVT_SI2SS: case OP_VCVT_SS2SI:
        case OP_VCVT_SI2SD: case OP_VCVT_SD2SI:
        case OP_VCMPSS: case OP_VCMPSD:
        case OP_VAND: case OP_VOR: case OP_VXOR:
            return true;
        default:
            return false;
    }
}
```

## Usage Example

### C Source
```c
float dot_product(float *a, float *b, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
```

### Vectorized IR (simplified)
```
; Load a[i] into xmm0
VLOADSS xmm0, [rdi + rsi*4]

; Load b[i] into xmm1  
VLOADSS xmm1, [rdi + rsi*4 + 4]

; Multiply: xmm2 = xmm0 * xmm1
VMULSS xmm2, xmm0, xmm1

; Add to sum: sum = sum + xmm2
VADDSS xmm3, xmm3, xmm2

; Store result
VSTORESS [rbp - 8], xmm3
```

## Implementation Priority

### Phase 1 (Sprint 10)
- [x] Define SIMD opcodes in ir.h
- [x] Add IR emission functions
- [x] Add IR dump printing
- [x] Update live_range.c
- [x] Update opt_dce.c
- [ ] Implement machine code emission (x86_64.c)
- [ ] Implement loop vectorization pass

### Phase 2 (Future)
- [ ] Packed operations (ADDPD, ADDPS, etc.)
- [ ] Shuffle operations
- [ ] Auto-vectorization heuristics
- [ ] SIMD optimization passes

## Testing

Use the following tools to test SIMD IR:

1. `simd_ir_validator.py` - Validate SIMD IR opcodes
2. `simd_instruction_validator.py` - Validate generated assembly
3. `xmm_register_debugger.py` - Debug XMM register usage
4. `simd_regression_suite.py` - Regression testing
5. `simd_benchmark_runner.py` - Performance benchmarking

## References

- Intel® 64 and IA-32 Architectures Software Developer's Manual
- System V AMD64 ABI - Floating Point Parameters
- LLVM SIMD Vectorization
