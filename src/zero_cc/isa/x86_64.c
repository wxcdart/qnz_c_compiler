#include "../isa_backend.h"
#include "zero_cc/parser/machine_emitter.h"
#include <string.h>

/* x86-64 register info */
static const ISARegInfo x86_64_regs = {
    .int_regs = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
        "r10", "r11", "r12", "r13", "r14", "r15", "rsp", "rbp",
    },
    .fp_regs = {
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
    },
    .int_arg_regs = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"},
    .fp_arg_regs = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"},
    .int_ret_reg = "rax",
    .fp_ret_reg = "xmm0",
    .num_int_regs = 16,
    .num_fp_regs = 16,
};

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
static void x86_rr(ZeroCCCompiler *c, const uint8_t *op, int oplen, int reg, int rm) {
    x86_rex(c, 1, (reg >> 3) & 1, 0, (rm >> 3) & 1);
    for (int i = 0; i < oplen; i++) me_emit_u8(c, op[i]);
    x86_modrm(c, 3, reg, rm);
}

/* Emit a 64-bit register-memory instruction: REX.W <op> ModRM/SIB [base + disp] */
static void x86_rm(ZeroCCCompiler *c, uint8_t op, int reg, int base, int32_t disp) {
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
static void x86_mov_rr(ZeroCCCompiler *c, int dst, int src) {
    static const uint8_t op[] = {0x89};  /* mov r/m64, r64 */
    if (dst != src) x86_rr(c, op, 1, src, dst);
}

/* dst = dst <op> src for "op r/m64, r64" style ALU instructions (add/sub/and/or/xor) */
static void x86_alu(ZeroCCCompiler *c, uint8_t opcode, int dst, int src) {
    x86_rr(c, &opcode, 1, src, dst);
}

/* Three-operand wrapper for commutative ALU ops: dst = src1 <op> src2 */
static void x86_alu3_comm(ZeroCCCompiler *c, uint8_t opcode, int dst, int src1, int src2) {
    if (dst == src2) {           /* avoid clobbering src2 with the initial mov */
        int t = src1; src1 = src2; src2 = t;
    }
    x86_mov_rr(c, dst, src1);
    x86_alu(c, opcode, dst, src2);
}

/* x86-64 backend implementation */
static void x86_emit_prologue(ZeroCCCompiler *c, size_t stack_size) {
    /* push rbp */
    me_emit_u8(c, 0x55);
    /* mov rbp, rsp */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xe5);
    /* sub rsp, stack_size (align to 16 bytes) */
    if (stack_size > 0) {
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x81);
        me_emit_u8(c, 0xec);
        me_emit_u32(c, (uint32_t)stack_size);
    }
}

static void x86_emit_epilogue(ZeroCCCompiler *c) {
    /* leave */
    me_emit_u8(c, 0xc9);
    /* ret */
    me_emit_u8(c, 0xc3);
}

static void x86_emit_mov_imm(ZeroCCCompiler *c, int dst_reg, int64_t imm) {
    /* mov reg, imm64 */
    x86_rex(c, 1, 0, 0, (dst_reg >> 3) & 1);
    me_emit_u8(c, 0xB8 | (dst_reg & 7));
    me_emit_u64(c, imm);
}

static void x86_emit_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x01, dst, src1, src2);
}

static void x86_emit_neg(ZeroCCCompiler *c, int dst, int src);

static void x86_emit_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst == src2 && dst != src1) {
        /* dst = src1 - dst  ==>  dst = -dst + src1 */
        x86_emit_neg(c, dst, dst);
        x86_alu(c, 0x01, dst, src1);
        return;
    }
    x86_mov_rr(c, dst, src1);
    x86_alu(c, 0x29, dst, src2);  /* sub dst, src2 */
}

static void x86_emit_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    static const uint8_t imul[] = {0x0F, 0xAF};  /* imul r64, r/m64 */
    if (dst == src2) { int t = src1; src1 = src2; src2 = t; }
    x86_mov_rr(c, dst, src1);
    x86_rr(c, imul, 2, dst, src2);
}

