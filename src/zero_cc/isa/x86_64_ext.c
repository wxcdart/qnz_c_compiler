/* x86-64 extended emitter helpers: 128-bit (__int128) integer operations,
 * C11 atomic operations, OS syscall/clone sequences, and nested-function
 * trampoline stub generation. */

#include <stdio.h>
#include <string.h>
#include "zero_cc/isa/x86_64_common.h"

/* F7 /4: mul [base+disp] -- rdx:rax = rax * [base+disp]. Used by the
 * spilled-MUL128 path where one factor lives in a frame slot. */
static void x86_mul_mem(ZeroCCCompiler *c, int base, int32_t disp) {
    x86_rm(c, 0xF7, 4, base, disp);
}

/* 01 /00: add [base+disp], reg. Used by the spilled-MUL128 path to
 * accumulate partial products into the d_hi frame slot. */
static void x86_add_mem(ZeroCCCompiler *c, int reg, int base, int32_t disp) {
    x86_rm(c, 0x01, reg, base, disp);
}

/* Spilled MUL128 (Sprint 109): all three operands live in frame slots.
 * Only rax/rdx are used as working registers (both are reserved around
 * OP_MUL128 by the VREG_0/VREG_2 pseudo-intervals, so no live value can
 * be in them). */
void x86_emit_mul128_spilled(ZeroCCCompiler *c,
                             int32_t d_lo, int32_t d_hi,
                             int32_t a_lo, int32_t a_hi,
                             int32_t b_lo, int32_t b_hi) {
    int base = zero_cc_frame_base_reg(c);
    int rax = 0, rdx = 2;
    x86_rm(c, 0x8B, rax, base, a_lo);   /* rax = a_lo */
    x86_mul_mem(c, base, b_lo);         /* rdx:rax = a_lo * b_lo (P0) */
    x86_rm(c, 0x89, rax, base, d_lo);   /* [d_lo] = P0.lo */
    x86_rm(c, 0x89, rdx, base, d_hi);   /* [d_hi] = P0.hi */
    x86_rm(c, 0x8B, rax, base, a_lo);   /* rax = a_lo */
    x86_mul_mem(c, base, b_hi);         /* rdx:rax = a_lo * b_hi (P1) */
    x86_add_mem(c, rax, base, d_hi);    /* [d_hi] += P1.lo */
    x86_rm(c, 0x8B, rax, base, a_hi);   /* rax = a_hi */
    x86_mul_mem(c, base, b_lo);         /* rdx:rax = a_hi * b_lo (P2) */
    x86_add_mem(c, rax, base, d_hi);    /* [d_hi] += P2.lo */
}

/* MUL128 with an arbitrary mix of register-pair / frame-slot operands
 * (Sprint 109 1-scratch-pair fix). Each of d/a/b is either REG (physical
 * registers in *_lo/*_hi) or MEM (frame displacements in *_lo/*_hi, selected
 * by the corresponding *_mem flag). Only rax/rdx are used as working
 * registers (both reserved around OP_MUL128), so d may alias a or b: every
 * source half is re-read from its home before being consumed. */
void x86_emit_mul128_mixed(ZeroCCCompiler *c,
                           int d_mem, int d_lo, int d_hi,
                           int a_mem, int a_lo, int a_hi,
                           int b_mem, int b_lo, int b_hi) {
    int base = zero_cc_frame_base_reg(c);
    int rax = 0, rdx = 2;
    static const uint8_t mul_op[] = {0xF7};  /* 0xF7 /4: mul r/m64 */
    static const uint8_t add_op[] = {0x01};  /* 0x01 /r: add r/m64, r64 */
    /* P0 = a_lo * b_lo */
    if (a_mem) x86_rm(c, 0x8B, rax, base, a_lo); else x86_mov_rr(c, rax, a_lo);
    if (b_mem) x86_mul_mem(c, base, b_lo);      else x86_rr(c, mul_op, 1, 4, b_lo);
    if (d_mem) { x86_rm(c, 0x89, rax, base, d_lo); x86_rm(c, 0x89, rdx, base, d_hi); }
    else       { x86_mov_rr(c, d_lo, rax);         x86_mov_rr(c, d_hi, rdx); }
    /* P1 = a_lo * b_hi; d_hi += P1.lo */
    if (a_mem) x86_rm(c, 0x8B, rax, base, a_lo); else x86_mov_rr(c, rax, a_lo);
    if (b_mem) x86_mul_mem(c, base, b_hi);      else x86_rr(c, mul_op, 1, 4, b_hi);
    if (d_mem) x86_add_mem(c, rax, base, d_hi); else x86_rr(c, add_op, 1, rax, d_hi);
    /* P2 = a_hi * b_lo; d_hi += P2.lo */
    if (a_mem) x86_rm(c, 0x8B, rax, base, a_hi); else x86_mov_rr(c, rax, a_hi);
    if (b_mem) x86_mul_mem(c, base, b_lo);      else x86_rr(c, mul_op, 1, 4, b_lo);
    if (d_mem) x86_add_mem(c, rax, base, d_hi); else x86_rr(c, add_op, 1, rax, d_hi);
}

