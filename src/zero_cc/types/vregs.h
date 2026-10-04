#ifndef ZERO_CC_VREGS_H
#define ZERO_CC_VREGS_H

/* Virtual Register (VREG) Definitions for ZeroCC
 *
 * Design: Simple Index-Based Approach
 * 
 * Virtual registers are just numbers (0-15 for int, 0-15 for float).
 * Each ISA backend interprets them as its native register encodings.
 * 
 * This works because most ISAs use sequential encodings:
 *   x86-64: 0=rax, 1=rcx, 2=rdx, 3=rbx, 4=rsp, 5=rbp, 6=rsi, 7=rdi, 8=r8...
 *   aarch64: 0=x0, 1=x1, 2=x2, 3=x3, 4=x4, 5=x5, 6=x6, 7=x7, 8=x8...
 *   riscv64: 0=a0, 1=a1, 2=a2, 3=a3, 4=a4, 5=a5, 6=a6, 7=a7, 8=t0...
 *
 * The lowering code uses them consistently:
 *   VREG_0   - result / return value register
 *   VREG_1-7 - argument passing / temporaries (ABI-dependent)
 *   VREG_8-15 - general temporaries / callee-saved (ABI-dependent)
 *
 * Special registers (SP, FP) are accessed via BE(c)->sp_reg / BE(c)->fp_reg
 */

/* Integer virtual registers (0-15) */
#define VREG_0      0   /* Result / return value */
#define VREG_1      1   /* Temp / argument */
#define VREG_2      2   /* Temp / argument */
#define VREG_3      3   /* Temp / argument */
#define VREG_4      4   /* Temp / argument */
#define VREG_5      5   /* Temp / argument */
#define VREG_6      6   /* Temp / argument */
#define VREG_7      7   /* Temp / argument */
#define VREG_8      8   /* Temp */
#define VREG_9      9   /* Temp */
#define VREG_10    10   /* Temp */
#define VREG_11    11   /* Temp */
#define VREG_12    12   /* Temp */
#define VREG_13    13   /* Temp */
#define VREG_14    14   /* Temp */
#define VREG_15    15   /* Temp */

/* Float virtual registers (0-15) */
#define VREG_F0     0   /* Float result / return value */
#define VREG_F1     1   /* Float temp / argument */
#define VREG_F2     2   /* Float temp / argument */
#define VREG_F3     3   /* Float temp / argument */
#define VREG_F4     4   /* Float temp / argument */
#define VREG_F5     5   /* Float temp / argument */
#define VREG_F6     6   /* Float temp / argument */
#define VREG_F7     7   /* Float temp / argument */
#define VREG_F8     8   /* Float temp */
#define VREG_F9     9   /* Float temp */
#define VREG_F10   10   /* Float temp */
#define VREG_F11   11   /* Float temp */
#define VREG_F12   12   /* Float temp */
#define VREG_F13   13   /* Float temp */
#define VREG_F14   14   /* Float temp */
#define VREG_F15   15   /* Float temp */

/* Special virtual registers (use high values to avoid collision) */
#define VREG_SP    256  /* Stack pointer - use BE(c)->sp_reg */
#define VREG_FP    257  /* Frame pointer - use BE(c)->fp_reg */
#define VREG_FLAGS 258  /* Condition flags (implicit on most ISAs) */
#define VREG_USER_START 16  /* First temp ID available for user allocation */

#endif /* ZERO_CC_VREGS_H */