static void x86_emit_cqo(ZeroCCCompiler *c) {
    /* cqo - sign extend rax into rdx:rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x99);
}

static void x86_emit_idiv(ZeroCCCompiler *c, int src) {
    /* idiv r/m64  (F7 /7) */
    x86_rex(c, 1, 0, 0, (src >> 3) & 1);
    me_emit_u8(c, 0xF7);
    x86_modrm(c, 3, 7, src);
}

/* Shared signed division: quotient (want_rem=0) or remainder (want_rem=1) of src1/src2 into dst.
 * Clobbers rax, rdx and r11. */
static void x86_divmod(ZeroCCCompiler *c, int dst, int src1, int src2, int want_rem) {
    int divisor = src2;
    if (divisor == X86_RAX || divisor == X86_RDX) {
        x86_mov_rr(c, X86_R11, divisor);  /* keep divisor out of rax/rdx */
        divisor = X86_R11;
    }
    /* Save rdx if it holds a live temp that is not the result register. */
    bool save_rdx = (dst != X86_RDX && src1 != X86_RDX && src2 != X86_RDX && divisor != X86_RDX);
    bool save_rax = (dst != X86_RAX && src1 != X86_RAX && src2 != X86_RAX && divisor != X86_RAX);
    
    if (save_rdx) me_emit_u8(c, 0x52);  /* push rdx */
    if (save_rax) me_emit_u8(c, 0x50);  /* push rax */
    
    x86_mov_rr(c, X86_RAX, src1);
    x86_emit_cqo(c);
    x86_emit_idiv(c, divisor);
    
    /* Move result to dst before restoring */
    if (dst != X86_RAX && dst != X86_RDX) {
        x86_mov_rr(c, dst, want_rem ? X86_RDX : X86_RAX);
    }
    /* If dst is rax or rdx, result is already there */
    
    if (save_rdx) me_emit_u8(c, 0x5A);  /* pop rdx */
    if (save_rax) me_emit_u8(c, 0x58);  /* pop rax */
}

static void x86_emit_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_divmod(c, dst, src1, src2, 0);
}

static void x86_emit_mod(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_divmod(c, dst, src1, src2, 1);
}

static void x86_emit_and(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x21, dst, src1, src2);
}

static void x86_emit_or(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x09, dst, src1, src2);
}

static void x86_emit_xor(ZeroCCCompiler *c, int dst, int src1, int src2) {
    x86_alu3_comm(c, 0x31, dst, src1, src2);
}

static void x86_emit_shl(ZeroCCCompiler *c, int dst, int src, int shift) {
    x86_mov_rr(c, dst, src);
    x86_rex(c, 1, 0, 0, (dst >> 3) & 1);
    if (shift == 1) {
        me_emit_u8(c, 0xD1);            /* shl r/m64, 1   (D1 /4) */
        x86_modrm(c, 3, 4, dst);
    } else {
        me_emit_u8(c, 0xC1);            /* shl r/m64, imm8 (C1 /4) */
        x86_modrm(c, 3, 4, dst);
        me_emit_u8(c, shift & 0xFF);
    }
}

static void x86_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    /* cmp src1, src2  (flags from src1 - src2) */
    x86_alu(c, 0x39, src1, src2);
}

static void x86_emit_neg(ZeroCCCompiler *c, int dst, int src) {
    x86_mov_rr(c, dst, src);
    x86_rex(c, 1, 0, 0, (dst >> 3) & 1);
    me_emit_u8(c, 0xF7);                /* neg r/m64 (F7 /3) */
    x86_modrm(c, 3, 3, dst);
}

static void x86_emit_load(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* mov dst, [base + offset] */
    x86_rm(c, 0x8B, dst_reg, base_reg, offset);
}

static void x86_emit_store(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* mov [base + offset], src */
    x86_rm(c, 0x89, src_reg, base_reg, offset);
}