/* Spilled ADD128/SUB128/AND128/OR128/XOR128 (Sprint 109 1-scratch-pair
 * fix): b and d live in frame slots, a in registers (or a slot). Uses only
 * the reserved 128-bit scratch pair (r10/r11 on x86-64 - rax/rdx are NOT
 * reserved around these ops). The hi half of add/sub chains the
 * carry/borrow from the lo half. */
void x86_emit_bin128_sp(ZeroCCCompiler *c, int op, int a_mem,
                        int a_lo, int a_hi,
                        int32_t b_lo, int32_t b_hi,
                        int32_t d_lo, int32_t d_hi) {
    int base = zero_cc_frame_base_reg(c);
    int plo = BE(c)->g128_scratch_lo[0], phi = BE(c)->g128_scratch_hi[0];
    uint8_t lo_op, hi_op;
    switch (op) {
        case OP_SUB128: lo_op = 0x29; hi_op = 0x19; break;  /* sub /r, sbb /r */
        case OP_AND128: lo_op = 0x21; hi_op = 0x21; break;  /* and /r */
        case OP_OR128:  lo_op = 0x09; hi_op = 0x09; break;  /* or  /r */
        case OP_XOR128: lo_op = 0x31; hi_op = 0x31; break;  /* xor /r */
        case OP_ADD128:
        default:        lo_op = 0x01; hi_op = 0x11; break;  /* add /r, adc /r */
    }
    if (a_mem) x86_rm(c, 0x8B, plo, base, a_lo); else x86_mov_rr(c, plo, a_lo);
    x86_rm(c, 0x8B, phi, base, b_lo);             /* phi = b_lo */
    x86_rr(c, &lo_op, 1, phi, plo);               /* plo = a_lo OP b_lo (CF) */
    x86_rm(c, 0x89, plo, base, d_lo);             /* [d_lo] = plo */
    if (a_mem) x86_rm(c, 0x8B, plo, base, a_hi); else x86_mov_rr(c, plo, a_hi);
    x86_rm(c, 0x8B, phi, base, b_hi);             /* phi = b_hi */
    x86_rr(c, &hi_op, 1, phi, plo);               /* plo = a_hi OP b_hi (+CF) */
    x86_rm(c, 0x89, plo, base, d_hi);             /* [d_hi] = plo */
}

/* Byte-width atomics: unextended ModRM fields 4-7 name AH/CH/DH/BH, so a
 * value register of RSP/RBP/RSI/RDI (4-7) must be relocated to scratch R11
 * (whose byte BBL encodes via REX.R) before use. */
void x86_emit_atomic_xadd(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    if (width == 1 && val_reg >= 4 && val_reg < 8) {
        x86_mov_rr(c, X86_R11, val_reg);
        val_reg = X86_R11;
    }
    /* lock prefix: 0xF0 */
    me_emit_u8(c, 0xF0);
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (val_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xC0);
    } else {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xC1);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, val_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* After xadd, val_reg holds the old value. For width 1 only the low byte
     * changed (upper bits stale), so zero-extend from the byte register. */
    if (width == 1) {
        x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (val_reg >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB6);
        x86_modrm(c, 3, dst_reg & 7, val_reg & 7);
    } else if (dst_reg != val_reg) {
        x86_mov_rr(c, dst_reg, val_reg);
    }
}

/* Shared emitter for OP_ATOMIC_OR/AND/XOR (C11 __atomic_fetch_or/and/xor,
 * GCC's pattern): snapshot [addr] into dst, then atomically update memory
 * in place with `lock or/and/xor val, [addr]`. After the sequence, memory
 * holds the updated value and dst holds the pre-update snapshot. */
