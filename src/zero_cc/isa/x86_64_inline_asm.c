#ifndef ZERO_CC_ISA_X86_64_INLINE_ASM_C
#define ZERO_CC_ISA_X86_64_INLINE_ASM_C

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/machine_emitter.h"

void zc_mc_record_or_patch_label(ZeroCCCompiler *c, size_t patch_pos, uint16_t label_idx);

/* Operand representation for lightweight x86-64 integrated assembler */
typedef enum {
    ASM_OP_NONE = 0,
    ASM_OP_REG,
    ASM_OP_IMM,
    ASM_OP_MEM,
    ASM_OP_LABEL,
} AsmOperandKind;

typedef struct {
    AsmOperandKind kind;
    int reg;         /* 0..15 */
    int width;       /* 8, 16, 32, 64 */
    int64_t imm;
    int base_reg;    /* 0..15, or -1 if none */
    int index_reg;   /* 0..15, or -1 if none */
    int scale;       /* 1, 2, 4, 8 */
    int32_t disp;
} AsmOperand;

static const char *asm_skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    return p;
}

static int x86_lookup_reg(const char *name, int *out_width) {
    static const char *n64[16] = {
        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    static const char *n32[16] = {
        "eax","ecx","edx","ebx","esp","ebp","esi","edi",
        "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"
    };
    static const char *n16[16] = {
        "ax","cx","dx","bx","sp","bp","si","di",
        "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"
    };
    static const char *n8[16] = {
        "al","cl","dl","bl","spl","bpl","sil","dil",
        "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"
    };
    for (int i = 0; i < 16; i++) {
        if (strcmp(name, n64[i]) == 0) { if (out_width) *out_width = 64; return i; }
        if (strcmp(name, n32[i]) == 0) { if (out_width) *out_width = 32; return i; }
        if (strcmp(name, n16[i]) == 0) { if (out_width) *out_width = 16; return i; }
        if (strcmp(name, n8[i]) == 0)  { if (out_width) *out_width = 8;  return i; }
    }
    if (strcmp(name, "ah") == 0) { if (out_width) *out_width = 8; return 4; }
    if (strcmp(name, "ch") == 0) { if (out_width) *out_width = 8; return 5; }
    if (strcmp(name, "dh") == 0) { if (out_width) *out_width = 8; return 6; }
    if (strcmp(name, "bh") == 0) { if (out_width) *out_width = 8; return 7; }
    return -1;
}

static const char *x86_parse_operand(const char *p, AsmOperand *op) {
    p = asm_skip_ws(p);
    memset(op, 0, sizeof(*op));
    op->base_reg = -1;
    op->index_reg = -1;

    if (!*p || *p == ',' || *p == ';' || *p == '\n') return p;

    if (*p == '$') {
        p++;
        char *end;
        op->imm = (int64_t)strtoll(p, &end, 0);
        op->kind = ASM_OP_IMM;
        return asm_skip_ws(end);
    }

    if (*p == '%') {
        p++;
        char reg_name[16];
        int len = 0;
        while (isalnum((unsigned char)*p) && len < 15) {
            reg_name[len++] = *p++;
        }
        reg_name[len] = '\0';
        int w = 0;
        int r = x86_lookup_reg(reg_name, &w);
        if (r >= 0) {
            op->kind = ASM_OP_REG;
            op->reg = r;
            op->width = w;
            return asm_skip_ws(p);
        }
        return NULL;
    }

    /* Label operand: .L<id> */
    if (*p == '.' && p[1] == 'L') {
        p += 2;
        char *end;
        uint16_t label_id = (uint16_t)strtoul(p, &end, 10);
        op->kind = ASM_OP_LABEL;
        op->imm = (int64_t)label_id;
        return asm_skip_ws(end);
    }

    /* Memory: [disp] ( %base [, %index [, scale]] ) */
    int32_t disp = 0;
    if (*p == '-' || isdigit((unsigned char)*p)) {
        char *end;
        disp = (int32_t)strtol(p, &end, 0);
        p = asm_skip_ws(end);
    }

    if (*p == '(') {
        p++;
        p = asm_skip_ws(p);
        int base = -1;
        if (*p == '%') {
            p++;
            char reg_name[16];
            int len = 0;
            while (isalnum((unsigned char)*p) && len < 15) {
                reg_name[len++] = *p++;
            }
            reg_name[len] = '\0';
            base = x86_lookup_reg(reg_name, NULL);
        }
        p = asm_skip_ws(p);
        int index = -1, scale = 1;
        if (*p == ',') {
            p++;
            p = asm_skip_ws(p);
            if (*p == '%') {
                p++;
                char reg_name[16];
                int len = 0;
                while (isalnum((unsigned char)*p) && len < 15) {
                    reg_name[len++] = *p++;
                }
                reg_name[len] = '\0';
                index = x86_lookup_reg(reg_name, NULL);
            }
            p = asm_skip_ws(p);
            if (*p == ',') {
                p++;
                p = asm_skip_ws(p);
                if (*p == '$') p++;
                char *end;
                scale = (int)strtol(p, &end, 0);
                p = asm_skip_ws(end);
            }
        }
        if (*p == ')') p++;
        op->kind = ASM_OP_MEM;
        op->base_reg = base;
        op->index_reg = index;
        op->scale = scale ? scale : 1;
        op->disp = disp;
        return asm_skip_ws(p);
    }

    return NULL;
}

static void x86_emit_mem_modrm(ZeroCCCompiler *c, int reg_or_subop, int base, int index, int scale, int32_t disp) {
    int mod;
    if (disp == 0 && (base & 7) != 5) {
        mod = 0;
    } else if (disp >= -128 && disp <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }

    if (index >= 0 || (base & 7) == 4) {
        x86_modrm(c, mod, reg_or_subop, 4);
        int ss = 0;
        if (scale == 2) ss = 1;
        else if (scale == 4) ss = 2;
        else if (scale == 8) ss = 3;
        int idx = (index >= 0) ? (index & 7) : 4;
        me_emit_u8(c, (ss << 6) | (idx << 3) | (base & 7));
    } else {
        x86_modrm(c, mod, reg_or_subop, base & 7);
    }

    if (mod == 1) {
        me_emit_u8(c, (uint8_t)(int8_t)disp);
    } else if (mod == 2 || (mod == 0 && (base & 7) == 5)) {
        me_emit_u32(c, (uint32_t)disp);
    }
}

static void x86_emit_mov_reg_width(ZeroCCCompiler *c, int dst, int src, int width) {
    if (dst == src) return;
    if (width == 64) {
        x86_mov_rr(c, dst, src);
    } else if (width == 32) {
        x86_rex(c, 0, (src >> 3) & 1, 0, (dst >> 3) & 1);
        me_emit_u8(c, 0x89);
        x86_modrm(c, 3, src, dst);
    } else if (width == 16) {
        me_emit_u8(c, 0x66);
        x86_rex(c, 0, (src >> 3) & 1, 0, (dst >> 3) & 1);
        me_emit_u8(c, 0x89);
        x86_modrm(c, 3, src, dst);
    } else if (width == 8) {
        x86_rex(c, 0, (src >> 3) & 1, 0, (dst >> 3) & 1);
        me_emit_u8(c, 0x88);
        x86_modrm(c, 3, src, dst);
    }
}

static void x86_mc_filter_dialect(const char *in, char *out, size_t out_cap) {
    size_t o = 0;
    while (*in && o + 1 < out_cap) {
        if (*in == '{') {
            const char *end_brace = strchr(in, '}');
            const char *pipe_ch = end_brace ? memchr(in, '|', (size_t)(end_brace - in)) : NULL;
            if (end_brace && pipe_ch) {
                in++;
                while (in < pipe_ch && o + 1 < out_cap) {
                    out[o++] = *in++;
                }
                in = end_brace + 1;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}

/* Interpolate template string into expanded AT&T text buffer */
static int x86_interpolate_template(ZeroCCCompiler *c, size_t idx, char *buf, size_t buf_cap) {
    const Instr *instr = ir_get(c, idx);
    if (!instr) return -1;
    const IRAsmArgs *aa = ir_asm_args_find(c, idx);
    if (!aa || !aa->template_str) return -1;

    size_t out_len = 0;
#define EMIT_CH(ch) do { \
    if (out_len + 1 < buf_cap) buf[out_len++] = (ch); \
    else return -1; \
} while(0)
#define EMIT_STR(str) do { \
    const char *_s = (str); \
    while (*_s) { EMIT_CH(*_s++); } \
} while(0)

    char filtered_tpl[4096];
    x86_mc_filter_dialect(aa->template_str, filtered_tpl, sizeof(filtered_tpl));
    const char *p = filtered_tpl;
    while (*p) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == '%') {
                EMIT_CH('%');
                p++;
                continue;
            }
            if (*p == 'l') {
                if (p[1] == '[') {
                    p += 2;
                    char lname[64];
                    int llen = 0;
                    while (*p && *p != ']' && llen < 63) lname[llen++] = *p++;
                    lname[llen] = '\0';
                    if (*p == ']') p++;
                    int target_lbl = -1;
                    for (int li = 0; li < aa->goto_label_count; li++) {
                        if (strcmp(aa->goto_label_names[li], lname) == 0) {
                            target_lbl = (int)aa->goto_labels[li];
                            break;
                        }
                    }
                    if (target_lbl >= 0) {
                        char lbuf[32];
                        snprintf(lbuf, sizeof(lbuf), ".L%d", target_lbl);
                        EMIT_STR(lbuf);
                    } else {
                        EMIT_STR("%bad_label");
                    }
                    continue;
                } else if (p[1] >= '0' && p[1] <= '9') {
                    int n = p[1] - '0';
                    p += 2;
                    int target_lbl = -1;
                    if (n >= aa->op_count && (n - aa->op_count) < aa->goto_label_count) {
                        target_lbl = (int)aa->goto_labels[n - aa->op_count];
                    } else if (n < aa->goto_label_count) {
                        target_lbl = (int)aa->goto_labels[n];
                    }
                    if (target_lbl >= 0) {
                        char lbuf[32];
                        snprintf(lbuf, sizeof(lbuf), ".L%d", target_lbl);
                        EMIT_STR(lbuf);
                    } else {
                        EMIT_STR("%bad_label");
                    }
                    continue;
                }
            }
            if (*p >= '0' && *p <= '9') {
                int n = *p - '0';
                p++;
                int width = 0;
                if (*p == 'a') { p++; }
                else if (*p == 'q') { p++; width = 64; }
                else if (*p == 'l') { p++; width = 32; }
                else if (*p == 'w') { p++; width = 16; }
                else if (*p == 'b' || *p == 'c') { p++; width = 8; }
                if (n >= aa->op_count) {
                    EMIT_STR("%bad");
                    continue;
                }
                const IRAsmOperand *op = &aa->ops[n];
                if (op->kind == ZC_ASM_OP_OUT_M || op->kind == ZC_ASM_OP_IN_M) {
                    int phys = regalloc_get_reg_for_temp(&c->regalloc, op->temp);
                    EMIT_CH('(');
                    EMIT_CH('%');
                    EMIT_STR(BE(c)->get_hw_reg_name(phys));
                    EMIT_CH(')');
                } else {
                    int phys = regalloc_get_reg_for_temp(&c->regalloc, op->temp);
                    int w = width ? width : (op->width ? op->width : 64);
                    EMIT_CH('%');
                    EMIT_STR(x86_asm_operand_reg_name(phys, w));
                }
                continue;
            }
            EMIT_CH('%');
            EMIT_CH(*p);
            p++;
            continue;
        }
        EMIT_CH(*p);
        p++;
    }
    buf[out_len] = '\0';
    return 0;