static size_t x86_emit_jmp(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0xE9);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);  /* placeholder for rel32 patch */
    /* Record this patch for later resolution */
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jeq(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x84);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jne(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x85);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jlt(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8C);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jle(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8E);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jgt(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8F);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static size_t x86_emit_jge(ZeroCCCompiler *c, const char *label) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8D);
    size_t patch_pos = me_get_offset(c);  /* rel32 field */
    me_emit_u32(c, 0);
    for (int i = 0; i < c->label_count; i++) {
        if (c->labels[i].name && strcmp(c->labels[i].name, label) == 0) {
            if (c->labels[i].patch_count < 16) {
                c->labels[i].patches[c->labels[i].patch_count++] = patch_pos;
            }
            break;
        }
    }
    return patch_pos;
}

static void x86_emit_call(ZeroCCCompiler *c, const char *func_name) {
    me_emit_u8(c, 0xE8);
    me_emit_u32(c, 0);
    (void)func_name;
}

static void x86_emit_ret(ZeroCCCompiler *c) {
    me_emit_u8(c, 0xC3);
}

/* Floating point operations */
static void x86_emit_load_fp(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset) {
    /* movsd xmm, [base + offset] */
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);
    x86_rex(c, 0, 0, 0, (base_reg >> 3) | ((dst_reg >> 3) << 1));
    if (offset == 0 && base_reg != 5 && base_reg != 13) {
        x86_modrm(c, 0, dst_reg & 7, base_reg & 7);
    } else {
        x86_modrm(c, 1, dst_reg & 7, base_reg & 7);
        me_emit_u32(c, (uint32_t)offset);
    }
}

static void x86_emit_store_fp(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset) {
    /* movsd [base + offset], xmm */
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x11);
    x86_rex(c, 0, 0, 0, (base_reg >> 3) | ((src_reg >> 3) << 1));
    if (offset == 0 && base_reg != 5 && base_reg != 13) {
        x86_modrm(c, 0, src_reg & 7, base_reg & 7);
    } else {
        x86_modrm(c, 1, src_reg & 7, base_reg & 7);
        me_emit_u32(c, (uint32_t)offset);
    }
}

static void x86_emit_load_fp_imm(ZeroCCCompiler *c, int dst_reg, double value) {
    /* Store to rodata and load via RIP-relative addressing */
    size_t rodata_offset = c->rodata_section.length;
    zero_cc_buf_put_bytes(&c->rodata_section, (const uint8_t*)&value, sizeof(value));
    
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movsd xmm, m64 */
    me_emit_u8(c, 0x05);  /* RIP-relative */
    size_t patch_pos = me_get_offset(c);
    me_emit_u32(c, 0);
    
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_relocs[c->str_reloc_count].dst_reg = dst_reg;
        c->str_reloc_count++;
    }
    /* Emit REX prefix for dst_reg after relocation is patched */
    x86_rex(c, 0, 0, 0, dst_reg >> 3);
}

static void x86_emit_fp_add(ZeroCCCompiler *c, int dst, int src1, int src2) {
    /* addsd dst, src2 (dst = src1 + src2) */
    if (dst != src1) {
        x86_rex(c, 0, 0, 0, src1 >> 3);
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);  /* movsd */
        x86_modrm(c, 3, dst & 7, src1 & 7);
    }
    x86_rex(c, 0, 0, 0, src2 >> 3);
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x58);  /* addsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_sub(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst != src1) {
        x86_rex(c, 0, 0, 0, src1 >> 3);
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        x86_modrm(c, 3, dst & 7, src1 & 7);
    }
    x86_rex(c, 0, 0, 0, src2 >> 3);
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5C);  /* subsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_mul(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst != src1) {
        x86_rex(c, 0, 0, 0, src1 >> 3);
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        x86_modrm(c, 3, dst & 7, src1 & 7);
    }
    x86_rex(c, 0, 0, 0, src2 >> 3);
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x59);  /* mulsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_div(ZeroCCCompiler *c, int dst, int src1, int src2) {
    if (dst != src1) {
        x86_rex(c, 0, 0, 0, src1 >> 3);
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);
        x86_modrm(c, 3, dst & 7, src1 & 7);
    }
    x86_rex(c, 0, 0, 0, src2 >> 3);
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x5E);  /* divsd */
    x86_modrm(c, 3, dst & 7, src2 & 7);
}

