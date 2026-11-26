#include "zero_cc/opt/opt_peephole.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/types/symbols.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/vregs.h"

#define PEEP_TRACE_MAX 8

/* Check if a temporary is a scalar general-purpose integer/pointer register
 * (excluding vector, complex, floating-point, and 128-bit values). */
static bool is_scalar_gpr(const ZeroCCCompiler *c, uint32_t t) {
    if (t == TEMP_NONE || t == 0) return true;
    uint8_t tag = zc_temp_tag(c, t);
    uint8_t cls = zc_tag_class(tag);
    if (cls == ZC_TAGCLS_VECTOR || cls == ZC_TAGCLS_COMPLEX ||
        cls == ZC_TAGCLS_FLOAT || cls == ZC_TAGCLS_INT128 ||
        cls == ZC_TAGCLS_UINT128) {
        return false;
    }
    return true;
}

/* Resolve temp to a 64-bit compile-time constant strictly within the current basic block */
static bool peep_resolve_const_local(ZeroCCCompiler *c, int pos, uint32_t t, int64_t *val_out) {
    if (t == TEMP_NONE || t == 0) return false;
    if (!is_scalar_gpr(c, t)) return false;

    int depth = 0;
    while (pos > 0 && depth <= PEEP_TRACE_MAX) {
        int k = -1;
        for (int j = pos - 1; j >= 0; j--) {
            Instr *d = ir_get(c, j);
            if (!d) continue;
            /* Do not cross block boundaries or control flow */
            if (d->op == OP_LABEL || d->op == OP_JMP || d->op == OP_JE || d->op == OP_JNE ||
                d->op == OP_JL || d->op == OP_JLE || d->op == OP_JG || d->op == OP_JCOND ||
                d->op == OP_CALL || d->op == OP_CALL_INDIRECT || d->op == OP_SYSCALL) {
                return false;
            }
            if (d->temp_dest == t) {
                k = j;
                break;
            }
        }
        if (k < 0) return false;

        Instr *d = ir_get(c, k);
        switch (d->op) {
            case OP_MOV_IMM:
                *val_out = d->imm64;
                return true;
            case OP_MOV_REG:
                pos = k;
                t = d->temp_src1;
                depth++;
                break;
            default:
                return false;
        }
    }
    return false;
}

/* Find last definition of temp before pos strictly within the same basic block */
static Instr *peep_find_def_local(ZeroCCCompiler *c, int pos, uint32_t t) {
    if (t == TEMP_NONE || t == 0) return NULL;
    for (int j = pos - 1; j >= 0; j--) {
        Instr *d = ir_get(c, j);
        if (!d) continue;
        if (d->op == OP_LABEL || d->op == OP_JMP || d->op == OP_JE || d->op == OP_JNE ||
            d->op == OP_JL || d->op == OP_JLE || d->op == OP_JG || d->op == OP_JCOND ||
            d->op == OP_CALL || d->op == OP_CALL_INDIRECT) {
            break;
        }
        if (d->temp_dest == t) return d;
    }
    return NULL;
}

static void peep_make_mov_imm(Instr *ins, int64_t val) {
    ins->op = OP_MOV_IMM;
    ins->temp_src1 = TEMP_NONE;
    ins->temp_src2 = TEMP_NONE;
    ins->imm = (int16_t)val;
    ins->imm64 = val;
    ins->flags_out = 0xFF;
    ins->flags_in = 0xFF;
}

static void peep_make_mov_reg(Instr *ins, uint32_t src) {
    if (ins->temp_dest == src) {
        ins->op = OP_NOP;
        ins->temp_dest = TEMP_NONE;
        ins->temp_src1 = TEMP_NONE;
        ins->temp_src2 = TEMP_NONE;
        ins->imm = 0;
        ins->imm64 = 0;
    } else {
        ins->op = OP_MOV_REG;
        ins->temp_src1 = src;
        ins->temp_src2 = TEMP_NONE;
        ins->imm = 0;
        ins->imm64 = 0;
        ins->flags_out = 0xFF;
        ins->flags_in = 0xFF;
    }
}

static void peep_make_neg(Instr *ins, uint32_t src) {
    ins->op = OP_NEG;
    ins->temp_src1 = src;
    ins->temp_src2 = TEMP_NONE;
    ins->imm = 0;
    ins->imm64 = 0;
    ins->flags_out = 0xFF;
    ins->flags_in = 0xFF;
}