static void x86_emit_atomic_bitwise(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width, uint8_t opcode32, uint8_t opcode8) {
    /* 1) dst = [addr] (pre-update snapshot) */
    if (width == 1) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (dst_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    me_emit_u8(c, (width == 1) ? 0x8A : 0x8B);
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, dst_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* 2) lock <op> val, [addr] (atomic in-place update). The /r source of a
     * byte op must be AL/CL/DL/BL/R8B..R15B - relocate RSI/RDI into R11. */
    if (width == 1 && val_reg >= 4 && val_reg < 8) {
        x86_mov_rr(c, X86_R11, val_reg);
        val_reg = X86_R11;
    }
    me_emit_u8(c, 0xF0);
    if (width <= 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (val_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    me_emit_u8(c, (width == 1) ? opcode8 : opcode32);
    x86_modrm(c, mod, val_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);
}

void x86_emit_atomic_or(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    x86_emit_atomic_bitwise(c, dst_reg, addr_reg, val_reg, width, 0x09, 0x08); /* or r/m, r */
}

void x86_emit_atomic_and(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    x86_emit_atomic_bitwise(c, dst_reg, addr_reg, val_reg, width, 0x21, 0x10); /* and r/m, r */
}

void x86_emit_atomic_xor(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    x86_emit_atomic_bitwise(c, dst_reg, addr_reg, val_reg, width, 0x31, 0x30); /* xor r/m, r */
}

void x86_emit_atomic_cmpxchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width) {
    /* If addr_reg or desired_reg is RAX, preserve them in a scratch register before RAX is overwritten with expected_reg */
    if (addr_reg == X86_RAX) {
        x86_mov_rr(c, 10 /* R10 */, addr_reg);
        addr_reg = 10;
    }
    if (desired_reg == X86_RAX) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    /* Byte cmpxchg source must be AL/CL/DL/BL/R8B..R15B: relocate RSI/RDI
     * (fields 6-7 name DH/BH unextended) into scratch R11 (BBL). */
    if (width == 1 && desired_reg >= 4 && desired_reg < 8) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    if (expected_reg != X86_RAX) {
        x86_mov_rr(c, X86_RAX, expected_reg);
    }
    /* lock prefix: 0xF0 */
    me_emit_u8(c, 0xF0);
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (desired_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB0);
    } else {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB1);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, desired_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* Old value from memory is in RAX. For width 1 only AL is defined
     * (byte cmpxchg leaves the upper bits stale), so zero-extend from AL. */
    if (width == 1) {
        x86_rex(c, 1, (dst_reg >> 3) & 1, 0, 0);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB6);
        x86_modrm(c, 3, dst_reg & 7, 0);
    } else if (dst_reg != X86_RAX) {
        x86_mov_rr(c, dst_reg, X86_RAX);
    }
}

void x86_emit_atomic_cmpxchg_bool(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width) {
    /* If addr_reg or desired_reg is RAX, preserve them in a scratch register before RAX is overwritten with expected_reg */
    if (addr_reg == X86_RAX) {
        x86_mov_rr(c, 10 /* R10 */, addr_reg);
        addr_reg = 10;
    }
    if (desired_reg == X86_RAX) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    /* Byte cmpxchg source must be a valid byte register (see cmpxchg). */
    if (width == 1 && desired_reg >= 4 && desired_reg < 8) {
        x86_mov_rr(c, 11 /* R11 */, desired_reg);
        desired_reg = 11;
    }
    if (expected_reg != X86_RAX) {
        x86_mov_rr(c, X86_RAX, expected_reg);
    }
    /* lock prefix: 0xF0 */
    me_emit_u8(c, 0xF0);
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (desired_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB0);
    } else {
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB1);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, desired_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    /* sete al: 0F 94 C0 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x94);
    me_emit_u8(c, 0xC0);
    /* movzx eax, al: 0F B6 C0 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xB6);
    me_emit_u8(c, 0xC0);

    if (dst_reg != X86_RAX) {
        x86_mov_rr(c, dst_reg, X86_RAX);
    }
}

void x86_emit_atomic_xchg(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width) {
    if (width == 1 && val_reg >= 4 && val_reg < 8) {
        x86_mov_rr(c, X86_R11, val_reg);
        val_reg = X86_R11;
    }
    if (width == 2) me_emit_u8(c, 0x66);
    x86_rex(c, (width == 8) ? 1 : 0, (val_reg >> 3) & 1, 0, (addr_reg >> 3) & 1);
    if (width == 1) {
        me_emit_u8(c, 0x86);
    } else {
        me_emit_u8(c, 0x87);
    }
    int mod = ((addr_reg & 7) == X86_RBP) ? 1 : 0;
    x86_modrm(c, mod, val_reg & 7, addr_reg & 7);
    if ((addr_reg & 7) == X86_RSP) me_emit_u8(c, 0x24);
    if (mod == 1) me_emit_u8(c, 0);

    if (width == 1) {
        x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (val_reg >> 3) & 1);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0xB6);
        x86_modrm(c, 3, dst_reg & 7, val_reg & 7);
    } else if (dst_reg != val_reg) {
        x86_mov_rr(c, dst_reg, val_reg);
    }
}

void x86_emit_mfence(ZeroCCCompiler *c) {
    /* mfence: 0F AE F0 */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xAE);
    me_emit_u8(c, 0xF0);
}