static void x86_emit_fp_neg(ZeroCCCompiler *c, int dst, int src) {
    /* xorps with sign bit mask: xorpd dst, [mask] */
    /* First move src to dst if different */
    if (dst != src) {
        x86_rex(c, 0, 0, 0, src >> 3);
        me_emit_u8(c, 0xF2);
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, 0x10);  /* movsd */
        x86_modrm(c, 3, dst & 7, src & 7);
    }
    /* Use xorpd with sign bit: xorpd xmm_dst, [neg_mask] */
    static const double neg_mask = -0.0;
    size_t off = c->rodata_section.length;
    zero_cc_buf_put_bytes(&c->rodata_section, (const uint8_t*)&neg_mask, sizeof(neg_mask));
    me_emit_u8(c, 0x66);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x54);  /* xorpd xmm, m128 */
    x86_rex(c, 0, 0, 0, dst >> 3);
    me_emit_u8(c, 0x05);  /* RIP-relative */
    size_t p = me_get_offset(c);
    me_emit_u32(c, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = p;
        c->str_relocs[c->str_reloc_count].rodata_offset = off;
        c->str_reloc_count++;
    }
}

static void x86_emit_fp_cmp(ZeroCCCompiler *c, int src1, int src2) {
    x86_rex(c, 0, 0, 0, (src1 >> 3) | ((src2 >> 3) << 1));
    me_emit_u8(c, 0x66);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2F);  /* ucomisd */
    x86_modrm(c, 3, src1 & 7, src2 & 7);
}

static void x86_emit_fp_cvtsi(ZeroCCCompiler *c, int int_dst, int fp_src) {
    /* cvttsd2si reg, xmm */
    x86_rex(c, 1, 0, 0, (int_dst >> 3) | ((fp_src >> 3) << 1));
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2C);
    x86_modrm(c, 3, int_dst & 7, fp_src & 7);
}

static void x86_emit_fp_cvtsf(ZeroCCCompiler *c, int fp_dst, int int_src) {
    /* cvtsi2sd xmm, reg */
    x86_rex(c, 1, 0, 0, (int_src >> 3) | ((fp_dst >> 3) << 1));
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x2A);
    x86_modrm(c, 3, fp_dst & 7, int_src & 7);
}

static void x86_emit_fp_mov(ZeroCCCompiler *c, int dst, int src) {
    if (dst == src) return;
    x86_rex(c, 0, 0, 0, (src >> 3) | ((dst >> 3) << 1));
    me_emit_u8(c, 0xF2);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x10);  /* movsd */
    x86_modrm(c, 3, dst & 7, src & 7);
}

static void x86_emit_const(ZeroCCCompiler *c, int64_t value) {
    x86_emit_mov_imm(c, 0, value);  /* rax */
}

static void x86_emit_stack_adjust(ZeroCCCompiler *c, int32_t delta) {
    if (delta == 0) return;
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x81);
    me_emit_u8(c, delta > 0 ? 0xC4 : 0xEC);  /* add/sub rsp */
    me_emit_u32(c, (uint32_t)(delta > 0 ? delta : -delta));
}

static void x86_emit_push_reg(ZeroCCCompiler *c, int reg) {
    /* push is 64-bit by default; REX.B only for r8-r15 */
    x86_rex(c, 0, 0, 0, (reg >> 3) & 1);
    me_emit_u8(c, 0x50 | (reg & 7));
}

static void x86_emit_pop_reg(ZeroCCCompiler *c, int reg) {
    x86_rex(c, 0, 0, 0, (reg >> 3) & 1);
    me_emit_u8(c, 0x58 | (reg & 7));
}

/* add rax, imm */
static void x86_emit_add_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x83);
    me_emit_u8(c, 0xc0);
    me_emit_u8(c, (uint8_t)imm);
}

