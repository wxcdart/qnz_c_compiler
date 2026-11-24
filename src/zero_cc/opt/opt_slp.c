#include "zero_cc/opt/opt_slp.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/opt/opt_profile.h"

typedef enum {
    ADDR_BASE_NONE = 0,
    ADDR_BASE_TEMP,
    ADDR_BASE_SLOT,
    ADDR_BASE_GLOBAL
} AddrBaseKind;

typedef struct {
    AddrBaseKind kind;
    int32_t id;      /* temp id, stack slot offset, or global symbol */
    int64_t offset;  /* byte offset relative to base */
} PtrAddr;

typedef struct {
    int instr_idx;
    uint32_t addr_temp;
    uint32_t val_temp;
    int elem_size;       /* 4 or 8 */
    bool is_float;
    PtrAddr addr;
    bool vectorized;
} SlpStore;

typedef struct {
    int instr_idx;
    int op;
    uint32_t temp_dest;
    uint32_t temp_src1;
    uint32_t temp_src2;
    int elem_size;
    bool is_float;
    bool vectorized;
} SlpArith;

/* Check if instruction defines temp */
static bool instr_writes_temp(const Instr *ins, uint32_t t) {
    if (!ins || t == TEMP_NONE) return false;
    if (ins->temp_dest == t) return true;
    if (ins->op == OP_SEXT32 || ins->op == OP_ZEXT32) {
        return ins->temp_src1 == t || ins->temp_dest == t;
    }
    return false;
}

/* Find the latest instruction before pos that defines temp */
static int find_last_def_before(ZeroCCCompiler *c, int pos, uint32_t temp) {
    if (!c || temp == TEMP_NONE || pos <= 0) return -1;
    for (int k = pos - 1; k >= 0; k--) {
        const Instr *ins = ir_get(c, k);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        if (instr_writes_temp(ins, temp)) return k;
        if (ins->op == OP_LABEL || ins->op == OP_RET) break;
    }
    return -1;
}

/* Resolve a constant immediate held in temp */
static bool resolve_const(ZeroCCCompiler *c, int cur_idx, uint32_t temp, int64_t *out) {
    if (!c || temp == TEMP_NONE || !zc_temp_is_user(temp) || !out) return false;
    for (int k = cur_idx - 1; k >= 0; k--) {
        const Instr *ins = ir_get(c, k);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        if (ins->temp_dest == temp) {
            if (ins->op == OP_MOV_IMM) {
                *out = ins->imm64 ? ins->imm64 : ins->imm;
                return true;
            }
            if (ins->op == OP_MOV_REG && ins->temp_src1 != TEMP_NONE) {
                return resolve_const(c, k, ins->temp_src1, out);
            }
            return false;
        }
        if (ins->op == OP_LABEL) break;
    }
    return false;
}

/* Recursively resolve the base address and offset of a pointer temporary */
static bool resolve_ptr_addr_rec(ZeroCCCompiler *c, int cur_idx, uint32_t t, PtrAddr *out, int depth) {
    if (!c || !out || t == TEMP_NONE || depth > 8) return false;

    int def_idx = find_last_def_before(c, cur_idx, t);
    if (def_idx < 0) {
        out->kind = ADDR_BASE_TEMP;
        out->id = (int32_t)t;
        out->offset = 0;
        return true;
    }

    const Instr *def = ir_get(c, def_idx);
    if (!def) return false;

    if (def->op == OP_MOV && def->temp_src1 == TEMP_NONE) {
        out->kind = ADDR_BASE_SLOT;
        out->id = (int32_t)def->imm64;
        out->offset = 0;
        return true;
    }

    if (def->op == OP_LEA) {
        out->kind = ADDR_BASE_SLOT;
        out->id = (int32_t)def->imm64;
        out->offset = 0;
        return true;
    }

    if (def->op == OP_MOV_REG && def->temp_src1 != TEMP_NONE) {
        return resolve_ptr_addr_rec(c, def_idx, def->temp_src1, out, depth + 1);
    }

    if (def->op == OP_ADD_IMM) {
        if (resolve_ptr_addr_rec(c, def_idx, def->temp_src1, out, depth + 1)) {
            out->offset += def->imm;
            return true;
        }
    }

    if (def->op == OP_ADD) {
        int64_t cval = 0;
        if (resolve_const(c, def_idx, def->temp_src2, &cval)) {
            if (resolve_ptr_addr_rec(c, def_idx, def->temp_src1, out, depth + 1)) {
                out->offset += cval;
                return true;
            }
        } else if (resolve_const(c, def_idx, def->temp_src1, &cval)) {
            if (resolve_ptr_addr_rec(c, def_idx, def->temp_src2, out, depth + 1)) {
                out->offset += cval;
                return true;
            }
        }
    }

    out->kind = ADDR_BASE_TEMP;
    out->id = (int32_t)t;
    out->offset = 0;
    return true;
}

