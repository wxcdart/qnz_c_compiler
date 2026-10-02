# Multi-ISA Support Plan: ARM64, ESP32, RISC-V

## Target Architectures

### 1. ARM64 (AArch64) - Priority: HIGH
**Use Cases:** Apple Silicon Macs, Linux servers, Android, Raspberry Pi 4/5
**Status:** Modern 64-bit, well-documented, good toolchain support

**Key Characteristics:**
- 31 general-purpose registers (x0-x30), x31 = SP
- 32 SIMD/FP registers (v0-v31, 128-bit each)
- Load-store architecture
- Fixed 32-bit instructions (mostly)
- System V ABI on Linux, Apple ABI on macOS

**Float ABI:**
- Float args: v0-v7
- Float return: v0
- Float add: `fadd d0, d0, d1`
- Float load: `ldr d0, [xN]`
- Float→int: `fcvtzs w0, d0`

### 2. RISC-V (RV64GC) - Priority: MEDIUM
**Use Cases:** Embedded systems, academic use, emerging servers
**Status:** Open ISA, growing ecosystem, good documentation

**Key Characteristics:**
- 32 general-purpose registers (x0-x31, x0=hardwired zero)
- 32 FP registers (f0-f31, if F/D extension present)
- Load-store architecture
- Variable-length instructions (32-bit base)
- Pure RISC design

**Float ABI:**
- Float args: fa0-fa7 (a0-a7 for integer args)
- Float return: fa0
- Float add: `fadd.d fa0, fa0, fa1`
- Float load: `fld fa0, 0(a0)`
- Float→int: `fcvt.w.d a0, fa0`

**Note:** Requires F (single) and D (double) extensions for full float support

### 3. ESP32 Family - Priority: LOW (Deferred)
**Use Cases:** IoT, embedded, microcontrollers
**Status:** Complex due to multiple variants

**ESP32 Variants:**
| Chip | ISA | Float Support | Priority |
|------|-----|---------------|----------|
| ESP32 | Xtensa LX6 | Software only | ❌ Skip |
| ESP32-S2 | Xtensa SX | Software only | ❌ Skip |
| ESP32-S3 | Xtensa SX | Software only | ❌ Skip |
| ESP32-C3 | RISC-V RV32 | F extension | ✅ RISC-V path |
| ESP32-C6 | RISC-V RV32 | F extension | ✅ RISC-V path |
| ESP32-H2 | RISC-V RV32 | F extension | ✅ RISC-V path |

**Strategy:** Only support ESP32-C/C6/H2 series (RISC-V based)
- Skip Xtensa variants (proprietary, complex, no hardware float)
- Use RISC-V backend for ESP32-C/C6/H2
- Target RV32IMACFD (32-bit RISC-V with FPU)

---

## Architecture Decision

### Recommended Approach: Backend Abstraction Layer

**Why Backend Layer (not conditional compilation):**

1. **ESP32 Consideration:** Xtensa is proprietary; RISC-V variants need different codegen anyway
2. **Maintainability:** One clean abstraction vs. `#ifdef` spaghetti
3. **Testing:** Can test backends independently
4. **Future-proof:** Easy to add more ISAs (PowerPC, MIPS, etc.)

**Architecture:**
```
┌─────────────────────────────────────┐
│         Frontend (Parser)           │
│  - Tokenization                     │
│  - AST Generation                   │
│  - Semantic Analysis                │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│      Middle End (Lowering)          │
│  - Type-driven codegen              │
│  - ISA-independent IR               │
│  - Register allocation              │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│        Backend Abstraction          │
│  - emit_add(c, dst, src)            │
│  - emit_load_fp(c, reg, offset)     │
│  - emit_call(c, target)             │
└──────────────┬──────────────────────┘
               │
    ┌──────────┼──────────┬──────────┐
    ▼          ▼          ▼          ▼
┌───────┐ ┌───────┐ ┌────────┐ ┌────────┐
│ x86-64│ │ ARM64 │ │ RISC-V │ │ Xtensa │
└───────┘ └───────┘ └────────┘ └────────┘
```