/* sub rax, imm */
static void x86_emit_sub_imm(ZeroCCCompiler *c, int dst_reg, int16_t imm) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x83);
    me_emit_u8(c, 0xe8);
    me_emit_u8(c, (uint8_t)imm);
}

/* inc rax */
static void x86_emit_inc(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0xff);
    me_emit_u8(c, 0xc0);
}

/* dec rax */
static void x86_emit_dec(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0xff);
    me_emit_u8(c, 0xc8);
}

/* shr rax, rbx */
static void x86_emit_shr(ZeroCCCompiler *c, int dst_reg, int src1, int src2) {
    (void)dst_reg; (void)src1; (void)src2; /* Always rax, rbx */
    x86_rex(c, 1, 0, 0, 3); /* rax, rbx */
    me_emit_u8(c, 0xd3);
    x86_modrm(c, 3, 5, 3); /* shr rax, rbx */
}

/* setl al, zero-extend to rax */
static void x86_emit_setl(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x9c);
    me_emit_u8(c, 0xc0);  /* setl al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);  /* movzx rax, al */
}

/* setle al, zero-extend to rax */
static void x86_emit_setle(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x9e);
    me_emit_u8(c, 0xc0);  /* setle al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);  /* movzx rax, al */
}

/* setg al, zero-extend to rax */
static void x86_emit_setg(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x9f);
    me_emit_u8(c, 0xc0);  /* setg al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);  /* movzx rax, al */
}

/* setge al, zero-extend to rax */
static void x86_emit_setge(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x9d);
    me_emit_u8(c, 0xc0);  /* setge al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);  /* movzx rax, al */
}

/* sete al, zero-extend to rax */
static void x86_emit_sete(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x94);
    me_emit_u8(c, 0xc0);  /* sete al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);  /* movzx rax, al */
}

/* setne al, zero-extend to rax */
static void x86_emit_setne(ZeroCCCompiler *c, int dst_reg) {
    (void)dst_reg; /* Always rax */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x95);
    me_emit_u8(c, 0xc0);  /* setne al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);  /* movzx rax, al */
}

/* deref: mov dst, [src] */
static void x86_emit_deref(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    /* mov dst, [src] - register indirect addressing */
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (src_reg >> 3) & 1);
    me_emit_u8(c, 0x8B);
    x86_modrm(c, 0, dst_reg & 7, src_reg & 7);
}

/* load_byte: movzx dst, byte ptr [src] */
static void x86_emit_load_byte(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    x86_rex(c, 1, (dst_reg >> 3) & 1, 0, (src_reg >> 3) & 1);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xB6);
    x86_modrm(c, 0, dst_reg & 7, src_reg & 7);
}

/* cmp rax, 0 */
static void x86_emit_cmp_zero(ZeroCCCompiler *c, int src_reg) {
    (void)src_reg; /* Always rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x3d);
    me_emit_u8(c, 0x00);
    me_emit_u32(c, 0);
}

/* not rax */
static void x86_emit_not(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    (void)dst_reg; (void)src_reg; /* Always rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0xf7);
    me_emit_u8(c, 0xd0);
}

/* lnot: !rax (test + sete + movzx) */
static void x86_emit_lnot(ZeroCCCompiler *c, int dst_reg, int src_reg) {
    (void)dst_reg; (void)src_reg; /* Always rax */
    /* test rax, rax */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x85);
    me_emit_u8(c, 0xc0);
    /* sete al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0x94);
    me_emit_u8(c, 0xc0);
    /* movzx rax, al */
    me_emit_u8(c, 0x0f);
    me_emit_u8(c, 0xb6);
    me_emit_u8(c, 0xc0);
}

static void x86_emit_label(ZeroCCCompiler *c, const char *name) {
    /* Record label position for jump patching */
    if (c->label_count < 32) {
        c->labels[c->label_count].name = name;
        c->labels[c->label_count].offset = me_get_offset(c);
        c->labels[c->label_count].patch_count = 0;
        c->label_count++;
    }
}

static void x86_emit_test(ZeroCCCompiler *c, int src1, int src2) {
    /* test src1, src2 */
    x86_alu(c, 0x85, src1, src2);
}

static void x86_emit_lea(ZeroCCCompiler *c, int dst, int base, int32_t offset) {
    /* lea dst, [base + offset] */
    x86_rm(c, 0x8D, dst, base, offset);
}

/* Resolve all recorded label patches */
static void x86_resolve_labels(ZeroCCCompiler *c) {
    for (int i = 0; i < c->label_count; i++) {
        size_t target = c->labels[i].offset;
        for (int j = 0; j < c->labels[i].patch_count; j++) {
            size_t patch_offset = c->labels[i].patches[j];
            int32_t rel = (int32_t)(target - (patch_offset + 4));
            if (patch_offset + 4 <= c->text_section.length) {
                me_patch_u32(c, patch_offset, (uint32_t)rel);
            }
        }
    }
}

/* patch_offset is the position of the rel32 field (as returned by emit_j*) */
static size_t x86_patch_jmp(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - (patch_offset + 4));
    if (patch_offset + 4 <= c->text_section.length) {
        me_patch_u32(c, patch_offset, (uint32_t)rel);
    }
    return patch_offset;
}