static bool resolve_ptr_addr(ZeroCCCompiler *c, int cur_idx, uint32_t t, PtrAddr *out) {
    if (!out) return false;
    out->kind = ADDR_BASE_NONE;
    out->id = -1;
    out->offset = 0;
    return resolve_ptr_addr_rec(c, cur_idx, t, out, 0);
}

/* Check if instruction is a scalar store */
static bool is_slp_store(const Instr *ins, uint32_t *addr_temp, uint32_t *val_temp, int *elem_size, bool *is_float) {
    if (!ins) return false;
    if (ins->op == OP_STORE_32) {
        *addr_temp = ins->temp_src2;
        *val_temp = ins->temp_src1;
        *elem_size = 4;
        *is_float = false;
        return true;
    }
    if (ins->op == OP_VSTORESS || ins->op == OP_VSTORESS_RAW) {
        *addr_temp = ins->temp_src1;
        *val_temp = ins->temp_src2;
        *elem_size = 4;
        *is_float = true;
        return true;
    }
    if (ins->op == OP_STORE_64) {
        *addr_temp = ins->temp_src2;
        *val_temp = ins->temp_src1;
        *elem_size = 8;
        *is_float = false;
        return true;
    }
    if (ins->op == OP_VSTORESD) {
        *addr_temp = ins->temp_src1;
        *val_temp = ins->temp_src2;
        *elem_size = 8;
        *is_float = true;
        return true;
    }
    return false;
}

/* Check if instruction is a scalar load */
static bool is_slp_load(const Instr *ins, uint32_t *addr_temp, uint32_t *dest_temp, int *elem_size, bool *is_float) {
    if (!ins) return false;
    if (ins->op == OP_DEREF || ins->op == OP_LOAD_32) {
        *addr_temp = ins->temp_src1;
        *dest_temp = ins->temp_dest;
        *elem_size = 4;
        *is_float = false;
        return true;
    }
    if (ins->op == OP_VLOADSS || ins->op == OP_VLOADSS_RAW) {
        *addr_temp = ins->temp_src1;
        *dest_temp = ins->temp_dest;
        *elem_size = 4;
        *is_float = true;
        return true;
    }
    if (ins->op == OP_LOAD64) {
        *addr_temp = ins->temp_src1;
        *dest_temp = ins->temp_dest;
        *elem_size = 8;
        *is_float = false;
        return true;
    }
    if (ins->op == OP_VLOADSD) {
        *addr_temp = ins->temp_src1;
        *dest_temp = ins->temp_dest;
        *elem_size = 8;
        *is_float = true;
        return true;
    }
    return false;
}