void x86_emit_trap(ZeroCCCompiler *c) {
    /* ud2: 0F 0B (__builtin_trap / __builtin_unreachable) */
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x0B);
}

void x86_emit_trap_asm(ZeroCCCompiler *c, FILE *out) {
    (void)c;
    fprintf(out, "\tud2\n");
}

/* OS syscall (SysV AMD64): eax = number, rdi/rsi/rdx/r10/r8/r9 = args 1-6.
 * The result is left in rax (VREG_0). */
void x86_emit_syscall(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int sysv_syscall_regs[7] = {0, 7, 6, 2, 10, 8, 9}; /* rax rdi rsi rdx r10 r8 r9 */
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int src_regs[8] = {0};
    for (int a = 0; a < arg_count && a < 7; a++) {
        int temp = arg_temps[a];
        int src_reg;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, BE(c)->fp_reg, -off);
            src_reg = BE(c)->scratch_reg2;
        } else {
            src_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        }
        src_regs[a] = src_reg;
        if (src_reg != sysv_syscall_regs[a] && BE(c)->emit_push_reg) {
            BE(c)->emit_push_reg(c, src_reg);
        }
    }
    for (int a = (arg_count < 7) ? arg_count - 1 : 6; a >= 0; a--) {
        if (src_regs[a] == sysv_syscall_regs[a]) continue;
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, sysv_syscall_regs[a]);
    }
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x05); /* syscall */
}

/* OS clone (SYS_clone = 56). Arguments: flags, child_sp, ptid, tls, ctid */
void x86_emit_clone(ZeroCCCompiler *c, int arg_count, const int *arg_temps) {
    static const int clone_regs[5] = {7 /* rdi */, 6 /* rsi */, 2 /* rdx */, 8 /* r8 */, 10 /* r10 */};
    int32_t spill_base_offset = c->regalloc.spill_base_offset;
    int n = (arg_count < 5) ? arg_count : 5;
    int src_regs[8] = {0};
    for (int a = 0; a < n; a++) {
        int temp = arg_temps[a];
        int src_reg;
        int sp = regalloc_get_spill_slot_for_temp(&c->regalloc, temp);
        if (sp >= 0) {
            int32_t off = spill_base_offset + (sp + 1) * 8;
            if (BE(c)->emit_load) BE(c)->emit_load(c, BE(c)->scratch_reg2, BE(c)->fp_reg, -off);
            src_reg = BE(c)->scratch_reg2;
        } else {
            src_reg = regalloc_get_reg_for_temp(&c->regalloc, temp);
        }
        src_regs[a] = src_reg;
        if (src_reg != clone_regs[a] && BE(c)->emit_push_reg) {
            BE(c)->emit_push_reg(c, src_reg);
        }
    }
    for (int a = n - 1; a >= 0; a--) {
        if (src_regs[a] == clone_regs[a]) continue;
        if (BE(c)->emit_pop_reg) BE(c)->emit_pop_reg(c, clone_regs[a]);
    }
    /* mov $56, %eax */
    me_emit_u8(c, 0xb8);
    me_emit_u32(c, 56);
    /* syscall */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x05);
    /* test %rax, %rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x85);
    me_emit_u8(c, 0xc0);
    /* jnz +17 */
    me_emit_u8(c, 0x75);
    me_emit_u8(c, 0x11);
    /* Child path: */
    /* xor %ebp, %ebp */
    me_emit_u8(c, 0x31);
    me_emit_u8(c, 0xed);
    /* pop %rax (fn) */
    me_emit_u8(c, 0x58);
    /* pop %rdi (arg) */
    me_emit_u8(c, 0x5f);
    /* call *%rax */
    me_emit_u8(c, 0xff);
    me_emit_u8(c, 0xd0);
    /* mov %rax, %rdi */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xc7);
    /* mov $60, %eax */
    me_emit_u8(c, 0xb8);
    me_emit_u32(c, 60);
    /* syscall */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x05);
    /* hlt */
    me_emit_u8(c, 0xf4);
}