/* x86-64 backend instance */

/* x86-64 VREG mapping: direct pass-through (VREG numbers = x86 encodings) */
static int x86_get_int_reg(int vreg) { return vreg; }
static int x86_get_fp_reg(int vreg) { return vreg; }

const ZeroCCISABackend x86_64_backend = {
    .isa = ISA_X86_64,
    .regs = &x86_64_regs,
    .fp_reg = X86_RBP,  /* hardware encoding 5 */
    .sp_reg = X86_RSP,  /* hardware encoding 4 */
    
    .emit_prologue = x86_emit_prologue,
    .emit_epilogue = x86_emit_epilogue,
    
    .emit_mov_imm = x86_emit_mov_imm,
    .emit_add = x86_emit_add,
    .emit_sub = x86_emit_sub,
    .emit_mul = x86_emit_mul,
    .emit_div = x86_emit_div,
    .emit_mod = x86_emit_mod,
    .emit_and = x86_emit_and,
    .emit_or = x86_emit_or,
    .emit_xor = x86_emit_xor,
    .emit_shl = x86_emit_shl,
    .emit_cmp = x86_emit_cmp,
    .emit_neg = x86_emit_neg,
    .emit_test = x86_emit_test,
    .emit_cqo = x86_emit_cqo,
    .emit_idiv = x86_emit_idiv,
    .emit_lea = x86_emit_lea,
    .patch_jmp = x86_patch_jmp,
    .resolve_labels = x86_resolve_labels,
    
    /* VREG mapping */
    .get_int_reg = x86_get_int_reg,
    .get_fp_reg = x86_get_fp_reg,
    
    .emit_load = x86_emit_load,
    .emit_store = x86_emit_store,
    
    .emit_jmp = x86_emit_jmp,
    .emit_jeq = x86_emit_jeq,
    .emit_jne = x86_emit_jne,
    .emit_jlt = x86_emit_jlt,
    .emit_jle = x86_emit_jle,
    .emit_jgt = x86_emit_jgt,
    .emit_jge = x86_emit_jge,
    .emit_call = x86_emit_call,
    .emit_ret = x86_emit_ret,
    
    .emit_load_fp = x86_emit_load_fp,
    .emit_store_fp = x86_emit_store_fp,
    .emit_load_fp_imm = x86_emit_load_fp_imm,
    .emit_fp_add = x86_emit_fp_add,
    .emit_fp_sub = x86_emit_fp_sub,
    .emit_fp_mul = x86_emit_fp_mul,
    .emit_fp_div = x86_emit_fp_div,
    .emit_fp_neg = x86_emit_fp_neg,
    .emit_fp_cmp = x86_emit_fp_cmp,
    .emit_fp_cvtsi = x86_emit_fp_cvtsi,
    .emit_fp_cvtsf = x86_emit_fp_cvtsf,
    .emit_fp_mov = x86_emit_fp_mov,
    
    .emit_const = x86_emit_const,
    .emit_stack_adjust = x86_emit_stack_adjust,
    .emit_push_reg = x86_emit_push_reg,
    .emit_pop_reg = x86_emit_pop_reg,
    
    /* Additional operations for IR-based codegen */
    .emit_mov_reg = x86_mov_rr,
    .emit_add_imm = x86_emit_add_imm,
    .emit_sub_imm = x86_emit_sub_imm,
    .emit_inc = x86_emit_inc,
    .emit_dec = x86_emit_dec,
    .emit_shr = x86_emit_shr,
    .emit_setl = x86_emit_setl,
    .emit_setle = x86_emit_setle,
    .emit_setg = x86_emit_setg,
    .emit_setge = x86_emit_setge,
    .emit_sete = x86_emit_sete,
    .emit_setne = x86_emit_setne,
    .emit_deref = x86_emit_deref,
    .emit_load_byte = x86_emit_load_byte,
    .emit_cmp_zero = x86_emit_cmp_zero,
    .emit_not = x86_emit_not,
    .emit_lnot = x86_emit_lnot,
    .emit_label = x86_emit_label,
};

