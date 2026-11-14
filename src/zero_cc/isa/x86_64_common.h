#ifndef ZERO_CC_ISA_X86_64_COMMON_H
#define ZERO_CC_ISA_X86_64_COMMON_H

/* Shared x86-64 machine-code encoding primitives and cross-TU emitter
 * declarations between x86_64.c and x86_64_ext.c. */

#include <stdio.h>
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/ir/regalloc.h"

/* Register numbers used by this backend are the hardware encodings:
 * 0=rax 1=rcx 2=rdx 3=rbx 4=rsp 5=rbp 6=rsi 7=rdi 8..15=r8..r15 */
#define X86_RAX 0
#define X86_RDX 2
#define X86_RSP 4
#define X86_RBP 5
#define X86_R11 11

/* Helper: emit ModR/M byte for x86-64 */
static inline void x86_modrm(ZeroCCCompiler *c, int mod, int reg, int rm) {
    me_emit_u8(c, (mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* REX prefix: w=64-bit, r extends ModRM.reg, x extends SIB.index, b extends ModRM.rm/base */
static inline void x86_rex(ZeroCCCompiler *c, int w, int r, int x, int b) {
    if (w || r || x || b) {
        me_emit_u8(c, 0x40 | (w << 3) | (r << 2) | (x << 1) | b);
    }
}

/* Emit a 64-bit register-register instruction: REX.W <op...> ModRM(11, reg, rm) */
static inline void x86_rr(ZeroCCCompiler *c, const uint8_t *op, int oplen, int reg, int rm) {
    x86_rex(c, 1, (reg >> 3) & 1, 0, (rm >> 3) & 1);
    for (int i = 0; i < oplen; i++) me_emit_u8(c, op[i]);
    x86_modrm(c, 3, reg, rm);
}

/* Emit a 64-bit register-memory instruction: REX.W <op> ModRM/SIB [base + disp] */
static inline void x86_rm(ZeroCCCompiler *c, uint8_t op, int reg, int base, int32_t disp) {
    x86_rex(c, 1, (reg >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, op);
    int mod;
    if (disp == 0 && (base & 7) != X86_RBP) mod = 0;          /* [base] (rbp/r13 need a disp) */
    else if (disp >= -128 && disp <= 127)  mod = 1;           /* [base + disp8] */
    else                                   mod = 2;           /* [base + disp32] */
    x86_modrm(c, mod, reg, base);
    if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);           /* SIB: base=rsp/r12, no index */
    if (mod == 1) me_emit_u8(c, (uint8_t)(int8_t)disp);
    else if (mod == 2) me_emit_u32(c, (uint32_t)disp);
}

/* mov dst, src */
static inline void x86_mov_rr(ZeroCCCompiler *c, int dst, int src) {
    static const uint8_t op[] = {0x89};  /* mov r/m64, r64 */
    if (dst != src) x86_rr(c, op, 1, src, dst);
}

/* dst = dst <op> src for "op r/m64, r64" style ALU instructions (add/sub/and/or/xor) */
static inline void x86_alu(ZeroCCCompiler *c, uint8_t opcode, int dst, int src) {
    x86_rr(c, &opcode, 1, src, dst);
}

/* Three-operand wrapper for commutative ALU ops: dst = src1 <op> src2 */
static inline void x86_alu3_comm(ZeroCCCompiler *c, uint8_t opcode, int dst, int src1, int src2) {
    if (dst == src2) {           /* avoid clobbering src2 with the initial mov */
        int t = src1; src1 = src2; src2 = t;
    }
    x86_mov_rr(c, dst, src1);
    x86_alu(c, opcode, dst, src2);
}

/* Defined in x86_64.c, shared with x86_64_ext.c */
void x86_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm);
void x86_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm);
void x86_emit_not(ZeroCCCompiler *c, int dst_reg, int src_reg);

/* Defined in x86_64_ext.c */
void zero_cc_x86_emit_tramp_stub(ZeroCCCompiler *c, const ZeroCCFunction *nf);
void x86_emit_mul128_spilled(ZeroCCCompiler *c, int32_t d_lo, int32_t d_hi,
                             int32_t a_lo, int32_t a_hi, int32_t b_lo, int32_t b_hi);
void x86_emit_mul128_mixed(ZeroCCCompiler *c, int d_mem, int d_lo, int d_hi,
                           int a_mem, int a_lo, int a_hi, int b_mem, int b_lo, int b_hi);
void x86_emit_bin128_sp(ZeroCCCompiler *c, int op, int a_mem, int a_lo, int a_hi,
                        int32_t b_lo, int32_t b_hi, int32_t d_lo, int32_t d_hi);
void x86_emit_add128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi);
void x86_emit_sub128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi);
void x86_emit_mul128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi);
void x86_emit_and128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi);
void x86_emit_or128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi);
void x86_emit_xor128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi);
void x86_emit_not128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi);
void x86_emit_neg128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi);
void x86_emit_mov128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi);
void x86_emit_load128(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset);
void x86_emit_store128(ZeroCCCompiler *c, int s_lo, int s_hi, int base, int32_t offset);
void x86_emit_zext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src);
void x86_emit_sext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src);
void x86_emit_trunc128_64(ZeroCCCompiler *c, int dst, int s_lo);
void x86_emit_mov128_imm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm);

void x86_emit_atomic_xadd(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
void x86_emit_atomic_or(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
void x86_emit_atomic_and(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
void x86_emit_atomic_xor(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
void x86_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width);
void x86_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width);
void x86_emit_atomic_xchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
void x86_emit_mfence(ZeroCCCompiler *c);
void x86_emit_trap(ZeroCCCompiler *c);
void x86_emit_trap_asm(ZeroCCCompiler *c, FILE *out);
void x86_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps);
void x86_emit_clone(ZeroCCCompiler *c, int arg_count, const int *arg_temps);

#endif /* ZERO_CC_ISA_X86_64_COMMON_H */