---

## Phase 1: Backend Abstraction Foundation (1 week)

### Task 1.1: Define Backend Interface

**File:** `src/zero_cc/isa_backend.h`

```c
#ifndef ZERO_CC_ISA_BACKEND_H
#define ZERO_CC_ISA_BACKEND_H

#include "types/symbols.h"

typedef enum {
    ISA_X86_64,
    ISA_AARCH64,
    ISA_RISCV32,
    ISA_RISCV64,
    ISA_XTENSA,  // For ESP32 (C/C6/H2 series)
} ZeroCCISA;

/* Register naming */
typedef struct {
    const char *int_regs[32];
    const char *fp_regs[32];
    const char *int_arg_regs[8];
    const char *fp_arg_regs[8];
    const char *int_ret_reg;
    const char *fp_ret_reg;
    int num_int_regs;
    int num_fp_regs;
} ISARegInfo;

/* Emit backend - instruction generation */
typedef struct ZeroCCISABackend {
    ZeroCCISA isa;
    const ISARegInfo *regs;
    
    /* Function prologue/epilogue */
    void (*emit_prologue)(struct ZeroCCCompiler *c, size_t stack_size);
    void (*emit_epilogue)(struct ZeroCCCompiler *c);
    
    /* Integer operations */
    void (*emit_mov_imm)(struct ZeroCCCompiler *c, int dst_reg, int64_t imm);
    void (*emit_add)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_sub)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_mul)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_div)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_and)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_or)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_xor)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_shl)(struct ZeroCCCompiler *c, int dst, int src, int shift);
    void (*emit_cmp)(struct ZeroCCCompiler *c, int src1, int src2);
    
    /* Load/store */
    void (*emit_load)(struct ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset);
    void (*emit_store)(struct ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
    void (*emit_load_imm)(struct ZeroCCCompiler *c, int dst_reg, int64_t imm);
    
    /* Control flow */
    void (*emit_jmp)(struct ZeroCCCompiler *c, const char *label);
    void (*emit_jeq)(struct ZeroCCCompiler *c, const char *label);
    void (*emit_jne)(struct ZeroCCCompiler *c, const char *label);
    void (*emit_jlt)(struct ZeroCCCompiler *c, const char *label);
    void (*emit_jgt)(struct ZeroCCCompiler *c, const char *label);
    void (*emit_call)(struct ZeroCCCompiler *c, const char *func_name);
    void (*emit_ret)(struct ZeroCCCompiler *c);
    
    /* Floating point */
    void (*emit_load_fp)(struct ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset);
    void (*emit_store_fp)(struct ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
    void (*emit_load_fp_imm)(struct ZeroCCCompiler *c, int dst_reg, double value);
    void (*emit_fp_add)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_sub)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_mul)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_div)(struct ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_neg)(struct ZeroCCCompiler *c, int dst, int src);
    void (*emit_fp_cmp)(struct ZeroCCCompiler *c, int src1, int src2);
    void (*emit_fp_cvtsi)(struct ZeroCCCompiler *c, int int_dst, int fp_src);  /* float→int */
    void (*emit_fp_cvtsf)(struct ZeroCCCompiler *c, int fp_dst, int int_src);  /* int→float */
    
    /* Immediate values */
    void (*emit_const)(struct ZeroCCCompiler *c, int64_t value);
} ZeroCCISABackend;

/* Backend registry */
const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa);
const ZeroCCISABackend *zero_cc_get_native_backend(void);

#endif /* ZERO_CC_ISA_BACKEND_H */
```

### Task 1.2: Implement x86-64 Backend (Existing code, refactored)

**File:** `src/zero_cc/isa/x86_64.c`