/* =====================================================================
 * Phase 3b: trampoline stub for a capturing nested function.
 * ===================================================================== */

#define TRAMP_MAX_UNITS 24

typedef struct {
    bool is_fp;       /* FP unit (XMM stream) vs integer unit (GPR stream) */
    int  fin_slot;    /* final register slot (0..5 GPR / 0..7 XMM), -1 = stack */
    int  fin_stack;   /* final stack unit index, -1 = register */
    int  src;         /* value source: 0=ctx, 1=frame8, 2=xmmframe16, 3=stacksave8 */
    int  src_off;     /* rbp-relative frame offset (src 1/2/3) or ctx offset (src 0) */
} ZeroCCTrampUnit;

static int tramp_unit_gpr_slots(const ZeroCCType *t) {
    if ((t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) && t->is_vla_struct)
        return 2; /* (hidden pointer, total size) GPR pair */
    if (t->size > 32) return 1; /* hidden pointer convention */
    size_t s = t->size ? t->size : 8;
    return (int)((s + 7) / 8);
}
static bool tramp_unit_is_fp(const ZeroCCType *t) {
    return (t->tag == TYPE_FLOAT || t->tag == TYPE_DOUBLE);
}
static int tramp_unit_slots(const ZeroCCType *t) {
    if (tramp_unit_is_fp(t)) return t->is_complex ? 2 : 1;
    return tramp_unit_gpr_slots(t);
}