const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa) {
    switch (isa) {
        case ISA_X86_64: return &x86_64_backend;
        default: return NULL;
    }
}

const ZeroCCISABackend *zero_cc_get_native_backend(void) {
#if defined(__x86_64__)
    return &x86_64_backend;
#elif defined(__aarch64__)
    return &aarch64_backend;
#elif defined(__riscv) && __riscv_xlen == 64
    return &riscv64_backend;
#elif defined(__riscv) && __riscv_xlen == 32
    return &riscv32_backend;
#else
    return &x86_64_backend;  /* fallback */
#endif
}
/* =========================================================================
 * Virtual Register (VREG) Mapping for x86-64
 * =========================================================================
 *
 * The lowering code uses virtual registers defined in ../types/vregs.h.
 * Each VREG number is interpreted as the x86-64 hardware encoding:
 *
 *   VREG_0  = 0 = rax   (result / return value)
 *   VREG_1  = 1 = rcx   (temp / 4th arg)
 *   VREG_2  = 2 = rdx   (temp / 3rd arg)
 *   VREG_3  = 3 = rbx   (temp / callee-saved)
 *   VREG_4  = 4 = rsp   (stack pointer - use BE(c)->sp_reg instead)
 *   VREG_5  = 5 = rbp   (frame pointer - use BE(c)->fp_reg instead)
 *   VREG_6  = 6 = rsi   (temp / 2nd arg)
 *   VREG_7  = 7 = rdi   (temp / 1st arg)
 *   VREG_8  = 8 = r8    (temp / 5th arg)
 *   VREG_9  = 9 = r9    (temp / 6th arg)
 *   VREG_10 = 10 = r10  (temp)
 *   VREG_11 = 11 = r11  (temp)
 *   VREG_12 = 12 = r12  (temp / callee-saved)
 *   VREG_13 = 13 = r13  (temp / callee-saved)
 *   VREG_14 = 14 = r14  (temp / callee-saved)
 *   VREG_15 = 15 = r15  (temp / callee-saved)
 *
 * Float registers (VREG_F0, VREG_F1, ...):
 *   VREG_F0 = 0 = xmm0  (float result / return value)
 *   VREG_F1 = 1 = xmm1  (float temp / 2nd float arg)
 *   VREG_F2 = 2 = xmm2  (float temp / 3rd float arg)
 *   ...
 *
 * Special registers:
 *   BE(c)->sp_reg = 4 (rsp) - use this instead of VREG_4
 *   BE(c)->fp_reg = 5 (rbp) - use this instead of VREG_5
 *
 * Note: The lowering code should use VREG_0-VREG_15 for general purposes,
 * and BE(c)->sp_reg / BE(c)->fp_reg for stack/frame pointers.
 */