```c
#include "isa_backend.h"
#include "../parser/machine_emitter.h"

static void x86_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
    me_emit_u8(c, 0x55);             // push rbp
    me_emit_u8(c, 0x48); me_emit_u8(c, 0x89); me_emit_u8(c, 0xe5); // mov rbp, rsp
    if (stack_size > 0) {
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x81);
        me_emit_u8(c, 0xec);
        me_emit_u32(c, (uint32_t)stack_size); // sub rsp, stack_size
    }
}

static void x86_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    // addsd xmm0, xmm1 (for dst=0, src1=0, src2=1)
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x58);
    me_emit_u8(c, 0xC0 | (dst << 3) | src2);
}

static void x86_emit_fp_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    // movsd xmm0, [rbp - offset]
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);
    me_emit_u8(c, 0x85);
    me_emit_u32(c, (uint32_t)(-offset));
}

static const ZeroCCISABackend x86_64_backend = {
    .isa = ISA_X86_64,
    .regs = &x86_64_regs,
    .emit_prologue = x86_emit_prologue,
    .emit_fp_add = x86_emit_fp_add,
    .emit_load_fp = x86_emit_fp_load,
    // ... fill in all functions ...
};

const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa) {
    switch (isa) {
        case ISA_X86_64: return &x86_64_backend;
        // ... other ISAs ...
    }
    return NULL;
}
```

### Task 1.3: Implement ARM64 Backend

**File:** `src/zero_cc/isa/aarch64.c`

```c
#include "isa_backend.h"

/* ARM64 instruction encodings */
#define ARM64_FADD_D(d, n, m) (0x4E208400 | ((d) << 0) | ((n) << 5) | ((m) << 10))
#define ARM64_FSUB_D(d, n, m) (0x4E209400 | ((d) << 0) | ((n) << 5) | ((m) << 10))
#define ARM64_FMUL_D(d, n, m) (0x4E20A400 | ((d) << 0) | ((n) << 5) | ((m) << 10))
#define ARM64_FDIV_D(d, n, m) (0x4E20B400 | ((d) << 0) | ((n) << 5) | ((m) << 10))
#define ARM64_FCVTWS(w, s)   (0x1E640000 | ((w) << 0) | ((s) << 16))
#define ARM64_SCVTF_D(w, s)  (0x1E620000 | ((w) << 0) | ((s) << 16))

static void arm64_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
    // stp x29, x30, [sp, #-16]!
    me_emit_u32(c, 0xA9BD7bfd | ((stack_size / 16) << 15));
    // mov x29, sp
    me_emit_u32(c, 0x910003fd);
}

static void arm64_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    // fadd d0, d0, d1
    me_emit_u32(c, ARM64_FADD_D(dst, src1, src2));
}

static void arm64_emit_fp_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    // ldr d0, [x29, #-offset]
    me_emit_u32(c, 0xF9400000 | ((dst_reg << 5) & 0x600) | ((base_reg << 5) & 0x600000) | 
                   ((-offset / 8) & 0x1FF));
}

static const ISARegInfo arm64_regs = {
    .int_regs = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", 
                 "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                 "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
                 "x24", "x25", "x26", "x27", "x28", "x29", "x30"},
    .fp_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
                "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
                "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"},
    .int_arg_regs = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"},
    .fp_arg_regs = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"},
    .int_ret_reg = "x0",
    .fp_ret_reg = "v0",
    .num_int_regs = 31,
    .num_fp_regs = 32,
};

static const ZeroCCISABackend arm64_backend = {
    .isa = ISA_AARCH64,
    .regs = &arm64_regs,
    .emit_prologue = arm64_emit_prologue,
    .emit_fp_add = arm64_emit_fp_add,
    .emit_load_fp = arm64_emit_fp_load,
    // ... fill in all functions ...
};
```

### Task 1.4: Implement RISC-V Backend

**File:** `src/zero_cc/isa/riscv.c`