#undef EMIT_CH
#undef EMIT_STR
}

/* Assemble a single AT&T instruction into machine code */
static int x86_assemble_single_stmt(ZeroCCCompiler *c, const char *stmt) {
    stmt = asm_skip_ws(stmt);
    if (!*stmt || *stmt == '#' || (stmt[0] == '/' && stmt[1] == '/')) return 0;

    int is_lock = 0;
    if (strncmp(stmt, "lock", 4) == 0 && (stmt[4] == ' ' || stmt[4] == '\t')) {
        is_lock = 1;
        stmt = asm_skip_ws(stmt + 4);
    }

    char mnem[32];
    size_t mlen = 0;
    while (*stmt && !isspace((unsigned char)*stmt) && *stmt != ';' && mlen + 1 < sizeof(mnem)) {
        mnem[mlen++] = *stmt++;
    }
    mnem[mlen] = '\0';
    stmt = asm_skip_ws(stmt);

    /* Check for zero-operand instructions */
    if (strcmp(mnem, "nop") == 0)     { me_emit_u8(c, 0x90); return 0; }
    if (strcmp(mnem, "rdtsc") == 0)   { me_emit_u8(c, 0x0F); me_emit_u8(c, 0x31); return 0; }
    if (strcmp(mnem, "cpuid") == 0)   { me_emit_u8(c, 0x0F); me_emit_u8(c, 0xA2); return 0; }
    if (strcmp(mnem, "pause") == 0)   { me_emit_u8(c, 0xF3); me_emit_u8(c, 0x90); return 0; }
    if (strcmp(mnem, "syscall") == 0) { me_emit_u8(c, 0x0F); me_emit_u8(c, 0x05); return 0; }
    if (strcmp(mnem, "mfence") == 0)  { me_emit_u8(c, 0x0F); me_emit_u8(c, 0xAE); me_emit_u8(c, 0xF0); return 0; }
    if (strcmp(mnem, "lfence") == 0)  { me_emit_u8(c, 0x0F); me_emit_u8(c, 0xAE); me_emit_u8(c, 0xE8); return 0; }
    if (strcmp(mnem, "sfence") == 0)  { me_emit_u8(c, 0x0F); me_emit_u8(c, 0xAE); me_emit_u8(c, 0xF8); return 0; }
    if (strcmp(mnem, "cld") == 0)     { me_emit_u8(c, 0xFC); return 0; }
    if (strcmp(mnem, "std") == 0)     { me_emit_u8(c, 0xFD); return 0; }
    if (strcmp(mnem, "leave") == 0)   { me_emit_u8(c, 0xC9); return 0; }
    if (strcmp(mnem, "ret") == 0)     { me_emit_u8(c, 0xC3); return 0; }

    int width = 0;
    size_t slen = strlen(mnem);
    if (slen > 2) {
        char last = mnem[slen - 1];
        if (last == 'b') width = 8;
        else if (last == 'w') width = 16;
        else if (last == 'l') width = 32;
        else if (last == 'q') width = 64;
        if (width > 0) {
            char base[32];
            strncpy(base, mnem, slen - 1);
            base[slen - 1] = '\0';
            if (strcmp(base, "mov") == 0 || strcmp(base, "add") == 0 ||
                strcmp(base, "sub") == 0 || strcmp(base, "and") == 0 ||
                strcmp(base, "or") == 0  || strcmp(base, "xor") == 0 ||
                strcmp(base, "cmp") == 0 || strcmp(base, "test") == 0 ||
                strcmp(base, "not") == 0 || strcmp(base, "neg") == 0 ||
                strcmp(base, "inc") == 0 || strcmp(base, "dec") == 0 ||
                strcmp(base, "lea") == 0 || strcmp(base, "shl") == 0 ||
                strcmp(base, "sal") == 0 || strcmp(base, "shr") == 0 ||
                strcmp(base, "sar") == 0 || strcmp(base, "rol") == 0 ||
                strcmp(base, "ror") == 0 || strcmp(base, "imul") == 0 ||
                strcmp(base, "xchg") == 0 || strcmp(base, "cmpxchg") == 0 ||
                strcmp(base, "xadd") == 0) {
                snprintf(mnem, sizeof(mnem), "%s", base);
            } else {
                width = 0;
            }
        }
    }

    AsmOperand op1, op2;
    memset(&op1, 0, sizeof(op1));
    memset(&op2, 0, sizeof(op2));
    stmt = x86_parse_operand(stmt, &op1);
    if (stmt && *stmt == ',') {
        stmt = asm_skip_ws(stmt + 1);
        stmt = x86_parse_operand(stmt, &op2);
    }

    if (width == 0) {
        if (op2.kind == ASM_OP_REG) width = op2.width;
        else if (op1.kind == ASM_OP_REG) width = op1.width;
        else width = 32;
    }

    if (is_lock) me_emit_u8(c, 0xF0);

    /* --- Branch instructions (asm goto) --- */
    if (op1.kind == ASM_OP_LABEL) {
        uint16_t lbl = (uint16_t)op1.imm;
        uint8_t jcc_code = 0;
        bool is_jmp = false;
        if (strcmp(mnem, "jmp") == 0) {
            is_jmp = true;
        } else if (strcmp(mnem, "je") == 0 || strcmp(mnem, "jz") == 0) {
            jcc_code = 0x84;
        } else if (strcmp(mnem, "jne") == 0 || strcmp(mnem, "jnz") == 0) {
            jcc_code = 0x85;
        } else if (strcmp(mnem, "jl") == 0 || strcmp(mnem, "jnge") == 0) {
            jcc_code = 0x8C;
        } else if (strcmp(mnem, "jge") == 0 || strcmp(mnem, "jnl") == 0) {
            jcc_code = 0x8D;
        } else if (strcmp(mnem, "jle") == 0 || strcmp(mnem, "jng") == 0) {
            jcc_code = 0x8E;
        } else if (strcmp(mnem, "jg") == 0 || strcmp(mnem, "jnle") == 0) {
            jcc_code = 0x8F;
        } else if (strcmp(mnem, "jb") == 0 || strcmp(mnem, "jc") == 0 || strcmp(mnem, "jnae") == 0) {
            jcc_code = 0x82;
        } else if (strcmp(mnem, "jae") == 0 || strcmp(mnem, "jnc") == 0 || strcmp(mnem, "jnb") == 0) {
            jcc_code = 0x83;
        } else if (strcmp(mnem, "jbe") == 0 || strcmp(mnem, "jna") == 0) {
            jcc_code = 0x86;
        } else if (strcmp(mnem, "ja") == 0 || strcmp(mnem, "jnbe") == 0) {
            jcc_code = 0x87;
        } else if (strcmp(mnem, "js") == 0) {
            jcc_code = 0x88;
        } else if (strcmp(mnem, "jns") == 0) {
            jcc_code = 0x89;
        } else if (strcmp(mnem, "jo") == 0) {
            jcc_code = 0x80;
        } else if (strcmp(mnem, "jno") == 0) {
            jcc_code = 0x81;
        } else if (strcmp(mnem, "jp") == 0 || strcmp(mnem, "jpe") == 0) {
            jcc_code = 0x8A;
        } else if (strcmp(mnem, "jnp") == 0 || strcmp(mnem, "jpo") == 0) {
            jcc_code = 0x8B;
        } else {
            return -1;
        }
        if (is_jmp) {
            me_emit_u8(c, 0xE9);
        } else {
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, jcc_code);
        }
        size_t patch_pos = me_get_offset(c);
        me_emit_u32(c, 0);
        zc_mc_record_or_patch_label(c, patch_pos, lbl);
        return 0;
    }

    /* --- MOV --- */
    if (strcmp(mnem, "mov") == 0) {
        if (op1.kind == ASM_OP_IMM && op2.kind == ASM_OP_REG) {
            if (width == 64) {
                if (op1.imm < -2147483648LL || op1.imm > 2147483647LL) {
                    me_emit_u8(c, 0x48 | ((op2.reg >> 3) & 1));
                    me_emit_u8(c, 0xB8 + (op2.reg & 7));
                    me_emit_u64(c, (uint64_t)op1.imm);
                } else if (op1.imm >= 0) {
                    if (op2.reg >= 8) me_emit_u8(c, 0x41);
                    me_emit_u8(c, 0xB8 + (op2.reg & 7));
                    me_emit_u32(c, (uint32_t)op1.imm);
                } else {
                    x86_rex(c, 1, 0, 0, (op2.reg >> 3) & 1);
                    me_emit_u8(c, 0xC7);
                    x86_modrm(c, 3, 0, op2.reg);
                    me_emit_u32(c, (uint32_t)(int32_t)op1.imm);
                }
            } else if (width == 32) {
                if (op2.reg >= 8) me_emit_u8(c, 0x41);
                me_emit_u8(c, 0xB8 + (op2.reg & 7));
                me_emit_u32(c, (uint32_t)op1.imm);
            } else if (width == 16) {
                me_emit_u8(c, 0x66);
                if (op2.reg >= 8) me_emit_u8(c, 0x41);
                me_emit_u8(c, 0xB8 + (op2.reg & 7));
                me_emit_u16(c, (uint16_t)op1.imm);
            } else if (width == 8) {
                if (op2.reg >= 8) me_emit_u8(c, 0x41);
                else if (op2.reg >= 4) me_emit_u8(c, 0x40);
                me_emit_u8(c, 0xB0 + (op2.reg & 7));
                me_emit_u8(c, (uint8_t)op1.imm);
            }
            return 0;
        }
        if (op1.kind == ASM_OP_IMM && op2.kind == ASM_OP_MEM) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, 0, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0), (op2.base_reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? 0xC6 : 0xC7);
            x86_emit_mem_modrm(c, 0, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
            if (width == 8) me_emit_u8(c, (uint8_t)op1.imm);
            else if (width == 16) me_emit_u16(c, (uint16_t)op1.imm);
            else me_emit_u32(c, (uint32_t)op1.imm);
            return 0;
        }
        if (op1.kind == ASM_OP_REG && op2.kind == ASM_OP_REG) {
            x86_emit_mov_reg_width(c, op2.reg, op1.reg, width);
            return 0;
        }
        if (op1.kind == ASM_OP_REG && op2.kind == ASM_OP_MEM) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op1.reg >> 3) & 1, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0), (op2.base_reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? 0x88 : 0x89);
            x86_emit_mem_modrm(c, op1.reg, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
            return 0;
        }
        if (op1.kind == ASM_OP_MEM && op2.kind == ASM_OP_REG) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op2.reg >> 3) & 1, (op1.index_reg >= 0 ? (op1.index_reg >> 3) & 1 : 0), (op1.base_reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? 0x8A : 0x8B);
            x86_emit_mem_modrm(c, op2.reg, op1.base_reg, op1.index_reg, op1.scale, op1.disp);
            return 0;
        }
    }

    /* --- ALU Ops (add, or, and, sub, xor, cmp) --- */
    int sub_op = -1, reg_op = -1;
    if (strcmp(mnem, "add") == 0)      { sub_op = 0; reg_op = 0x01; }
    else if (strcmp(mnem, "or") == 0)  { sub_op = 1; reg_op = 0x09; }
    else if (strcmp(mnem, "and") == 0) { sub_op = 4; reg_op = 0x21; }
    else if (strcmp(mnem, "sub") == 0) { sub_op = 5; reg_op = 0x29; }
    else if (strcmp(mnem, "xor") == 0) { sub_op = 6; reg_op = 0x31; }
    else if (strcmp(mnem, "cmp") == 0) { sub_op = 7; reg_op = 0x39; }

    if (sub_op >= 0) {
        if (op1.kind == ASM_OP_IMM && op2.kind == ASM_OP_REG) {
            if (width == 8) {
                if (op2.reg >= 8) me_emit_u8(c, 0x41);
                else if (op2.reg >= 4) me_emit_u8(c, 0x40);
                me_emit_u8(c, 0x80);
                x86_modrm(c, 3, sub_op, op2.reg);
                me_emit_u8(c, (uint8_t)op1.imm);
            } else if (op1.imm >= -128 && op1.imm <= 127) {
                if (width == 16) me_emit_u8(c, 0x66);
                x86_rex(c, width == 64, 0, 0, (op2.reg >> 3) & 1);
                me_emit_u8(c, 0x83);
                x86_modrm(c, 3, sub_op, op2.reg);
                me_emit_u8(c, (uint8_t)(int8_t)op1.imm);
            } else {
                if (width == 16) {
                    me_emit_u8(c, 0x66);
                    x86_rex(c, 0, 0, 0, (op2.reg >> 3) & 1);
                    me_emit_u8(c, 0x81);
                    x86_modrm(c, 3, sub_op, op2.reg);
                    me_emit_u16(c, (uint16_t)op1.imm);
                } else {
                    x86_rex(c, width == 64, 0, 0, (op2.reg >> 3) & 1);
                    me_emit_u8(c, 0x81);
                    x86_modrm(c, 3, sub_op, op2.reg);
                    me_emit_u32(c, (uint32_t)op1.imm);
                }
            }
            return 0;
        }
        if (op1.kind == ASM_OP_IMM && op2.kind == ASM_OP_MEM) {
            if (width == 8) {
                x86_rex(c, 0, 0, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0), (op2.base_reg >> 3) & 1);
                me_emit_u8(c, 0x80);
                x86_emit_mem_modrm(c, sub_op, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
                me_emit_u8(c, (uint8_t)op1.imm);
            } else if (op1.imm >= -128 && op1.imm <= 127) {
                if (width == 16) me_emit_u8(c, 0x66);
                x86_rex(c, width == 64, 0, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0), (op2.base_reg >> 3) & 1);
                me_emit_u8(c, 0x83);
                x86_emit_mem_modrm(c, sub_op, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
                me_emit_u8(c, (uint8_t)(int8_t)op1.imm);
            } else {
                if (width == 16) me_emit_u8(c, 0x66);
                x86_rex(c, width == 64, 0, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0), (op2.base_reg >> 3) & 1);
                me_emit_u8(c, 0x81);
                x86_emit_mem_modrm(c, sub_op, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
                if (width == 16) me_emit_u16(c, (uint16_t)op1.imm);
                else me_emit_u32(c, (uint32_t)op1.imm);
            }
            return 0;
        }
        if (op1.kind == ASM_OP_REG && op2.kind == ASM_OP_REG) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op1.reg >> 3) & 1, 0, (op2.reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? reg_op - 1 : reg_op);
            x86_modrm(c, 3, op1.reg, op2.reg);
            return 0;
        }
        if (op1.kind == ASM_OP_REG && op2.kind == ASM_OP_MEM) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op1.reg >> 3) & 1, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0), (op2.base_reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? reg_op - 1 : reg_op);
            x86_emit_mem_modrm(c, op1.reg, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
            return 0;
        }
        if (op1.kind == ASM_OP_MEM && op2.kind == ASM_OP_REG) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op2.reg >> 3) & 1, (op1.index_reg >= 0 ? (op1.index_reg >> 3) & 1 : 0), (op1.base_reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? reg_op - 1 + 2 : reg_op + 2);
            x86_emit_mem_modrm(c, op2.reg, op1.base_reg, op1.index_reg, op1.scale, op1.disp);
            return 0;
        }
    }

    /* --- Unary (not, neg, inc, dec) --- */
    int u_op = -1, is_inc_dec = 0;
    if (strcmp(mnem, "inc") == 0)      { u_op = 0; is_inc_dec = 1; }
    else if (strcmp(mnem, "dec") == 0) { u_op = 1; is_inc_dec = 1; }
    else if (strcmp(mnem, "not") == 0) { u_op = 2; is_inc_dec = 0; }
    else if (strcmp(mnem, "neg") == 0) { u_op = 3; is_inc_dec = 0; }

    if (u_op >= 0) {
        if (op1.kind == ASM_OP_REG) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, 0, 0, (op1.reg >> 3) & 1);
            if (width == 8 && op1.reg >= 4 && op1.reg < 8) me_emit_u8(c, 0x40);
            me_emit_u8(c, (width == 8) ? (is_inc_dec ? 0xFE : 0xF6) : (is_inc_dec ? 0xFF : 0xF7));
            x86_modrm(c, 3, u_op, op1.reg);
            return 0;
        }
        if (op1.kind == ASM_OP_MEM) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, 0, (op1.index_reg >= 0 ? (op1.index_reg >> 3) & 1 : 0), (op1.base_reg >> 3) & 1);
            me_emit_u8(c, (width == 8) ? (is_inc_dec ? 0xFE : 0xF6) : (is_inc_dec ? 0xFF : 0xF7));
            x86_emit_mem_modrm(c, u_op, op1.base_reg, op1.index_reg, op1.scale, op1.disp);
            return 0;
        }
    }

    /* --- LEA --- */
    if (strcmp(mnem, "lea") == 0 && op1.kind == ASM_OP_MEM && op2.kind == ASM_OP_REG) {
        x86_rex(c, width == 64, (op2.reg >> 3) & 1, (op1.index_reg >= 0 ? (op1.index_reg >> 3) & 1 : 0), (op1.base_reg >> 3) & 1);
        me_emit_u8(c, 0x8D);
        x86_emit_mem_modrm(c, op2.reg, op1.base_reg, op1.index_reg, op1.scale, op1.disp);
        return 0;
    }

    /* --- TEST --- */
    if (strcmp(mnem, "test") == 0) {
        if (op1.kind == ASM_OP_REG && op2.kind == ASM_OP_REG) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op1.reg >> 3) & 1, 0, (op2.reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? 0x84 : 0x85);
            x86_modrm(c, 3, op1.reg, op2.reg);
            return 0;
        }
        if (op1.kind == ASM_OP_IMM && op2.kind == ASM_OP_REG) {
            if (width == 8) {
                if (op2.reg >= 8) me_emit_u8(c, 0x41);
                else if (op2.reg >= 4) me_emit_u8(c, 0x40);
                me_emit_u8(c, 0xF6);
                x86_modrm(c, 3, 0, op2.reg);
                me_emit_u8(c, (uint8_t)op1.imm);
            } else if (width == 16) {
                me_emit_u8(c, 0x66);
                x86_rex(c, 0, 0, 0, (op2.reg >> 3) & 1);
                me_emit_u8(c, 0xF7);
                x86_modrm(c, 3, 0, op2.reg);
                me_emit_u16(c, (uint16_t)op1.imm);
            } else {
                x86_rex(c, width == 64, 0, 0, (op2.reg >> 3) & 1);
                me_emit_u8(c, 0xF7);
                x86_modrm(c, 3, 0, op2.reg);
                me_emit_u32(c, (uint32_t)op1.imm);
            }
            return 0;
        }
    }

    /* --- PUSH / POP --- */
    if (strcmp(mnem, "push") == 0 && op1.kind == ASM_OP_REG) {
        if (op1.reg >= 8) me_emit_u8(c, 0x41);
        me_emit_u8(c, 0x50 + (op1.reg & 7));
        return 0;
    }
    if (strcmp(mnem, "pop") == 0 && op1.kind == ASM_OP_REG) {
        if (op1.reg >= 8) me_emit_u8(c, 0x41);
        me_emit_u8(c, 0x58 + (op1.reg & 7));
        return 0;
    }

    /* --- XCHG --- */
    if (strcmp(mnem, "xchg") == 0) {
        if (op1.kind == ASM_OP_REG && op2.kind == ASM_OP_REG) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, (op1.reg >> 3) & 1, 0, (op2.reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? 0x86 : 0x87);
            x86_modrm(c, 3, op1.reg, op2.reg);
            return 0;
        }
    }

    /* --- CMPXCHG & XADD --- */
    if (strcmp(mnem, "cmpxchg") == 0) {
        if (width == 16) me_emit_u8(c, 0x66);
        x86_rex(c, width == 64, (op1.reg >> 3) & 1, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0),
                (op2.kind == ASM_OP_REG ? (op2.reg >> 3) & 1 : (op2.base_reg >> 3) & 1));
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, width == 8 ? 0xB0 : 0xB1);
        if (op2.kind == ASM_OP_REG) {
            x86_modrm(c, 3, op1.reg, op2.reg);
        } else {
            x86_emit_mem_modrm(c, op1.reg, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
        }
        return 0;
    }
    if (strcmp(mnem, "xadd") == 0) {
        if (width == 16) me_emit_u8(c, 0x66);
        x86_rex(c, width == 64, (op1.reg >> 3) & 1, (op2.index_reg >= 0 ? (op2.index_reg >> 3) & 1 : 0),
                (op2.kind == ASM_OP_REG ? (op2.reg >> 3) & 1 : (op2.base_reg >> 3) & 1));
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, width == 8 ? 0xC0 : 0xC1);
        if (op2.kind == ASM_OP_REG) {
            x86_modrm(c, 3, op1.reg, op2.reg);
        } else {
            x86_emit_mem_modrm(c, op1.reg, op2.base_reg, op2.index_reg, op2.scale, op2.disp);
        }
        return 0;
    }

    /* --- SHL, SHR, SAR, ROL, ROR --- */
    int sh_op = -1;
    if (strcmp(mnem, "rol") == 0)      sh_op = 0;
    else if (strcmp(mnem, "ror") == 0) sh_op = 1;
    else if (strcmp(mnem, "shl") == 0 || strcmp(mnem, "sal") == 0) sh_op = 4;
    else if (strcmp(mnem, "shr") == 0) sh_op = 5;
    else if (strcmp(mnem, "sar") == 0) sh_op = 7;

    if (sh_op >= 0 && op2.kind == ASM_OP_REG) {
        if (op1.kind == ASM_OP_IMM) {
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, 0, 0, (op2.reg >> 3) & 1);
            if (op1.imm == 1) {
                me_emit_u8(c, width == 8 ? 0xD0 : 0xD1);
                x86_modrm(c, 3, sh_op, op2.reg);
            } else {
                me_emit_u8(c, width == 8 ? 0xC0 : 0xC1);
                x86_modrm(c, 3, sh_op, op2.reg);
                me_emit_u8(c, (uint8_t)op1.imm);
            }
            return 0;
        }
        if (op1.kind == ASM_OP_REG && op1.reg == 1) { /* %cl */
            if (width == 16) me_emit_u8(c, 0x66);
            x86_rex(c, width == 64, 0, 0, (op2.reg >> 3) & 1);
            me_emit_u8(c, width == 8 ? 0xD2 : 0xD3);
            x86_modrm(c, 3, sh_op, op2.reg);
            return 0;
        }
    }

    /* Unsupported instruction */
    return -1;
}

