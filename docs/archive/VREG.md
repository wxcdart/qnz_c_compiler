# Virtual Register (VREG) System

## Overview

ZeroCC uses an ISA-independent virtual register (VREG) system to enable multi-ISA code generation. The lowering code writes to virtual registers, and each backend maps them to physical registers.

## Design: Simple Index-Based Approach

Virtual registers are just numbers (0-15 for integer, 0-15 for float). Each ISA backend interprets them as its native register encodings:

| VREG | x86-64 | aarch64 | riscv64 | Purpose |
|------|--------|---------|---------|---------|
| VREG_0 | rax | x0 | a0 | Result / return value |
| VREG_1 | rcx | x1 | a1 | Temp / 2nd arg |
| VREG_2 | rdx | x2 | a2 | Temp / 3rd arg |
| VREG_3 | rbx | x3 | a3 | Temp / 4th arg |
| VREG_4 | rsp | x4 | a4 | Temp / 5th arg |
| VREG_5 | rbp | x5 | a5 | Temp / 6th arg |
| VREG_6 | rsi | x6 | a6 | Temp / 7th arg |
| VREG_7 | rdi | x7 | a7 | Temp / 8th arg |
| VREG_8 | r8 | x8 | t0 | Temp |
| VREG_9 | r9 | x9 | t1 | Temp |
| VREG_10 | r10 | x10 | t2 | Temp |
| VREG_11 | r11 | x11 | t3 | Temp |
| VREG_12 | r12 | x12 | t4 | Temp |
| VREG_13 | r13 | x13 | t5 | Temp |
| VREG_14 | r14 | x14 | t6 | Temp |
| VREG_15 | r15 | x15 | t7 | Temp |

**Special Registers:**
- `BE(c)->fp_reg` - Frame pointer (ISA-specific: x86=rbp, ARM=x29, RV=s0)
- `BE(c)->sp_reg` - Stack pointer (ISA-specific: x86=rsp, ARM=sp, RV=sp)

## Usage

### In Lowering Code

```c
#include "zero_cc/types/vregs.h"

// Load a local variable into the result register
BE(c)->emit_load(c, VREG_0, BE(c)->fp_reg, -offset);

// Perform arithmetic: VREG_0 = VREG_0 + VREG_1
BE(c)->emit_add(c, VREG_0, VREG_0, VREG_1);

// Store result to stack
BE(c)->emit_store(c, VREG_0, BE(c)->fp_reg, -offset);
```

### In Backend Implementation

Each backend provides a mapping function:

```c
// x86-64: direct pass-through (VREG numbers = x86 encodings)
static int x86_get_int_reg(int vreg) { return vreg; }

// RISC-V: special remapping (VREG_0 -> a0/x10, not x0)
static int riscv_get_int_reg(int vreg) {
    switch (vreg) {
        case 0:  return 10;  // a0 - result
        case 1:  return 11;  // a1
        // ...
        default: return vreg;
    }
}
```

## Float Registers

Float registers follow the same pattern:

| VREG | x86-64 | aarch64 | riscv64 |
|------|--------|---------|---------|
| VREG_F0 | xmm0 | d0 | fa0 |
| VREG_F1 | xmm1 | d1 | fa1 |
| VREG_F2 | xmm2 | d2 | fa2 |
| ... | ... | ... | ... |

## Implementation

### Files

- `src/zero_cc/types/vregs.h` - VREG definitions
- `src/zero_cc/isa_backend.h` - Backend interface with `get_int_reg()`/`get_fp_reg()`
- `src/zero_cc/parser/ast_lowering.h` - Lowering code using VREGs
- `src/zero_cc/isa/x86_64.c` - x86-64 backend implementation
- `src/zero_cc/isa/aarch64.c` - aarch64 backend implementation
- `src/zero_cc/isa/riscv.c` - riscv backend implementation

### Backend Interface

```c
typedef struct ZeroCCISABackend {
    // ... existing fields ...
    
    /* VREG mapping (ISA-independent virtual to physical register) */
    int (*get_int_reg)(int vreg);  /* Map VREG_0-15 to physical */
    int (*get_fp_reg)(int vreg);   /* Map VREG_F0-F15 to physical */
    
    int fp_reg;  /* Frame pointer physical register */
    int sp_reg;  /* Stack pointer physical register */
} ZeroCCISABackend;
```

## Future Enhancements

1. **Register Allocator**: Implement proper register allocation with spilling
2. **ABI Variations**: Support different ABIs (Windows x64 vs System V)
3. **32-bit Support**: Separate mappings for x86, ARM, RISC-V 32-bit
4. **More Virtual Registers**: Extend to 32 int + 32 float registers if needed