```c
#include "isa_backend.h"

/* RISC-V instruction encodings */
#define RV_FADD_D(rd, rs1, rs2) (0x00000053 | ((rd) << 7) | ((rs1) << 15) | ((rs2) << 20) | 0x00000000)
#define RV_FSUB_D(rd, rs1, rs2) (0x00000053 | ((rd) << 7) | ((rs1) << 15) | ((rs2) << 20) | 0x00001000)
#define RV_FMUL_D(rd, rs1, rs2) (0x00000053 | ((rd) << 7) | ((rs1) << 15) | ((rs2) << 20) | 0x00002000)
#define RV_FDIV_D(rd, rs1, rs2) (0x00000053 | ((rd) << 7) | ((rs1) << 15) | ((rs2) << 20) | 0x00003000)
#define RV_FCVT_W_D(rd, rs1)    (0x00000053 | ((rd) << 7) | ((rs1) << 15) | 0x00006000)
#define RV_FLD(rd, offset, rs1) (0x00000003 | ((rd) << 7) | ((rs1) << 15) | ((offset & 0x7FF) << 20))

static void riscv_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
    // addi sp, sp, -stack_size
    me_emit_u32(c, 0x00000113 | ((stack_size & 0xFFF) << 20));
    // sd s0, 0(sp)
    me_emit_u32(c, 0x00002223);
}

static void riscv_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    // fadd.d fa0, fa0, fa1
    me_emit_u32(c, RV_FADD_D(dst, src1, src2));
}

static void riscv_emit_fp_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    // fld fa0, offset(sp)
    me_emit_u32(c, RV_FLD(dst_reg, offset, base_reg));
}

static const ISARegInfo riscv_regs = {
    .int_regs = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
                 "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
                 "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
                 "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"},
    .fp_regs = {"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
                "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
                "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
                "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"},
    .int_arg_regs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"},
    .fp_arg_regs = {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"},
    .int_ret_reg = "a0",
    .fp_ret_reg = "fa0",
    .num_int_regs = 32,
    .num_fp_regs = 32,
};

static const ZeroCCISABackend riscv_backend = {
    .isa = ISA_RISCV64,
    .regs = &riscv_regs,
    .emit_prologue = riscv_emit_prologue,
    .emit_fp_add = riscv_emit_fp_add,
    .emit_load_fp = riscv_emit_fp_load,
    // ... fill in all functions ...
};
```

---

## Phase 2: Refactor Lowering to Use Backend (1 week)

### Task 2.1: Update ast_lowering.h

**Current (x86-64 hardcoded):**
```c
case NODE_BINOP: {
    lower_expr(c, arena, node->left);
    bool left_is_float = c->last_expr_is_float;
    
    if (left_is_float) {
        me_emit_u8(c, 0xF2); me_emit_u8(c, 0x0F); me_emit_u8(c, 0x10); me_emit_u8(c, 0xC9);
    }
    // ...
}
```

**New (backend-driven):**
```c
case NODE_BINOP: {
    ZeroCCISABackend *be = c->isa_backend;
    
    lower_expr(c, arena, node->left);
    bool left_is_float = c->last_expr_is_float;
    
    if (left_is_float) {
        // Use backend to copy float reg
        be->emit_fp_mov(c, 1, 0);  // dst=xmm1/v0/f1, src=xmm0/v0/f0
    }
    // ...
}
```

### Task 2.2: Initialize Backend in Compiler

**File:** `src/zero_cc/jit/compile.h`

```c
void zero_cc_compile_c99(ZeroCCCompiler *c) {
    // Detect native ISA or use target ISA
    #ifdef __x86_64__
        c->isa = ISA_X86_64;
    #elif defined(__aarch64__)
        c->isa = ISA_AARCH64;
    #elif defined(__riscv)
        c->isa = ISA_RISCV64;
    #endif
    
    c->isa_backend = zero_cc_get_backend(c->isa);
    
    zero_cc_tokenize_c99(c->preprocessed, c);
    zero_cc_parse_c99(c);
    zero_cc_semantic_analysis(c);
    zero_cc_lower_ast(c);
    zero_cc_emit_code(c);
}
```

---

## Phase 3: ESP32 Support (RISC-V variants only)

### ESP32-C3/C6/H2 Specifics

**Target:** RV32IMACFD (32-bit RISC-V with FPU)

**Differences from RV64:**
- 32-bit integers (not 64-bit)
- Pointer size = 4 bytes
- Stack alignment = 16 bytes
- Same FP registers (f0-f31)