static void tramp_mov_xmm_mem(ZeroCCCompiler *c, int xmm, int base, int32_t off, bool wide16) {
    x86_rex(c, 1, (xmm >> 3) & 1, 0, (base >> 3) & 1);
    me_emit_u8(c, wide16 ? 0xF3 : 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, wide16 ? 0x6F : 0x10);
    if ((off >= -128 && off <= 127 && off != 0) || (off == 0 && (base & 7) == X86_RBP)) {
        x86_modrm(c, 1, xmm & 7, base & 7);
        me_emit_u8(c, (uint8_t)(int8_t)off);
    } else {
        x86_modrm(c, 2, xmm & 7, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        me_emit_u32(c, (uint32_t)off);
    }
}

/* push [rbp|rm + disp] (REX.W FF /6) */
static void tramp_push_mem(ZeroCCCompiler *c, int base, int32_t off) {
    x86_rex(c, 1, 0, 0, (base >> 3) & 1);
    me_emit_u8(c, 0xFF);
    if (off >= -128 && off <= 127) {
        x86_modrm(c, 1, 6, base & 7);
        me_emit_u8(c, (uint8_t)(int8_t)off);
    } else {
        x86_modrm(c, 2, 6, base & 7);
        if ((base & 7) == X86_RSP) me_emit_u8(c, 0x24);
        me_emit_u32(c, (uint32_t)off);
    }
}

void zero_cc_x86_emit_tramp_stub(ZeroCCCompiler *c, const ZeroCCFunction *nf) {
    size_t C = nf->nested_captured_count;
    size_t U = nf->param_count - C;
    bool has_sret = (nf->ret_struct_sz > 16);

    ZeroCCTrampUnit u[TRAMP_MAX_UNITS];
    int n = 0, gpr_run = 0, xmm_run = 0, decl_stack_run = 0;

    int entry_stack_n = 0;

    /* sret pointer: entry RDI -> final GPR slot 0 (passes through) */
    if (has_sret) {
        u[n].is_fp = false;
        u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
        u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
        u[n].src = 1; u[n].src_off = -8;      /* the saved RDI */
        n++;
    }
    /* captures: values come from ctx (32 bytes per capture, word j at
     * 32*ci + 8j). */
    for (size_t ci = 0; ci < C; ci++) {
        int slots;
        bool isfp;
        if (nf->nested_capture_byref[ci]) {
            slots = 1; isfp = false;
        } else {
            const ZeroCCType *t = &nf->nested_capture_types[ci];
            slots = tramp_unit_slots(t);
            isfp = tramp_unit_is_fp(t);
        }
        for (int w = 0; w < slots; w++) {
            if (n >= TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            if (isfp)      u[n].fin_slot = xmm_run < 8 ? xmm_run++ : -1;
            else           u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * ci) + 8 * w;
            n++;
        }
    }
    /* Sprint 99: nonlocal goto pairs, between captures and user params:
     * pair i holds (rbp, rsp) of the i-th ancestor scope; ctx words at
     * 32*C + 16*i (rbp) and +8 (rsp). */
    for (int pi = 0; pi < nf->goto_pair_count; pi++) {
        for (int w = 0; w < 2; w++) {
            if (n >= TRAMP_MAX_UNITS) return;
            u[n].is_fp = false;
            u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            u[n].src = 0;
            u[n].src_off = (int)(32 * C + 16 * pi) + 8 * w;
            n++;
        }
    }
    /* user params */
    int ugpr = 0, uxmm = 0;
    for (size_t ui = 0; ui < U; ui++) {
        const ZeroCCType *t = &nf->param_types[C + ui];
        int slots = tramp_unit_slots(t);
        bool isfp = tramp_unit_is_fp(t);
        for (int w = 0; w < slots; w++) {
            if (n >= TRAMP_MAX_UNITS) return;
            u[n].is_fp = isfp;
            int entry_slot, entry_stack = -1;
            if (isfp) {
                entry_slot = uxmm++;
                if (entry_slot >= 8) { entry_slot = -1; entry_stack = entry_stack_n++; }
            } else {
                entry_slot = ugpr++ + (has_sret ? 1 : 0);
                if (entry_slot >= 6) { entry_slot = -1; entry_stack = entry_stack_n++; }
            }
            if (isfp)      u[n].fin_slot = xmm_run < 8 ? xmm_run++ : -1;
            else           u[n].fin_slot = gpr_run < 6 ? gpr_run++ : -1;
            u[n].fin_stack = u[n].fin_slot < 0 ? decl_stack_run++ : -1;
            if (isfp) {
                if (entry_slot >= 0) { u[n].src = 2; u[n].src_off = -64 - 16 * entry_slot; }
                else                 { u[n].src = 3; u[n].src_off = -192 - 8 * entry_stack; }
            } else {
                if (entry_slot >= 0) { u[n].src = 1; u[n].src_off = -8 * (entry_slot + 1); }
                else                 { u[n].src = 3; u[n].src_off = -192 - 8 * entry_stack; }
            }
            n++;
        }
    }
    int fin_stack_n = decl_stack_run;

    int fin_stack_n_pre = decl_stack_run;
    int frame = 192 + 8 * entry_stack_n;
    if (fin_stack_n_pre & 1) frame += 8;
    if (frame & 15) frame += 16 - (frame & 15);

    /* ---- prologue ---- */
    me_emit_u8(c, 0x55);                 /* push rbp */
    me_emit_u8(c, 0x48); me_emit_u8(c, 0x89); me_emit_u8(c, 0xE5); /* mov rbp, rsp */
    if (frame >= 128) {
        me_emit_u8(c, 0x48); me_emit_u8(c, 0x81); me_emit_u8(c, 0xEC);
        me_emit_u32(c, (uint32_t)frame);
    } else {
        me_emit_u8(c, 0x48); me_emit_u8(c, 0x83); me_emit_u8(c, 0xEC);
        me_emit_u8(c, (uint8_t)frame);
    }

    /* save entry GPR arg regs -> [rbp-8..-48] */
    static const int gpr_arg_reg[6] = {7, 6, 2, 1, 8, 9};
    for (int s = 0; s < 6; s++) x86_rm(c, 0x89, gpr_arg_reg[s], X86_RBP, -8 * (s + 1));
    /* save entry XMM arg regs -> [rbp-64-16x] */
    for (int x = 0; x < 8; x++) {
        int32_t off = -64 - 16 * x;
        x86_rex(c, 1, x >> 3, 0, 0);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x29);
        if (off >= -128 && off <= 127) {
            x86_modrm(c, 1, x & 7, X86_RBP);
            me_emit_u8(c, (uint8_t)(int8_t)off);
        } else {
            x86_modrm(c, 2, x & 7, X86_RBP);
            me_emit_u32(c, (uint32_t)off);
        }
    }
    /* save entry stack units: [rbp+16+8m] -> [rbp-192-8m] (via r11) */
    for (int m = 0; m < entry_stack_n; m++) {
        x86_rm(c, 0x8B, X86_R11, X86_RBP, 16 + 8 * m);
        x86_rm(c, 0x89, X86_R11, X86_RBP, -192 - 8 * m);
    }

    /* ctx base: movabs r10, 0 + 8-byte absolute data reloc */
    x86_rex(c, 1, 0, 0, 1);
    me_emit_u8(c, 0xBA);                 /* movabs r10, imm64 */
    size_t ctx_patch = me_get_offset(c);
    me_emit_u64(c, 0);
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = ctx_patch;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", nf->tramp_ctx_name);
        r->is_call = false;
    } else {
        zero_cc_reloc_overflow_note(c);
    }

    /* ---- place final REGISTER units ---- */
    static const int gpr_slot_to_reg[6] = {7, 6, 2, 1, 8, 9};
    for (int i = 0; i < n; i++) {
        ZeroCCTrampUnit *a = &u[i];
        if (a->fin_slot < 0) continue;
        if (a->is_fp) {
            bool wide16 = (a->src == 2) || (a->src == 0 && a->src_off % 16 == 0);
            tramp_mov_xmm_mem(c, a->fin_slot, a->src == 0 ? 10 : X86_RBP,
                              a->src_off, wide16);
        } else {
            x86_rm(c, 0x8B, gpr_slot_to_reg[a->fin_slot], a->src == 0 ? 10 : X86_RBP, a->src_off);
        }
    }

    /* ---- final STACK units: push in DESCENDING index order ---- */
    for (int idx = fin_stack_n - 1; idx >= 0; idx--) {
        for (int i = 0; i < n; i++) {
            if (u[i].fin_stack != idx) continue;
            if (u[i].src == 0) tramp_push_mem(c, 10, u[i].src_off);
            else               tramp_push_mem(c, X86_RBP, u[i].src_off);
        }
    }

    /* call the real nested function (same image: exact rel32, no reloc) */
    me_emit_u8(c, 0xE8);
    size_t call_pos = me_get_offset(c);
    int32_t rel = (int32_t)(nf->text_offset - (call_pos + 4));
    me_emit_u32(c, (uint32_t)rel);

    /* the target's `ret` lands here: return to the original caller */
    me_emit_u8(c, 0xC9);                 /* leave */
    me_emit_u8(c, 0xC3);                 /* ret */
}

