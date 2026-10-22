#ifndef ZERO_CC_OPT_BRANCHLESS_H
#define ZERO_CC_OPT_BRANCHLESS_H

#include "zero_cc/types/symbols.h"

/*
 * Branchless conditional select (Sprint 39), -O3.
 *
 * Converts the ternary-lowering branch shape
 *
 *   <def t>                    ; COND_SET t, cc   (t is 0/1)
 *   JE       t, L_else         ; or: any plain GPR t, jump tests t == 0
 *   [LOAD A, [fp+off]]         ; then: pure plumbing (load/copy/SEXT-ZEXT)
 *   MOV_REG  res, A
 *   JMP      L_end
 *   L_else:
 *   [LOAD B, [fp+off2]]        ; else: pure plumbing
 *   MOV_REG  res, B
 *   L_end:
 *
 * into the branchless bitwise select
 *
 *   [then plumbing]
 *   [else plumbing]
 *   mask = (t ? -1 : 0)        ; 0/1 form: mask = t * -1
 *                               ; int form: mask = ((t == 0) ? 1 : 0) - 1
 *   diff = A ^ B
 *   sel  = mask & diff
 *   res  = B ^ sel             ; t true -> A, t false -> B
 *
 * The mask and select are built only from opcodes that BOTH code paths
 * emit (-S asm_emitter has no OP_NOT / OP_SUB_INC / OP_ADD_INC / OP_INC /
 * OP_DEC): MOV_IMM, MUL, SUB, XOR, AND, CMP_ZERO, COND_SET.
 *
 * The select is exact for any 64-bit pattern (int, pointer, struct
 * chunk), so no new opcode is needed.  Values must be provably
 * GPR-resident at the IR level (float/double locals load via OP_FLOAD
 * into XMM class and are rejected; FP opcodes are not in the
 * provably-GPR definition set).  The CFG layer provides the block /
 * predecessor checks: L_else must be reached only from the JE block and
 * L_end exactly from {then-block, else-block}.
 */
void opt_branchless_select(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_BRANCHLESS_H */