**Implementation:**
```c
// Use existing RISC-V backend with RV32 adjustments
static const ISARegInfo esp32_riscv_regs = {
    .int_regs = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", ...},
    .fp_regs = {"ft0", "ft1", "ft2", ...},  // Same as RV64
    .int_arg_regs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"},
    .fp_arg_regs = {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"},
    .int_ret_reg = "a0",
    .fp_ret_reg = "fa0",
    .num_int_regs = 32,
    .num_fp_regs = 32,
};

// Adjust stack frame calculations for 32-bit
if (c->isa == ISA_RISCV32) {
    stack_size = (stack_size + 3) & ~3;  // 4-byte alignment
} else {
    stack_size = (stack_size + 15) & ~15; // 16-byte alignment
}
```

**ESP32 Toolchain:**
```bash
# Install ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh

# Compile for ESP32-C3
export IDF_TARGET=riscv
idf.py build

# Cross-compile with our compiler
./qnz_cc -target riscv32imacfd -o firmware.elf source.c
```

---

## Phase 4: Testing Strategy

### Test Matrix

| Test | x86-64 | ARM64 | RISC-V | ESP32-C3 |
|------|--------|-------|--------|----------|
| Float literal | ✅ | ⏳ | ⏳ | ⏳ |
| Float arithmetic | ✅ | ⏳ | ⏳ | ⏳ |
| Float cast | ✅ | ⏳ | ⏳ | ⏳ |
| Float function args | ⏳ | ⏳ | ⏳ | ⏳ |
| Float comparisons | ⏳ | ⏳ | ⏳ | ⏳ |
| Struct by value | ✅ | ⏳ | ⏳ | ⏳ |
| Function pointers | ✅ | ⏳ | ⏳ | ⏳ |

### Cross-Compilation Testing

```bash
# ARM64 (Linux)
aarch64-linux-gnu-gcc -nostdlib -o test_arm64 test.c
qemu-aarch64 test_arm64

# RISC-V
riscv64-linux-gnu-gcc -nostdlib -o test_riscv test.c
qemu-riscv64 test_riscv

# ESP32-C3
esp32c3-elf-gcc -nostdlib -o test_esp32 test.c
esptool.py --chip esp32c3 write_flash 0x1000 test_esp32
```

---

## Timeline

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| Phase 1: Backend Foundation | 1 week | Backend API + x86-64/ARM64/RISC-V implementations |
| Phase 2: Refactor Lowering | 1 week | Backend-driven codegen in ast_lowering.h |
| Phase 3: ESP32 Support | 3-5 days | RV32 adjustments + ESP-IDF integration |
| Phase 4: Testing | 1 week | Test matrix + CI/CD pipeline |
| **Total** | **3-4 weeks** | **Multi-ISA compiler** |

---

## Success Criteria

### Phase 1
- [ ] Backend API defined and documented
- [ ] x86-64 backend passes all tests
- [ ] ARM64 backend compiles and runs hello world
- [ ] RISC-V backend compiles and runs hello world

### Phase 2
- [ ] ast_lowering.h uses backend API
- [ ] All 70+ tests pass on x86-64
- [ ] Float arithmetic works on ARM64
- [ ] Float arithmetic works on RISC-V

### Phase 3
- [ ] ESP32-C3 toolchain integration
- [ ] Float code runs on ESP32-C3
- [ ] Memory usage < 100KB (ESP32 constraint)

### Phase 4
- [ ] CI/CD pipeline with multi-ISA testing
- [ ] Performance benchmarks per ISA
- [ ] Documentation for adding new ISAs

---

## Notes

- **ESP32 Xtensa (LX6/SX) variants are NOT supported** - proprietary ISA, no hardware float, complex licensing
- **ESP32-C/C6/H2 (RISC-V) are supported** via RISC-V backend
- **ARM64 is highest priority** after x86-64 (Apple Silicon, mobile, embedded)
- **RISC-V is medium priority** (growing ecosystem, ESP32-C series)
- **Backend abstraction is critical** for maintainability and future ISAs

---

*Created: $(date)*
*Status: Ready for implementation*