/* Find the underlying load instruction, skipping sign-extensions or register moves */
static int find_underlying_load(ZeroCCCompiler *c, int cur_idx, uint32_t temp, int *intermediate_idx) {
    if (intermediate_idx) *intermediate_idx = -1;
    int def_idx = find_last_def_before(c, cur_idx, temp);
    if (def_idx < 0) return -1;
    const Instr *ins = ir_get(c, def_idx);
    if (!ins) return -1;
    if (ins->op == OP_SEXT32 || ins->op == OP_ZEXT32 || ins->op == OP_MOV_REG) {
        if (intermediate_idx) *intermediate_idx = def_idx;
        uint32_t src = (ins->op == OP_MOV_REG) ? ins->temp_src1 : ins->temp_dest;
        int prev = find_last_def_before(c, def_idx, src);
        if (prev >= 0) {
            uint32_t a = 0, d = 0; int esz = 0; bool flt = false;
            if (is_slp_load(ir_get(c, prev), &a, &d, &esz, &flt)) {
                return prev;
            }
        }
        return def_idx;
    }
    return def_idx;
}

/* Map scalar arithmetic opcode to packed vector opcode */
static int get_vector_arith_op(int scalar_op, int elem_size, bool is_float) {
    if (elem_size == 4 && !is_float) {
        switch (scalar_op) {
            case OP_ADD: return OP_VPADDD;
            case OP_SUB: return OP_VPSUBD;
            case OP_MUL: return OP_VPMULLD;
            case OP_AND: return OP_VANDPS;
            case OP_OR:  return OP_VORPS;
            case OP_XOR: return OP_VXORPS;
            default: return OP_NOP;
        }
    } else if (elem_size == 4 && is_float) {
        switch (scalar_op) {
            case OP_FADD:
            case OP_VADDSS: return OP_VADDPS;
            case OP_FSUB:
            case OP_VSUBSS: return OP_VSUBPS;
            case OP_FMUL:
            case OP_VMULSS: return OP_VMULPS;
            case OP_FDIV:
            case OP_VDIVSS: return OP_VDIVPS;
            default: return OP_NOP;
        }
    } else if (elem_size == 8 && is_float) {
        switch (scalar_op) {
            case OP_FADD:
            case OP_VADDSD: return OP_VADDPD;
            case OP_FSUB:
            case OP_VSUBSD: return OP_VSUBPD;
            case OP_FMUL:
            case OP_VMULSD: return OP_VMULPD;
            case OP_FDIV:
            case OP_VDIVSD: return OP_VDIVPD;
            default: return OP_NOP;
        }
    }
    return OP_NOP;
}

/* Clean up sNaN fixup diamonds on scalar floats so basic blocks contain straight-line code */
static void mark_and_nop_snan_diamonds(ZeroCCCompiler *c, int start, int end) {
    int span = end - start + 1;
    if (!c || span <= 0) return;
    for (int idx = start; idx <= end; idx++) {
        const Instr *ins = ir_get(c, idx);
        if (!ins) continue;
        if (ins->op == OP_VLOADSS) {
            if (idx >= start + 20 && idx + 8 <= end) {
                const Instr *ld = ir_get(c, idx - 20);
                const Instr *je = ir_get(c, idx + 1);
                const Instr *fs = ir_get(c, idx + 2);
                const Instr *fl = ir_get(c, idx + 7);
                const Instr *lb = ir_get(c, idx + 8);
                if (ld && je && fs && fl && lb &&
                    ld->op == OP_LOAD_32 && ld->temp_src1 == ins->temp_src1 &&
                    je->op == OP_JE &&
                    fs->op == OP_FSTORE && fs->temp_src1 == ins->temp_dest &&
                    fl->op == OP_FLOAD && fl->temp_dest == ins->temp_dest &&
                    lb->op == OP_LABEL && lb->label_idx == je->label_idx) {
                    for (int k = idx - 20; k < idx; k++) {
                        Instr *sk = ir_get(c, k);
                        if (sk) sk->op = OP_NOP;
                    }
                    for (int k = idx + 1; k <= idx + 8; k++) {
                        Instr *sk = ir_get(c, k);
                        if (sk) sk->op = OP_NOP;
                    }
                }
            }
        } else if (ins->op == OP_VSTORESS) {
            if (idx + 31 <= end) {
                const Instr *fs = ir_get(c, idx + 1);
                const Instr *ld = ir_get(c, idx + 2);
                const Instr *je = ir_get(c, idx + 25);
                const Instr *st = ir_get(c, idx + 30);
                const Instr *lb = ir_get(c, idx + 31);
                if (fs && ld && je && st && lb &&
                    fs->op == OP_FSTORE && fs->temp_src1 == ins->temp_src2 &&
                    ld->op == OP_LOAD_32 && ld->temp_src1 == ins->temp_src1 &&
                    je->op == OP_JE &&
                    st->op == OP_STORE_32 && st->temp_src2 == ins->temp_src1 &&
                    lb->op == OP_LABEL && lb->label_idx == je->label_idx) {
                    for (int k = idx + 1; k <= idx + 31; k++) {
                        Instr *sk = ir_get(c, k);
                        if (sk) sk->op = OP_NOP;
                    }
                }
            }
        }
    }
}