/* Main entry point for machine code emission of OP_ASM (Sprint 136, Phase 2) */
static void x86_emit_inline_asm_mc(ZeroCCCompiler *c, size_t idx) {
    const Instr *instr = ir_get(c, idx);
    if (!instr) return;
    const IRAsmArgs *aa = ir_asm_args_find(c, idx);
    if (!aa || !aa->template_str) return;

    /* 1. Materialize digit-bound inputs into their output's register */
    int first_out = -1;
    for (int o = 0; o < aa->op_count; o++) {
        if (aa->ops[o].kind == ZC_ASM_OP_OUT_R || aa->ops[o].kind == ZC_ASM_OP_INOUT_R ||
            aa->ops[o].kind == ZC_ASM_OP_OUT_M) {
            first_out = o;
            break;
        }
    }
    for (int o = 0; o < aa->op_count; o++) {
        const IRAsmOperand *op = &aa->ops[o];
        if (op->digit < 0 || first_out < 0) continue;
        int out_pos = first_out + op->digit;
        if (out_pos < 0 || out_pos >= aa->op_count) continue;
        const IRAsmOperand *oop = &aa->ops[out_pos];
        int width = oop->width ? oop->width : 64;
        int out_phys = regalloc_get_reg_for_temp(&c->regalloc, oop->temp);
        int in_phys = regalloc_get_reg_for_temp(&c->regalloc, op->temp);
        if (in_phys != out_phys) {
            x86_emit_mov_reg_width(c, out_phys, in_phys, width);
        }
    }

    /* 2. Interpolate template into expanded buffer */
    char expanded[4096];
    if (x86_interpolate_template(c, idx, expanded, sizeof(expanded)) != 0) {
        diag_report_error_at_token(c, "inline asm template too long or malformed");
        c->had_error = 1;
        return;
    }

    /* 3. Split by ';' or '\n' and assemble each statement */
    char *cur = expanded;
    while (*cur) {
        char *end = cur;
        while (*end && *end != ';' && *end != '\n') end++;
        char saved = *end;
        *end = '\0';

        char *line = (char *)asm_skip_ws(cur);
        if (*line) {
            if (x86_assemble_single_stmt(c, line) != 0) {
                diag_report_error_at_token(c, "integrated inline assembler: unsupported instruction '%s'", line);
                c->had_error = 1;
                return;
            }
        }

        if (!saved) break;
        cur = end + 1;
    }
}

#endif /* ZERO_CC_ISA_X86_64_INLINE_ASM_C */