bool opt_peephole_simplify(ZeroCCCompiler *c) {
    if (!c) return false;

    size_t count = ir_get_count(c);
    if (count == 0) return false;

    bool changed = false;

    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;

        uint32_t d = ins->temp_dest;
        uint32_t s1 = ins->temp_src1;
        uint32_t s2 = ins->temp_src2;

        /* Only optimize scalar integer / pointer temporaries */
        if (!is_scalar_gpr(c, d) || !is_scalar_gpr(c, s1) || !is_scalar_gpr(c, s2)) {
            continue;
        }

        /* If flags produced by this op are consumed by the immediate next instruction,
         * skip algebraic replacement that doesn't produce flags. */
        if (ins->flags_out != 0xFF && i + 1 < count) {
            Instr *next = ir_get(c, i + 1);
            if (next && next->flags_in != 0xFF) continue;
        }

        /* 1. Self moves: MOV_REG x, x -> NOP */
        if (ins->op == OP_MOV_REG && d == s1 && d != TEMP_NONE) {
            ins->op = OP_NOP;
            ins->temp_dest = TEMP_NONE;
            ins->temp_src1 = TEMP_NONE;
            changed = true;
            continue;
        }

        /* 2. Self identities: s1 == s2 */
        if (s1 != TEMP_NONE && s1 != 0 && s1 == s2) {
            if (ins->op == OP_SUB || ins->op == OP_XOR) {
                /* x - x -> 0, x ^ x -> 0 */
                peep_make_mov_imm(ins, 0);
                changed = true;
                continue;
            }
            if (ins->op == OP_AND || ins->op == OP_OR) {
                /* x & x -> x, x | x -> x */
                peep_make_mov_reg(ins, s1);
                changed = true;
                continue;
            }
        }

        /* 3. Constant resolution strictly within the local basic block */
        int64_t c1 = 0, c2 = 0;
        bool has_c1 = peep_resolve_const_local(c, (int)i, s1, &c1);
        bool has_c2 = peep_resolve_const_local(c, (int)i, s2, &c2);

        /* 4. Binary operations with neutral elements or annihilators */
        switch (ins->op) {
            case OP_ADD:
                if (has_c2 && c2 == 0) {
                    /* x + 0 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                if (has_c1 && c1 == 0) {
                    /* 0 + x -> x */
                    peep_make_mov_reg(ins, s2);
                    changed = true;
                    continue;
                }
                break;

            case OP_SUB:
                if (has_c2 && c2 == 0) {
                    /* x - 0 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                if (has_c1 && c1 == 0) {
                    /* 0 - x -> -x */
                    peep_make_neg(ins, s2);
                    changed = true;
                    continue;
                }
                break;

            case OP_MUL:
                if ((has_c2 && c2 == 0) || (has_c1 && c1 == 0)) {
                    /* x * 0 -> 0 */
                    peep_make_mov_imm(ins, 0);
                    changed = true;
                    continue;
                }
                if (has_c2 && c2 == 1) {
                    /* x * 1 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                if (has_c1 && c1 == 1) {
                    /* 1 * x -> x */
                    peep_make_mov_reg(ins, s2);
                    changed = true;
                    continue;
                }
                break;

            case OP_DIV: case OP_DIV64: case OP_UDIV64:
                if (has_c2 && c2 == 1) {
                    /* x / 1 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                break;

            case OP_AND:
                if ((has_c2 && c2 == 0) || (has_c1 && c1 == 0)) {
                    /* x & 0 -> 0 */
                    peep_make_mov_imm(ins, 0);
                    changed = true;
                    continue;
                }
                if (has_c2 && c2 == -1) {
                    /* x & -1 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                if (has_c1 && c1 == -1) {
                    /* -1 & x -> x */
                    peep_make_mov_reg(ins, s2);
                    changed = true;
                    continue;
                }
                break;

            case OP_OR:
                if (has_c2 && c2 == 0) {
                    /* x | 0 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                if (has_c1 && c1 == 0) {
                    /* 0 | x -> x */
                    peep_make_mov_reg(ins, s2);
                    changed = true;
                    continue;
                }
                break;

            case OP_XOR:
                if (has_c2 && c2 == 0) {
                    /* x ^ 0 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                if (has_c1 && c1 == 0) {
                    /* 0 ^ x -> x */
                    peep_make_mov_reg(ins, s2);
                    changed = true;
                    continue;
                }
                break;

            case OP_SHL: case OP_SHR: case OP_SAR:
                if (has_c2 && c2 == 0) {
                    /* x << 0 -> x, x >> 0 -> x */
                    peep_make_mov_reg(ins, s1);
                    changed = true;
                    continue;
                }
                break;

            case OP_NEG: {
                /* -(-x) -> x */
                Instr *def = peep_find_def_local(c, (int)i, s1);
                if (def && def->op == OP_NEG && def->temp_src1 != TEMP_NONE) {
                    peep_make_mov_reg(ins, def->temp_src1);
                    changed = true;
                    continue;
                }
                break;
            }

            case OP_NOT: {
                /* ~(~x) -> x */
                Instr *def = peep_find_def_local(c, (int)i, s1);
                if (def && def->op == OP_NOT && def->temp_src1 != TEMP_NONE) {
                    peep_make_mov_reg(ins, def->temp_src1);
                    changed = true;
                    continue;
                }
                break;
            }

            default:
                break;
        }
    }

    if (changed) {
        c->cfg_version++;
    }

    return changed;
}