/* Check if instruction is a basic block boundary */
static bool is_bb_boundary(const Instr *ins) {
    if (!ins) return false;
    switch (ins->op) {
        case OP_LABEL:
        case OP_JMP:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
        case OP_RET:
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_ASM:
        case OP_SETJMP:
        case OP_LONGJMP:
        case OP_RVV_VSE:
        case OP_RVV_VSSE:
        case OP_SVE_ST1:
        case OP_V512_STORE:
        case OP_V512_MASK_STORE:
        case OP_V256_STORE:
            return true;
        default:
            return false;
    }
}

/* Process a single basic block for SLP vectorization */
static bool slp_vectorize_block(ZeroCCCompiler *c, int start, int end) {
    if (!c || start >= end) return false;

    mark_and_nop_snan_diamonds(c, start, end);

    bool changed = false;

    /* 1. Collect all stores in this basic block */
    #define MAX_SLP_STORES 128
    SlpStore stores[MAX_SLP_STORES];
    int store_count = 0;

    for (int i = start; i <= end && store_count < MAX_SLP_STORES; i++) {
        const Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        uint32_t addr_temp = 0, val_temp = 0;
        int elem_size = 0;
        bool is_float = false;
        if (is_slp_store(ins, &addr_temp, &val_temp, &elem_size, &is_float)) {
            stores[store_count].instr_idx = i;
            stores[store_count].addr_temp = addr_temp;
            stores[store_count].val_temp = val_temp;
            stores[store_count].elem_size = elem_size;
            stores[store_count].is_float = is_float;
            stores[store_count].vectorized = false;
            resolve_ptr_addr(c, i, addr_temp, &stores[store_count].addr);
            store_count++;
        }
    }

    if (store_count < 2) return false;

    /* 2. Search for groups of isomorphic stores */
    for (int i = 0; i < store_count; i++) {
        if (stores[i].vectorized) continue;
        int esz = stores[i].elem_size;
        int width = (esz == 8) ? 2 : 4;
        if (i + width > store_count) break;

        /* Check if next width-1 stores match contiguous offsets */
        bool contiguous = true;
        for (int k = 0; k < width; k++) {
            if (stores[i + k].vectorized ||
                stores[i + k].elem_size != esz ||
                stores[i + k].is_float != stores[i].is_float ||
                stores[i + k].addr.kind != stores[i].addr.kind ||
                stores[i + k].addr.id != stores[i].addr.id ||
                stores[i + k].addr.offset != stores[i].addr.offset + k * esz) {
                contiguous = false;
                break;
            }
        }
        if (!contiguous) continue;

        /* Found a contiguous store group! Check if stored values are isomorphic */
        int store_idx0 = stores[i].instr_idx;
        uint32_t store_addr0 = stores[i].addr_temp;

        /* Inspect definitions of stored values */
        int def_idx[4] = {-1, -1, -1, -1};
        int inter_idx[4] = {-1, -1, -1, -1};
        for (int k = 0; k < width; k++) {
            def_idx[k] = find_underlying_load(c, stores[i + k].instr_idx, stores[i + k].val_temp, &inter_idx[k]);
        }

        /* Check if all defs are valid */
        bool all_defs_ok = true;
        for (int k = 0; k < width; k++) {
            if (def_idx[k] < 0) { all_defs_ok = false; break; }
        }
        if (!all_defs_ok) continue;

        /* Pattern A: All defs are contiguous scalar LOADS (Vector Copy / Struct Assign) */
        bool is_pure_copy = true;
        PtrAddr src_addrs[4];
        for (int k = 0; k < width; k++) {
            const Instr *d_ins = ir_get(c, def_idx[k]);
            uint32_t la = 0, ld = 0; int le = 0; bool lf = false;
            if (!is_slp_load(d_ins, &la, &ld, &le, &lf) || le != esz) {
                is_pure_copy = false;
                break;
            }
            resolve_ptr_addr(c, def_idx[k], la, &src_addrs[k]);
            if (k > 0 && (src_addrs[k].kind != src_addrs[0].kind ||
                          src_addrs[k].id != src_addrs[0].id ||
                          src_addrs[k].offset != src_addrs[0].offset + k * esz)) {
                is_pure_copy = false;
                break;
            }
        }

        if (is_pure_copy) {
            /* Vectorize memory copy! */
            int v_tmp = zc_alloc_temp(c);
            Instr *load0 = ir_get(c, def_idx[0]);
            Instr *store0 = ir_get(c, store_idx0);

            load0->op = OP_VLOAD;
            load0->temp_dest = (uint32_t)v_tmp;
            load0->temp_src2 = TEMP_NONE;
            load0->width = 0;
            load0->cond = 0;
            load0->flags_out = 0xFF;
            load0->flags_in = 0xFF;
            load0->imm = 0;
            load0->imm64 = 0;

            store0->op = OP_VSTORE;
            store0->temp_dest = TEMP_NONE;
            store0->temp_src1 = store_addr0;
            store0->temp_src2 = (uint32_t)v_tmp;
            store0->width = 0;
            store0->cond = 0;
            store0->flags_out = 0xFF;
            store0->flags_in = 0xFF;
            store0->imm = 0;
            store0->imm64 = 0;

            /* NOP out other scalar loads and stores */
            for (int k = 1; k < width; k++) {
                Instr *sk = ir_get(c, def_idx[k]);
                if (sk) sk->op = OP_NOP;
                if (inter_idx[k] >= 0) {
                    Instr *ik = ir_get(c, inter_idx[k]);
                    if (ik) ik->op = OP_NOP;
                }
                Instr *stk = ir_get(c, stores[i + k].instr_idx);
                if (stk) stk->op = OP_NOP;
                stores[i + k].vectorized = true;
            }
            if (inter_idx[0] >= 0) {
                Instr *ik = ir_get(c, inter_idx[0]);
                if (ik) ik->op = OP_NOP;
            }
            stores[i].vectorized = true;
            changed = true;
            continue;
        }

        /* Pattern B: Stored values are produced by isomorphic ARITHMETIC instructions */
        int arith_op = ir_get(c, def_idx[0])->op;
        int vec_op = get_vector_arith_op(arith_op, esz, stores[i].is_float);
        if (vec_op == OP_NOP) continue;

        bool same_arith = true;
        for (int k = 1; k < width; k++) {
            if (ir_get(c, def_idx[k])->op != arith_op) {
                same_arith = false;
                break;
            }
        }
        if (!same_arith) continue;

        /* Inspect operand 1 and operand 2 of each arithmetic instruction */
        uint32_t src1_temps[4], src2_temps[4];
        for (int k = 0; k < width; k++) {
            const Instr *ai = ir_get(c, def_idx[k]);
            src1_temps[k] = ai->temp_src1;
            src2_temps[k] = ai->temp_src2;
        }

        /* Check if operand 1 comes from contiguous loads */
        int s1_load_idx[4] = {-1, -1, -1, -1};
        int s1_inter[4] = {-1, -1, -1, -1};
        PtrAddr s1_addrs[4];
        bool s1_contiguous = true;
        for (int k = 0; k < width; k++) {
            s1_load_idx[k] = find_underlying_load(c, def_idx[k], src1_temps[k], &s1_inter[k]);
            if (s1_load_idx[k] < 0) { s1_contiguous = false; break; }
            const Instr *li = ir_get(c, s1_load_idx[k]);
            uint32_t la = 0, ld = 0; int le = 0; bool lf = false;
            if (!is_slp_load(li, &la, &ld, &le, &lf) || le != esz) {
                s1_contiguous = false;
                break;
            }
            resolve_ptr_addr(c, s1_load_idx[k], la, &s1_addrs[k]);
            if (k > 0 && (s1_addrs[k].kind != s1_addrs[0].kind ||
                          s1_addrs[k].id != s1_addrs[0].id ||
                          s1_addrs[k].offset != s1_addrs[0].offset + k * esz)) {
                s1_contiguous = false;
                break;
            }
        }

        /* Check if operand 2 comes from contiguous loads */
        int s2_load_idx[4] = {-1, -1, -1, -1};
        int s2_inter[4] = {-1, -1, -1, -1};
        PtrAddr s2_addrs[4];
        bool s2_contiguous = true;
        for (int k = 0; k < width; k++) {
            s2_load_idx[k] = find_underlying_load(c, def_idx[k], src2_temps[k], &s2_inter[k]);
            if (s2_load_idx[k] < 0) { s2_contiguous = false; break; }
            const Instr *li = ir_get(c, s2_load_idx[k]);
            uint32_t la = 0, ld = 0; int le = 0; bool lf = false;
            if (!is_slp_load(li, &la, &ld, &le, &lf) || le != esz) {
                s2_contiguous = false;
                break;
            }
            resolve_ptr_addr(c, s2_load_idx[k], la, &s2_addrs[k]);
            if (k > 0 && (s2_addrs[k].kind != s2_addrs[0].kind ||
                          s2_addrs[k].id != s2_addrs[0].id ||
                          s2_addrs[k].offset != s2_addrs[0].offset + k * esz)) {
                s2_contiguous = false;
                break;
            }
        }

        /* Check for broadcast scalar on operand 2 (e.g. constant multiplier or scalar add) */
        bool s2_broadcast = false;
        if (!s2_contiguous) {
            bool all_same_temp = true;
            for (int k = 1; k < width; k++) {
                if (src2_temps[k] != src2_temps[0]) { all_same_temp = false; break; }
            }
            if (all_same_temp && src2_temps[0] != TEMP_NONE) {
                s2_broadcast = true;
            }
        }

        if (s1_contiguous && (s2_contiguous || s2_broadcast)) {
            /* Vectorize binary arithmetic! */
            int v_in1 = zc_alloc_temp(c);
            int v_in2 = zc_alloc_temp(c);
            int v_res = zc_alloc_temp(c);

            /* In-place vector load for operand 1 */
            Instr *ld1 = ir_get(c, s1_load_idx[0]);
            ld1->op = OP_VLOAD;
            ld1->temp_dest = (uint32_t)v_in1;
            ld1->temp_src2 = TEMP_NONE;
            ld1->width = 0;
            ld1->cond = 0;
            ld1->flags_out = 0xFF;
            ld1->flags_in = 0xFF;
            ld1->imm = 0;
            ld1->imm64 = 0;

            /* In-place vector load or broadcast for operand 2 */
            if (s2_contiguous) {
                Instr *ld2 = ir_get(c, s2_load_idx[0]);
                ld2->op = OP_VLOAD;
                ld2->temp_dest = (uint32_t)v_in2;
                ld2->temp_src2 = TEMP_NONE;
                ld2->width = 0;
                ld2->cond = 0;
                ld2->flags_out = 0xFF;
                ld2->flags_in = 0xFF;
                ld2->imm = 0;
                ld2->imm64 = 0;
            } else {
                /* Broadcast scalar: strided load with stride 0 */
                int def_s2 = find_last_def_before(c, def_idx[0], src2_temps[0]);
                uint32_t b_addr = (def_s2 >= 0) ? ir_get(c, def_s2)->temp_src1 : src2_temps[0];
                Instr *a0 = ir_get(c, def_idx[0]);
                /* Insert broadcast before arith */
                Instr bcast = {
                    .op = OP_V128_LOAD_STRIDED,
                    .temp_dest = (uint32_t)v_in2,
                    .temp_src1 = b_addr,
                    .temp_src2 = TEMP_NONE,
                    .width = 0,
                    .cond = 0,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF,
                    .imm = 0,
                    .imm64 = esz,
                    .label_idx = 0
                };
                ir_insert_at(c, (size_t)def_idx[0], bcast);
                /* Shift recorded indices */
                for (int k = 0; k < width; k++) {
                    def_idx[k]++;
                    stores[i + k].instr_idx++;
                }
                store_idx0++;
                (void)a0;
            }

            /* In-place vector arithmetic at arith 0 */
            Instr *ar0 = ir_get(c, def_idx[0]);
            ar0->op = (uint16_t)vec_op;
            ar0->temp_dest = (uint32_t)v_res;
            ar0->temp_src1 = (uint32_t)v_in1;
            ar0->temp_src2 = (uint32_t)v_in2;
            ar0->width = 0;
            ar0->cond = 0;
            ar0->flags_out = 0xFF;
            ar0->flags_in = 0xFF;
            ar0->imm = 0;
            ar0->imm64 = 0;

            /* In-place vector store at store 0 */
            Instr *st0 = ir_get(c, store_idx0);
            st0->op = OP_VSTORE;
            st0->temp_dest = TEMP_NONE;
            st0->temp_src1 = store_addr0;
            st0->temp_src2 = (uint32_t)v_res;
            st0->width = 0;
            st0->cond = 0;
            st0->flags_out = 0xFF;
            st0->flags_in = 0xFF;
            st0->imm = 0;
            st0->imm64 = 0;

            /* NOP out other scalar loads, arithmetics, and stores */
            for (int k = 1; k < width; k++) {
                if (s1_load_idx[k] >= 0) {
                    Instr *sk1 = ir_get(c, s1_load_idx[k]);
                    if (sk1) sk1->op = OP_NOP;
                }
                if (s1_inter[k] >= 0) {
                    Instr *ik1 = ir_get(c, s1_inter[k]);
                    if (ik1) ik1->op = OP_NOP;
                }
                if (s2_contiguous && s2_load_idx[k] >= 0) {
                    Instr *sk2 = ir_get(c, s2_load_idx[k]);
                    if (sk2) sk2->op = OP_NOP;
                }
                if (s2_inter[k] >= 0) {
                    Instr *ik2 = ir_get(c, s2_inter[k]);
                    if (ik2) ik2->op = OP_NOP;
                }
                Instr *ark = ir_get(c, def_idx[k]);
                if (ark) ark->op = OP_NOP;
                Instr *stk = ir_get(c, stores[i + k].instr_idx);
                if (stk) stk->op = OP_NOP;
                stores[i + k].vectorized = true;
            }
            if (s1_inter[0] >= 0) {
                Instr *ik1 = ir_get(c, s1_inter[0]);
                if (ik1) ik1->op = OP_NOP;
            }
            if (s2_inter[0] >= 0) {
                Instr *ik2 = ir_get(c, s2_inter[0]);
                if (ik2) ik2->op = OP_NOP;
            }
            stores[i].vectorized = true;
            changed = true;
        }
    }

    return changed;
}

/* Public entry point for SLP Vectorization Pass */
void opt_slp_vectorize(ZeroCCCompiler *c) {
    if (!c) return;

    size_t count = ir_get_count(c);
    if (count < 8) return;

    int bb_start = 0;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, (int)i);
        if (!ins) continue;
        if (is_bb_boundary(ins)) {
            if ((int)i - 1 > bb_start + 4) {
                slp_vectorize_block(c, bb_start, (int)i - 1);
            }
            bb_start = (int)i + 1;
            count = ir_get_count(c); /* Update count in case instructions were inserted */
        }
    }

    /* Process the final basic block */
    if ((int)count - 1 > bb_start + 4) {
        slp_vectorize_block(c, bb_start, (int)count - 1);
    }
}