/* ==========================================================================
 * GNU __int128 (Sprint 109): 128-bit integer ops on a (lo,hi) GPR pair.
 * ========================================================================= */
void x86_emit_add128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo);
    x86_alu(c, 0x01, dlo, b_lo);   /* add dlo, b_lo  (CF) */
    x86_mov_rr(c, dhi, a_hi);
    x86_alu(c, 0x11, dhi, b_hi);   /* adc dhi, b_hi */
}
void x86_emit_sub128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo);
    x86_alu(c, 0x29, dlo, b_lo);   /* sub dlo, b_lo  (CF = borrow) */
    x86_mov_rr(c, dhi, a_hi);
    x86_alu(c, 0x19, dhi, b_hi);   /* sbb dhi, b_hi */
}
void x86_emit_mul128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    int rax = 0, rdx = 2;
    x86_mov_rr(c, rax, a_lo);
    x86_rex(c, 1, 0, 0, (b_lo >> 3) & 1); me_emit_u8(c, 0xF7); x86_modrm(c, 3, 4, b_lo); /* mul b_lo -> rdx:rax */
    x86_mov_rr(c, dlo, rax);   /* dlo = (a_lo*b_lo) low  */
    x86_mov_rr(c, dhi, rdx);   /* dhi = (a_lo*b_lo) high */
    x86_mov_rr(c, rax, a_lo);
    x86_rex(c, 1, 0, 0, (b_hi >> 3) & 1); me_emit_u8(c, 0xF7); x86_modrm(c, 3, 4, b_hi); /* mul b_hi -> rdx:rax */
    x86_alu(c, 0x01, dhi, rax); /* dhi += (a_lo*b_hi) low */
    x86_mov_rr(c, rax, a_hi);
    x86_rex(c, 1, 0, 0, (b_lo >> 3) & 1); me_emit_u8(c, 0xF7); x86_modrm(c, 3, 4, b_lo); /* mul b_lo -> rdx:rax */
    x86_alu(c, 0x01, dhi, rax); /* dhi += (a_hi*b_lo) low */
}
void x86_emit_and128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo); x86_alu(c, 0x21, dlo, b_lo);
    x86_mov_rr(c, dhi, a_hi); x86_alu(c, 0x21, dhi, b_hi);
}
void x86_emit_or128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo); x86_alu(c, 0x09, dlo, b_lo);
    x86_mov_rr(c, dhi, a_hi); x86_alu(c, 0x09, dhi, b_hi);
}
void x86_emit_xor128(ZeroCCCompiler *c, int dlo, int dhi, int a_lo, int a_hi, int b_lo, int b_hi) {
    x86_mov_rr(c, dlo, a_lo); x86_alu(c, 0x31, dlo, b_lo);
    x86_mov_rr(c, dhi, a_hi); x86_alu(c, 0x31, dhi, b_hi);
}
void x86_emit_not128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi) {
    x86_emit_not(c, dlo, s_lo);
    x86_emit_not(c, dhi, s_hi);
}
void x86_emit_neg128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi) {
    x86_emit_not(c, dlo, s_lo);      /* dlo = ~s_lo (CF preserved) */
    x86_emit_add_imm(c, dlo, 1);     /* dlo = ~s_lo+1 = low; CF = (s_lo==0) */
    x86_emit_not(c, dhi, s_hi);      /* dhi = ~s_hi (CF preserved) */
    x86_rex(c, 1, 0, 0, (dhi >> 3) & 1); me_emit_u8(c, 0x83); x86_modrm(c, 3, 2, dhi); me_emit_u8(c, 0x00); /* adc dhi, 0 (0x83 /2) */
}
void x86_emit_mov128(ZeroCCCompiler *c, int dlo, int dhi, int s_lo, int s_hi) {
    x86_mov_rr(c, dlo, s_lo);
    x86_mov_rr(c, dhi, s_hi);
}
void x86_emit_load128(ZeroCCCompiler *c, int dlo, int dhi, int base, int32_t offset) {
    if (dlo == base) {
        x86_rm(c, 0x8B, dhi, base, offset + 8);  /* mov dhi, [base+offset+8] */
        x86_rm(c, 0x8B, dlo, base, offset);      /* mov dlo, [base+offset]   */
    } else {
        x86_rm(c, 0x8B, dlo, base, offset);      /* mov dlo, [base+offset]   */
        x86_rm(c, 0x8B, dhi, base, offset + 8);  /* mov dhi, [base+offset+8] */
    }
}
void x86_emit_store128(ZeroCCCompiler *c, int s_lo, int s_hi, int base, int32_t offset) {
    x86_rm(c, 0x89, s_lo, base, offset);      /* mov [base+offset], s_lo   */
    x86_rm(c, 0x89, s_hi, base, offset + 8);  /* mov [base+offset+8], s_hi */
}
void x86_emit_zext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    x86_mov_rr(c, dlo, src);
    x86_emit_mov_imm(c, dhi, 0);
}
void x86_emit_sext64_128(ZeroCCCompiler *c, int dlo, int dhi, int src) {
    x86_mov_rr(c, dlo, src);
    x86_mov_rr(c, dhi, src);
    x86_rex(c, 1, 0, 0, (dhi >> 3) & 1);
    me_emit_u8(c, 0xC1);
    x86_modrm(c, 3, 7, dhi & 7);  /* SAR r64, imm8 */
    me_emit_u8(c, 63);
}
void x86_emit_trunc128_64(ZeroCCCompiler *c, int dst, int s_lo) {
    x86_mov_rr(c, dst, s_lo);
}
void x86_emit_mov128_imm(ZeroCCCompiler *c, int dlo, int dhi, int64_t imm) {
    x86_emit_mov_imm(c, dlo, imm);
    x86_emit_mov_imm(c, dhi, 0);
}
