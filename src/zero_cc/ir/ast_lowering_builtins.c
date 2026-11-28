/* AST Lowering - GCC/Clang Builtins, Intrinsics, and Atomics (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"


/* Builtin emit helpers (bitops/overflow/atomics/const_eval/objsize) -> ast_lowering_builtin_helpers.c,
 * va_start/va_arg -> ast_lowering_varargs.c (Sprint 114). */

/* Allocate a 16-byte aligned stack slot for a 128-bit SIMD vector result
 * and emit LEA into a fresh address temp. */
static int zc_simd_alloc_res128(ZeroCCCompiler *c, int32_t *off_out) {
    c->current_stack_frame = (c->current_stack_frame + 16 + 15) & ~15;
    int32_t res_off = c->current_stack_frame;
    if (off_out) *off_out = res_off;
    int res_addr = zc_alloc_temp(c);
    ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
    return res_addr;
}

/* Offset a vector base address temp by `byte_off` bytes. */
static int zc_simd_lane_addr(ZeroCCCompiler *c, int base_addr, size_t byte_off) {
    if (byte_off == 0) return base_addr;
    int off_t = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off_t, (int64_t)byte_off);
    int lane_t = zc_alloc_temp(c);
    ir_emit_add(c, lane_t, base_addr, off_t);
    return lane_t;
}

/* True when the active target implements 128-bit hardware vector load/store. */
static bool zc_simd_has_hw128(const ZeroCCCompiler *c) {
    return zc_target_has_simd(c) && BE(c)->emit_vload && BE(c)->emit_vstore;
}

static inline void zc_simd_emit_vload(ZeroCCCompiler *c, int vt, int addr) {
    ir_emit(c, (Instr){.op = OP_VLOAD, .temp_dest = (uint32_t)vt,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = TEMP_NONE,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_simd_emit_vstore(ZeroCCCompiler *c, int addr, int vt) {
    ir_emit(c, (Instr){.op = OP_VSTORE, .temp_dest = TEMP_NONE,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = (uint32_t)vt,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_simd_emit_vbin(ZeroCCCompiler *c, Opcode op, int vr, int va, int vb) {
    ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                       .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

/* Copy 16 bytes between vector addresses (hardware SIMD when available, else 4x32-bit). */
static void zc_simd_copy128(ZeroCCCompiler *c, int dst_addr, int src_addr) {
    if (zc_simd_has_hw128(c)) {
        int vt = zc_alloc_temp(c);
        zc_simd_emit_vload(c, vt, src_addr);
        zc_simd_emit_vstore(c, dst_addr, vt);
        return;
    }
    for (size_t i = 0; i < 4; i++) {
        int sa = zc_simd_lane_addr(c, src_addr, i * 4);
        int da = zc_simd_lane_addr(c, dst_addr, i * 4);
        int tmp = zc_alloc_temp(c);
        ir_emit_load_32(c, tmp, sa);
        ir_emit_store_32(c, tmp, da);
    }
}

/* Allocate a 32-byte aligned stack slot for a 256-bit AVX vector result. */
static int zc_simd_alloc_res256(ZeroCCCompiler *c, int32_t *off_out) {
    c->current_stack_frame = (c->current_stack_frame + 32 + 31) & ~31;
    int32_t res_off = c->current_stack_frame;
    if (off_out) *off_out = res_off;
    int res_addr = zc_alloc_temp(c);
    ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
    return res_addr;
}

/* True when the active target implements 256-bit hardware vector load/store. */
static bool zc_simd_has_hw256(const ZeroCCCompiler *c) {
    return c->has_avx && c->isa == ISA_X86_64 && BE(c)->emit_v256_load && BE(c)->emit_v256_store;
}

static inline void zc_simd_emit_v256_load(ZeroCCCompiler *c, int vt, int addr) {
    ir_emit(c, (Instr){.op = OP_V256_LOAD, .temp_dest = (uint32_t)vt,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = TEMP_NONE,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_simd_emit_v256_store(ZeroCCCompiler *c, int addr, int vt) {
    ir_emit(c, (Instr){.op = OP_V256_STORE, .temp_dest = TEMP_NONE,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = (uint32_t)vt,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_simd_emit_v256_bin(ZeroCCCompiler *c, Opcode op, int vr, int va, int vb) {
    ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                       .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

/* Copy 32 bytes between vector addresses (hardware AVX when available, else 8x32-bit). */
static void zc_simd_copy256(ZeroCCCompiler *c, int dst_addr, int src_addr) {
    if (zc_simd_has_hw256(c)) {
        c->used_avx = true;
        int vt = zc_alloc_temp(c);
        zc_simd_emit_v256_load(c, vt, src_addr);
        zc_simd_emit_v256_store(c, dst_addr, vt);
        return;
    }
    for (size_t i = 0; i < 8; i++) {
        int sa = zc_simd_lane_addr(c, src_addr, i * 4);
        int da = zc_simd_lane_addr(c, dst_addr, i * 4);
        int tmp = zc_alloc_temp(c);
        ir_emit_load_32(c, tmp, sa);
        ir_emit_store_32(c, tmp, da);
    }
}

static int zc_simd_alloc_res512(ZeroCCCompiler *c, int32_t *off_out) {
    c->current_stack_frame = (c->current_stack_frame + 64 + 63) & ~63;
    int32_t res_off = c->current_stack_frame;
    if (off_out) *off_out = res_off;
    int res_addr = zc_alloc_temp(c);
    ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
    return res_addr;
}

/* True when the active target implements ARM SVE. */
static bool zc_simd_has_sve(const ZeroCCCompiler *c) {
    return c->has_sve && c->isa == ISA_AARCH64;
}

static int64_t zc_get_imm_arg(ZeroCCASTArena *arena, ASTRef ref) {
    if (!ref) return 0;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    while (n && n->tag == NODE_CAST) {
        ref = n->left;
        n = ref ? zero_cc_ast_get(arena, ref) : NULL;
    }
    if (n && n->tag == NODE_INT) {
        return n->int_val;
    }
    return 0;
}

static inline void zc_sve_emit_load(ZeroCCCompiler *c, int vt, int addr, int esz) {
    ir_emit(c, (Instr){.op = OP_SVE_LD1, .temp_dest = (uint32_t)vt,
                       .temp_src1 = (uint32_t)addr, .imm = esz,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_sve_emit_store(ZeroCCCompiler *c, int addr, int vt, int esz) {
    ir_emit(c, (Instr){.op = OP_SVE_ST1, .temp_dest = TEMP_NONE,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = (uint32_t)vt,
                       .imm = esz,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static bool zc_simd_has_rvv(const ZeroCCCompiler *c) {
    return c->has_rvv && (c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32);
}

static inline void zc_rvv_emit_load(ZeroCCCompiler *c, int vt, int addr, int esz) {
    ir_emit(c, (Instr){.op = OP_RVV_VLE, .temp_dest = (uint32_t)vt,
                       .temp_src1 = (uint32_t)addr, .imm = esz,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_rvv_emit_store(ZeroCCCompiler *c, int addr, int vt, int esz) {
    ir_emit(c, (Instr){.op = OP_RVV_VSE, .temp_dest = TEMP_NONE,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = (uint32_t)vt,
                       .imm = esz,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

/* True when the active target implements 512-bit hardware vector load/store. */
static bool zc_simd_has_hw512(const ZeroCCCompiler *c) {
    return c->has_avx512f && c->isa == ISA_X86_64 && BE(c)->emit_v512_load && BE(c)->emit_v512_store;
}

static inline void zc_simd_emit_v512_load(ZeroCCCompiler *c, int vt, int addr) {
    ir_emit(c, (Instr){.op = OP_V512_LOAD, .temp_dest = (uint32_t)vt,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = TEMP_NONE,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_simd_emit_v512_store(ZeroCCCompiler *c, int addr, int vt) {
    ir_emit(c, (Instr){.op = OP_V512_STORE, .temp_dest = TEMP_NONE,
                       .temp_src1 = (uint32_t)addr, .temp_src2 = (uint32_t)vt,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

static inline void zc_simd_emit_v512_bin(ZeroCCCompiler *c, Opcode op, int vr, int va, int vb) {
    ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                       .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                       .flags_out = 0xFF, .flags_in = 0xFF});
}

/* Copy 64 bytes between vector addresses (hardware AVX-512 when available, else 16x32-bit). */
static void zc_simd_copy512(ZeroCCCompiler *c, int dst_addr, int src_addr) {
    if (zc_simd_has_hw512(c)) {
        c->used_avx512 = true;
        c->used_avx = true;
        int vt = zc_alloc_temp(c);
        zc_simd_emit_v512_load(c, vt, src_addr);
        zc_simd_emit_v512_store(c, dst_addr, vt);
        return;
    }
    for (size_t i = 0; i < 16; i++) {
        int sa = zc_simd_lane_addr(c, src_addr, i * 4);
        int da = zc_simd_lane_addr(c, dst_addr, i * 4);
        int tmp = zc_alloc_temp(c);
        ir_emit_load_32(c, tmp, sa);
        ir_emit_store_32(c, tmp, da);
    }
}

/* Scalar emulation of AVX-512 32-bit integer min/max (alu 8..11 = min_s, max_s,
 * min_u, max_u).  Operates on 64-bit extended lane values so a-b cannot wrap:
 * m = (a-b) & ((a-b)>>63); min = b + m; max = a - m. */
static void zc_simd_emul_minmax32(ZeroCCCompiler *c, int vr, int va, int vb, int alu) {
    bool is_signed = (alu == 8 || alu == 9);
    bool is_max = (alu == 9 || alu == 11);
    int a64 = va, b64 = vb; /* lane temps are scratch: extend in place to 64 bits */
    if (is_signed) { ir_emit_sext(c, a64, 4); ir_emit_sext(c, b64, 4); }
    else           { ir_emit_zext(c, a64, 4); ir_emit_zext(c, b64, 4); }
    int diff = zc_alloc_temp(c), sh = zc_alloc_temp(c), s63 = zc_alloc_temp(c), m = zc_alloc_temp(c);
    ir_emit_sub(c, (uint32_t)diff, (uint32_t)a64, (uint32_t)b64);
    ir_emit_mov_imm(c, (uint32_t)s63, 63);
    ir_emit_sar(c, sh, diff, s63);
    ir_emit_and(c, m, diff, sh);
    if (is_max) ir_emit_sub(c, (uint32_t)vr, (uint32_t)a64, (uint32_t)m);
    else        ir_emit_add(c, (uint32_t)vr, (uint32_t)b64, (uint32_t)m);
}

/* Scalar emulation of vpermd/vpermps: res[i] = data[idx[i] & 15] (32-bit lanes). */
static void zc_simd_emul_permute32(ZeroCCCompiler *c, int res_addr, int idx_addr, int data_addr) {
    for (size_t i = 0; i < 16; i++) {
        int li = zc_simd_lane_addr(c, idx_addr, i * 4);
        int t_idx = zc_alloc_temp(c);
        ir_emit_load_32(c, (uint32_t)t_idx, (uint32_t)li);
        int t15 = zc_alloc_temp(c);
        ir_emit_mov_imm(c, (uint32_t)t15, 15);
        int t_and = zc_alloc_temp(c);
        ir_emit_and(c, t_and, t_idx, t15);
        int t2 = zc_alloc_temp(c);
        ir_emit_mov_imm(c, (uint32_t)t2, 2);
        int t_off = zc_alloc_temp(c);
        ir_emit_shl(c, t_off, t_and, t2);
        int t_src = zc_alloc_temp(c);
        ir_emit_add(c, (uint32_t)t_src, (uint32_t)data_addr, (uint32_t)t_off);
        int t_val = zc_alloc_temp(c);
        ir_emit_load_32(c, (uint32_t)t_val, (uint32_t)t_src);
        int lr = zc_simd_lane_addr(c, res_addr, i * 4);
        ir_emit_store_32(c, t_val, lr);
    }
}

/* Lower `__builtin_zerocc_v*` SIMD vendor intrinsic helpers. */
static bool zc_lower_simd_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                  ZeroCCASTNode *node, const char *name,
                                  int *out_temp) {
    const char *suf = name + 18; /* skip "__builtin_zerocc_v" */
    ASTRef arg0_ref = node->right;
    ZeroCCASTNode *arg0 = arg0_ref ? zero_cc_ast_get(arena, arg0_ref) : NULL;
    ASTRef arg1_ref = arg0 ? arg0->next : AST_NULL;
    ZeroCCASTNode *arg1 = arg1_ref ? zero_cc_ast_get(arena, arg1_ref) : NULL;
    ASTRef arg2_ref = arg1 ? arg1->next : AST_NULL;

    /* 0. 512-bit AVX-512 Operations (hardware EVEX encoding on x86-64 when -mavx512f, else 16-lane/8-lane scalar fallback) */
    if (strncmp(suf, "512_", 4) == 0) {
        const char *s512 = suf + 4;

        static const struct {
            const char *suffix;
            Opcode vop;
            uint8_t alu;   /* 0=add, 1=sub, 2=mul, 3=div, 4=and, 5=or, 6=xor, 7=andnot */
            uint8_t is_fp; /* 1=float/double, 0=int32/bits */
            uint8_t esz;   /* 1, 2, 4, 8 */
        } binops512[] = {
            {"addps", OP_V512_ADDPS,  0, 1, 4},
            {"subps", OP_V512_SUBPS,  1, 1, 4},
            {"mulps", OP_V512_MULPS,  2, 1, 4},
            {"divps", OP_V512_DIVPS,  3, 1, 4},
            {"addpd", OP_V512_ADDPD,  0, 1, 8},
            {"subpd", OP_V512_SUBPD,  1, 1, 8},
            {"mulpd", OP_V512_MULPD,  2, 1, 8},
            {"divpd", OP_V512_DIVPD,  3, 1, 8},
            {"andps", OP_V512_ANDPS,  4, 0, 4},
            {"orps",  OP_V512_VORPS,  5, 0, 4},
            {"xorps", OP_V512_VXORPS, 6, 0, 4},
            /* AVX-512 Integer SIMD operations (Sprint 202) */
            {"paddd",  OP_V512_PADDD,  0, 0, 4},
            {"psubd",  OP_V512_PSUBD,  1, 0, 4},
            {"pmulld", OP_V512_PMULLD, 2, 0, 4},
            {"paddq",  OP_V512_PADDQ,  0, 0, 8},
            {"psubq",  OP_V512_PSUBQ,  1, 0, 8},
            {"pmullq", OP_V512_PMULLQ, 2, 0, 8},
            {"pandd",  OP_V512_PANDD,  4, 0, 4},
            {"pord",   OP_V512_PORD,   5, 0, 4},
            {"pxord",  OP_V512_PXORD,  6, 0, 4},
            {"pandnd", OP_V512_PANDND, 7, 0, 4},
            {"paddb",  OP_V512_PADDB,  0, 0, 1},
            {"psubb",  OP_V512_PSUBB,  1, 0, 1},
            {"paddw",  OP_V512_PADDW,  0, 0, 2},
            {"psubw",  OP_V512_PSUBW,  1, 0, 2},
            {"pmullw", OP_V512_PMULLW, 2, 0, 2},
            {"pminsd", OP_V512_PMINSD, 8, 0, 4},
            {"pmaxsd", OP_V512_PMAXSD, 9, 0, 4},
            {"pminud", OP_V512_PMINUD, 10, 0, 4},
            {"pmaxud", OP_V512_PMAXUD, 11, 0, 4},
            {"permd",  OP_V512_PERMD,  12, 0, 4},
            {"permps", OP_V512_PERMPS, 12, 0, 4},
        };
        for (size_t k = 0; k < sizeof(binops512) / sizeof(binops512[0]); k++) {
            if (strcmp(s512, binops512[k].suffix) != 0) continue;
            if (!arg0_ref || !arg1_ref) return false;
            int a_addr = lower_expr(c, arena, arg0_ref);
            int b_addr = lower_expr(c, arena, arg1_ref);
            int res_addr = zc_simd_alloc_res512(c, NULL);
            if (zc_simd_has_hw512(c)) {
                c->used_avx512 = true;
                c->used_avx = true;
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                zc_simd_emit_v512_load(c, va, a_addr);
                zc_simd_emit_v512_load(c, vb, b_addr);
                zc_simd_emit_v512_bin(c, binops512[k].vop, vr, va, vb);
                zc_simd_emit_v512_store(c, res_addr, vr);
            } else if (binops512[k].alu == 12) {
                zc_simd_emul_permute32(c, res_addr, a_addr, b_addr);
            } else {
                size_t esz = binops512[k].esz;
                size_t lanes = 64 / esz;
                for (size_t i = 0; i < lanes; i++) {
                    int la = zc_simd_lane_addr(c, a_addr, i * esz);
                    int lb = zc_simd_lane_addr(c, b_addr, i * esz);
                    int lr = zc_simd_lane_addr(c, res_addr, i * esz);
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                    if (binops512[k].is_fp) {
                        if (esz == 4) { ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb); }
                        else          { ir_emit_vloadsd(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadsd(c, (uint32_t)vb, (uint32_t)lb); }
                        if (binops512[k].alu == 0)      ir_emit_fadd(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else if (binops512[k].alu == 1) ir_emit_fsub(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else if (binops512[k].alu == 2) ir_emit_fmul(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else                            ir_emit_fdiv(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        if (esz == 4) ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)vr);
                        else          ir_emit_vstoresd(c, (uint32_t)lr, (uint32_t)vr);
                    } else if (esz == 8) {
                        ir_emit_deref(c, va, la);
                        ir_emit_deref(c, vb, lb);
                        if (binops512[k].alu == 0) ir_emit_add(c, vr, va, vb);
                        else if (binops512[k].alu == 1) ir_emit_sub(c, vr, va, vb);
                        else if (binops512[k].alu == 2) ir_emit_mul(c, vr, va, vb);
                        else if (binops512[k].alu == 4) ir_emit_and(c, vr, va, vb);
                        else if (binops512[k].alu == 5) ir_emit_or(c, vr, va, vb);
                        else if (binops512[k].alu == 6) ir_emit_xor(c, vr, va, vb);
                        else if (binops512[k].alu == 7) { ir_emit_not(c, vr, va); ir_emit_and(c, vr, vr, vb); }
                        ir_emit_store_64(c, vr, lr);
                    } else if (esz == 4) {
                        ir_emit_load_32(c, va, la);
                        ir_emit_load_32(c, vb, lb);
                        if (binops512[k].alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 2) ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 4) ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 5) ir_emit(c, (Instr){.op = OP_OR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 6) ir_emit(c, (Instr){.op = OP_XOR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 7) { ir_emit_not(c, vr, va); ir_emit_and(c, vr, vr, vb); }
                        else if (binops512[k].alu >= 8 && binops512[k].alu <= 11) zc_simd_emul_minmax32(c, vr, va, vb, binops512[k].alu);
                        ir_emit_store_32(c, vr, lr);
                    } else if (esz == 2) {
                        ir_emit_load_16(c, va, la);
                        ir_emit_load_16(c, vb, lb);
                        if (binops512[k].alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 2) ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_16(c, vr, lr);
                    } else if (esz == 1) {
                        ir_emit_load_byte(c, va, la);
                        ir_emit_load_byte(c, vb, lb);
                        if (binops512[k].alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops512[k].alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_byte(c, vr, lr);
                    }
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        /* AVX-512 Masked Binary Operations (Sprint 201 & Sprint 202) */
        static const struct {
            const char *suffix;
            Opcode vop;
            uint8_t alu;
            uint8_t is_fp;
            uint8_t esz;
        } mask_binops512[] = {
            {"addps", OP_V512_MASK_ADDPS, 0, 1, 4},
            {"subps", OP_V512_MASK_SUBPS, 1, 1, 4},
            {"mulps", OP_V512_MASK_MULPS, 2, 1, 4},
            {"divps", OP_V512_MASK_DIVPS, 3, 1, 4},
            {"addpd", OP_V512_MASK_ADDPD, 0, 1, 8},
            {"subpd", OP_V512_MASK_SUBPD, 1, 1, 8},
            {"mulpd", OP_V512_MASK_MULPD, 2, 1, 8},
            {"divpd", OP_V512_MASK_DIVPD, 3, 1, 8},
            {"paddd",  OP_V512_PADDD,  0, 0, 4},
            {"psubd",  OP_V512_PSUBD,  1, 0, 4},
            {"pmulld", OP_V512_PMULLD, 2, 0, 4},
            {"paddq",  OP_V512_PADDQ,  0, 0, 8},
            {"psubq",  OP_V512_PSUBQ,  1, 0, 8},
            {"pmullq", OP_V512_PMULLQ, 2, 0, 8},
            {"pandd",  OP_V512_PANDD,  4, 0, 4},
            {"pord",   OP_V512_PORD,   5, 0, 4},
            {"pxord",  OP_V512_PXORD,  6, 0, 4},
            {"pandnd", OP_V512_PANDND, 7, 0, 4},
            {"paddb",  OP_V512_PADDB,  0, 0, 1},
            {"psubb",  OP_V512_PSUBB,  1, 0, 1},
            {"paddw",  OP_V512_PADDW,  0, 0, 2},
            {"psubw",  OP_V512_PSUBW,  1, 0, 2},
            {"pmullw", OP_V512_PMULLW, 2, 0, 2},
            {"pminsd", OP_V512_PMINSD, 8, 0, 4},
            {"pmaxsd", OP_V512_PMAXSD, 9, 0, 4},
            {"pminud", OP_V512_PMINUD, 10, 0, 4},
            {"pmaxud", OP_V512_PMAXUD, 11, 0, 4},
        };
        for (size_t k = 0; k < sizeof(mask_binops512) / sizeof(mask_binops512[0]); k++) {
            bool is_mask = (strncmp(s512, "mask_", 5) == 0 && strcmp(s512 + 5, mask_binops512[k].suffix) == 0);
            bool is_maskz = (strncmp(s512, "maskz_", 6) == 0 && strcmp(s512 + 6, mask_binops512[k].suffix) == 0);
            if (!is_mask && !is_maskz) continue;

            int z = is_maskz ? 1 : 0;
            ASTRef src_ref = is_mask ? arg0_ref : AST_NULL;
            ASTRef k_ref   = is_mask ? arg1_ref : arg2_ref;
            ASTRef a_ref   = is_mask ? arg2_ref : arg0_ref;
            ASTRef b_ref   = AST_NULL;
            if (is_mask) {
                ZeroCCASTNode *a_node = arg2_ref ? zero_cc_ast_get(arena, arg2_ref) : NULL;
                b_ref = a_node ? a_node->next : AST_NULL;
            } else {
                b_ref = arg1_ref;
            }
            if (!k_ref || !a_ref || !b_ref) return false;

            int k_val = lower_expr(c, arena, k_ref);
            int a_addr = lower_expr(c, arena, a_ref);
            int b_addr = lower_expr(c, arena, b_ref);
            int res_addr = zc_simd_alloc_res512(c, NULL);

            if (zc_simd_has_hw512(c)) {
                c->used_avx512 = true;
                c->used_avx = true;
                ir_emit(c, (Instr){.op = OP_KMOVW_TO_K1, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)k_val, .flags_out = 0xFF, .flags_in = 0xFF});
                int vr = zc_alloc_temp(c), va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                if (!z && src_ref) {
                    int src_addr = lower_expr(c, arena, src_ref);
                    zc_simd_emit_v512_load(c, vr, src_addr);
                }
                zc_simd_emit_v512_load(c, va, a_addr);
                zc_simd_emit_v512_load(c, vb, b_addr);
                bool is_new_int = (mask_binops512[k].vop >= OP_V512_PADDD && mask_binops512[k].vop <= OP_V512_PERMPS);
                uint8_t m_cond = is_new_int ? (uint8_t)(z ? 2 : 1) : 0;
                int32_t m_imm = is_new_int ? 0 : (int32_t)z;
                ir_emit(c, (Instr){.op = mask_binops512[k].vop, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .imm = m_imm, .cond = m_cond, .flags_out = 0xFF, .flags_in = 0xFF});
                zc_simd_emit_v512_store(c, res_addr, vr);
            } else {
                size_t esz = mask_binops512[k].esz;
                size_t lanes = 64 / esz;
                if (!z && src_ref) {
                    int src_addr = lower_expr(c, arena, src_ref);
                    zc_simd_copy512(c, res_addr, src_addr);
                } else {
                    int zt = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, zt, 0);
                    for (size_t i = 0; i < 16; i++) {
                        int da = zc_simd_lane_addr(c, res_addr, i * 4);
                        ir_emit_store_32(c, zt, da);
                    }
                }
                for (size_t i = 0; i < lanes; i++) {
                    int t_mask = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t_mask, (int64_t)1 << i);
                    int t_bit = zc_alloc_temp(c);
                    ir_emit_and(c, t_bit, k_val, t_mask);
                    uint16_t l_skip = (uint16_t)c->ir_next_label++;
                    ir_emit_je(c, (uint32_t)t_bit, l_skip);

                    int la = zc_simd_lane_addr(c, a_addr, i * esz);
                    int lb = zc_simd_lane_addr(c, b_addr, i * esz);
                    int lr = zc_simd_lane_addr(c, res_addr, i * esz);
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                    if (mask_binops512[k].is_fp) {
                        if (esz == 4) { ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb); }
                        else          { ir_emit_vloadsd(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadsd(c, (uint32_t)vb, (uint32_t)lb); }
                        if (mask_binops512[k].alu == 0)      ir_emit_fadd(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else if (mask_binops512[k].alu == 1) ir_emit_fsub(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else if (mask_binops512[k].alu == 2) ir_emit_fmul(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else                                 ir_emit_fdiv(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        if (esz == 4) ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)vr);
                        else          ir_emit_vstoresd(c, (uint32_t)lr, (uint32_t)vr);
                    } else if (esz == 8) {
                        ir_emit_deref(c, va, la);
                        ir_emit_deref(c, vb, lb);
                        if (mask_binops512[k].alu == 0) ir_emit_add(c, vr, va, vb);
                        else if (mask_binops512[k].alu == 1) ir_emit_sub(c, vr, va, vb);
                        else if (mask_binops512[k].alu == 2) ir_emit_mul(c, vr, va, vb);
                        else if (mask_binops512[k].alu == 4) ir_emit_and(c, vr, va, vb);
                        else if (mask_binops512[k].alu == 5) ir_emit_or(c, vr, va, vb);
                        else if (mask_binops512[k].alu == 6) ir_emit_xor(c, vr, va, vb);
                        else if (mask_binops512[k].alu == 7) { ir_emit_not(c, vr, va); ir_emit_and(c, vr, vr, vb); }
                        ir_emit_store_64(c, vr, lr);
                    } else if (esz == 4) {
                        ir_emit_load_32(c, va, la);
                        ir_emit_load_32(c, vb, lb);
                        if (mask_binops512[k].alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 2) ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 4) ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 5) ir_emit(c, (Instr){.op = OP_OR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 6) ir_emit(c, (Instr){.op = OP_XOR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 7) { ir_emit_not(c, vr, va); ir_emit_and(c, vr, vr, vb); }
                        else if (mask_binops512[k].alu >= 8 && mask_binops512[k].alu <= 11) zc_simd_emul_minmax32(c, vr, va, vb, mask_binops512[k].alu);
                        ir_emit_store_32(c, vr, lr);
                    } else if (esz == 2) {
                        ir_emit_load_16(c, va, la);
                        ir_emit_load_16(c, vb, lb);
                        if (mask_binops512[k].alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 2) ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_16(c, vr, lr);
                    } else if (esz == 1) {
                        ir_emit_load_byte(c, va, la);
                        ir_emit_load_byte(c, vb, lb);
                        if (mask_binops512[k].alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (mask_binops512[k].alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_byte(c, vr, lr);
                    }

                    ir_emit_label(c, l_skip);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        /* AVX-512 Masked Load Operations (Sprint 201) */
        if (strncmp(s512, "mask", 4) == 0 && strstr(s512, "load512")) {
            bool is_z = (strncmp(s512, "maskz_", 6) == 0);
            bool is_pd = (strstr(s512, "_pd") != NULL);
            size_t esz = is_pd ? 8 : 4;
            size_t lanes = 64 / esz;
            ASTRef src_ref = is_z ? AST_NULL : arg0_ref;
            ASTRef k_ref   = arg1_ref;
            ASTRef ptr_ref = arg2_ref;
            if (!k_ref || !ptr_ref) return false;

            int k_val = lower_expr(c, arena, k_ref);
            int ptr_val = lower_expr(c, arena, ptr_ref);
            int res_addr = zc_simd_alloc_res512(c, NULL);

            if (zc_simd_has_hw512(c)) {
                c->used_avx512 = true;
                c->used_avx = true;
                ir_emit(c, (Instr){.op = OP_KMOVW_TO_K1, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)k_val, .flags_out = 0xFF, .flags_in = 0xFF});
                int vr = zc_alloc_temp(c);
                if (!is_z && src_ref) {
                    int src_addr = lower_expr(c, arena, src_ref);
                    zc_simd_emit_v512_load(c, vr, src_addr);
                }
                ir_emit(c, (Instr){.op = OP_V512_MASK_LOAD, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)ptr_val, .imm = 0, .cond = (uint8_t)(is_z ? 1 : 0), .flags_out = 0xFF, .flags_in = 0xFF});
                zc_simd_emit_v512_store(c, res_addr, vr);
            } else {
                if (!is_z && src_ref) {
                    int src_addr = lower_expr(c, arena, src_ref);
                    zc_simd_copy512(c, res_addr, src_addr);
                } else {
                    int zt = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, zt, 0);
                    for (size_t i = 0; i < 16; i++) {
                        int da = zc_simd_lane_addr(c, res_addr, i * 4);
                        ir_emit_store_32(c, zt, da);
                    }
                }
                for (size_t i = 0; i < lanes; i++) {
                    int t_mask = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t_mask, (int64_t)1 << i);
                    int t_bit = zc_alloc_temp(c);
                    ir_emit_and(c, t_bit, k_val, t_mask);
                    uint16_t l_skip = (uint16_t)c->ir_next_label++;
                    ir_emit_je(c, (uint32_t)t_bit, l_skip);

                    int pa = zc_simd_lane_addr(c, ptr_val, i * esz);
                    int lr = zc_simd_lane_addr(c, res_addr, i * esz);
                    int tmp = zc_alloc_temp(c);
                    if (esz == 4) {
                        ir_emit_vloadss(c, (uint32_t)tmp, (uint32_t)pa);
                        ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)tmp);
                    } else {
                        ir_emit_vloadsd(c, (uint32_t)tmp, (uint32_t)pa);
                        ir_emit_vstoresd(c, (uint32_t)lr, (uint32_t)tmp);
                    }
                    ir_emit_label(c, l_skip);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        /* AVX-512 Masked Store Operations (Sprint 201) */
        if (strncmp(s512, "mask_store512", 13) == 0) {
            bool is_pd = (strstr(s512, "_pd") != NULL);
            size_t esz = is_pd ? 8 : 4;
            size_t lanes = 64 / esz;
            if (!arg0_ref || !arg1_ref || !arg2_ref) return false;

            int ptr_val = lower_expr(c, arena, arg0_ref);
            int k_val = lower_expr(c, arena, arg1_ref);
            int vec_addr = lower_expr(c, arena, arg2_ref);

            if (zc_simd_has_hw512(c)) {
                c->used_avx512 = true;
                c->used_avx = true;
                ir_emit(c, (Instr){.op = OP_KMOVW_TO_K1, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)k_val, .flags_out = 0xFF, .flags_in = 0xFF});
                int vt = zc_alloc_temp(c);
                zc_simd_emit_v512_load(c, vt, vec_addr);
                ir_emit(c, (Instr){.op = OP_V512_MASK_STORE, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)ptr_val, .temp_src2 = (uint32_t)vt, .imm = 0, .flags_out = 0xFF, .flags_in = 0xFF});
            } else {
                for (size_t i = 0; i < lanes; i++) {
                    int t_mask = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t_mask, (int64_t)1 << i);
                    int t_bit = zc_alloc_temp(c);
                    ir_emit_and(c, t_bit, k_val, t_mask);
                    uint16_t l_skip = (uint16_t)c->ir_next_label++;
                    ir_emit_je(c, (uint32_t)t_bit, l_skip);

                    int pa = zc_simd_lane_addr(c, ptr_val, i * esz);
                    int va = zc_simd_lane_addr(c, vec_addr, i * esz);
                    int tmp = zc_alloc_temp(c);
                    if (esz == 4) {
                        ir_emit_vloadss(c, (uint32_t)tmp, (uint32_t)va);
                        ir_emit_vstoress(c, (uint32_t)pa, (uint32_t)tmp);
                    } else {
                        ir_emit_vloadsd(c, (uint32_t)tmp, (uint32_t)va);
                        ir_emit_vstoresd(c, (uint32_t)pa, (uint32_t)tmp);
                    }
                    ir_emit_label(c, l_skip);
                }
            }
            int z = zc_alloc_temp(c);
            ir_emit_mov_imm(c, z, 0);
            c->last_expr_is_float = 0;
            *out_temp = z;
            return true;
        }

        /* AVX-512 Masked Blend Operations (Sprint 205 - Phase 11) */
        if (strncmp(s512, "mask_blend", 10) == 0) {
            bool is_pd = (strstr(s512, "_pd") != NULL);
            bool is_q  = (strstr(s512, "_epi64") != NULL);
            size_t esz = (is_pd || is_q) ? 8 : 4;
            size_t lanes = 64 / esz;
            if (!arg0_ref || !arg1_ref || !arg2_ref) return false;

            int k_val = lower_expr(c, arena, arg0_ref);
            int a_addr = lower_expr(c, arena, arg1_ref);
            int b_addr = lower_expr(c, arena, arg2_ref);
            int res_addr = zc_simd_alloc_res512(c, NULL);

            if (zc_simd_has_hw512(c)) {
                c->used_avx512 = true;
                c->used_avx = true;
                ir_emit(c, (Instr){.op = OP_KMOVW_TO_K1, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)k_val, .flags_out = 0xFF, .flags_in = 0xFF});
                int va = zc_alloc_temp(c);
                int vb = zc_alloc_temp(c);
                int vr = zc_alloc_temp(c);
                zc_simd_emit_v512_load(c, va, a_addr);
                zc_simd_emit_v512_load(c, vb, b_addr);
                Opcode op = (is_pd || is_q) ? OP_V512_BLENDPD : OP_V512_BLENDPS;
                ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .imm = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                zc_simd_emit_v512_store(c, res_addr, vr);
            } else {
                for (size_t i = 0; i < lanes; i++) {
                    int t_mask = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t_mask, (int64_t)1 << i);
                    int t_bit = zc_alloc_temp(c);
                    ir_emit_and(c, t_bit, k_val, t_mask);
                    int pa = zc_simd_lane_addr(c, a_addr, i * esz);
                    int pb = zc_simd_lane_addr(c, b_addr, i * esz);
                    int pr = zc_simd_lane_addr(c, res_addr, i * esz);

                    uint16_t l_else = (uint16_t)c->ir_next_label++;
                    uint16_t l_end = (uint16_t)c->ir_next_label++;
                    ir_emit_je(c, (uint32_t)t_bit, l_else);
                    /* k bit is 1 -> select b */
                    int val_b = zc_alloc_temp(c);
                    if (esz == 8) {
                        ir_emit_deref(c, val_b, pb);
                        ir_emit_store_64(c, val_b, pr);
                    } else {
                        ir_emit_load_32(c, val_b, pb);
                        ir_emit_store_32(c, val_b, pr);
                    }
                    ir_emit_jmp(c, l_end);
                    ir_emit_label(c, l_else);
                    /* k bit is 0 -> select a */
                    int val_a = zc_alloc_temp(c);
                    if (esz == 8) {
                        ir_emit_deref(c, val_a, pa);
                        ir_emit_store_64(c, val_a, pr);
                    } else {
                        ir_emit_load_32(c, val_a, pa);
                        ir_emit_store_32(c, val_a, pr);
                    }
                    ir_emit_label(c, l_end);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        /* AVX-512 Vector Comparisons Producing Masks (Sprint 201/202) */
        if (strncmp(s512, "cmp", 3) == 0 && strstr(s512, "_mask") != NULL) {
            bool is_pd = (strstr(s512, "_pd_") != NULL);
            bool is_epi64 = (strstr(s512, "_epi64_") != NULL);
            bool is_epu64 = (strstr(s512, "_epu64_") != NULL);
            bool is_epi32 = (strstr(s512, "_epi32_") != NULL);
            bool is_epu32 = (strstr(s512, "_epu32_") != NULL);
            bool is_64 = (is_pd || is_epi64 || is_epu64);
            size_t esz = is_64 ? 8 : 4;
            size_t lanes = 64 / esz;

            bool is_lt = (strstr(s512, "cmplt_") != NULL);
            bool is_gt = (strstr(s512, "cmpgt_") != NULL);
            int imm_pred = is_lt ? 1 : (is_gt ? 6 : 0);
            uint8_t cond = is_lt ? CC_LT : (is_gt ? CC_GT : CC_EQ);
            if (is_epu32 || is_epu64) {
                if (is_lt) cond = CC_FLT_LT;
                else if (is_gt) cond = CC_FLT_GT;
            }

            int cmp_type = 0;
            if (is_pd) cmp_type = 1;
            else if (is_epi32) cmp_type = 2;
            else if (is_epu32) cmp_type = 3;
            else if (is_epi64) cmp_type = 4;
            else if (is_epu64) cmp_type = 5;

            if (!arg0_ref || !arg1_ref) return false;
            int a_addr = lower_expr(c, arena, arg0_ref);
            int b_addr = lower_expr(c, arena, arg1_ref);

            int res_mask = zc_alloc_temp(c);

            if (zc_simd_has_hw512(c)) {
                c->used_avx512 = true;
                c->used_avx = true;
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                zc_simd_emit_v512_load(c, va, a_addr);
                zc_simd_emit_v512_load(c, vb, b_addr);
                ir_emit(c, (Instr){.op = OP_V512_CMP_MASK, .temp_dest = (uint32_t)res_mask, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .imm = imm_pred, .cond = (uint8_t)cmp_type, .flags_out = 0xFF, .flags_in = 0xFF});
            } else {
                ir_emit_mov_imm(c, res_mask, 0);
                for (size_t i = 0; i < lanes; i++) {
                    int la = zc_simd_lane_addr(c, a_addr, i * esz);
                    int lb = zc_simd_lane_addr(c, b_addr, i * esz);
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                    if (cmp_type == 0) {
                        ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la);
                        ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb);
                        ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    } else if (cmp_type == 1) {
                        ir_emit_vloadsd(c, (uint32_t)va, (uint32_t)la);
                        ir_emit_vloadsd(c, (uint32_t)vb, (uint32_t)lb);
                        ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    } else if (esz == 4) {
                        ir_emit_load_32(c, va, la);
                        ir_emit_load_32(c, vb, lb);
                        ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    } else {
                        ir_emit_deref(c, va, la);
                        ir_emit_deref(c, vb, lb);
                        ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    }
                    int cc = zc_alloc_temp(c);
                    ir_emit_cond_set(c, (uint32_t)cc, cond);
                    if (i > 0) {
                        int t_cnt = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, t_cnt, (int32_t)i);
                        int t_sh = zc_alloc_temp(c);
                        ir_emit_shl(c, t_sh, cc, t_cnt);
                        cc = t_sh;
                    }
                    ir_emit_or(c, res_mask, res_mask, cc);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_mask;
            return true;
        }

        if (strcmp(s512, "load512") == 0) {
            if (!arg1_ref) return false;
            int ptr_temp = lower_expr(c, arena, arg1_ref);
            int res_addr = zc_simd_alloc_res512(c, NULL);
            zc_simd_copy512(c, res_addr, ptr_temp);
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        if (strcmp(s512, "store512") == 0) {
            if (!arg0_ref || !arg1_ref) return false;
            int ptr_temp = lower_expr(c, arena, arg0_ref);
            int vec_addr = lower_expr(c, arena, arg1_ref);
            zc_simd_copy512(c, ptr_temp, vec_addr);
            int z = zc_alloc_temp(c);
            ir_emit_mov_imm(c, z, 0);
            c->last_expr_is_float = 0;
            *out_temp = z;
            return true;
        }

        if (strncmp(s512, "set1_", 5) == 0) {
            if (!arg1_ref) return false;
            const char *ts = s512 + 5;
            bool is_fp = (strcmp(ts, "ps") == 0 || strcmp(ts, "pd") == 0);
            size_t esz = 4;
            if (strcmp(ts, "pd") == 0 || strcmp(ts, "epi64") == 0 || strcmp(ts, "epi64x") == 0) esz = 8;
            else if (strcmp(ts, "epi16") == 0) esz = 2;
            else if (strcmp(ts, "epi8") == 0) esz = 1;
            else if (strcmp(ts, "ps") != 0 && strcmp(ts, "epi32") != 0) return false;

            c->last_expr_is_float = 0;
            int val = lower_expr(c, arena, arg1_ref);
            bool val_flt = (c->last_expr_is_float != 0);
            if (is_fp && !val_flt) {
                int tf = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)tf, (uint32_t)val);
                val = tf;
            } else if (!is_fp && val_flt) {
                int ti = zc_alloc_temp(c);
                ir_emit_cvttsd2si(c, (uint32_t)ti, (uint32_t)val);
                val = ti;
            }
            int res_addr = zc_simd_alloc_res512(c, NULL);
            size_t lanes = 64 / esz;
            for (size_t i = 0; i < lanes; i++) {
                int la = zc_simd_lane_addr(c, res_addr, i * esz);
                if (is_fp) {
                    if (esz == 4) ir_emit_vstoress(c, (uint32_t)la, (uint32_t)val);
                    else          ir_emit_vstoresd(c, (uint32_t)la, (uint32_t)val);
                } else if (esz == 8) {
                    ir_emit_store_64(c, val, la);
                } else if (esz == 4) {
                    ir_emit_store_32(c, val, la);
                } else if (esz == 2) {
                    ir_emit_store_16(c, val, la);
                } else if (esz == 1) {
                    ir_emit_store_byte(c, val, la);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        return false;
    }

    /* 0. 256-bit AVX Operations (hardware VEX encoding on x86-64 when -mavx, else 8-lane/4-lane scalar fallback) */
    if (strncmp(suf, "256_", 4) == 0) {
        const char *s256 = suf + 4;
        if (strcmp(s256, "zeroupper") == 0) {
            if (zc_simd_has_hw256(c)) {
                c->used_avx = true;
                ir_emit(c, (Instr){.op = OP_V256_ZEROUPPER, .flags_out = 0xFF, .flags_in = 0xFF});
            }
            int z = zc_alloc_temp(c);
            ir_emit_mov_imm(c, z, 0);
            c->last_expr_is_float = 0;
            *out_temp = z;
            return true;
        }

        static const struct {
            const char *suffix;
            Opcode vop;
            uint8_t alu;   /* 0=add, 1=sub, 2=mul, 3=div, 4=and, 5=or, 6=xor */
            uint8_t is_fp; /* 1=float/double, 0=int32/bits */
            uint8_t esz;   /* 4 or 8 */
        } binops256[] = {
            {"addps", OP_V256_ADDPS,  0, 1, 4},
            {"subps", OP_V256_SUBPS,  1, 1, 4},
            {"mulps", OP_V256_MULPS,  2, 1, 4},
            {"divps", OP_V256_DIVPS,  3, 1, 4},
            {"addpd", OP_V256_ADDPD,  0, 1, 8},
            {"subpd", OP_V256_SUBPD,  1, 1, 8},
            {"mulpd", OP_V256_MULPD,  2, 1, 8},
            {"divpd", OP_V256_DIVPD,  3, 1, 8},
            {"andps", OP_V256_ANDPS,  4, 0, 4},
            {"orps",  OP_V256_VORPS,  5, 0, 4},
            {"xorps", OP_V256_VXORPS, 6, 0, 4},
        };
        for (size_t k = 0; k < sizeof(binops256) / sizeof(binops256[0]); k++) {
            if (strcmp(s256, binops256[k].suffix) != 0) continue;
            if (!arg0_ref || !arg1_ref) return false;
            int a_addr = lower_expr(c, arena, arg0_ref);
            int b_addr = lower_expr(c, arena, arg1_ref);
            int res_addr = zc_simd_alloc_res256(c, NULL);
            if (zc_simd_has_hw256(c)) {
                c->used_avx = true;
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                zc_simd_emit_v256_load(c, va, a_addr);
                zc_simd_emit_v256_load(c, vb, b_addr);
                zc_simd_emit_v256_bin(c, binops256[k].vop, vr, va, vb);
                zc_simd_emit_v256_store(c, res_addr, vr);
            } else {
                size_t esz = binops256[k].esz;
                size_t lanes = 32 / esz;
                for (size_t i = 0; i < lanes; i++) {
                    int la = zc_simd_lane_addr(c, a_addr, i * esz);
                    int lb = zc_simd_lane_addr(c, b_addr, i * esz);
                    int lr = zc_simd_lane_addr(c, res_addr, i * esz);
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                    if (binops256[k].is_fp) {
                        if (esz == 4) { ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb); }
                        else          { ir_emit_vloadsd(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadsd(c, (uint32_t)vb, (uint32_t)lb); }
                        if (binops256[k].alu == 0)      ir_emit_fadd(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else if (binops256[k].alu == 1) ir_emit_fsub(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else if (binops256[k].alu == 2) ir_emit_fmul(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        else                            ir_emit_fdiv(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                        if (esz == 4) ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)vr);
                        else          ir_emit_vstoresd(c, (uint32_t)lr, (uint32_t)vr);
                    } else {
                        ir_emit_load_32(c, va, la);
                        ir_emit_load_32(c, vb, lb);
                        if (binops256[k].alu == 4) ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops256[k].alu == 5) ir_emit(c, (Instr){.op = OP_OR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (binops256[k].alu == 6) ir_emit(c, (Instr){.op = OP_XOR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_32(c, vr, lr);
                    }
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        if (strcmp(s256, "load256") == 0) {
            if (!arg1_ref) return false;
            int ptr_temp = lower_expr(c, arena, arg1_ref);
            int res_addr = zc_simd_alloc_res256(c, NULL);
            zc_simd_copy256(c, res_addr, ptr_temp);
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        if (strcmp(s256, "store256") == 0) {
            if (!arg0_ref || !arg1_ref) return false;
            int ptr_temp = lower_expr(c, arena, arg0_ref);
            int vec_addr = lower_expr(c, arena, arg1_ref);
            zc_simd_copy256(c, ptr_temp, vec_addr);
            int z = zc_alloc_temp(c);
            ir_emit_mov_imm(c, z, 0);
            c->last_expr_is_float = 0;
            *out_temp = z;
            return true;
        }

        if (strncmp(s256, "set1_", 5) == 0) {
            if (!arg1_ref) return false;
            const char *ts = s256 + 5;
            bool is_fp = (strcmp(ts, "ps") == 0 || strcmp(ts, "pd") == 0);
            size_t esz = 4;
            if (strcmp(ts, "pd") == 0 || strcmp(ts, "epi64") == 0) esz = 8;
            else if (strcmp(ts, "ps") != 0 && strcmp(ts, "epi32") != 0) return false;

            c->last_expr_is_float = 0;
            int val = lower_expr(c, arena, arg1_ref);
            bool val_flt = (c->last_expr_is_float != 0);
            if (is_fp && !val_flt) {
                int tf = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)tf, (uint32_t)val);
                val = tf;
            } else if (!is_fp && val_flt) {
                int ti = zc_alloc_temp(c);
                ir_emit_cvttsd2si(c, (uint32_t)ti, (uint32_t)val);
                val = ti;
            }
            int res_addr = zc_simd_alloc_res256(c, NULL);
            size_t lanes = 32 / esz;
            for (size_t i = 0; i < lanes; i++) {
                int la = zc_simd_lane_addr(c, res_addr, i * esz);
                if (is_fp) {
                    if (esz == 4) ir_emit_vstoress(c, (uint32_t)la, (uint32_t)val);
                    else          ir_emit_vstoresd(c, (uint32_t)la, (uint32_t)val);
                } else if (esz == 4) {
                    ir_emit_store_32(c, val, la);
                } else {
                    ir_emit_store_64(c, val, la);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
        }

        return false;
    }

    /* 1. Packed binary hardware SIMD operations (+ 32-bit scalar fallback). */
    static const struct {
        const char *suffix;
        Opcode vop;
        uint8_t alu;   /* 0=add, 1=sub, 2=mul, 3=div */
        uint8_t is_fp; /* 1=float/double, 0=int32 */
        uint8_t esz;   /* 4 or 8 */
    } binops[] = {
        {"addps",  OP_VADDPS,  0, 1, 4},
        {"subps",  OP_VSUBPS,  1, 1, 4},
        {"mulps",  OP_VMULPS,  2, 1, 4},
        {"divps",  OP_VDIVPS,  3, 1, 4},
        {"addpd",  OP_VADDPD,  0, 1, 8},
        {"subpd",  OP_VSUBPD,  1, 1, 8},
        {"mulpd",  OP_VMULPD,  2, 1, 8},
        {"divpd",  OP_VDIVPD,  3, 1, 8},
        {"paddd",  OP_VPADDD,  0, 0, 4},
        {"psubd",  OP_VPSUBD,  1, 0, 4},
        {"pmulld", OP_VPMULLD, 2, 0, 4},
        {"andps",  OP_VANDPS,  4, 0, 4},
        {"orps",   OP_VORPS,   5, 0, 4},
        {"xorps",  OP_VXORPS,  6, 0, 4},
    };
    for (size_t k = 0; k < sizeof(binops) / sizeof(binops[0]); k++) {
        if (strcmp(suf, binops[k].suffix) != 0) continue;
        if (!arg0_ref || !arg1_ref) return false;
        int a_addr = lower_expr(c, arena, arg0_ref);
        int b_addr = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_hw128(c)) {
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_simd_emit_vload(c, va, a_addr);
            zc_simd_emit_vload(c, vb, b_addr);
            zc_simd_emit_vbin(c, binops[k].vop, vr, va, vb);
            zc_simd_emit_vstore(c, res_addr, vr);
        } else {
            size_t esz = binops[k].esz;
            size_t lanes = 16 / esz;
            for (size_t i = 0; i < lanes; i++) {
                int la = zc_simd_lane_addr(c, a_addr, i * esz);
                int lb = zc_simd_lane_addr(c, b_addr, i * esz);
                int lr = zc_simd_lane_addr(c, res_addr, i * esz);
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                if (binops[k].is_fp) {
                    if (esz == 4) { ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb); }
                    else          { ir_emit_vloadsd(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadsd(c, (uint32_t)vb, (uint32_t)lb); }
                    if (binops[k].alu == 0)      ir_emit_fadd(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                    else if (binops[k].alu == 1) ir_emit_fsub(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                    else if (binops[k].alu == 2) ir_emit_fmul(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                    else                         ir_emit_fdiv(c, (uint32_t)vr, (uint32_t)va, (uint32_t)vb);
                    if (esz == 4) ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)vr);
                    else          ir_emit_vstoresd(c, (uint32_t)lr, (uint32_t)vr);
                } else {
                    ir_emit_load_32(c, va, la);
                    ir_emit_load_32(c, vb, lb);
                    if (binops[k].alu == 0)      ir_emit_add(c, vr, va, vb);
                    else if (binops[k].alu == 1) ir_emit_sub(c, vr, va, vb);
                    else if (binops[k].alu == 2) ir_emit_mul(c, vr, va, vb);
                    else if (binops[k].alu == 4) ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                    else if (binops[k].alu == 5) ir_emit(c, (Instr){.op = OP_OR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                    else if (binops[k].alu == 6) ir_emit(c, (Instr){.op = OP_XOR, .temp_dest = (uint32_t)vr, .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                    ir_emit_store_32(c, vr, lr);
                }
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    /* 2. Unaligned 128-bit load & store:
     *    vload128(proto_unused, ptr) -> vector value
     *    vstore128(ptr, vec)         -> 0 */
    if (strcmp(suf, "load128") == 0) {
        if (!arg1_ref) return false;
        int ptr_temp = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        zc_simd_copy128(c, res_addr, ptr_temp);
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }
    if (strcmp(suf, "store128") == 0) {
        if (!arg0_ref || !arg1_ref) return false;
        int ptr_temp = lower_expr(c, arena, arg0_ref);
        int vec_addr = lower_expr(c, arena, arg1_ref);
        zc_simd_copy128(c, ptr_temp, vec_addr);
        int z = zc_alloc_temp(c);
        ir_emit_mov_imm(c, z, 0);
        c->last_expr_is_float = 0;
        *out_temp = z;
        return true;
    }

    /* 3. Scalar-to-vector broadcasts: vset1_*(proto_unused, scalar) */
    if (strncmp(suf, "set1_", 5) == 0) {
        if (!arg1_ref) return false;
        const char *ts = suf + 5;
        bool is_fp = (strcmp(ts, "ps") == 0 || strcmp(ts, "pd") == 0);
        size_t esz = 4;
        if (strcmp(ts, "pd") == 0 || strcmp(ts, "epi64") == 0) esz = 8;
        else if (strcmp(ts, "epi16") == 0) esz = 2;
        else if (strcmp(ts, "epi8") == 0) esz = 1;
        else if (strcmp(ts, "ps") != 0 && strcmp(ts, "epi32") != 0) return false;

        c->last_expr_is_float = 0;
        int val = lower_expr(c, arena, arg1_ref);
        bool val_flt = (c->last_expr_is_float != 0);
        if (is_fp && !val_flt) {
            int tf = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)tf, (uint32_t)val);
            val = tf;
        } else if (!is_fp && val_flt) {
            int ti = zc_alloc_temp(c);
            ir_emit_cvttsd2si(c, (uint32_t)ti, (uint32_t)val);
            val = ti;
        }
        int res_addr = zc_simd_alloc_res128(c, NULL);
        size_t lanes = 16 / esz;
        for (size_t i = 0; i < lanes; i++) {
            int la = zc_simd_lane_addr(c, res_addr, i * esz);
            if (is_fp) {
                if (esz == 4) ir_emit_vstoress(c, (uint32_t)la, (uint32_t)val);
                else          ir_emit_vstoresd(c, (uint32_t)la, (uint32_t)val);
            } else if (esz == 1) {
                ir_emit_store_byte(c, val, la);
            } else if (esz == 2) {
                ir_emit_store_16(c, val, la);
            } else if (esz == 4) {
                ir_emit_store_32(c, val, la);
            } else {
                ir_emit_store_64(c, val, la);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    /* 4. Per-lane min/max: vmin/vmax_{ps,pd,epi32,epu32} */
    if (strncmp(suf, "min_", 4) == 0 || strncmp(suf, "max_", 4) == 0) {
        if (!arg0_ref || !arg1_ref) return false;
        bool is_max = (suf[1] == 'a');
        const char *ts = suf + 4;
        bool is_fp = (strcmp(ts, "ps") == 0 || strcmp(ts, "pd") == 0);
        bool is_u32 = (strcmp(ts, "epu32") == 0);
        if (!is_fp && !is_u32 && strcmp(ts, "epi32") != 0) return false;
        size_t esz = (strcmp(ts, "pd") == 0) ? 8 : 4;
        size_t lanes = 16 / esz;
        int cond = (is_fp || !is_u32) ? (is_max ? CC_GT : CC_LT)
                                      : (is_max ? CC_FLT_GT : CC_FLT_LT);
        int a_addr = lower_expr(c, arena, arg0_ref);
        int b_addr = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        for (size_t i = 0; i < lanes; i++) {
            int la = zc_simd_lane_addr(c, a_addr, i * esz);
            int lb = zc_simd_lane_addr(c, b_addr, i * esz);
            int lr = zc_simd_lane_addr(c, res_addr, i * esz);
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
            if (is_fp) {
                if (esz == 4) { ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb); }
                else          { ir_emit_vloadsd(c, (uint32_t)va, (uint32_t)la); ir_emit_vloadsd(c, (uint32_t)vb, (uint32_t)lb); }
            } else {
                ir_emit_load_32(c, va, la);
                ir_emit_load_32(c, vb, lb);
                if (is_u32) { ir_emit_zext(c, va, 4); ir_emit_zext(c, vb, 4); }
                else        { ir_emit_sext(c, va, 4); ir_emit_sext(c, vb, 4); }
            }
            ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
            int cc = zc_alloc_temp(c);
            ir_emit_cond_set(c, (uint32_t)cc, cond);
            uint16_t l_sel_b = (uint16_t)c->ir_next_label++;
            uint16_t l_done = (uint16_t)c->ir_next_label++;
            int vr = zc_alloc_temp(c);
            ir_emit_je(c, (uint32_t)cc, l_sel_b);
            if (is_fp) ir_emit_fmov(c, (uint32_t)vr, (uint32_t)va);
            else       ir_emit_mov_reg(c, vr, va);
            ir_emit_jmp(c, l_done);
            ir_emit_label(c, l_sel_b);
            if (is_fp) ir_emit_fmov(c, (uint32_t)vr, (uint32_t)vb);
            else       ir_emit_mov_reg(c, vr, vb);
            ir_emit_label(c, l_done);
            if (is_fp) {
                if (esz == 4) ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)vr);
                else          ir_emit_vstoresd(c, (uint32_t)lr, (uint32_t)vr);
            } else {
                ir_emit_store_32(c, vr, lr);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    /* 5. Packed float/int32 absolute value: vabs_ps / vabs_epi32 */
    if (strcmp(suf, "abs_ps") == 0 || strcmp(suf, "abs_epi32") == 0) {
        if (!arg0_ref) return false;
        bool is_fp = (strcmp(suf, "abs_ps") == 0);
        int a_addr = lower_expr(c, arena, arg0_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        for (size_t i = 0; i < 4; i++) {
            int la = zc_simd_lane_addr(c, a_addr, i * 4);
            int lr = zc_simd_lane_addr(c, res_addr, i * 4);
            int va = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            ir_emit_load_32(c, va, la);
            if (is_fp) {
                int mask = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask, 0x7FFFFFFF);
                ir_emit_and(c, vr, va, mask);
            } else {
                ir_emit_sext(c, va, 4);
                int zero = zc_alloc_temp(c);
                ir_emit_mov_imm(c, zero, 0);
                ir_emit_cmp(c, va, zero);
                int cc = zc_alloc_temp(c);
                ir_emit_cond_set(c, (uint32_t)cc, CC_LT);
                uint16_t l_pos = (uint16_t)c->ir_next_label++;
                uint16_t l_done = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cc, l_pos);
                ir_emit_neg(c, vr, va);
                ir_emit_jmp(c, l_done);
                ir_emit_label(c, l_pos);
                ir_emit_mov_reg(c, vr, va);
                ir_emit_label(c, l_done);
            }
            ir_emit_store_32(c, vr, lr);
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    /* 6. Fused/packed multiply-accumulate & subtract: vmla/vmls_{ps,epi32}(acc, a, b) */
    if (strcmp(suf, "mla_ps") == 0 || strcmp(suf, "mls_ps") == 0 ||
        strcmp(suf, "mla_epi32") == 0 || strcmp(suf, "mls_epi32") == 0) {
        if (!arg0_ref || !arg1_ref || !arg2_ref) return false;
        bool is_sub = (suf[2] == 's');
        bool is_int = (strstr(suf, "epi32") != NULL);
        int acc_addr = lower_expr(c, arena, arg0_ref);
        int a_addr = lower_expr(c, arena, arg1_ref);
        int b_addr = lower_expr(c, arena, arg2_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_hw128(c)) {
            int vacc = zc_alloc_temp(c), va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
            int vp = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_simd_emit_vload(c, vacc, acc_addr);
            zc_simd_emit_vload(c, va, a_addr);
            zc_simd_emit_vload(c, vb, b_addr);
            zc_simd_emit_vbin(c, is_int ? OP_VPMULLD : OP_VMULPS, vp, va, vb);
            Opcode aop = is_int ? (is_sub ? OP_VPSUBD : OP_VPADDD)
                                : (is_sub ? OP_VSUBPS : OP_VADDPS);
            zc_simd_emit_vbin(c, aop, vr, vacc, vp);
            zc_simd_emit_vstore(c, res_addr, vr);
        } else {
            for (size_t i = 0; i < 4; i++) {
                int lacc = zc_simd_lane_addr(c, acc_addr, i * 4);
                int la = zc_simd_lane_addr(c, a_addr, i * 4);
                int lb = zc_simd_lane_addr(c, b_addr, i * 4);
                int lr = zc_simd_lane_addr(c, res_addr, i * 4);
                int vacc = zc_alloc_temp(c), va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                int vp = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                if (is_int) {
                    ir_emit_load_32(c, vacc, lacc);
                    ir_emit_load_32(c, va, la);
                    ir_emit_load_32(c, vb, lb);
                    ir_emit_mul(c, vp, va, vb);
                    if (is_sub) ir_emit_sub(c, vr, vacc, vp);
                    else        ir_emit_add(c, vr, vacc, vp);
                    ir_emit_store_32(c, vr, lr);
                } else {
                    ir_emit_vloadss(c, (uint32_t)vacc, (uint32_t)lacc);
                    ir_emit_vloadss(c, (uint32_t)va, (uint32_t)la);
                    ir_emit_vloadss(c, (uint32_t)vb, (uint32_t)lb);
                    ir_emit_fmul(c, (uint32_t)vp, (uint32_t)va, (uint32_t)vb);
                    if (is_sub) ir_emit_fsub(c, (uint32_t)vr, (uint32_t)vacc, (uint32_t)vp);
                    else        ir_emit_fadd(c, (uint32_t)vr, (uint32_t)vacc, (uint32_t)vp);
                    ir_emit_vstoress(c, (uint32_t)lr, (uint32_t)vr);
                }
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    /* 7. Lane insertion: vset_lane_{f32,f64,epi32}(vec, val, idx) */
    if (strncmp(suf, "set_lane_", 9) == 0) {
        if (!arg0_ref || !arg1_ref || !arg2_ref) return false;
        const char *ts = suf + 9;
        bool is_fp = (strcmp(ts, "f32") == 0 || strcmp(ts, "f64") == 0);
        if (!is_fp && strcmp(ts, "epi32") != 0) return false;
        size_t esz = (strcmp(ts, "f64") == 0) ? 8 : 4;
        size_t lanes = 16 / esz;

        int vec_addr = lower_expr(c, arena, arg0_ref);
        c->last_expr_is_float = 0;
        int val = lower_expr(c, arena, arg1_ref);
        bool val_flt = (c->last_expr_is_float != 0);
        if (is_fp && !val_flt) {
            int tf = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)tf, (uint32_t)val);
            val = tf;
        } else if (!is_fp && val_flt) {
            int ti = zc_alloc_temp(c);
            ir_emit_cvttsd2si(c, (uint32_t)ti, (uint32_t)val);
            val = ti;
        }

        int res_addr = zc_simd_alloc_res128(c, NULL);
        zc_simd_copy128(c, res_addr, vec_addr);

        int64_t idx_const = 0;
        int dst_lane;
        if (zc_ast_const_eval(c, arena, arg2_ref, &idx_const)) {
            size_t lane = (size_t)idx_const & (lanes - 1);
            dst_lane = zc_simd_lane_addr(c, res_addr, lane * esz);
        } else {
            int idx_t = lower_expr(c, arena, arg2_ref);
            int mask = zc_alloc_temp(c), masked = zc_alloc_temp(c);
            ir_emit_mov_imm(c, mask, (int64_t)(lanes - 1));
            ir_emit_and(c, masked, idx_t, mask);
            int sz_t = zc_alloc_temp(c), off_t = zc_alloc_temp(c);
            ir_emit_mov_imm(c, sz_t, (int64_t)esz);
            ir_emit_mul(c, off_t, masked, sz_t);
            dst_lane = zc_alloc_temp(c);
            ir_emit_add(c, dst_lane, res_addr, off_t);
        }

        if (is_fp) {
            if (esz == 4) ir_emit_vstoress(c, (uint32_t)dst_lane, (uint32_t)val);
            else          ir_emit_vstoresd(c, (uint32_t)dst_lane, (uint32_t)val);
        } else {
            ir_emit_store_32(c, val, dst_lane);
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    return false;
}

/* Lower `__builtin_zerocc_sve_*` ARM SVE / SVE2 intrinsic helpers. */
static bool zc_lower_sve_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                 ZeroCCASTNode *node, const char *sve_name,
                                 int *out_temp) {
    ASTRef arg0_ref = node->right;
    ZeroCCASTNode *arg0 = arg0_ref ? zero_cc_ast_get(arena, arg0_ref) : NULL;
    ASTRef arg1_ref = arg0 ? arg0->next : AST_NULL;
    ZeroCCASTNode *arg1 = arg1_ref ? zero_cc_ast_get(arena, arg1_ref) : NULL;
    ASTRef arg2_ref = arg1 ? arg1->next : AST_NULL;
    ZeroCCASTNode *arg2 = arg2_ref ? zero_cc_ast_get(arena, arg2_ref) : NULL;
    ASTRef arg3_ref = arg2 ? arg2->next : AST_NULL;
    ZeroCCASTNode *arg3 = arg3_ref ? zero_cc_ast_get(arena, arg3_ref) : NULL;
    ASTRef arg4_ref = arg3 ? arg3->next : AST_NULL;
    ZeroCCASTNode *arg4 = arg4_ref ? zero_cc_ast_get(arena, arg4_ref) : NULL;
    ASTRef arg5_ref = arg4 ? arg4->next : AST_NULL;
    (void)arg3; (void)arg4; (void)arg5_ref;
    c->last_expr_is_float = 0;

    if (strcmp(sve_name, "ptrue") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int pat = (int)zc_get_imm_arg(arena, arg1_ref);
        int res = zc_alloc_temp(c);
        int elem_size = 1 << esz;
        int max_lanes = 64 / elem_size;
        int count = max_lanes;
        if (pat >= 1 && pat <= 8) count = pat;
        else if (pat == 9) count = 16;
        else if (pat == 10) count = 32;
        else if (pat == 11) count = 64;
        if (count > max_lanes) count = max_lanes;
        uint64_t mask = (count >= 64) ? ~0ULL : ((1ULL << count) - 1);
        ir_emit_mov_imm(c, res, (int64_t)mask);
        if (zc_simd_has_sve(c)) {
            ir_emit(c, (Instr){.op = OP_SVE_PTRUE, .temp_dest = TEMP_NONE,
                               .imm = esz, .imm64 = pat,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        }
        *out_temp = res;
        return true;
    }

    if (strcmp(sve_name, "pfalse") == 0) {
        int res = zc_alloc_temp(c);
        ir_emit_mov_imm(c, res, 0);
        if (zc_simd_has_sve(c)) {
            ir_emit(c, (Instr){.op = OP_SVE_PFALSE, .temp_dest = TEMP_NONE,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        }
        *out_temp = res;
        return true;
    }

    if (strcmp(sve_name, "cnt") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int res = zc_alloc_temp(c);
        if (zc_simd_has_sve(c)) {
            ir_emit(c, (Instr){.op = OP_SVE_CNT, .temp_dest = (uint32_t)res,
                               .imm = esz, .imm64 = 31,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            ir_emit_mov_imm(c, res, lanes);
        }
        *out_temp = res;
        return true;
    }

    if (strcmp(sve_name, "whilelt") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int t1 = lower_expr(c, arena, arg1_ref);
        int t2 = lower_expr(c, arena, arg2_ref);
        int res = zc_alloc_temp(c);
        int max_lanes = 64 / (1 << esz);
        ir_emit_mov_imm(c, res, 0);
        for (int i = 0; i < max_lanes; i++) {
            int t_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, t_i, i);
            int sum_t = zc_alloc_temp(c);
            ir_emit_add(c, sum_t, t1, t_i);
            ir_emit_cmp(c, sum_t, t2);
            int c_lt = zc_alloc_temp(c);
            ir_emit_cond_set(c, c_lt, CC_LT);
            uint16_t l_skip = (uint16_t)c->ir_next_label++;
            ir_emit_je(c, (uint32_t)c_lt, l_skip);
            int bit_val = zc_alloc_temp(c);
            ir_emit_mov_imm(c, bit_val, 1ULL << i);
            ir_emit_or(c, res, res, bit_val);
            ir_emit_label(c, l_skip);
        }
        if (zc_simd_has_sve(c)) {
            ir_emit(c, (Instr){.op = OP_SVE_WHILELT, .temp_dest = TEMP_NONE,
                               .temp_src1 = (uint32_t)t1, .temp_src2 = (uint32_t)t2,
                               .imm = esz, .flags_out = 0xFF, .flags_in = 0xFF});
        }
        *out_temp = res;
        return true;
    }

    if (strcmp(sve_name, "ld1") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int pg = lower_expr(c, arena, arg1_ref);
        int ptr = lower_expr(c, arena, arg2_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);
        if (zc_simd_has_sve(c)) {
            (void)pg;
            int vt = zc_alloc_temp(c);
            zc_sve_emit_load(c, vt, ptr, esz);
            zc_sve_emit_store(c, res_addr, vt, esz);
        } else {
            int zt = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zt, 0);
            for (size_t i = 0; i < 16; i++) {
                int da = zc_simd_lane_addr(c, res_addr, i * 4);
                ir_emit_store_32(c, zt, da);
            }
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int mask_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask_t, 1ULL << i);
                int bit_t = zc_alloc_temp(c);
                ir_emit_and(c, bit_t, pg, mask_t);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)bit_t, l_skip);
                int src_elem = zc_simd_lane_addr(c, ptr, (size_t)i * elem_size);
                int dst_elem = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                int val_t = zc_alloc_temp(c);
                if (elem_size == 1) { ir_emit_load_byte(c, val_t, src_elem); ir_emit_store_byte(c, val_t, dst_elem); }
                else if (elem_size == 2) { ir_emit_load_16(c, val_t, src_elem); ir_emit_store_16(c, val_t, dst_elem); }
                else if (elem_size == 4) { ir_emit_load_32(c, val_t, src_elem); ir_emit_store_32(c, val_t, dst_elem); }
                else { ir_emit_deref(c, val_t, src_elem); ir_emit_store_64(c, val_t, dst_elem); }
                ir_emit_label(c, l_skip);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(sve_name, "st1") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int pg = lower_expr(c, arena, arg1_ref);
        int ptr = lower_expr(c, arena, arg2_ref);
        int data_addr = lower_expr(c, arena, arg3_ref);
        if (zc_simd_has_sve(c)) {
            (void)pg;
            int vt = zc_alloc_temp(c);
            zc_sve_emit_load(c, vt, data_addr, esz);
            zc_sve_emit_store(c, ptr, vt, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int mask_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask_t, 1ULL << i);
                int bit_t = zc_alloc_temp(c);
                ir_emit_and(c, bit_t, pg, mask_t);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)bit_t, l_skip);
                int src_elem = zc_simd_lane_addr(c, data_addr, (size_t)i * elem_size);
                int dst_elem = zc_simd_lane_addr(c, ptr, (size_t)i * elem_size);
                int val_t = zc_alloc_temp(c);
                if (elem_size == 1) { ir_emit_load_byte(c, val_t, src_elem); ir_emit_store_byte(c, val_t, dst_elem); }
                else if (elem_size == 2) { ir_emit_load_16(c, val_t, src_elem); ir_emit_store_16(c, val_t, dst_elem); }
                else if (elem_size == 4) { ir_emit_load_32(c, val_t, src_elem); ir_emit_store_32(c, val_t, dst_elem); }
                else { ir_emit_deref(c, val_t, src_elem); ir_emit_store_64(c, val_t, dst_elem); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = 0;
        return true;
    }

    if (strcmp(sve_name, "dup") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int val_t = lower_expr(c, arena, arg2_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);
        int elem_size = 1 << esz;
        int lanes = 64 / elem_size;
        for (int i = 0; i < lanes; i++) {
            int dst_elem = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
            if (elem_size == 1) ir_emit_store_byte(c, val_t, dst_elem);
            else if (elem_size == 2) ir_emit_store_16(c, val_t, dst_elem);
            else if (elem_size == 4) ir_emit_store_32(c, val_t, dst_elem);
            else ir_emit_store_64(c, val_t, dst_elem);
        }
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(sve_name, "dup_f32") == 0) {
        int val_t = lower_expr(c, arena, arg0_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);
        for (int i = 0; i < 16; i++) {
            int dst_elem = zc_simd_lane_addr(c, res_addr, (size_t)i * 4);
            ir_emit_vstoress(c, dst_elem, val_t);
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(sve_name, "dup_f64") == 0) {
        int val_t = lower_expr(c, arena, arg0_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);
        for (int i = 0; i < 8; i++) {
            int dst_elem = zc_simd_lane_addr(c, res_addr, (size_t)i * 8);
            ir_emit_vstoresd(c, dst_elem, val_t);
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(sve_name, "add") == 0 || strcmp(sve_name, "sub") == 0 ||
        strcmp(sve_name, "mul") == 0 || strcmp(sve_name, "div") == 0) {
        Opcode op = (strcmp(sve_name, "add") == 0) ? OP_SVE_ADD :
                    (strcmp(sve_name, "sub") == 0) ? OP_SVE_SUB :
                    (strcmp(sve_name, "mul") == 0) ? OP_SVE_MUL : OP_SVE_DIV;
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int is_fp = (int)zc_get_imm_arg(arena, arg1_ref);
        int merge_mode = (int)zc_get_imm_arg(arena, arg2_ref);
        int pg = lower_expr(c, arena, arg3_ref);
        int a_addr = lower_expr(c, arena, arg4_ref);
        int b_addr = lower_expr(c, arena, arg5_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);

        if (zc_simd_has_sve(c)) {
            (void)merge_mode; (void)pg;
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_sve_emit_load(c, va, a_addr, esz);
            zc_sve_emit_load(c, vb, b_addr, esz);
            ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .imm = esz, .imm64 = is_fp,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_sve_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            if (merge_mode == 1) {
                for (size_t i = 0; i < 16; i++) {
                    int sa = zc_simd_lane_addr(c, a_addr, i * 4);
                    int da = zc_simd_lane_addr(c, res_addr, i * 4);
                    int tmp = zc_alloc_temp(c);
                    ir_emit_load_32(c, tmp, sa);
                    ir_emit_store_32(c, tmp, da);
                }
            } else {
                int zt = zc_alloc_temp(c);
                ir_emit_mov_imm(c, zt, 0);
                for (size_t i = 0; i < 16; i++) {
                    int da = zc_simd_lane_addr(c, res_addr, i * 4);
                    ir_emit_store_32(c, zt, da);
                }
            }
            int alu = (op == OP_SVE_ADD) ? 0 : (op == OP_SVE_SUB) ? 1 : (op == OP_SVE_MUL) ? 2 : 3;
            for (int i = 0; i < lanes; i++) {
                int mask_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask_t, 1ULL << i);
                int bit_t = zc_alloc_temp(c);
                ir_emit_and(c, bit_t, pg, mask_t);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)bit_t, l_skip);
                int la = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                int lb = zc_simd_lane_addr(c, b_addr, (size_t)i * elem_size);
                int lr = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                if (is_fp) {
                    if (elem_size == 4) {
                        ir_emit_vloadss(c, va, la); ir_emit_vloadss(c, vb, lb);
                        if (alu == 0)      ir_emit_fadd(c, vr, va, vb);
                        else if (alu == 1) ir_emit_fsub(c, vr, va, vb);
                        else if (alu == 2) ir_emit_fmul(c, vr, va, vb);
                        else               ir_emit_fdiv(c, vr, va, vb);
                        ir_emit_vstoress(c, lr, vr);
                    } else {
                        ir_emit_vloadsd(c, va, la); ir_emit_vloadsd(c, vb, lb);
                        if (alu == 0)      ir_emit_fadd(c, vr, va, vb);
                        else if (alu == 1) ir_emit_fsub(c, vr, va, vb);
                        else if (alu == 2) ir_emit_fmul(c, vr, va, vb);
                        else               ir_emit_fdiv(c, vr, va, vb);
                        ir_emit_vstoresd(c, lr, vr);
                    }
                } else {
                    if (elem_size == 1) {
                        ir_emit_load_byte(c, va, la); ir_emit_load_byte(c, vb, lb);
                        if (alu == 0) ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                        else          ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 1, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_byte(c, vr, lr);
                    } else if (elem_size == 2) {
                        ir_emit_load_16(c, va, la); ir_emit_load_16(c, vb, lb);
                        if (alu == 0)      ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        else               ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 2, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_16(c, vr, lr);
                    } else if (elem_size == 4) {
                        ir_emit_load_32(c, va, la); ir_emit_load_32(c, vb, lb);
                        if (alu == 0)      ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else if (alu == 1) ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        else               ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = vr, .temp_src1 = va, .temp_src2 = vb, .width = 4, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit_store_32(c, vr, lr);
                    } else {
                        ir_emit_deref(c, va, la); ir_emit_deref(c, vb, lb);
                        if (alu == 0)      ir_emit_add(c, vr, va, vb);
                        else if (alu == 1) ir_emit_sub(c, vr, va, vb);
                        else               ir_emit_mul(c, vr, va, vb);
                        ir_emit_store_64(c, vr, lr);
                    }
                }
                ir_emit_label(c, l_skip);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(sve_name, "and") == 0 || strcmp(sve_name, "orr") == 0 || strcmp(sve_name, "eor") == 0) {
        Opcode op = (strcmp(sve_name, "and") == 0) ? OP_SVE_AND :
                    (strcmp(sve_name, "orr") == 0) ? OP_SVE_ORR : OP_SVE_EOR;
        int a_addr = lower_expr(c, arena, arg0_ref);
        int b_addr = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);
        if (zc_simd_has_sve(c)) {
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_sve_emit_load(c, va, a_addr, 2);
            zc_sve_emit_load(c, vb, b_addr, 2);
            ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_sve_emit_store(c, res_addr, vr, 2);
        } else {
            for (size_t i = 0; i < 16; i++) {
                int sa = zc_simd_lane_addr(c, a_addr, i * 4);
                int sb = zc_simd_lane_addr(c, b_addr, i * 4);
                int sr = zc_simd_lane_addr(c, res_addr, i * 4);
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                ir_emit_load_32(c, va, sa);
                ir_emit_load_32(c, vb, sb);
                if (op == OP_SVE_AND)      ir_emit_and(c, vr, va, vb);
                else if (op == OP_SVE_ORR) ir_emit_or(c, vr, va, vb);
                else                       ir_emit_xor(c, vr, va, vb);
                ir_emit_store_32(c, vr, sr);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(sve_name, "cmp") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int cond = (int)zc_get_imm_arg(arena, arg1_ref);
        int is_fp = (int)zc_get_imm_arg(arena, arg2_ref);
        int pg = lower_expr(c, arena, arg3_ref);
        int a_addr = lower_expr(c, arena, arg4_ref);
        int b_addr = lower_expr(c, arena, arg5_ref);
        int res = zc_alloc_temp(c);
        if (zc_simd_has_sve(c)) {
            (void)pg;
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
            zc_sve_emit_load(c, va, a_addr, esz);
            zc_sve_emit_load(c, vb, b_addr, esz);
            ir_emit(c, (Instr){.op = OP_SVE_CMP, .temp_dest = (uint32_t)res,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .imm = esz, .imm64 = (int64_t)(cond | (is_fp << 8)),
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            ir_emit_mov_imm(c, res, 0);
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int mask_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask_t, 1ULL << i);
                int bit_t = zc_alloc_temp(c);
                ir_emit_and(c, bit_t, pg, mask_t);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)bit_t, l_skip);
                int la = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                int lb = zc_simd_lane_addr(c, b_addr, (size_t)i * elem_size);
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                int c_res = zc_alloc_temp(c);
                if (is_fp) {
                    if (elem_size == 4) {
                        ir_emit_vloadss(c, va, la); ir_emit_vloadss(c, vb, lb);
                    } else {
                        ir_emit_vloadsd(c, va, la); ir_emit_vloadsd(c, vb, lb);
                    }
                    ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    ir_emit_cond_set(c, c_res, (cond == 0) ? CC_EQ : CC_FLT_GT);
                } else {
                    if (elem_size == 4) {
                        ir_emit_load_32(c, va, la); ir_emit_load_32(c, vb, lb);
                        ir_emit(c, (Instr){.op = OP_SEXT32, .temp_dest = (uint32_t)va, .temp_src1 = (uint32_t)va, .flags_out = 0xFF, .flags_in = 0xFF});
                        ir_emit(c, (Instr){.op = OP_SEXT32, .temp_dest = (uint32_t)vb, .temp_src1 = (uint32_t)vb, .flags_out = 0xFF, .flags_in = 0xFF});
                    } else {
                        ir_emit_deref(c, va, la); ir_emit_deref(c, vb, lb);
                    }
                    ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    ir_emit_cond_set(c, c_res, (cond == 0) ? CC_EQ : CC_GT);
                }
                ir_emit_je(c, (uint32_t)c_res, l_skip);
                ir_emit_or(c, res, res, mask_t);
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = res;
        return true;
    }

    if (strcmp(sve_name, "addv") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int is_fp = (int)zc_get_imm_arg(arena, arg1_ref);
        int pg = lower_expr(c, arena, arg2_ref);
        int a_addr = lower_expr(c, arena, arg3_ref);
        if (zc_simd_has_sve(c)) {
            (void)pg;
            int va = zc_alloc_temp(c);
            int vr = zc_alloc_temp(c);
            zc_sve_emit_load(c, va, a_addr, esz);
            ir_emit(c, (Instr){.op = OP_SVE_ADDV, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .imm = esz, .imm64 = is_fp,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            if (is_fp) {
                c->last_expr_is_float = 1;
                *out_temp = vr;
            } else {
                int slot = zc_simd_alloc_res128(c, NULL);
                ir_emit_vstoresd(c, slot, vr);
                int gpr_res = zc_alloc_temp(c);
                if (esz == 3) ir_emit_deref(c, gpr_res, slot);
                else ir_emit_load_32(c, gpr_res, slot);
                *out_temp = gpr_res;
            }
        } else {
            int res = zc_alloc_temp(c);
            ir_emit_mov_imm(c, res, 0);
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int mask_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask_t, 1ULL << i);
                int bit_t = zc_alloc_temp(c);
                ir_emit_and(c, bit_t, pg, mask_t);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)bit_t, l_skip);
                int la = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                int val_t = zc_alloc_temp(c);
                if (elem_size == 4) {
                    ir_emit_load_32(c, val_t, la);
                    ir_emit(c, (Instr){.op = OP_SEXT32, .temp_dest = (uint32_t)val_t, .temp_src1 = (uint32_t)val_t, .flags_out = 0xFF, .flags_in = 0xFF});
                } else {
                    ir_emit_deref(c, val_t, la);
                }
                ir_emit_add(c, res, res, val_t);
                ir_emit_label(c, l_skip);
            }
            *out_temp = res;
        }
        return true;
    }

    if (strcmp(sve_name, "addv_f32") == 0) {
        int pg = lower_expr(c, arena, arg0_ref);
        int a_addr = lower_expr(c, arena, arg1_ref);
        int res = zc_alloc_temp(c);
        if (zc_simd_has_sve(c)) {
            (void)pg;
            int va = zc_alloc_temp(c);
            zc_sve_emit_load(c, va, a_addr, 2);
            ir_emit(c, (Instr){.op = OP_SVE_ADDV, .temp_dest = (uint32_t)res,
                               .temp_src1 = (uint32_t)va, .imm = 2, .imm64 = 1,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int zero_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_addr = zc_simd_alloc_res128(c, NULL);
            ir_emit_store_32(c, zero_i, zero_addr);
            ir_emit_vloadss(c, res, zero_addr);
            for (int i = 0; i < 16; i++) {
                int mask_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, mask_t, 1ULL << i);
                int bit_t = zc_alloc_temp(c);
                ir_emit_and(c, bit_t, pg, mask_t);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)bit_t, l_skip);
                int la = zc_simd_lane_addr(c, a_addr, (size_t)i * 4);
                int val_t = zc_alloc_temp(c);
                ir_emit_vloadss(c, val_t, la);
                ir_emit_fadd(c, res, res, val_t);
                ir_emit_label(c, l_skip);
            }
        }
        c->last_expr_is_float = 1;
        *out_temp = res;
        return true;
    }

    if (strcmp(sve_name, "qadd") == 0 || strcmp(sve_name, "qsub") == 0) {
        Opcode op = (strcmp(sve_name, "qadd") == 0) ? OP_SVE_QADD : OP_SVE_QSUB;
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int a_addr = lower_expr(c, arena, arg1_ref);
        int b_addr = lower_expr(c, arena, arg2_ref);
        int res_addr = zc_simd_alloc_res512(c, NULL);
        if (zc_simd_has_sve(c)) {
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_sve_emit_load(c, va, a_addr, esz);
            zc_sve_emit_load(c, vb, b_addr, esz);
            ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .imm = esz, .flags_out = 0xFF, .flags_in = 0xFF});
            zc_sve_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 64 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int la = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                int lb = zc_simd_lane_addr(c, b_addr, (size_t)i * elem_size);
                int lr = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                ir_emit_load_32(c, va, la);
                ir_emit_load_32(c, vb, lb);
                ir_emit(c, (Instr){.op = OP_SEXT32, .temp_dest = (uint32_t)va, .temp_src1 = (uint32_t)va, .flags_out = 0xFF, .flags_in = 0xFF});
                ir_emit(c, (Instr){.op = OP_SEXT32, .temp_dest = (uint32_t)vb, .temp_src1 = (uint32_t)vb, .flags_out = 0xFF, .flags_in = 0xFF});
                int vres = zc_alloc_temp(c);
                if (op == OP_SVE_QADD) ir_emit_add(c, vres, va, vb);
                else                   ir_emit_sub(c, vres, va, vb);

                int max_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, max_t, 0x7FFFFFFFLL);
                ir_emit_cmp(c, vres, max_t);
                int c_gt = zc_alloc_temp(c);
                ir_emit_cond_set(c, c_gt, CC_GT);
                uint16_t l_not_max = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)c_gt, l_not_max);
                ir_emit_mov_imm(c, vres, 0x7FFFFFFFLL);
                ir_emit_label(c, l_not_max);

                int min_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, min_t, -0x80000000LL);
                ir_emit_cmp(c, vres, min_t);
                int c_lt = zc_alloc_temp(c);
                ir_emit_cond_set(c, c_lt, CC_LT);
                uint16_t l_not_min = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)c_lt, l_not_min);
                ir_emit_mov_imm(c, vres, -0x80000000LL);
                ir_emit_label(c, l_not_min);

                ir_emit_store_32(c, vres, lr);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    return false;
}

/* Lower `__builtin_zerocc_rvv_*` RISC-V Vector Extension (RVV 1.0) intrinsic helpers. */
static bool zc_lower_rvv_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                 ZeroCCASTNode *node, const char *rvv_name,
                                 int *out_temp) {
    ASTRef arg0_ref = node->right;
    ZeroCCASTNode *arg0 = arg0_ref ? zero_cc_ast_get(arena, arg0_ref) : NULL;
    ASTRef arg1_ref = arg0 ? arg0->next : AST_NULL;
    ZeroCCASTNode *arg1 = arg1_ref ? zero_cc_ast_get(arena, arg1_ref) : NULL;
    ASTRef arg2_ref = arg1 ? arg1->next : AST_NULL;
    ZeroCCASTNode *arg2 = arg2_ref ? zero_cc_ast_get(arena, arg2_ref) : NULL;
    ASTRef arg3_ref = arg2 ? arg2->next : AST_NULL;
    ZeroCCASTNode *arg3 = arg3_ref ? zero_cc_ast_get(arena, arg3_ref) : NULL;
    ASTRef arg4_ref = arg3 ? arg3->next : AST_NULL;
    ZeroCCASTNode *arg4 = arg4_ref ? zero_cc_ast_get(arena, arg4_ref) : NULL;
    ASTRef arg5_ref = arg4 ? arg4->next : AST_NULL;
    (void)arg3; (void)arg4; (void)arg5_ref;
    c->last_expr_is_float = 0;

    if (strcmp(rvv_name, "vsetvl") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int avl = lower_expr(c, arena, arg1_ref);
        int res = zc_alloc_temp(c);
        if (zc_simd_has_rvv(c)) {
            ir_emit(c, (Instr){.op = OP_RVV_VSETVLI, .temp_dest = (uint32_t)res,
                               .temp_src1 = (uint32_t)avl, .imm = esz, .imm64 = 0,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int elem_size = 1 << esz;
            int max_lanes = 16 / elem_size;
            int max_t = zc_alloc_temp(c);
            ir_emit_mov_imm(c, max_t, max_lanes);
            ir_emit_cmp(c, avl, max_t);
            int cond = zc_alloc_temp(c);
            ir_emit_cond_set(c, cond, CC_LE);
            ir_emit_mov_reg(c, res, avl);
            uint16_t l_skip = (uint16_t)c->ir_next_label++;
            ir_emit_je(c, (uint32_t)cond, l_skip);
            ir_emit_mov_reg(c, res, max_t);
            ir_emit_label(c, l_skip);
        }
        *out_temp = res;
        return true;
    }

    if (strcmp(rvv_name, "vle") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int base = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int vt = zc_alloc_temp(c);
            zc_rvv_emit_load(c, vt, base, esz);
            zc_rvv_emit_store(c, res_addr, vt, esz);
        } else {
            int vl = lower_expr(c, arena, arg2_ref);
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int idx_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, idx_t, i);
                ir_emit_cmp(c, idx_t, vl);
                int cond = zc_alloc_temp(c);
                ir_emit_cond_set(c, cond, CC_LT);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cond, l_skip);
                int sa = zc_simd_lane_addr(c, base, (size_t)i * elem_size);
                int da = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                int tmp = zc_alloc_temp(c);
                if (elem_size == 1)      { ir_emit_load_byte(c, tmp, sa); ir_emit_store_byte(c, tmp, da); }
                else if (elem_size == 2) { ir_emit_load_16(c, tmp, sa); ir_emit_store_16(c, tmp, da); }
                else if (elem_size == 4) { ir_emit_load_32(c, tmp, sa); ir_emit_store_32(c, tmp, da); }
                else                     { ir_emit_deref(c, tmp, sa); ir_emit_store_64(c, tmp, da); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vse") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int base = lower_expr(c, arena, arg1_ref);
        int val = lower_expr(c, arena, arg2_ref);
        if (zc_simd_has_rvv(c)) {
            int vt = zc_alloc_temp(c);
            zc_rvv_emit_load(c, vt, val, esz);
            zc_rvv_emit_store(c, base, vt, esz);
        } else {
            int vl = lower_expr(c, arena, arg3_ref);
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int idx_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, idx_t, i);
                ir_emit_cmp(c, idx_t, vl);
                int cond = zc_alloc_temp(c);
                ir_emit_cond_set(c, cond, CC_LT);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cond, l_skip);
                int sa = zc_simd_lane_addr(c, val, (size_t)i * elem_size);
                int da = zc_simd_lane_addr(c, base, (size_t)i * elem_size);
                int tmp = zc_alloc_temp(c);
                if (elem_size == 1)      { ir_emit_load_byte(c, tmp, sa); ir_emit_store_byte(c, tmp, da); }
                else if (elem_size == 2) { ir_emit_load_16(c, tmp, sa); ir_emit_store_16(c, tmp, da); }
                else if (elem_size == 4) { ir_emit_load_32(c, tmp, sa); ir_emit_store_32(c, tmp, da); }
                else                     { ir_emit_deref(c, tmp, sa); ir_emit_store_64(c, tmp, da); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = zc_alloc_temp(c);
        return true;
    }

    if (strcmp(rvv_name, "vlse") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int base = lower_expr(c, arena, arg1_ref);
        int stride = lower_expr(c, arena, arg2_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int vr = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op = OP_RVV_VLSE, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)base, .temp_src2 = (uint32_t)stride,
                               .imm = esz, .imm64 = 0,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int vl = lower_expr(c, arena, arg3_ref);
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int idx_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, idx_t, i);
                ir_emit_cmp(c, idx_t, vl);
                int cond = zc_alloc_temp(c);
                ir_emit_cond_set(c, cond, CC_LT);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cond, l_skip);

                int off_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off_t, i);
                int byte_off = zc_alloc_temp(c);
                ir_emit_mul(c, byte_off, off_t, stride);
                int sa = zc_alloc_temp(c);
                ir_emit_add(c, sa, base, byte_off);

                int da = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                int tmp = zc_alloc_temp(c);
                if (elem_size == 1)      { ir_emit_load_byte(c, tmp, sa); ir_emit_store_byte(c, tmp, da); }
                else if (elem_size == 2) { ir_emit_load_16(c, tmp, sa); ir_emit_store_16(c, tmp, da); }
                else if (elem_size == 4) { ir_emit_load_32(c, tmp, sa); ir_emit_store_32(c, tmp, da); }
                else                     { ir_emit_deref(c, tmp, sa); ir_emit_store_64(c, tmp, da); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vsse") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int base = lower_expr(c, arena, arg1_ref);
        int stride = lower_expr(c, arena, arg2_ref);
        int val = lower_expr(c, arena, arg3_ref);
        if (zc_simd_has_rvv(c)) {
            int vt = zc_alloc_temp(c);
            zc_rvv_emit_load(c, vt, val, esz);
            ir_emit(c, (Instr){.op = OP_RVV_VSSE, .temp_dest = (uint32_t)vt,
                               .temp_src1 = (uint32_t)base, .temp_src2 = (uint32_t)stride,
                               .imm = esz, .imm64 = 0,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int vl = lower_expr(c, arena, arg4_ref);
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int idx_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, idx_t, i);
                ir_emit_cmp(c, idx_t, vl);
                int cond = zc_alloc_temp(c);
                ir_emit_cond_set(c, cond, CC_LT);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cond, l_skip);

                int sa = zc_simd_lane_addr(c, val, (size_t)i * elem_size);
                int off_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off_t, i);
                int byte_off = zc_alloc_temp(c);
                ir_emit_mul(c, byte_off, off_t, stride);
                int da = zc_alloc_temp(c);
                ir_emit_add(c, da, base, byte_off);

                int tmp = zc_alloc_temp(c);
                if (elem_size == 1)      { ir_emit_load_byte(c, tmp, sa); ir_emit_store_byte(c, tmp, da); }
                else if (elem_size == 2) { ir_emit_load_16(c, tmp, sa); ir_emit_store_16(c, tmp, da); }
                else if (elem_size == 4) { ir_emit_load_32(c, tmp, sa); ir_emit_store_32(c, tmp, da); }
                else                     { ir_emit_deref(c, tmp, sa); ir_emit_store_64(c, tmp, da); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = zc_alloc_temp(c);
        return true;
    }

    if (strcmp(rvv_name, "vluxei") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int base = lower_expr(c, arena, arg1_ref);
        int bindex = lower_expr(c, arena, arg2_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int vidx = zc_alloc_temp(c);
            zc_rvv_emit_load(c, vidx, bindex, esz);
            int vr = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op = OP_RVV_VLUXEI, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)base, .temp_src2 = (uint32_t)vidx,
                               .imm = esz, .imm64 = 0,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int vl = lower_expr(c, arena, arg3_ref);
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int idx_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, idx_t, i);
                ir_emit_cmp(c, idx_t, vl);
                int cond = zc_alloc_temp(c);
                ir_emit_cond_set(c, cond, CC_LT);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cond, l_skip);

                int idx_addr = zc_simd_lane_addr(c, bindex, (size_t)i * elem_size);
                int byte_off = zc_alloc_temp(c);
                if (elem_size == 4)      ir_emit_load_32(c, byte_off, idx_addr);
                else if (elem_size == 8) ir_emit_deref(c, byte_off, idx_addr);
                else if (elem_size == 2) ir_emit_load_16(c, byte_off, idx_addr);
                else                     ir_emit_load_byte(c, byte_off, idx_addr);

                int sa = zc_alloc_temp(c);
                ir_emit_add(c, sa, base, byte_off);

                int da = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                int tmp = zc_alloc_temp(c);
                if (elem_size == 1)      { ir_emit_load_byte(c, tmp, sa); ir_emit_store_byte(c, tmp, da); }
                else if (elem_size == 2) { ir_emit_load_16(c, tmp, sa); ir_emit_store_16(c, tmp, da); }
                else if (elem_size == 4) { ir_emit_load_32(c, tmp, sa); ir_emit_store_32(c, tmp, da); }
                else                     { ir_emit_deref(c, tmp, sa); ir_emit_store_64(c, tmp, da); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vsuxei") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int base = lower_expr(c, arena, arg1_ref);
        int bindex = lower_expr(c, arena, arg2_ref);
        int val = lower_expr(c, arena, arg3_ref);
        if (zc_simd_has_rvv(c)) {
            int vidx = zc_alloc_temp(c);
            zc_rvv_emit_load(c, vidx, bindex, esz);
            int vt = zc_alloc_temp(c);
            zc_rvv_emit_load(c, vt, val, esz);
            ir_emit(c, (Instr){.op = OP_RVV_VSUXEI, .temp_dest = (uint32_t)vt,
                               .temp_src1 = (uint32_t)base, .temp_src2 = (uint32_t)vidx,
                               .imm = esz, .imm64 = 0,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int vl = lower_expr(c, arena, arg4_ref);
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int idx_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, idx_t, i);
                ir_emit_cmp(c, idx_t, vl);
                int cond = zc_alloc_temp(c);
                ir_emit_cond_set(c, cond, CC_LT);
                uint16_t l_skip = (uint16_t)c->ir_next_label++;
                ir_emit_je(c, (uint32_t)cond, l_skip);

                int sa = zc_simd_lane_addr(c, val, (size_t)i * elem_size);
                int idx_addr = zc_simd_lane_addr(c, bindex, (size_t)i * elem_size);
                int byte_off = zc_alloc_temp(c);
                if (elem_size == 4)      ir_emit_load_32(c, byte_off, idx_addr);
                else if (elem_size == 8) ir_emit_deref(c, byte_off, idx_addr);
                else if (elem_size == 2) ir_emit_load_16(c, byte_off, idx_addr);
                else                     ir_emit_load_byte(c, byte_off, idx_addr);

                int da = zc_alloc_temp(c);
                ir_emit_add(c, da, base, byte_off);

                int tmp = zc_alloc_temp(c);
                if (elem_size == 1)      { ir_emit_load_byte(c, tmp, sa); ir_emit_store_byte(c, tmp, da); }
                else if (elem_size == 2) { ir_emit_load_16(c, tmp, sa); ir_emit_store_16(c, tmp, da); }
                else if (elem_size == 4) { ir_emit_load_32(c, tmp, sa); ir_emit_store_32(c, tmp, da); }
                else                     { ir_emit_deref(c, tmp, sa); ir_emit_store_64(c, tmp, da); }
                ir_emit_label(c, l_skip);
            }
        }
        *out_temp = zc_alloc_temp(c);
        return true;
    }

    if (strcmp(rvv_name, "vadd") == 0 || strcmp(rvv_name, "vsub") == 0 ||
        strcmp(rvv_name, "vmul") == 0 || strcmp(rvv_name, "vdiv") == 0) {
        Opcode op = (strcmp(rvv_name, "vadd") == 0) ? OP_RVV_VADD :
                    (strcmp(rvv_name, "vsub") == 0) ? OP_RVV_VSUB :
                    (strcmp(rvv_name, "vmul") == 0) ? OP_RVV_VMUL : OP_RVV_VDIV;
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int is_fp = (int)zc_get_imm_arg(arena, arg1_ref);
        int a_addr = lower_expr(c, arena, arg2_ref);
        int b_addr = lower_expr(c, arena, arg3_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);

        if (zc_simd_has_rvv(c)) {
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_rvv_emit_load(c, va, a_addr, esz);
            zc_rvv_emit_load(c, vb, b_addr, esz);
            ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .imm = esz, .imm64 = is_fp,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int sa = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                int sb = zc_simd_lane_addr(c, b_addr, (size_t)i * elem_size);
                int sr = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                if (is_fp) {
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                    if (elem_size == 4) {
                        ir_emit_vloadss(c, va, sa);
                        ir_emit_vloadss(c, vb, sb);
                    } else {
                        ir_emit_vloadsd(c, va, sa);
                        ir_emit_vloadsd(c, vb, sb);
                    }
                    if (op == OP_RVV_VADD)      ir_emit_fadd(c, vr, va, vb);
                    else if (op == OP_RVV_VSUB) ir_emit_fsub(c, vr, va, vb);
                    else if (op == OP_RVV_VMUL) ir_emit_fmul(c, vr, va, vb);
                    else                        ir_emit_fdiv(c, vr, va, vb);

                    if (elem_size == 4) ir_emit_vstoress(c, sr, vr);
                    else                ir_emit_vstoresd(c, sr, vr);
                } else {
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                    if (elem_size == 4) {
                        ir_emit_load_32(c, va, sa);
                        ir_emit_load_32(c, vb, sb);
                    } else if (elem_size == 8) {
                        ir_emit_deref(c, va, sa);
                        ir_emit_deref(c, vb, sb);
                    } else if (elem_size == 2) {
                        ir_emit_load_16(c, va, sa);
                        ir_emit_load_16(c, vb, sb);
                    } else {
                        ir_emit_load_byte(c, va, sa);
                        ir_emit_load_byte(c, vb, sb);
                    }
                    if (op == OP_RVV_VADD)      ir_emit_add(c, vr, va, vb);
                    else if (op == OP_RVV_VSUB) ir_emit_sub(c, vr, va, vb);
                    else if (op == OP_RVV_VMUL) ir_emit_mul(c, vr, va, vb);
                    else                        ir_emit_div(c, vr, va, vb);

                    if (elem_size == 4)      ir_emit_store_32(c, vr, sr);
                    else if (elem_size == 8) ir_emit_store_64(c, vr, sr);
                    else if (elem_size == 2) ir_emit_store_16(c, vr, sr);
                    else                     ir_emit_store_byte(c, vr, sr);
                }
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vand") == 0 || strcmp(rvv_name, "vor") == 0 || strcmp(rvv_name, "vxor") == 0) {
        Opcode op = (strcmp(rvv_name, "vand") == 0) ? OP_RVV_VAND :
                    (strcmp(rvv_name, "vor") == 0)  ? OP_RVV_VOR  : OP_RVV_VXOR;
        int a_addr = lower_expr(c, arena, arg0_ref);
        int b_addr = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_rvv_emit_load(c, va, a_addr, 2);
            zc_rvv_emit_load(c, vb, b_addr, 2);
            ir_emit(c, (Instr){.op = op, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, 2);
        } else {
            for (size_t i = 0; i < 4; i++) {
                int sa = zc_simd_lane_addr(c, a_addr, i * 4);
                int sb = zc_simd_lane_addr(c, b_addr, i * 4);
                int sr = zc_simd_lane_addr(c, res_addr, i * 4);
                int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
                ir_emit_load_32(c, va, sa);
                ir_emit_load_32(c, vb, sb);
                if (op == OP_RVV_VAND)     ir_emit_and(c, vr, va, vb);
                else if (op == OP_RVV_VOR) ir_emit_or(c, vr, va, vb);
                else                       ir_emit_xor(c, vr, va, vb);
                ir_emit_store_32(c, vr, sr);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vcmp") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int is_fp = (int)zc_get_imm_arg(arena, arg1_ref);
        int cond = (int)zc_get_imm_arg(arena, arg2_ref);
        int a_addr = lower_expr(c, arena, arg3_ref);
        int b_addr = lower_expr(c, arena, arg4_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int va = zc_alloc_temp(c), vb = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_rvv_emit_load(c, va, a_addr, esz);
            zc_rvv_emit_load(c, vb, b_addr, esz);
            ir_emit(c, (Instr){.op = OP_RVV_VCMP, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vb,
                               .imm = esz, .imm64 = (int64_t)(cond | (is_fp << 8)),
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int sa = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                int sb = zc_simd_lane_addr(c, b_addr, (size_t)i * elem_size);
                int sr = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                if (is_fp) {
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                    ir_emit_vloadss(c, va, sa);
                    ir_emit_vloadss(c, vb, sb);
                    ir_emit_cmp(c, (uint32_t)va, (uint32_t)vb);
                    int c_set = zc_alloc_temp(c);
                    ir_emit_cond_set(c, c_set, (cond == 0) ? CC_EQ : CC_FLT_LT);
                    ir_emit_store_byte(c, c_set, sr);
                } else {
                    int va = zc_alloc_temp(c), vb = zc_alloc_temp(c);
                    ir_emit_load_32(c, va, sa);
                    ir_emit_load_32(c, vb, sb);
                    ir_emit_cmp(c, va, vb);
                    int c_set = zc_alloc_temp(c);
                    ir_emit_cond_set(c, c_set, (cond == 0) ? CC_EQ : CC_LT);
                    ir_emit_store_byte(c, c_set, sr);
                }
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vredsum") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int is_fp = (int)zc_get_imm_arg(arena, arg1_ref);
        int a_addr = lower_expr(c, arena, arg2_ref);
        int init_addr = lower_expr(c, arena, arg3_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int va = zc_alloc_temp(c), vi = zc_alloc_temp(c), vr = zc_alloc_temp(c);
            zc_rvv_emit_load(c, va, a_addr, esz);
            zc_rvv_emit_load(c, vi, init_addr, esz);
            ir_emit(c, (Instr){.op = OP_RVV_VREDSUM, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)va, .temp_src2 = (uint32_t)vi,
                               .imm = esz, .imm64 = is_fp,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            if (is_fp) {
                int sum = zc_alloc_temp(c);
                ir_emit_vloadss(c, sum, init_addr);
                for (int i = 0; i < lanes; i++) {
                    int sa = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                    int val = zc_alloc_temp(c);
                    ir_emit_vloadss(c, val, sa);
                    ir_emit_fadd(c, sum, sum, val);
                }
                ir_emit_vstoress(c, res_addr, sum);
            } else {
                int sum = zc_alloc_temp(c);
                ir_emit_load_32(c, sum, init_addr);
                for (int i = 0; i < lanes; i++) {
                    int sa = zc_simd_lane_addr(c, a_addr, (size_t)i * elem_size);
                    int val = zc_alloc_temp(c);
                    ir_emit_load_32(c, val, sa);
                    ir_emit_add(c, sum, sum, val);
                }
                ir_emit_store_32(c, sum, res_addr);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vmv_v_x") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int val = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int vr = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op = OP_RVV_VMV_V_X, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)val, .imm = esz,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int da = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                if (elem_size == 4)      ir_emit_store_32(c, val, da);
                else if (elem_size == 8) ir_emit_store_64(c, val, da);
                else if (elem_size == 2) ir_emit_store_16(c, val, da);
                else                     ir_emit_store_byte(c, val, da);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vfmv_v_f") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int val = lower_expr(c, arena, arg1_ref);
        int res_addr = zc_simd_alloc_res128(c, NULL);
        if (zc_simd_has_rvv(c)) {
            int vr = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op = OP_RVV_VFMV_V_F, .temp_dest = (uint32_t)vr,
                               .temp_src1 = (uint32_t)val, .imm = esz,
                               .flags_out = 0xFF, .flags_in = 0xFF});
            zc_rvv_emit_store(c, res_addr, vr, esz);
        } else {
            int elem_size = 1 << esz;
            int lanes = 16 / elem_size;
            for (int i = 0; i < lanes; i++) {
                int da = zc_simd_lane_addr(c, res_addr, (size_t)i * elem_size);
                if (elem_size == 4) ir_emit_vstoress(c, da, val);
                else                ir_emit_vstoresd(c, da, val);
            }
        }
        c->last_expr_is_float = 0;
        *out_temp = res_addr;
        return true;
    }

    if (strcmp(rvv_name, "vmv_x_s") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int src = lower_expr(c, arena, arg1_ref);
        int res = zc_alloc_temp(c);
        if (zc_simd_has_rvv(c)) {
            int va = zc_alloc_temp(c);
            zc_rvv_emit_load(c, va, src, esz);
            ir_emit(c, (Instr){.op = OP_RVV_VMV_X_S, .temp_dest = (uint32_t)res,
                               .temp_src1 = (uint32_t)va, .imm = esz,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int elem_size = 1 << esz;
            if (elem_size == 4)      ir_emit_load_32(c, res, src);
            else if (elem_size == 8) ir_emit_deref(c, res, src);
            else if (elem_size == 2) ir_emit_load_16(c, res, src);
            else                     ir_emit_load_byte(c, res, src);
        }
        c->last_expr_is_float = 0;
        *out_temp = res;
        return true;
    }

    if (strcmp(rvv_name, "vfmv_f_s") == 0) {
        int esz = (int)zc_get_imm_arg(arena, arg0_ref);
        int src = lower_expr(c, arena, arg1_ref);
        int res = zc_alloc_temp(c);
        if (zc_simd_has_rvv(c)) {
            int va = zc_alloc_temp(c);
            zc_rvv_emit_load(c, va, src, esz);
            ir_emit(c, (Instr){.op = OP_RVV_VFMV_F_S, .temp_dest = (uint32_t)res,
                               .temp_src1 = (uint32_t)va, .imm = esz,
                               .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            if (esz == 3) ir_emit_vloadsd(c, res, src);
            else          ir_emit_vloadss(c, res, src);
        }
        c->last_expr_is_float = 1;
        *out_temp = res;
        return true;
    }

    return false;
}

/* Lower AVX-512 Opmask / Predication builtins (Sprint 205 - Phase 11) */
static bool zc_lower_kmask_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                                   ZeroCCASTNode *node, const char *kname,
                                   int *out_temp) {
    if (!kname || !node) return false;
    ASTRef arg0_ref = node->right;
    ZeroCCASTNode *arg0 = arg0_ref ? zero_cc_ast_get(arena, arg0_ref) : NULL;
    ASTRef arg1_ref = arg0 ? arg0->next : AST_NULL;
    c->last_expr_is_float = 0;

    bool is_8 = (strstr(kname, "8") != NULL);
    uint32_t mask_val = is_8 ? 0xFF : 0xFFFF;

    /* 1. Mask test operations: kortestz, kortestc */
    if (strncmp(kname, "kortest", 7) == 0) {
        if (!arg0_ref || !arg1_ref) return false;
        bool is_c = (strstr(kname, "kortestc") != NULL);
        int ta = lower_expr(c, arena, arg0_ref);
        int tb = lower_expr(c, arena, arg1_ref);
        int res = zc_alloc_temp(c);

        if (zc_simd_has_hw512(c)) {
            c->used_avx512 = true;
            c->used_avx = true;
            ir_emit(c, (Instr){.op = OP_KMOVW_TO_K, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)ta, .imm = 1, .flags_out = 0xFF, .flags_in = 0xFF});
            ir_emit(c, (Instr){.op = OP_KMOVW_TO_K, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)tb, .imm = 2, .flags_out = 0xFF, .flags_in = 0xFF});
            ir_emit(c, (Instr){.op = OP_KORTESTW, .temp_dest = (uint32_t)res, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
                               .imm = 1 | (2 << 4) | ((is_c ? 1 : 0) << 8), .flags_out = 0xFF, .flags_in = 0xFF});
        } else {
            int tor = zc_alloc_temp(c);
            ir_emit_or(c, tor, ta, tb);
            int tm = zc_alloc_temp(c);
            ir_emit_mov_imm(c, tm, mask_val);
            int tval = zc_alloc_temp(c);
            ir_emit_and(c, tval, tor, tm);
            if (!is_c) {
                int zero = zc_alloc_temp(c);
                ir_emit_mov_imm(c, zero, 0);
                ir_emit_cmp(c, tval, zero);
                ir_emit_cond_set(c, res, CC_EQ);
            } else {
                ir_emit_cmp(c, tval, tm);
                ir_emit_cond_set(c, res, CC_EQ);
            }
        }
        *out_temp = res;
        return true;
    }

    /* 2. Binary mask operations: kand, kor, kxor */
    if (strncmp(kname, "kand", 4) == 0 ||
        (strncmp(kname, "kor", 3) == 0 && strncmp(kname, "kortest", 7) != 0) ||
        strncmp(kname, "kxor", 4) == 0) {
        if (!arg0_ref || !arg1_ref) return false;
        int ta = lower_expr(c, arena, arg0_ref);
        int tb = lower_expr(c, arena, arg1_ref);
        int res = zc_alloc_temp(c);

        if (zc_simd_has_hw512(c)) {
            c->used_avx512 = true;
            c->used_avx = true;
            ir_emit(c, (Instr){.op = OP_KMOVW_TO_K, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)ta, .imm = 1, .flags_out = 0xFF, .flags_in = 0xFF});
            ir_emit(c, (Instr){.op = OP_KMOVW_TO_K, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)tb, .imm = 2, .flags_out = 0xFF, .flags_in = 0xFF});
            Opcode op = (strncmp(kname, "kand", 4) == 0) ? OP_KANDW : (strncmp(kname, "kor", 3) == 0 ? OP_KORW : OP_KXORW);
            ir_emit(c, (Instr){.op = op, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
                               .imm = 3 | (1 << 4) | (2 << 8), .flags_out = 0xFF, .flags_in = 0xFF});
            ir_emit(c, (Instr){.op = OP_KMOVW_FROM_K, .temp_dest = (uint32_t)res, .temp_src1 = TEMP_NONE, .imm = 3, .flags_out = 0xFF, .flags_in = 0xFF});
            if (is_8) {
                int tm = zc_alloc_temp(c);
                ir_emit_mov_imm(c, tm, 0xFF);
                ir_emit_and(c, res, res, tm);
            }
        } else {
            if (strncmp(kname, "kand", 4) == 0) ir_emit_and(c, res, ta, tb);
            else if (strncmp(kname, "kor", 3) == 0) ir_emit_or(c, res, ta, tb);
            else ir_emit_xor(c, res, ta, tb);
            int tm = zc_alloc_temp(c);
            ir_emit_mov_imm(c, tm, mask_val);
            ir_emit_and(c, res, res, tm);
        }
        *out_temp = res;
        return true;
    }

    /* 3. Unary mask operation: knot */
    if (strncmp(kname, "knot", 4) == 0) {
        if (!arg0_ref) return false;
        int ta = lower_expr(c, arena, arg0_ref);
        int res = zc_alloc_temp(c);

        if (zc_simd_has_hw512(c)) {
            c->used_avx512 = true;
            c->used_avx = true;
            ir_emit(c, (Instr){.op = OP_KMOVW_TO_K, .temp_dest = TEMP_NONE, .temp_src1 = (uint32_t)ta, .imm = 1, .flags_out = 0xFF, .flags_in = 0xFF});
            ir_emit(c, (Instr){.op = OP_KNOTW, .temp_dest = TEMP_NONE, .temp_src1 = TEMP_NONE,
                               .imm = 2 | (1 << 4), .flags_out = 0xFF, .flags_in = 0xFF});
            ir_emit(c, (Instr){.op = OP_KMOVW_FROM_K, .temp_dest = (uint32_t)res, .temp_src1 = TEMP_NONE, .imm = 2, .flags_out = 0xFF, .flags_in = 0xFF});
            int tm = zc_alloc_temp(c);
            ir_emit_mov_imm(c, tm, mask_val);
            ir_emit_and(c, res, res, tm);
        } else {
            int tm = zc_alloc_temp(c);
            ir_emit_mov_imm(c, tm, mask_val);
            ir_emit_xor(c, res, ta, tm);
        }
        *out_temp = res;
        return true;
    }

    return false;
}

bool lower_expr_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                        ZeroCCASTNode *node, const char *name,
                        int *out_temp) {
    if (!name || !out_temp) return false;
    ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
    bool callee_is_ident = (callee && callee->tag == NODE_IDENT);
    (void)callee_is_ident;

        /* __builtin_object_size(ptr, mode) - folded to a compile-time
         * constant (see the walker above). Without this the builtin stayed
         * an external call: the JIT failed symbol resolution ("undefined
         * reference to __builtin_object_size") and the AOT link left dead
         * `call 0 <link_error>` stubs (c-torture execute/builtins *chk.c).
         * Non-constant arguments are still lowered for side effects. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_object_size") == 0) {
            int64_t sz = -1;
            ASTRef ptr_ref = node->right;
            ZeroCCASTNode *p0 = (ptr_ref != AST_NULL) ? zero_cc_ast_get(arena, ptr_ref) : NULL;
            bool folded = false;
            if (p0) {
                ZcObjInfo o;
                if (zc_objsize_walk(c, arena, p0, &o)) {
                    sz = zc_objsize_value(&o);
                    folded = true;
                }
            }
            if (!folded) {
                /* side effects in an unknown-shape pointer argument */
                if (ptr_ref != AST_NULL) (void)lower_expr(c, arena, ptr_ref);
            }
            ASTRef mode_ref = p0 ? p0->next : AST_NULL;
            int64_t mode_dummy;
            if (mode_ref != AST_NULL &&
                !zc_ast_const_eval(c, arena, mode_ref, &mode_dummy)) {
                (void)lower_expr(c, arena, mode_ref); /* side effects */
            }
            int res = zc_alloc_temp(c);
            ir_emit_mov_imm(c, res, (uint64_t)sz);
            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
        }

        /* strncmp() / __builtin_strncmp() with n == 0: GCC folds the result
         * to 0 at every -O level without emitting a call (calibrated against
         * gcc-14 at -O0: `movl $0,%eax`, no strncmp call). The c-torture
         * user strncmp (lib/strncmp.c) returns the *uninitialized* c1 - c2
         * when n == 0, so a real call is UB-dependent - execute/builtins/
         * strncmp.c aborted on its first `strncmp (++s2, ++s3, 0)`. The
         * string arguments are still lowered for their side effects
         * (`++s2`); the result is the int constant 0. -fno-builtin-strncmp
         * (or -fno-builtin) keeps the real call, like the abs family. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "strncmp") == 0 ||
             strcmp(callee->s.str, "__builtin_strncmp") == 0) &&
            !zero_cc_no_builtin(c, "strncmp")) {
            ZeroCCASTNode *sa0 = (node->right != AST_NULL) ? zero_cc_ast_get(arena, node->right) : NULL;
            ZeroCCASTNode *sa1 = (sa0 && sa0->next != AST_NULL) ? zero_cc_ast_get(arena, sa0->next) : NULL;
            if (sa0 && sa1 && sa1->next != AST_NULL) {
                int64_t snval = 0;
                if (zc_ast_const_eval(c, arena, sa1->next, &snval) && snval == 0) {
                    (void)lower_expr(c, arena, node->right); /* side effects */
                    (void)lower_expr(c, arena, sa0->next);
                    int res = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, res, 0);
                    c->last_expr_is_float = 0;
                    *out_temp = res;
                    return true;
                }
            }
        }

        /* __builtin_return_address(level) / __builtin_frame_address(level) */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_return_address") == 0 ||
             strcmp(callee->s.str, "__builtin_frame_address") == 0)) {
            bool is_ret_addr = (strcmp(callee->s.str, "__builtin_return_address") == 0);
            c->lowering.has_frame_address = true;

            int cur_fp = zc_alloc_temp(c);
            ir_emit_lea(c, cur_fp, BE(c)->fp_reg, 0);

            int fp = cur_fp;
            int64_t const_lvl = 0;
            if (node->right != AST_NULL && zc_ast_const_eval(c, arena, node->right, &const_lvl)) {
                if (const_lvl < 0) const_lvl = 0;
                if (const_lvl > 100) const_lvl = 100;
                for (int64_t i = 0; i < const_lvl; i++) {
                    int next_fp = zc_alloc_temp(c);
                    ir_emit_deref(c, next_fp, fp);
                    fp = next_fp;
                }
            } else if (node->right != AST_NULL) {
                int lvl_temp = lower_expr(c, arena, node->right);
                int cnt_temp = zc_alloc_temp(c);
                ir_emit_mov_reg(c, cnt_temp, lvl_temp);
                uint16_t loop_lbl = (uint32_t)c->ir_next_label++;
                uint16_t end_lbl = (uint32_t)c->ir_next_label++;

                ir_emit_label(c, loop_lbl);
                int zero_t = zc_alloc_temp(c);
                ir_emit_mov_imm(c, zero_t, 0);
                ir_emit_cmp(c, cnt_temp, zero_t);
                int is_le = zc_alloc_temp(c);
                ir_emit_cond_set(c, is_le, CC_LE);
                ir_emit_jne(c, (uint32_t)is_le, end_lbl);

                int next_fp = zc_alloc_temp(c);
                ir_emit_deref(c, next_fp, fp);
                fp = next_fp;

                int one = zc_alloc_temp(c);
                ir_emit_mov_imm(c, one, 1);
                ir_emit_sub(c, cnt_temp, cnt_temp, one);
                ir_emit_jmp(c, loop_lbl);

                ir_emit_label(c, end_lbl);
            }

            int res = fp;
            if (is_ret_addr) {
                int ra_off = zc_target_slot_size(c);
                int off_temp = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off_temp, ra_off);
                int ra_ptr = zc_alloc_temp(c);
                ir_emit_add(c, ra_ptr, fp, off_temp);
                res = zc_alloc_temp(c);
                ir_emit_deref(c, res, ra_ptr);
            }

            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
        }

        /* __builtin_apply_args() / __builtin_apply(fn, args, nbytes) - GNU
         * function application with an explicit argument block
         * (c-torture execute/pr47237).
         *
         * `__builtin_apply_args()` returns a pointer to a fresh frame block
         * with the CURRENT function's arguments copied in, one 8-byte slot
         * per parameter (the ABI register width; only the parameter's type
         * width is written, so narrow args are not padded with garbage).
         *
         * `__builtin_apply(fn, p, n)` calls the function pointer `fn`,
         * taking its arguments from successive 8-byte slots of `p` using
         * the pointee's declared parameter types (the `n` byte hint is the
         * caller's block size and is ignored - the signature is the truth).
         * Struct/union/complex/array parameters cannot be represented in
         * the 8-byte slot model and are a clean REJECT (no sret handling
         * either). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_apply_args") == 0) {
            ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
            int nparams = (cur_fn && cur_fn->param_types && cur_fn->param_count > 0)
                           ? (int)cur_fn->param_count : 0;
            if (nparams > 16) nparams = 16;
            int block_sz = (nparams * 8) < 16 ? 16 : (nparams * 8);
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += block_sz;
            int32_t block_off = c->current_stack_frame;
            int base = zc_alloc_temp(c);
            ir_emit_lea(c, base, BE(c)->fp_reg, -block_off);
            for (int p = 0; p < nparams && p < (int)c->local_count; p++) {
                ZeroCCSym *psym = &c->locals[p];
                if (!psym->is_param) continue;
                const ZeroCCType *pt = (cur_fn && cur_fn->param_types) ? &cur_fn->param_types[p] : NULL;
                int w = (pt && pt->size >= 5) ? 8 : 4; /* int/float -> 4, long/ptr/double -> 8 */
                int slot_addr = zc_alloc_temp(c);
                ir_emit_lea(c, slot_addr, BE(c)->fp_reg, -(block_off - 8 * p));
                int val = zc_alloc_temp(c);
                zc_sym_load(c, psym, 0, val);
                if (w == 8) ir_emit_store_64(c, (uint32_t)val, (uint32_t)slot_addr);
                else ir_emit_store_32(c, (uint32_t)val, (uint32_t)slot_addr);
            }
            c->last_expr_is_float = 0;
            *out_temp = base;
            return true;
        }
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_apply") == 0) {
            ASTRef arg0 = node->right;
            ZeroCCASTNode *a0n = zero_cc_ast_get(arena, arg0);
            ASTRef arg1 = (a0n && a0n->next != AST_NULL) ? a0n->next : AST_NULL;
            int fn_temp = lower_expr(c, arena, arg0);
            c->last_expr_is_float = 0;
            int p_temp = (arg1 != AST_NULL) ? lower_expr(c, arena, arg1) : -1;
            c->last_expr_is_float = 0;
            /* Third arg (nbytes) - lowered for side effects, value unused. */
            ZeroCCASTNode *a1n = (arg1 != AST_NULL) ? zero_cc_ast_get(arena, arg1) : NULL;
            ASTRef arg2 = (a1n && a1n->next != AST_NULL) ? a1n->next : AST_NULL;
            if (arg2 != AST_NULL) { lower_expr(c, arena, arg2); c->last_expr_is_float = 0; }
            if (p_temp < 0) {
                diag_report_error_at_token(c, "__builtin_apply requires (fn, args, nbytes)");
                *out_temp = VREG_0;
                return true;
            }
            /* The pointee's declared signature drives the argument layout.
             * For an identifier callee the ZeroCCFunction table is the
             * authoritative parameter list (the semantic TypeInfo often
             * carries no prototype), mirroring the direct-call path. */
            int npp = 0;
            ZeroCCType apply_pparams[IR_CALL_MAX_ARGS];
            ZeroCCType apply_ret = zero_cc_type_int();
            ZeroCCFunction *apply_callee_fn = NULL;
            if (a0n && a0n->tag == NODE_IDENT && a0n->s.str) {
                apply_callee_fn = zero_cc_find_function(c, a0n->s.str);
            }
            if (apply_callee_fn) {
                /* Single source of truth (Sprint 121): the function table is
                 * authoritative for a known function. NULL callee_node disables
                 * the TypeInfo fallback - a `()` callee marshals with 0 params.
                 * (The semantic TypeInfo often carries no prototype - reading it
                 * would silently yield param_cnt == 0 and stale-register garbage.) */
                npp = zero_cc_effective_call_params(c, apply_callee_fn, NULL,
                                                    apply_pparams, IR_CALL_MAX_ARGS - 2);
                apply_ret = apply_callee_fn->return_type;
            } else {
                npp = zero_cc_effective_call_params(c, NULL, a0n,
                                                    apply_pparams, IR_CALL_MAX_ARGS - 2);
                /* Return type from the pointee when recoverable. */
                const TypeInfo *pointee = zc_call_pointee_typeinfo(c, a0n);
                if (pointee && pointee->details.function.ret)
                    apply_ret = typeinfo_to_zero_cc_type(pointee->details.function.ret);
                /* else: no recoverable pointee signature (e.g. the function
                 * pointer was cast through void *).  Fall back to a
                 * K&R-style indirect call with no argument metadata -
                 * the same shape the compiler already emits for calls
                 * through unprototyped pointers. */
            }
            /* Reject the shapes the 8-byte slot model cannot represent. */
            for (int j = 0; j < npp; j++) {
                const ZeroCCType *pt = &apply_pparams[j];
                if (pt->tag == TYPE_STRUCT || pt->tag == TYPE_UNION ||
                    pt->tag == TYPE_ARRAY || pt->is_complex) {
                    diag_report_error_at_token(c, "__builtin_apply with struct/complex parameters unsupported");
                    *out_temp = VREG_0;
                    return true;
                }
            }
            if ((apply_ret.tag == TYPE_STRUCT || apply_ret.tag == TYPE_UNION) && apply_ret.size > 16) {
                diag_report_error_at_token(c, "__builtin_apply of an sret-returning function unsupported");
                *out_temp = VREG_0;
                return true;
            }
            int gp_regs_avail = BE(c)->conv ? BE(c)->conv->int_arg_reg_count : 8;
            int fp_regs_avail = BE(c)->conv ? BE(c)->conv->fp_arg_reg_count : 8;
            int gp_stream = 0, fp_stream = 0, stack_arg_count = 0;
            int int_arg_idx = 0, fp_arg_idx = 0;
            int32_t fp_spill_offsets[IR_CALL_MAX_ARGS];
            int all_arg_temps[IR_CALL_MAX_ARGS];
            int all_arg_is_fp[IR_CALL_MAX_ARGS];   /* int[] - is_fp class per arg */
            int all_arg_stack_pos[IR_CALL_MAX_ARGS];
            int all_arg_count = 0;
            for (int j = 0; j < npp; j++) {
                ZeroCCType pt = apply_pparams[j];
                int slot_addr = zc_alloc_temp(c);
                if (j == 0) slot_addr = p_temp;
                else {
                    int off_t = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, off_t, 8 * j);
                    int add_t = zc_alloc_temp(c);
                    ir_emit_add(c, add_t, p_temp, off_t);
                    slot_addr = add_t;
                }
                bool param_is_fp = (pt.tag == TYPE_FLOAT || pt.tag == TYPE_DOUBLE);
                int val = zc_alloc_temp(c);
                if (param_is_fp) {
                    /* FP argument: load the bit pattern, spill it to a frame
                     * slot the MC emitter reads into the FP arg register
                     * (same mechanism as the direct-call path). */
                    if (pt.tag == TYPE_FLOAT) ir_emit_load_32(c, (uint32_t)val, (uint32_t)slot_addr);
                    else ir_emit_deref(c, val, slot_addr);
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    fp_spill_offsets[fp_arg_idx] = c->current_stack_frame;
                    ir_emit_store(c, val, BE(c)->fp_reg, -fp_spill_offsets[fp_arg_idx]);
                    uint8_t sp = (fp_stream < fp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    all_arg_temps[all_arg_count] = val;
                    all_arg_is_fp[all_arg_count] = 1;
                    all_arg_stack_pos[all_arg_count] = sp;
                    all_arg_count++;
                    fp_stream++;
                    fp_arg_idx++;
                } else {
                    size_t w = pt.size;
                    if (w <= 4) ir_emit_load_32(c, (uint32_t)val, (uint32_t)slot_addr);
                    else ir_emit_deref(c, val, slot_addr);
                    uint8_t sp = (gp_stream < gp_regs_avail) ? 255 : (uint8_t)(stack_arg_count++);
                    all_arg_temps[all_arg_count] = val;
                    all_arg_is_fp[all_arg_count] = 0;
                    all_arg_stack_pos[all_arg_count] = sp;
                    all_arg_count++;
                    gp_stream++;
                    int_arg_idx++;
                }
            }
            /* Spill the callee pointer (indirect-call convention). */
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t callee_spill_off = c->current_stack_frame;
            ir_emit_store(c, fn_temp, BE(c)->fp_reg, -callee_spill_off);
            int callee_reload = zc_alloc_temp(c);
            ir_emit_load(c, callee_reload, BE(c)->fp_reg, -callee_spill_off);
            all_arg_temps[all_arg_count] = callee_reload;
            all_arg_is_fp[all_arg_count] = 2;
            all_arg_stack_pos[all_arg_count] = 255;
            all_arg_count++;
            ir_emit_call(c, NULL);
            if (ir_get_count(c) > 0) {
                Instr *last_instr = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
                last_instr->temp_dest = (uint32_t)all_arg_count;
                if (all_arg_count > 0) last_instr->temp_src1 = (uint32_t)all_arg_temps[0];
                if (all_arg_count > 1) last_instr->temp_src2 = (uint32_t)all_arg_temps[1];
                last_instr->imm = (int32_t)(all_arg_count > 0 ? all_arg_stack_pos[0] : 0)
                                 | (int32_t)(all_arg_count > 1 ? all_arg_stack_pos[1] << 8 : 0);
                last_instr->cond = (uint8_t)(all_arg_count > 0 ? all_arg_is_fp[0] : 0);
                last_instr->width = (uint8_t)(all_arg_count > 1 ? all_arg_is_fp[1] : 0);
                last_instr->label_idx = (uint16_t)(fp_arg_idx < fp_regs_avail ? fp_arg_idx : fp_regs_avail);
                for (int a = 2; a < all_arg_count; a++) {
                    ir_emit_call_arg(c, (uint32_t)all_arg_temps[a], all_arg_is_fp[a], all_arg_stack_pos[a]);
                }
            }
            if (c->lowering.ir_ctx.call_count < 64) {
                int cidx = c->lowering.ir_ctx.call_count++;
                c->lowering.ir_ctx.call_fp_spill_counts[cidx] = fp_arg_idx;
                for (int f = 0; f < fp_arg_idx && f < 16; f++) {
                    c->lowering.ir_ctx.call_fp_spills[cidx][f] = fp_spill_offsets[f];
                }
            }
            /* Result: mirror the direct-call path's materialization. */
            ZeroCCType call_ret = apply_ret;
            if (call_ret.tag == TYPE_FLOAT || call_ret.tag == TYPE_DOUBLE) {
                c->last_expr_is_float = 1;
                int fp_res = zc_alloc_temp(c);
                ir_emit_fmov(c, (uint32_t)fp_res, (uint32_t)VREG_F0);
                *out_temp = fp_res;
                return true;
            }
            c->last_expr_is_float = 0;
            *out_temp = VREG_0;
            return true;
        }

        /* __builtin_setjmp(buf) / __builtin_longjmp(buf, val) - non-local
         * jumps lowered inline via OP_SETJMP / OP_LONGJMP across all target
         * ISAs (x86-64, AArch64, RISC-V64, RISC-V32) in both JIT and AOT.
         * Buffer layout (3 pointer-sized words, fitting GCC's 5-word buf):
         *   buf[0] = caller FP (%rbp / x29 / s0)
         *   buf[1] = resume PC (.Lsetjmp_resume)
         *   buf[2] = caller SP (%rsp / sp / x2)
         * Functions calling setjmp keep a real frame pointer (has_frame_address),
         * keep automatics and call-spanning temps on the stack (has_setjmp), and
         * save/restore the full ISA callee-saved GPR set in prologue/epilogue. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_setjmp") == 0 ||
             strcmp(callee->s.str, "__builtin_longjmp") == 0)) {
            bool is_longjmp = (strcmp(callee->s.str, "__builtin_longjmp") == 0);
            c->lowering.has_frame_address = true;
            if (!is_longjmp) {
                c->lowering.has_setjmp = true;
            }

            ASTRef buf_arg = node->right;                 /* first arg: the buf */
            int buf_temp = lower_expr(c, arena, buf_arg);
            c->last_expr_is_float = 0;

            if (is_longjmp) {
                ZeroCCASTNode *bn = zero_cc_ast_get(arena, buf_arg);
                ASTRef val_arg = (bn && bn->next != AST_NULL) ? bn->next : AST_NULL;
                int val_raw = lower_expr(c, arena, val_arg);
                /* C99 7.13.2.1: if val == 0, setjmp returns 1.
                 * Branchless normalization: val = val_raw | (val_raw == 0). */
                int is_zero = zc_alloc_temp(c);
                ir_emit_cmp_zero(c, val_raw);
                ir_emit_cond_set(c, is_zero, CC_EQ);
                int val_norm = zc_alloc_temp(c);
                ir_emit_or(c, val_norm, val_raw, is_zero);

                ir_emit_longjmp(c, (uint32_t)buf_temp, (uint32_t)val_norm);
                /* longjmp never returns: block fall-through with an infinite
                 * loop (the setjmp site in the target function resumes). */
                uint16_t loop_lbl = (uint32_t)c->ir_next_label++;
                ir_emit_label(c, loop_lbl);
                ir_emit_jmp(c, loop_lbl);
                *out_temp = VREG_0;
                return true;
            }

            /* setjmp: emit inline OP_SETJMP (sets VREG_0 = 0 on first pass,
             * or lands at resume label with VREG_0 = val from OP_LONGJMP),
             * then capture VREG_0 into a fresh allocatable temp. */
            uint16_t resume_lbl = (uint32_t)c->ir_next_label++;
            ir_emit_setjmp(c, (uint32_t)buf_temp, resume_lbl);
            int res = zc_alloc_temp(c);
            ir_emit_mov_reg(c, res, VREG_0);
            *out_temp = res;
            return true;
        }

        /* __builtin_alloca(size) / alloca(size) - dynamic stack allocation */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_alloca") == 0 ||
             strcmp(callee->s.str, "alloca") == 0)) {
            int size_temp = lower_expr(c, arena, node->right);
            int add15 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, add15, 15);
            int size_p15 = zc_alloc_temp(c);
            ir_emit_add(c, size_p15, size_temp, add15);
            int mask16 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, mask16, -16);
            int aligned_size = zc_alloc_temp(c);
            ir_emit_and(c, aligned_size, size_p15, mask16);
            int ptr_temp = zc_alloc_temp(c);
            ir_emit_vla_alloc(c, ptr_temp, aligned_size);
            c->last_expr_is_float = 0;
            *out_temp = ptr_temp;
            return true;
        }

        /* GNU complex part extractors: __real(E) / __real__(E) load
         * the first half of the address-form complex value, __imag / __imag__
         * the second. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__real") == 0 ||
             strcmp(callee->s.str, "__real__") == 0 ||
             strcmp(callee->s.str, "__imag") == 0 ||
             strcmp(callee->s.str, "__imag__") == 0)) {
            bool is_imag = (callee->s.str[2] == 'i');
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *cx_ti = ast_node_get_typeinfo(c, cx_node);
            const TypeInfo *el_ti = (cx_ti && cx_ti->kind == TYPE_KIND_COMPLEX) ? cx_ti->details.array.base : NULL;
            const ZeroCCType *el_zt = NULL;
            if (!el_ti && cx_node && cx_node->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, cx_node->s.str);
                if (sym && zero_cc_type_is_complex(&sym->type)) el_zt = sym->type.base;
                else {
                    ZeroCCGlobalVar *gv = find_global_var(c, cx_node->s.str);
                    if (gv && zero_cc_type_is_complex(&gv->type)) el_zt = gv->type.base;
                }
            }
            size_t esz = el_ti ? type_size(el_ti) : (el_zt ? el_zt->size : 8);
            if (esz == 0) esz = 8;
            bool is_elem_fp = el_ti ? type_is_floating(el_ti) :
                              (el_zt ? (el_zt->tag == TYPE_FLOAT || el_zt->tag == TYPE_DOUBLE) : true);
            if (is_elem_fp && esz != 4 && esz != 8) esz = 8; /* long double: double model */
            int addr = lower_expr(c, arena, node->right);
            int res = zc_alloc_temp(c);
            int la = addr;
            if (is_imag) {
                int off = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off, (int)esz);
                la = zc_alloc_temp(c);
                ir_emit_add(c, la, addr, off);
            }
            if (is_elem_fp) {
                if (esz == 4) ir_emit_vloadss(c, res, (uint32_t)la);
                else ir_emit_vloadsd(c, res, (uint32_t)la);
                c->last_expr_is_float = 1;
            } else {
                bool is_u = el_ti ? type_is_unsigned(el_ti) :
                            (el_zt ? zero_cc_type_is_unsigned(el_zt) : false);
                if (esz == 1) {
                    ir_emit_load_byte(c, res, la);
                    if (!is_u) ir_emit_sext(c, res, 8);
                } else if (esz == 2) {
                    ir_emit_load_16(c, res, la);
                    if (!is_u) ir_emit_sext(c, res, 16);
                } else if (esz == 4) {
                    ir_emit_load_32(c, res, la);
                    if (!is_u) ir_emit_sext(c, res, 32);
                } else {
                    ir_emit_deref(c, res, la);
                }
                c->last_expr_is_float = 0;
            }
            *out_temp = res;
            return true;
        }

        /* C99 complex builtins (Sprint 115l): conj/conjf/conjl,
         * creal/crealf/creall, cimag/cimagf/cimagl - with or without the
         * __builtin_ prefix. GCC attaches matching user declarations to
         * these builtins and expands them inline (even at -O0): conj
         * negates the imaginary half, creal/cimag extract a half - no libm
         * call is emitted, so user-defined trap functions (and any host
         * math library) are never referenced. The bare names require a
         * known-complex argument, so a user-defined `int conj(int)` still
         * lowers as an ordinary call. */
        {
            int cx_kind = zc_cx_builtin_kind(name);
            if (cx_kind) {
                bool has_prefix = strncmp(name, "__builtin_", 10) == 0;
                ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, node->right);
                size_t esz = 8;
                bool is_elem_fp = true, is_elem_u = false, cx_known = false;
                zc_cx_elem_info(c, cx_node, name, &esz, &is_elem_fp, &is_elem_u, &cx_known);
                if (!has_prefix && !cx_known) return false; /* not complex: ordinary call */
                int addr = lower_expr(c, arena, node->right);
                if (cx_kind != 1) {
                    /* creal/cimag: the requested half is a scalar result.
                     * Load only that half (the object may be volatile). */
                    int res = zc_alloc_temp(c);
                    int base = addr;
                    if (cx_kind == 3) {
                        int off = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, off, (int)esz);
                        base = zc_alloc_temp(c);
                        ir_emit_add(c, base, addr, off);
                    }
                    if (is_elem_fp) {
                        if (esz == 4) ir_emit_vloadss(c, (uint32_t)res, (uint32_t)base);
                        else ir_emit_vloadsd(c, (uint32_t)res, (uint32_t)base);
                        c->last_expr_is_float = 1;
                    } else {
                        if (esz == 1) ir_emit_load_byte(c, res, base);
                        else if (esz == 2) ir_emit_load_16(c, res, base);
                        else if (esz == 4) ir_emit_load_32(c, res, base);
                        else ir_emit_deref(c, res, base);
                        if (!is_elem_u) {
                            if (esz == 1) ir_emit_sext(c, res, 8);
                            else if (esz == 2) ir_emit_sext(c, res, 16);
                            else if (esz == 4) ir_emit_sext(c, res, 32);
                        }
                        c->last_expr_is_float = 0;
                    }
                    *out_temp = res;
                    return true;
                }
                /* conj: load both halves, negate the imaginary, store pair. */
                int rh = zc_alloc_temp(c), li = zc_alloc_temp(c);
                int off = zc_alloc_temp(c);
                ir_emit_mov_imm(c, off, (int)esz);
                int la = zc_alloc_temp(c);
                ir_emit_add(c, la, addr, off);
                if (is_elem_fp) {
                    if (esz == 4) {
                        ir_emit_vloadss(c, (uint32_t)rh, (uint32_t)addr);
                        ir_emit_vloadss(c, (uint32_t)li, (uint32_t)la);
                    } else {
                        ir_emit_vloadsd(c, (uint32_t)rh, (uint32_t)addr);
                        ir_emit_vloadsd(c, (uint32_t)li, (uint32_t)la);
                    }
                } else {
                    if (esz == 1) {
                        ir_emit_load_byte(c, rh, addr);
                        ir_emit_load_byte(c, li, la);
                    } else if (esz == 2) {
                        ir_emit_load_16(c, rh, addr);
                        ir_emit_load_16(c, li, la);
                    } else if (esz == 4) {
                        ir_emit_load_32(c, rh, addr);
                        ir_emit_load_32(c, li, la);
                    } else {
                        ir_emit_deref(c, rh, addr);
                        ir_emit_deref(c, li, la);
                    }
                }
                int neg_li = zc_alloc_temp(c);
            if (is_elem_fp) {
                int zero_i = zc_alloc_temp(c);
                ir_emit_mov_imm(c, zero_i, 0);
                int zero_f = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)zero_f, (uint32_t)zero_i);
                ir_emit_fsub(c, (uint32_t)neg_li, (uint32_t)zero_f, (uint32_t)li);
            } else {
                ir_emit_neg(c, neg_li, li);
            }

            size_t csz = esz * 2;
            c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
            c->current_stack_frame += (int32_t)csz;
            int32_t res_off = c->current_stack_frame;
            int res_addr = zc_alloc_temp(c);
            ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
            int off_m = zc_alloc_temp(c);
            ir_emit_mov_imm(c, off_m, (int)esz);
            int res_a2 = zc_alloc_temp(c);
            ir_emit_add(c, res_a2, res_addr, off_m);
            if (is_elem_fp) {
                if (esz == 4) {
                    ir_emit_vstoress(c, (uint32_t)res_addr, (uint32_t)rh);
                    ir_emit_vstoress(c, (uint32_t)res_a2, (uint32_t)neg_li);
                } else {
                    ir_emit_vstoresd(c, (uint32_t)res_addr, (uint32_t)rh);
                    ir_emit_vstoresd(c, (uint32_t)res_a2, (uint32_t)neg_li);
                }
            } else {
                if (esz == 1) {
                    ir_emit_store_byte(c, rh, res_addr);
                    ir_emit_store_byte(c, neg_li, res_a2);
                } else if (esz == 2) {
                    ir_emit_store_16(c, rh, res_addr);
                    ir_emit_store_16(c, neg_li, res_a2);
                } else if (esz == 4) {
                    ir_emit_store_32(c, rh, res_addr);
                    ir_emit_store_32(c, neg_li, res_a2);
                } else {
                    ir_emit_store_64(c, rh, res_addr);
                    ir_emit_store_64(c, neg_li, res_a2);
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = res_addr;
            return true;
            }
        }

        /* __builtin_expect(cond, val) is a branch-hint builtin: it
         * evaluates to `cond` (the hint is ignored by ZeroCC's codegen).
         * Lower to the condition expression and evaluate `val` for side effects. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_expect", 16) == 0) {
            ASTRef expect_arg = node->right;
            int res = lower_expr(c, arena, expect_arg);
            ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, expect_arg);
            if (arg_node && arg_node->next != AST_NULL) {
                (void)lower_expr(c, arena, arg_node->next);
            }
            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
        }

        /* __builtin_prefetch(addr, rw, locality) is a cache hint.
         * It is a no-op for ZeroCC: evaluate the arguments (so any
         * side effects occur) but emit no memory access. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_prefetch", 18) == 0) {
            ASTRef pa = node->right;
            while (pa != AST_NULL) {
                lower_expr(c, arena, pa);
                ZeroCCASTNode *an = zero_cc_ast_get(arena, pa);
                pa = (an && an->next != AST_NULL) ? an->next : AST_NULL;
            }
            c->last_expr_is_float = 0;
            *out_temp = VREG_0;
            return true;
        }

        /* __builtin_shuffle(a, b) - lane selection: result[i] =
         * (b[i] < n) ? a[b[i]] : b[b[i]-n], n = lanes of a (power of
         * two). Both operands are n-lane vectors (address form); b is
         * the index vector (any integer element width). GCC tolerates
         * out-of-range index values (folded through the low log2(2n)
         * bits), so lower branchlessly per lane on the raw index bits:
         *   m = b[i] & (n-1)              shared element offset
         *   from_a = ((b[i] & n) == 0)
         *   res = (sel & a[m]) | (~sel & b[m])  bit-level select (valid
         *   for integer and float lanes alike). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_shuffle") == 0) {
            ASTRef sh_a_ref = node->right;
            ZeroCCASTNode *sh_a = sh_a_ref ? zero_cc_ast_get(arena, sh_a_ref) : NULL;
            ZeroCCASTNode *sh_b = (sh_a && sh_a->next != AST_NULL) ?
                zero_cc_ast_get(arena, sh_a->next) : NULL;
            if (sh_a && sh_b) {
                ZeroCCType sh_a_t;
                memset(&sh_a_t, 0, sizeof(sh_a_t));
                bool have_a_t = false;
                TypeInfo *a_ti = ast_node_get_typeinfo(c, sh_a);
                ZeroCCType sh_el_t;
                if (a_ti && type_is_vector(a_ti)) {
                    sh_a_t.tag = TYPE_USER_0;
                    sh_a_t.size = (int32_t)type_size(a_ti);
                    if (a_ti->details.array.base) {
                        sh_el_t = typeinfo_to_zero_cc_type(a_ti->details.array.base);
                        sh_a_t.base = &sh_el_t;
                    }
                    have_a_t = true;
                } else if (sh_a->tag == NODE_IDENT) {
                    ZeroCCSym *as = zero_cc_find_local(c, sh_a->s.str);
                    ZeroCCGlobalVar *ag = as ? NULL : find_global_var(c, sh_a->s.str);
                    if (as && as->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&as->type)) {
                        sh_a_t = as->type; have_a_t = true;
                    } else if (ag && ag->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&ag->type)) {
                        sh_a_t = ag->type; have_a_t = true;
                    }
                }
                if (have_a_t && sh_a_t.size >= 1 && sh_a_t.size <= 128) {
                    size_t a_sz = (size_t)sh_a_t.size;
                    const ZeroCCType *ael = sh_a_t.base;
                    size_t elsz = (ael && ael->size > 0) ? ael->size : 4;
                    size_t n = a_sz / (elsz ? elsz : 1);
                    /* index element size from b */
                    size_t idsz = 4;
                    TypeInfo *b_ti = ast_node_get_typeinfo(c, sh_b);
                    if (b_ti && type_is_vector(b_ti) && b_ti->details.array.base) {
                        idsz = type_size(b_ti->details.array.base);
                    } else if (sh_b->tag == NODE_IDENT) {
                        ZeroCCSym *bs = zero_cc_find_local(c, sh_b->s.str);
                        if (bs && bs->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&bs->type) &&
                            bs->type.base && bs->type.base->size > 0) {
                            idsz = bs->type.base->size;
                        }
                    }
                    if (n >= 1 && (n & (n - 1)) == 0 && n <= 16) {
                        int a_addr = lower_expr(c, arena, sh_a_ref);
                        int b_addr = lower_expr(c, arena, sh_a->next);
                        c->current_stack_frame = (c->current_stack_frame + (int32_t)a_sz + 7) & ~7;
                        int32_t res_off = c->current_stack_frame;
                        int64_t bit_n = (int64_t)n;              /* k-th bit */
                        int64_t mask_nm1 = (int64_t)(n - 1);
                        for (size_t i = 0; i < n; i++) {
                            /* idx = b[i] (raw bits, width-correct load) */
                            int bi_off = zc_alloc_temp(c);
                            if (i == 0) { ir_emit_mov_reg(c, bi_off, b_addr); }
                            else {
                                int bo = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, bo, (int32_t)(i * idsz));
                                ir_emit_add(c, bi_off, b_addr, bo);
                            }
                            int idx = zc_alloc_temp(c);
                            if (idsz == 1) { ir_emit_load_byte(c, idx, bi_off); ir_emit_zext(c, idx, 1); }
                            else if (idsz == 2) { ir_emit_load_16(c, idx, bi_off); ir_emit_zext(c, idx, 2); }
                            else if (idsz == 4) { ir_emit_load_32(c, idx, bi_off); ir_emit_zext(c, idx, 4); }
                            else { ir_emit_deref(c, idx, bi_off); }
                            /* m = idx & (n-1) */
                            int m_mask = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, m_mask, mask_nm1);
                            int m = zc_alloc_temp(c);
                            ir_emit_and(c, m, idx, m_mask);
                            /* off = m * elsz */
                            int off = zc_alloc_temp(c);
                            if (elsz > 1) {
                                int es = zc_alloc_temp(c);
                                ir_emit_mov_imm(c, es, (int32_t)elsz);
                                ir_emit_mul(c, off, m, es);
                            } else {
                                ir_emit_mov_reg(c, off, m);
                            }
                            int aa = zc_alloc_temp(c);
                            ir_emit_add(c, aa, a_addr, off);
                            int ba = zc_alloc_temp(c);
                            ir_emit_add(c, ba, b_addr, off);
                            int av = zc_alloc_temp(c);
                            if (elsz == 1) { ir_emit_load_byte(c, av, aa); ir_emit_zext(c, av, 1); }
                            else if (elsz == 2) { ir_emit_load_16(c, av, aa); ir_emit_zext(c, av, 2); }
                            else if (elsz == 4) { ir_emit_load_32(c, av, aa); ir_emit_zext(c, av, 4); }
                            else { ir_emit_deref(c, av, aa); }
                            int bv = zc_alloc_temp(c);
                            if (elsz == 1) { ir_emit_load_byte(c, bv, ba); ir_emit_zext(c, bv, 1); }
                            else if (elsz == 2) { ir_emit_load_16(c, bv, ba); ir_emit_zext(c, bv, 2); }
                            else if (elsz == 4) { ir_emit_load_32(c, bv, ba); ir_emit_zext(c, bv, 4); }
                            else { ir_emit_deref(c, bv, ba); }
                            /* sel = (idx & n) == 0  ->  0/1 */
                            int n_bit = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, n_bit, bit_n);
                            int ib = zc_alloc_temp(c);
                            ir_emit_and(c, ib, idx, n_bit);
                            int zero_t = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, zero_t, 0);
                            ir_emit_cmp(c, (uint32_t)ib, (uint32_t)zero_t);
                            int cmp_t = zc_alloc_temp(c);
                            ir_emit_cond_set(c, (uint32_t)cmp_t, CC_EQ);
                            int sel = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, sel, -1);
                            int selm = zc_alloc_temp(c);
                            ir_emit_mul(c, selm, cmp_t, sel);
                            int nv = zc_alloc_temp(c);
                            int all_ones = zc_alloc_temp(c);
                            ir_emit_mov_imm(c, all_ones, -1);
                            ir_emit_xor(c, nv, selm, all_ones);
                            int s1 = zc_alloc_temp(c);
                            ir_emit_and(c, s1, selm, av);
                            int s2 = zc_alloc_temp(c);
                            ir_emit_and(c, s2, nv, bv);
                            int lv = zc_alloc_temp(c);
                            ir_emit_or(c, lv, s1, s2);
                            /* store lane i into the result temp */
                            int ra = zc_alloc_temp(c);
                            if (i == 0) {
                                ir_emit_lea(c, ra, BE(c)->fp_reg, -res_off);
                            } else {
                                ir_emit_lea(c, ra, BE(c)->fp_reg, -(int32_t)(res_off - (int32_t)(i * elsz)));
                            }
                            if (elsz == 1) ir_emit_store_byte(c, lv, ra);
                            else if (elsz == 2) ir_emit_store_16(c, lv, ra);
                            else if (elsz == 4) ir_emit_store_32(c, lv, ra);
                            else ir_emit_store_64(c, lv, ra);
                        }
                        int addr = zc_alloc_temp(c);
                        ir_emit_lea(c, addr, BE(c)->fp_reg, -res_off);
                        c->last_expr_is_float = 0;
                        *out_temp = addr;
                        return true;
                    }
                }
            }
        }

        /* __builtin_shufflevector(a, b, idx0, idx1, ...) - lane selection:
         * result[i] = (idx < n) ? a[idx] : (idx < 2n) ? b[idx - n] : 0.
         * The index arguments are compile-time integer constants. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_shufflevector") == 0) {
            ASTRef sh_a_ref = node->right;
            ZeroCCASTNode *sh_a = sh_a_ref ? zero_cc_ast_get(arena, sh_a_ref) : NULL;
            ZeroCCASTNode *sh_b = (sh_a && sh_a->next != AST_NULL) ?
                zero_cc_ast_get(arena, sh_a->next) : NULL;
            if (sh_a && sh_b) {
                ZeroCCType sh_a_t;
                memset(&sh_a_t, 0, sizeof(sh_a_t));
                bool have_a_t = false;
                TypeInfo *a_ti = ast_node_get_typeinfo(c, sh_a);
                ZeroCCType sh_el_t;
                if (a_ti && type_is_vector(a_ti)) {
                    sh_a_t.tag = TYPE_USER_0;
                    sh_a_t.size = (int32_t)type_size(a_ti);
                    if (a_ti->details.array.base) {
                        sh_el_t = typeinfo_to_zero_cc_type(a_ti->details.array.base);
                        sh_a_t.base = &sh_el_t;
                    }
                    have_a_t = true;
                } else if (sh_a->tag == NODE_IDENT) {
                    ZeroCCSym *as = zero_cc_find_local(c, sh_a->s.str);
                    ZeroCCGlobalVar *ag = as ? NULL : find_global_var(c, sh_a->s.str);
                    if (as && as->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&as->type)) {
                        sh_a_t = as->type; have_a_t = true;
                    } else if (ag && ag->type.tag == TYPE_USER_0 && zero_cc_type_is_vector(&ag->type)) {
                        sh_a_t = ag->type; have_a_t = true;
                    }
                }
                if (have_a_t && sh_a_t.size >= 1 && sh_a_t.size <= 128) {
                    size_t a_sz = (size_t)sh_a_t.size;
                    const ZeroCCType *ael = sh_a_t.base;
                    size_t elsz = (ael && ael->size > 0) ? ael->size : 4;
                    size_t n = a_sz / (elsz ? elsz : 1);

                    size_t num_indices = 0;
                    ASTRef cur_idx_ref = sh_b->next;
                    while (cur_idx_ref != AST_NULL) {
                        num_indices++;
                        ZeroCCASTNode *in = zero_cc_ast_get(arena, cur_idx_ref);
                        cur_idx_ref = in ? in->next : AST_NULL;
                    }

                    if (n >= 1 && num_indices >= 1 && num_indices <= 64) {
                        int a_addr = lower_expr(c, arena, sh_a_ref);
                        int b_addr = lower_expr(c, arena, sh_a->next);
                        size_t res_size = num_indices * elsz;
                        if (res_size > 16) c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
                        c->current_stack_frame = (c->current_stack_frame + (int32_t)res_size + 7) & ~7;
                        int32_t res_off = c->current_stack_frame;

                        cur_idx_ref = sh_b->next;
                        for (size_t k = 0; k < num_indices && cur_idx_ref != AST_NULL; k++) {
                            ZeroCCASTNode *in = zero_cc_ast_get(arena, cur_idx_ref);
                            int64_t idx_val = -1;
                            if (in && in->tag == NODE_INT) {
                                idx_val = in->int_val;
                            } else if (in) {
                                idx_val = (int64_t)lower_expr(c, arena, cur_idx_ref);
                            }

                            int val_lane = zc_alloc_temp(c);
                            if (idx_val >= 0 && (size_t)idx_val < n) {
                                int aa = zc_alloc_temp(c);
                                if (idx_val == 0) {
                                    ir_emit_mov_reg(c, aa, a_addr);
                                } else {
                                    int off = zc_alloc_temp(c);
                                    ir_emit_mov_imm(c, off, (int32_t)(idx_val * elsz));
                                    ir_emit_add(c, aa, a_addr, off);
                                }
                                if (elsz == 1) { ir_emit_load_byte(c, val_lane, aa); ir_emit_zext(c, val_lane, 1); }
                                else if (elsz == 2) { ir_emit_load_16(c, val_lane, aa); ir_emit_zext(c, val_lane, 2); }
                                else if (elsz == 4) { ir_emit_load_32(c, val_lane, aa); ir_emit_zext(c, val_lane, 4); }
                                else { ir_emit_deref(c, val_lane, aa); }
                            } else if (idx_val >= (int64_t)n && (size_t)idx_val < 2 * n) {
                                size_t b_idx = (size_t)idx_val - n;
                                int ba = zc_alloc_temp(c);
                                if (b_idx == 0) {
                                    ir_emit_mov_reg(c, ba, b_addr);
                                } else {
                                    int off = zc_alloc_temp(c);
                                    ir_emit_mov_imm(c, off, (int32_t)(b_idx * elsz));
                                    ir_emit_add(c, ba, b_addr, off);
                                }
                                if (elsz == 1) { ir_emit_load_byte(c, val_lane, ba); ir_emit_zext(c, val_lane, 1); }
                                else if (elsz == 2) { ir_emit_load_16(c, val_lane, ba); ir_emit_zext(c, val_lane, 2); }
                                else if (elsz == 4) { ir_emit_load_32(c, val_lane, ba); ir_emit_zext(c, val_lane, 4); }
                                else { ir_emit_deref(c, val_lane, ba); }
                            } else {
                                ir_emit_mov_imm(c, val_lane, 0);
                            }

                            int ra = zc_alloc_temp(c);
                            if (k == 0) {
                                ir_emit_lea(c, ra, BE(c)->fp_reg, -res_off);
                            } else {
                                ir_emit_lea(c, ra, BE(c)->fp_reg, -(int32_t)(res_off - (int32_t)(k * elsz)));
                            }
                            if (elsz == 1) ir_emit_store_byte(c, val_lane, ra);
                            else if (elsz == 2) ir_emit_store_16(c, val_lane, ra);
                            else if (elsz == 4) ir_emit_store_32(c, val_lane, ra);
                            else ir_emit_store_64(c, val_lane, ra);

                            cur_idx_ref = in ? in->next : AST_NULL;
                        }

                        int addr = zc_alloc_temp(c);
                        ir_emit_lea(c, addr, BE(c)->fp_reg, -res_off);
                        c->last_expr_is_float = 0;
                        *out_temp = addr;
                        return true;
                    }
                }
            }
        }

        /* __builtin_unreachable() / __builtin_trap() - no value, no libc
         * symbol: emit the target's trap instruction (x86-64 ud2,
         * AArch64 brk #0, RISC-V unimp). Reaching __builtin_unreachable
         * is UB; __builtin_trap guarantees a fault either way. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_unreachable") == 0 ||
             strcmp(callee->s.str, "__builtin_trap") == 0)) {
            ir_emit_trap(c);
            c->last_expr_is_float = 0;
            *out_temp = VREG_0;
            return true;
        }

        /* __builtin_{ffs,ctz,clz,clrsb,popcount,parity}{,l,ll}(x) -
         * 32-bit and 64-bit integer bit manipulation builtins.
         * Constant arguments fold at compile time; runtime arguments
         * use branchless binary-search / SWAR sequences (~20-40 IR ops). */
        if (callee_is_ident && callee->s.str &&
            zc_is_bitop_builtin_name(callee->s.str)) {
            ASTRef b_arg = node->right;
            const char *bname = callee->s.str;
            int nbits = zc_bitop_width(c, bname);
            int64_t cval = 0;
            if (zc_ast_const_eval(c, arena, b_arg, &cval)) {
                int folded = zc_eval_bitop_const(bname, (uint64_t)cval, nbits);
                int t_res = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_res, folded);
                c->last_expr_is_float = 0;
                *out_temp = t_res;
                return true;
            }
            int t_x = lower_expr(c, arena, b_arg);
            int t_result = 0;
            if (strncmp(bname, "__builtin_ctz", 13) == 0) {
                t_result = zc_emit_ctz_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_clz", 13) == 0) {
                t_result = zc_emit_clz_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_clrsb", 15) == 0) {
                t_result = zc_emit_clrsb_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_popcount", 18) == 0) {
                t_result = zc_emit_popcount_nbits(c, t_x, nbits);
            } else if (strncmp(bname, "__builtin_parity", 16) == 0) {
                int t_pc = zc_emit_popcount_nbits(c, t_x, nbits);
                int t_one = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_one, 1);
                t_result = zc_alloc_temp(c);
                ir_emit(c, (Instr){.op = OP_AND, .temp_dest = t_result,
                                   .temp_src1 = t_pc, .temp_src2 = t_one});
            } else {
                /* __builtin_ffs / ffsl / ffsll */
                int t_in = zc_alloc_temp(c);
                ir_emit_mov_reg(c, t_in, t_x);
                if (nbits == 32) ir_emit_zext(c, t_in, 4);
                int t_ctz = zc_emit_ctz_nbits(c, t_in, nbits);
                int t_one = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_one, 1);
                int t_zero = zc_alloc_temp(c);
                ir_emit_mov_imm(c, t_zero, 0);
                int t_plus1 = zc_alloc_temp(c);
                ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = t_plus1,
                                   .temp_src1 = t_ctz, .temp_src2 = t_one});
                int t_nz = zc_alloc_temp(c);
                ir_emit_cmp(c, t_in, t_zero);
                ir_emit(c, (Instr){.op = OP_COND_SET, .temp_dest = t_nz,
                                   .temp_src1 = t_in, .temp_src2 = t_zero,
                                   .cond = CC_NE});
                t_result = zc_alloc_temp(c);
                ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = t_result,
                                   .temp_src1 = t_plus1, .temp_src2 = t_nz});
            }
            c->last_expr_is_float = 0;
            *out_temp = t_result;
            return true;
        }

        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_va_arg_pack") == 0 ||
             strcmp(callee->s.str, "__builtin_va_arg_pack_len") == 0)) {
            int temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, temp, 0);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
        }

        /* __builtin_constant_p(expr) returns 1 if expr is a compile-time
         * constant, 0 otherwise (execute/bcp-1). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_constant_p") == 0) {
            ASTRef cp_arg = node->right;
            int res = zc_node_is_const_expr(c, arena, cp_arg) ? 1 : 0;
            int temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, temp, res);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
        }

        /* __alignof__(E) / _Alignof(E) (C99 6.5.3.4): the alignment of
         * E's type.  A function name operand uses the function's
         * __attribute__((aligned(N))) when present (execute/align-3). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__alignof__") == 0 ||
             strcmp(callee->s.str, "_Alignof") == 0 ||
             strcmp(callee->s.str, "__builtin_alignof") == 0)) {
            size_t al = 0;
            ZeroCCFunction *aln_fn = NULL;
            if (callee_is_ident && node->right != AST_NULL) {
                ZeroCCASTNode *an = zero_cc_ast_get(arena, node->right);
                if (an && an->tag == NODE_IDENT && an->s.str) {
                    aln_fn = zero_cc_find_function(c, an->s.str);
                }
                if (!aln_fn) {
                    TypeInfo *at = ast_node_get_typeinfo(c, an);
                    if (at) {
                        ZeroCCType aty = typeinfo_to_zero_cc_type(at);
                        if (aty.align > 0) al = aty.align;
                    }
                }
            }
            if (aln_fn) al = aln_fn->align > 0 ? aln_fn->align : 1;
            if (al == 0) al = 8;
            int temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, temp, (int32_t)al);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
        }

        /* __builtin_choose_expr(cond, a, b) - the condition is a
         * constant expression (it may itself contain
         * __builtin_constant_p).  When it folds at compile time, lower
         * ONLY the selected operand (execute/pr19449: the unselected
         * `y++` must not be evaluated).  A genuinely dynamic condition
         * evaluates both operands and selects at runtime. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_choose_expr") == 0) {
            ASTRef r1 = node->right;
            ZeroCCASTNode *an1 = (r1 != AST_NULL) ? zero_cc_ast_get(arena, r1) : NULL;
            ASTRef r2 = an1 ? an1->next : AST_NULL;
            ZeroCCASTNode *an2 = (r2 != AST_NULL) ? zero_cc_ast_get(arena, r2) : NULL;
            ASTRef r3 = an2 ? an2->next : AST_NULL;
            int64_t cval = 0;
            if (zc_ast_const_eval(c, arena, r1, &cval) &&
                r2 != AST_NULL && r3 != AST_NULL) {
                int sel = lower_expr(c, arena, cval ? r2 : r3);
                *out_temp = sel;
                return true;
            }
            /* Dynamic: evaluate both, select with a conditional
             * move (cv != 0 ? bv : av). */
            int cv = lower_expr(c, arena, r1);
            int av = (r2 != AST_NULL) ? lower_expr(c, arena, r2) : 0;
            int bv = (r3 != AST_NULL) ? lower_expr(c, arena, r3) : 0;
            int sel = zc_alloc_temp(c);
            ir_emit_mov_reg(c, sel, av);
            int zt = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zt, 0);
            ir_emit_cmp(c, cv, zt);
            int nz = zc_alloc_temp(c);
            ir_emit_cond_set(c, (uint32_t)nz, CC_NE);
            uint16_t done_lbl = (uint32_t)c->ir_next_label++;
            ir_emit_je(c, (uint32_t)nz, done_lbl);
            ir_emit_mov_reg(c, sel, bv);
            ir_emit_label(c, done_lbl);
            c->last_expr_is_float = 0;
            *out_temp = sel;
            return true;
        }

        /* __builtin_classify_type(expr) returns the GCC type_class enum code:
         * no_type_class = -1, void = 0, integer = 1, char = 2, enum = 3,
         * boolean = 4, pointer = 5, real = 8, complex = 9, function = 10,
         * record (struct) = 12, union = 13, array = 14. */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_classify_type") == 0) {
            ASTRef ct_arg = node->right;
            ZeroCCASTNode *an = zero_cc_ast_get(arena, ct_arg);
            TypeInfo *ti = an ? ast_node_get_typeinfo(c, an) : NULL;
            int class_id = 1; /* default integer */
            if (ti) {
                if (type_is_floating(ti)) class_id = 8;
                else if (type_is_pointer(ti)) class_id = 5;
                else if (ti->kind == TYPE_KIND_ARRAY) class_id = 14;
                else if (ti->kind == TYPE_KIND_STRUCT) class_id = 12;
                else if (ti->kind == TYPE_KIND_UNION) class_id = 13;
                else if (ti->kind == TYPE_KIND_VOID) class_id = 0;
                else if (ti->kind == TYPE_KIND_BOOL) class_id = 4;
                else if (ti->kind == TYPE_KIND_CHAR) class_id = 2;
                else if (ti->kind == TYPE_KIND_ENUM) class_id = 3;
                else if (ti->kind == TYPE_KIND_FUNCTION) class_id = 10;
                else class_id = 1;
            } else if (an) {
                if (an->tag == NODE_FLOAT) class_id = 8;
                else if (an->tag == NODE_STRING) class_id = 5;
            }
            int temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, temp, class_id);
            c->last_expr_is_float = 0;
            *out_temp = temp;
            return true;
        }

        /* __builtin_bswap16(x) - byte-swap the low 16 bits:
         * result = ((x & 0xFF) << 8) | ((x >> 8) & 0xFF).
         * (c-torture execute/pr97386-1) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap16") == 0) {
            ASTRef b16_arg = node->right;
            int u_a = lower_expr(c, arena, b16_arg);
            int u_ff = zc_alloc_temp(c); ir_emit_mov_imm(c, u_ff, 0xFF);
            int u_8  = zc_alloc_temp(c); ir_emit_mov_imm(c, u_8,  8);
            int u_b0 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)u_b0,
                .temp_src1=(uint32_t)u_a,.temp_src2=(uint32_t)u_ff});
            int u_a8 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint32_t)u_a8,
                .temp_src1=(uint32_t)u_a,.temp_src2=(uint32_t)u_8});
            int u_b1 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)u_b1,
                .temp_src1=(uint32_t)u_a8,.temp_src2=(uint32_t)u_ff});
            int u_b0s = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint32_t)u_b0s,
                .temp_src1=(uint32_t)u_b0,.temp_src2=(uint32_t)u_8});
            int u_result = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint32_t)u_result,
                .temp_src1=(uint32_t)u_b0s,.temp_src2=(uint32_t)u_b1});
            c->last_expr_is_float = 0;
            *out_temp = u_result;
            return true;
        }

        /* __builtin_bswap32(x) - byte-swap a 32-bit value.
         * result = ((x & 0xFF) << 24) | ((x >> 8) & 0xFF) << 16 |
         *          ((x >> 16) & 0xFF) << 8  | (x >> 24) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap32") == 0) {
            ASTRef b_arg = node->right;
            int t_a = lower_expr(c, arena, b_arg);
            int t_ff = zc_alloc_temp(c); ir_emit_mov_imm(c, t_ff, 0xFF);
            int t_8  = zc_alloc_temp(c); ir_emit_mov_imm(c, t_8,  8);
            int t_16 = zc_alloc_temp(c); ir_emit_mov_imm(c, t_16, 16);
            int t_24 = zc_alloc_temp(c); ir_emit_mov_imm(c, t_24, 24);
            int t_b0 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)t_b0,
                .temp_src1=(uint32_t)t_a,.temp_src2=(uint32_t)t_ff});
            int t_a8 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint32_t)t_a8,
                .temp_src1=(uint32_t)t_a,.temp_src2=(uint32_t)t_8});
            int t_b1 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)t_b1,
                .temp_src1=(uint32_t)t_a8,.temp_src2=(uint32_t)t_ff});
            int t_a16 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint32_t)t_a16,
                .temp_src1=(uint32_t)t_a,.temp_src2=(uint32_t)t_16});
            int t_b2 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)t_b2,
                .temp_src1=(uint32_t)t_a16,.temp_src2=(uint32_t)t_ff});
            int t_a24 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint32_t)t_a24,
                .temp_src1=(uint32_t)t_a,.temp_src2=(uint32_t)t_24});
            int t_b3 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)t_b3,
                .temp_src1=(uint32_t)t_a24,.temp_src2=(uint32_t)t_ff});
            int t_b0s = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint32_t)t_b0s,
                .temp_src1=(uint32_t)t_b0,.temp_src2=(uint32_t)t_24});
            int t_b1s = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint32_t)t_b1s,
                .temp_src1=(uint32_t)t_b1,.temp_src2=(uint32_t)t_16});
            int t_b2s = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint32_t)t_b2s,
                .temp_src1=(uint32_t)t_b2,.temp_src2=(uint32_t)t_8});
            int t_r1 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint32_t)t_r1,
                .temp_src1=(uint32_t)t_b0s,.temp_src2=(uint32_t)t_b1s});
            int t_r2 = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint32_t)t_r2,
                .temp_src1=(uint32_t)t_r1,.temp_src2=(uint32_t)t_b2s});
            int t_result = zc_alloc_temp(c);
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint32_t)t_result,
                .temp_src1=(uint32_t)t_r2,.temp_src2=(uint32_t)t_b3});
            c->last_expr_is_float = 0;
            *out_temp = t_result;
            return true;
        }

        /* __builtin_inf{,f,l,f16,f32,f64,f128,f32x,f64x,f128x,f16b}()
         * / __builtin_nan.../ __builtin_nans...(payload string): FP
         * special constants (c-torture ieee/copysign1/2 + the
         * *_builtin-issignaling-1 family). The string argument (when
         * present) is ignored. ZeroCC models every FP width (float,
         * double, long double, _FloatN, __bf16) as 8-byte double, so
         * every spelling emits the same double-width bit pattern -
         * the caller's conversion does any narrowing. Previously the
         * suffixed variants were emitted as undefined external calls
         * (link failure). */
        if (callee_is_ident && callee->s.str) {
            const char *fn159 = callee->s.str;
            if ((strncmp(fn159, "__builtin_nans", 14) == 0 &&
                 zc_fp_special_suffix(fn159 + 14)) ||
                (strncmp(fn159, "__builtin_nan", 13) == 0 &&
                 zc_fp_special_suffix(fn159 + 13)) ||
                (strncmp(fn159, "__builtin_inf", 13) == 0 &&
                 zc_fp_special_suffix(fn159 + 13))) {
                bool is_nan = (fn159[10] == 'n');
                bool is_signaling = (strncmp(fn159, "__builtin_nans", 14) == 0);
            /* qNaN: exp all-ones, quiet bit set, payload 1;
             * sNaN: exp all-ones, quiet bit CLEAR, payload 1;
             * inf: exp all-ones, mantissa 0. */
            uint64_t fpbits = !is_nan ? 0x7FF0000000000000ULL
                          : is_signaling ? 0x7FF0000000000001ULL
                                         : 0x7FF8000000000000ULL;
            double fpc; memcpy(&fpc, &fpbits, sizeof(fpc));
            size_t fpad = (8 - (c->rodata_section.length % 8)) % 8;
            if (fpad) {
                uint8_t z8[8] = {0};
                zero_cc_buf_put_bytes(&c->rodata_section, z8, fpad);
            }
            size_t fro_off = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &fpc, sizeof(double));
            int ft = zc_alloc_temp(c);
            ir_emit_fmov_imm(c, (uint32_t)ft, (int64_t)fro_off);
            c->last_expr_is_float = 1;
            *out_temp = ft;
            return true;
            }
        }

        /* __builtin_issignaling(x) - 1 iff x is a SIGNALING NaN
         * (exponent all-ones, mantissa non-zero, quiet bit clear).
         * C23 7.12.1.4; all width spellings share the name (GCC
         * overloads by the argument type; ZeroCC holds every FP value
         * as 64-bit double, so the double bit test is the test).
         * (c-torture execute/ieee/*-builtin-issignaling-1.) */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_issignaling") == 0) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint32_t)arg_temp, TEMP_NONE, -slot);
            int bits = zc_alloc_temp(c);
            ir_emit_load(c, bits, BE(c)->fp_reg, -slot);   /* bit pattern */
            int t_exp = zc_alloc_temp(c);
            int sh1 = zc_alloc_temp(c); ir_emit_mov_imm(c, sh1, 52);
            ir_emit_shr(c, t_exp, bits, sh1);
            int m1 = zc_alloc_temp(c); ir_emit_mov_imm(c, m1, 0x7FF);
            ir_emit_and(c, t_exp, t_exp, m1);
            int eq_exp = zc_ovf_cmp_set_imm(c, t_exp, 0x7FF, CC_EQ);
            int t_man = zc_alloc_temp(c);
            int m2 = zc_alloc_temp(c); ir_emit_mov_imm(c, m2, (int64_t)0x000FFFFFFFFFFFFFULL);
            ir_emit_and(c, t_man, bits, m2);
            int ne_man = zc_ovf_cmp_set_imm(c, t_man, 0, CC_NE);
            int t_q = zc_alloc_temp(c);
            int sh2 = zc_alloc_temp(c); ir_emit_mov_imm(c, sh2, 51);
            ir_emit_shr(c, t_q, bits, sh2);
            int m3 = zc_alloc_temp(c); ir_emit_mov_imm(c, m3, 1);
            ir_emit_and(c, t_q, t_q, m3);
            int quiet = zc_ovf_cmp_set_imm(c, t_q, 0, CC_EQ);
            int res = zc_ovf_and(c, zc_ovf_and(c, eq_exp, ne_man), quiet);
            c->last_expr_is_float = 0;
            *out_temp = res;
            return true;
        }

        /* __builtin_bswap64(x) - byte-swap a 64-bit value:
         * byte i of the result is byte (7-i) of x.
         * (c-torture execute/bswap-1.c - previously emitted as an
         * undefined external call and segfaulted at runtime). */
        if (callee_is_ident && callee->s.str &&
            strcmp(callee->s.str, "__builtin_bswap64") == 0) {
            ASTRef b64_arg = node->right;
            int s_a = lower_expr(c, arena, b64_arg);
            int s_ff = zc_alloc_temp(c); ir_emit_mov_imm(c, s_ff, 0xFF);
            int s_b[8];
            int sh[8] = {0, 8, 16, 24, 32, 40, 48, 56};
            int shd[8] = {56, 48, 40, 32, 24, 16, 8, 0};
            for (int i = 0; i < 8; i++) {
                int s_s = zc_alloc_temp(c); ir_emit_mov_imm(c, s_s, sh[i]);
                int s_t = zc_alloc_temp(c);
                ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint32_t)s_t,
                    .temp_src1=(uint32_t)s_a,.temp_src2=(uint32_t)s_s});
                s_b[i] = zc_alloc_temp(c);
                ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint32_t)s_b[i],
                    .temp_src1=(uint32_t)s_t,.temp_src2=(uint32_t)s_ff});
            }
            int s_r = zc_alloc_temp(c);
            for (int i = 0; i < 8; i++) {
                int s_s = zc_alloc_temp(c); ir_emit_mov_imm(c, s_s, shd[i]);
                int s_t = zc_alloc_temp(c);
                ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint32_t)s_t,
                    .temp_src1=(uint32_t)s_b[i],.temp_src2=(uint32_t)s_s});
                if (i == 0) {
                    ir_emit_mov_reg(c, s_r, s_t);
                    continue;
                }
                ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint32_t)s_r,
                    .temp_src1=(uint32_t)s_r,.temp_src2=(uint32_t)s_t});
            }
            c->last_expr_is_float = 0;
            *out_temp = s_r;
            return true;
        }

        /* __builtin_huge_val{,f,l}() - infinity (C11 <math.h> macros;
         * the __builtin_inf* spellings are handled by the FP
         * special-constant matcher above). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_huge_val") == 0 ||
             strcmp(callee->s.str, "__builtin_huge_valf") == 0 ||
             strcmp(callee->s.str, "__builtin_huge_vall") == 0)) {
            /* Infinity bit pattern: sign=0, exp=all-ones, mantissa=0 */
            union { double d; int64_t i; } u;
            u.i = 0x7FF0000000000000LL;
            double inf = u.d;
            size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
            if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &inf, sizeof(double));
            int temp = zc_alloc_temp(c);
            ir_emit_fmov_imm(c, (uint32_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            *out_temp = temp;
            return true;
        }

        /* llabs() / labs() / imaxabs() / abs() / uabs() / ulabs() /
         * ullabs() / umaxabs() - integer absolute value.
         *
         * uabs/ulabs (c-torture execute/builtins/uabs-1..3): GCC treats
         * them as builtins too - the same inline abs sequence, the result
         * just has the unsigned type (the |INT_MIN| bit pattern is the
         * correct unsigned value, so the shared lowering is exact).
         *
         * GCC semantics (calibrated against gcc-14 at -O0/-O1/-O2, 2026):
         * the builtin is used at every builtin-recognized call site -
         * even when the program declares/defines the function itself and
         * even for non-constant (volatile, global) arguments - as long as
         * it is not disabled with -fno-builtin / -fno-builtin-<name>.
         * GCC emits the inline abs sequence (neg/cmovs) at every -O level
         * and never calls the user's definition from those sites (the user
         * function still exists in the binary; -fno-builtin[-name] forces
         * the real call in every case, even for constant arguments). The
         * explicit __builtin_<name> form always uses the builtin, even
         * under -fno-builtin. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_llabs") == 0 ||
             strcmp(callee->s.str, "llabs") == 0 ||
             strcmp(callee->s.str, "__builtin_labs") == 0 ||
             strcmp(callee->s.str, "labs") == 0 ||
             strcmp(callee->s.str, "__builtin_ulabs") == 0 ||
             strcmp(callee->s.str, "ulabs") == 0 ||
             strcmp(callee->s.str, "__builtin_ullabs") == 0 ||
             strcmp(callee->s.str, "ullabs") == 0 ||
             strcmp(callee->s.str, "__builtin_umaxabs") == 0 ||
             strcmp(callee->s.str, "umaxabs") == 0 ||
             strcmp(callee->s.str, "__builtin_imaxabs") == 0 ||
             strcmp(callee->s.str, "imaxabs") == 0 ||
             strcmp(callee->s.str, "__builtin_abs") == 0 ||
             strcmp(callee->s.str, "abs") == 0 ||
             strcmp(callee->s.str, "__builtin_uabs") == 0 ||
             strcmp(callee->s.str, "uabs") == 0)) {
            const char *bname = callee->s.str;
            const char *bplain =
                (strncmp(bname, "__builtin_", 10) == 0) ? bname + 10 : bname;
            bool b_builtin_form = (bname != bplain);
            bool b_builtin = b_builtin_form || !zero_cc_no_builtin(c, bplain);
            if (b_builtin) {
            bool is_32bit = (strcmp(callee->s.str, "__builtin_abs") == 0 ||
                             strcmp(callee->s.str, "abs") == 0 ||
                             strcmp(callee->s.str, "__builtin_uabs") == 0 ||
                             strcmp(callee->s.str, "uabs") == 0);
            /* uabs/ulabs return the UNSIGNED type: the 32-bit result must
             * be zero-extended, not sign-extended, or a consumer in an
             * unsigned context (cmp against 1U+INT_MAX etc.) sees
             * 0xFFFFFFFF80000000 where 0x0000000080000000 belongs
             * (c-torture uabs-2 `uabs(imin-1) != 1U+INT_MAX`).  The 64-bit
             * paths need no tail extend (the 64-bit abs bits are correct
             * for both readings, incl. LONG_MIN). */
            bool is_uabs32 = (strcmp(callee->s.str, "__builtin_uabs") == 0 ||
                              strcmp(callee->s.str, "uabs") == 0);
            int arg_temp = lower_expr(c, arena, node->right);
            if (is_32bit) {
                ir_emit_sext(c, arg_temp, 4);
            }
            int zero_temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zero_temp, 0);
            int cond_temp = zc_alloc_temp(c);
            ir_emit_cmp(c, arg_temp, zero_temp);
            ir_emit_cond_set(c, cond_temp, CC_LT);
            uint16_t pos_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;
            ir_emit_je(c, (uint32_t)cond_temp, pos_label);
            int neg_temp = zc_alloc_temp(c);
            ir_emit_sub(c, neg_temp, zero_temp, arg_temp);
            int res_temp = zc_alloc_temp(c);
            ir_emit_mov_reg(c, res_temp, neg_temp);
            ir_emit_jmp(c, end_label);
            ir_emit_label(c, pos_label);
            ir_emit_mov_reg(c, res_temp, arg_temp);
            ir_emit_label(c, end_label);
            if (is_32bit) {
                if (is_uabs32) ir_emit_zext(c, res_temp, 4);
                else ir_emit_sext(c, res_temp, 4);
            }
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
            } /* b_builtin */
        }

        /* __builtin_fabs() / __builtin_fabsf() - absolute value.
         * Clears the sign bit (bit 63) in IEEE 754 representation. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_fabs") == 0 ||
             strcmp(callee->s.str, "__builtin_fabsf") == 0 ||
             strcmp(callee->s.str, "__builtin_fabsl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint32_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = zc_alloc_temp(c);
            ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
            int mask_temp = zc_alloc_temp(c);
            ir_emit_mov_imm(c, mask_temp, 0x7FFFFFFFFFFFFFFFLL);
            int abs_int = zc_alloc_temp(c);
            ir_emit_and(c, abs_int, int_temp, mask_temp);
            ir_emit_store(c, abs_int, BE(c)->fp_reg, -slot);
            int res_temp = zc_alloc_temp(c);
            ir_emit_fload(c, (uint32_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_isinf() / __builtin_isinff() / isinf() - infinity test.
         * In IEEE 754, x != 0.0 && (x * 2.0 == x) holds iff x is +/-inf. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_isinf") == 0 ||
             strcmp(callee->s.str, "__builtin_isinff") == 0 ||
             strcmp(callee->s.str, "__builtin_isinfl") == 0 ||
             (!zero_cc_find_local(c, callee->s.str) &&
              (strcmp(callee->s.str, "isinf") == 0 ||
               strcmp(callee->s.str, "isinff") == 0 ||
               strcmp(callee->s.str, "isinfl") == 0)))) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)arg_temp);
                arg_temp = conv;
            }
            int two_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, two_i, 2);
            int two_f = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)two_f, (uint32_t)two_i);

            int zero_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)zero_f, (uint32_t)zero_i);

            int mul_temp = zc_alloc_temp(c);
            ir_emit_fmul(c, (uint32_t)mul_temp, (uint32_t)arg_temp, (uint32_t)two_f);

            int c1 = zc_alloc_temp(c);
            ir_emit_cmp(c, arg_temp, zero_f);
            ir_emit_cond_set(c, c1, CC_NE);

            int c2 = zc_alloc_temp(c);
            ir_emit_cmp(c, mul_temp, arg_temp);
            ir_emit_cond_set(c, c2, CC_EQ);

            int res_temp = zc_alloc_temp(c);
            ir_emit_and(c, res_temp, c1, c2);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_signbit() / __builtin_signbitf() - sign bit test.
         * Returns 1 if sign bit is set (negative), 0 otherwise. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_signbit") == 0 ||
             strcmp(callee->s.str, "__builtin_signbitf") == 0 ||
             strcmp(callee->s.str, "__builtin_signbitl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint32_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = zc_alloc_temp(c);
            ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
            int shift_imm = zc_alloc_temp(c);
            ir_emit_mov_imm(c, shift_imm, 63);
            int res_temp = zc_alloc_temp(c);
            ir_emit_shr(c, res_temp, int_temp, shift_imm);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* IEEE 754-2008 comparison predicates (C11 <math.h>), also
         * available as GCC builtins.  All reduce to ordinary FP
         * compares: in IEEE 754 a relational compare being true
         * already implies both operands are ordered (non-NaN).
         *   isless(x,y)         == x < y
         *   islessequal(x,y)    == x <= y
         *   isgreater(x,y)      == x > y
         *   isgreaterequal(x,y) == x >= y
         *   islessgreater(x,y)  == (x < y) || (x > y)
         *   isunordered(x,y)    == !((x <= y) || (x >= y))
         * Return int 0/1; the f/l width variants are accepted
         * (ZeroCC holds all FP values as 64-bit double).  Note the
         * prefix order below: islessequal/islessgreater must be
         * tested before isless, isgreaterequal before isgreater. */
        if (callee_is_ident && callee->s.str) {
            const char *bn = callee->s.str;
            if (strncmp(bn, "__builtin_islessequal", 19) == 0 ||
                strncmp(bn, "__builtin_islessgreater", 21) == 0 ||
                strncmp(bn, "__builtin_isunordered", 19) == 0 ||
                strncmp(bn, "__builtin_isless", 14) == 0 ||
                strncmp(bn, "__builtin_isgreater", 17) == 0 ||
                strncmp(bn, "__builtin_isgreaterequal", 22) == 0) {
                ASTRef ia1 = node->right;
                ZeroCCASTNode *a1n = zero_cc_ast_get(arena, ia1);
                ASTRef ia2 = (a1n && a1n->next != AST_NULL) ? a1n->next : AST_NULL;
                int t_x = lower_expr(c, arena, ia1);
                if (!c->last_expr_is_float) {
                    int conv = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_x);
                    t_x = conv;
                }
                int t_y = lower_expr(c, arena, ia2);
                if (!c->last_expr_is_float) {
                    int conv = zc_alloc_temp(c);
                    ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_y);
                    t_y = conv;
                }
                int res_temp = zc_alloc_temp(c);
                if (strncmp(bn, "__builtin_isunordered", 19) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, 12); /* CC_FP_UNORD */
                } else if (strncmp(bn, "__builtin_islessgreater", 21) == 0) {
                    int t_lt = zc_alloc_temp(c);
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, t_lt, CC_LT);
                    int t_gt = zc_alloc_temp(c);
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, t_gt, CC_GT);
                    ir_emit_or(c, res_temp, t_lt, t_gt);
                } else if (strncmp(bn, "__builtin_islessequal", 19) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_LE);
                } else if (strncmp(bn, "__builtin_isgreaterequal", 22) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_GE);
                } else if (strncmp(bn, "__builtin_isless", 14) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_LT);
                } else { /* __builtin_isgreater[f/l] */
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, CC_GT);
                }
                c->last_expr_is_float = 0;
                *out_temp = res_temp;
                return true;
            }
        }

        /* __builtin_isnan(x) / isnan(x) - NaN is the only value that is not
         * equal to itself. */
        if (callee_is_ident && callee->s.str &&
            (strncmp(callee->s.str, "__builtin_isnan", 13) == 0 ||
             (!zero_cc_find_local(c, callee->s.str) &&
              (strcmp(callee->s.str, "isnan") == 0 ||
               strcmp(callee->s.str, "isnanf") == 0 ||
               strcmp(callee->s.str, "isnanl") == 0)))) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_x);
                t_x = conv;
            }
            int res_temp = zc_alloc_temp(c);
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, res_temp, 12); /* CC_FP_UNORD (setp) */
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_isfinite(x) - neither inf nor NaN:
         * (x * 0 == 0) && (x == x).  x*0 is NaN for inf/NaN (unordered
         * -> false), 0 for finite (ordered -> true). */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isfinite", 18) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_x);
                t_x = conv;
            }
            int zero_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)zero_f, (uint32_t)zero_i);
            int t_x0 = zc_alloc_temp(c);
            ir_emit_fmul(c, (uint32_t)t_x0, (uint32_t)t_x, (uint32_t)zero_f);
            int t_zero = zc_alloc_temp(c);
            ir_emit_cmp(c, t_x0, zero_f);
            ir_emit_cond_set(c, t_zero, CC_EQ);
            int t_eq = zc_alloc_temp(c);
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, t_eq, CC_EQ);
            int res_temp = zc_alloc_temp(c);
            ir_emit_and(c, res_temp, t_zero, t_eq);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_isnormal(x) - neither zero, subnormal, inf, nor
         * NaN: x != 0 && x == x && x/2^30 != x (subnormals flush to 0
         * at /2^30; inf scales to itself). */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isnormal", 18) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_x);
                t_x = conv;
            }
            int zero_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)zero_f, (uint32_t)zero_i);
            int p230_i = zc_alloc_temp(c);
            ir_emit_mov_imm(c, p230_i, 1073741824); /* 2^30 */
            int p230_f = zc_alloc_temp(c);
            ir_emit_cvtsi2sd(c, (uint32_t)p230_f, (uint32_t)p230_i);
            int t_xd = zc_alloc_temp(c);
            ir_emit_fdiv(c, (uint32_t)t_xd, (uint32_t)t_x, (uint32_t)p230_f);
            int t_nz = zc_alloc_temp(c);
            ir_emit_cmp(c, t_x, zero_f);
            ir_emit_cond_set(c, t_nz, CC_NE);
            int t_ne = zc_alloc_temp(c);
            ir_emit_cmp(c, t_xd, t_x);
            ir_emit_cond_set(c, t_ne, CC_NE);
            int t_eq = zc_alloc_temp(c);
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, t_eq, CC_EQ);
            int t_a = zc_alloc_temp(c);
            ir_emit_and(c, t_a, t_nz, t_ne);
            int res_temp = zc_alloc_temp(c);
            ir_emit_and(c, res_temp, t_a, t_eq);
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_copysign(x, y) - magnitude of x with the sign
         * of y (IEEE 754-2008).  Bit surgery: clear x's sign bit and
         * set it from y's. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_copysign", 16) == 0) {
            ASTRef ca1 = node->right;
            ZeroCCASTNode *ca1n = zero_cc_ast_get(arena, ca1);
            ASTRef ca2 = (ca1n && ca1n->next != AST_NULL) ? ca1n->next : AST_NULL;
            int t_x = lower_expr(c, arena, ca1);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_x);
                t_x = conv;
            }
            int t_y = lower_expr(c, arena, ca2);
            if (!c->last_expr_is_float) {
                int conv = zc_alloc_temp(c);
                ir_emit_cvtsi2sd(c, (uint32_t)conv, (uint32_t)t_y);
                t_y = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint32_t)t_y, TEMP_NONE, -slot);
            int y_int = zc_alloc_temp(c);
            ir_emit_load(c, y_int, BE(c)->fp_reg, -slot);
            int sh63 = zc_alloc_temp(c);
            ir_emit_mov_imm(c, sh63, 63);
            int y_sign = zc_alloc_temp(c);
            ir_emit_shr(c, y_sign, y_int, sh63);
            int sign_mask = zc_alloc_temp(c);
            ir_emit_shl(c, sign_mask, y_sign, sh63);
            ir_emit_fstore(c, (uint32_t)t_x, TEMP_NONE, -slot);
            int x_int = zc_alloc_temp(c);
            ir_emit_load(c, x_int, BE(c)->fp_reg, -slot);
            int nosig_mask = zc_alloc_temp(c);
            ir_emit_mov_imm(c, nosig_mask, 0x7FFFFFFFFFFFFFFFLL);
            int res_int = zc_alloc_temp(c);
            ir_emit_and(c, res_int, x_int, nosig_mask);
            ir_emit_or(c, res_int, res_int, sign_mask);
            ir_emit_store(c, res_int, BE(c)->fp_reg, -slot);
            int res_temp = zc_alloc_temp(c);
            ir_emit_fload(c, (uint32_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            *out_temp = res_temp;
            return true;
        }

        /* GCC Integer Overflow Builtins:
         * 1. Value predicates: __builtin_{add,sub,mul}_overflow_p(a, b, (type)0)
         *    Evaluates arguments and returns 0 (conservative: no overflow).
         * 2. Store variants: __builtin_{add,sub,mul}_overflow(a, b, &r)
         *    Computes a OP b (operands normalized to 64 bits), stores the
         *    truncated result to *r, and returns 1 when the mathematical
         *    result does not fit in the type of *r. */
        if (callee_is_ident && callee->s.str &&
            (strstr(callee->s.str, "overflow_p") != NULL ||
             strstr(callee->s.str, "add_overflow") != NULL ||
             strstr(callee->s.str, "sub_overflow") != NULL ||
             strstr(callee->s.str, "mul_overflow") != NULL)) {
            /* The `_p` predicate variant (`__builtin_mul_overflow_p(a,b,r)`)
             * flows through the same real detection below - the third
             * argument is an rvalue of the RESULT type (not a pointer):
             * width/signedness come from its type, no store is emitted,
             * and the overflow flag is the call's value. (The pre-159
             * stub returned a constant 0 - wrong whenever the product
             * actually overflows: c-torture execute/pr30314, pr105777.) */
            int is_pred = (strstr(callee->s.str, "overflow_p") != NULL);

            int is_add = (strstr(callee->s.str, "add_overflow") != NULL);
            int is_sub = (strstr(callee->s.str, "sub_overflow") != NULL);
            int is_mul = !(is_add || is_sub);
            ASTRef mo_arg = node->right;
            ASTRef arg_a_ref = mo_arg;
            int t_a = lower_expr(c, arena, mo_arg);
            ZeroCCASTNode *an0 = zero_cc_ast_get(arena, mo_arg);
            mo_arg = (an0 && an0->next != AST_NULL) ? an0->next : AST_NULL;
            ASTRef arg_b_ref = mo_arg;
            int t_b = lower_expr(c, arena, mo_arg);
            ZeroCCASTNode *an1 = zero_cc_ast_get(arena, mo_arg);
            mo_arg = (an1 && an1->next != AST_NULL) ? an1->next : AST_NULL;
            int t_ptr = lower_expr(c, arena, mo_arg);
            ZeroCCASTNode *an2 = zero_cc_ast_get(arena, mo_arg);
            int width = an2 ? zc_atomic_access_width(c, an2) : 4;
            int t_signed = 1;
            /* *r target width/signedness: the argument may be `&x`
             * (address-of) or a bare pointer variable/parameter. */
            ZeroCCASTNode *target = NULL;
            if (an2 && an2->tag == NODE_UNOP && an2->aux.op == TK_AMP) {
                target = zero_cc_ast_get(arena, an2->left);
            } else if (an2 && an2->tag == NODE_IDENT) {
                target = an2;
            }
            if (target && target->tag == NODE_IDENT) {
                ZeroCCSym *sym = zero_cc_find_local(c, target->s.str);
                const ZeroCCType *zt = NULL;
                if (sym) zt = &sym->type;
                if (!zt) {
                    ZeroCCGlobalVar *gv = find_global_var(c, target->s.str);
                    if (gv) zt = &gv->type;
                }
                if (zt) {
                    const ZeroCCType *pt = zt;
                    if (pt->tag == TYPE_PTR && pt->base) pt = pt->base; /* bare pointer arg: look at the pointee */
                    if (pt->size == 1 || pt->size == 2 || pt->size == 4 || pt->size == 8) {
                        width = (int)pt->size;
                        t_signed = !(pt->tag == TYPE_UINT8 || pt->tag == TYPE_UINT16 ||
                                      pt->tag == TYPE_UINT32 || pt->tag == TYPE_UINT64);
                    }
                }
            }
            if (is_pred) {
                /* `_p` variant: the third argument is an rvalue whose type
                 * is the overflow target (width/signedness), not a pointer. */
                TypeInfo *r3 = ast_node_get_typeinfo(c, an2);
                if (r3) {
                    size_t r3sz = type_size(r3);
                    if (r3sz == 1 || r3sz == 2 || r3sz == 4 || r3sz == 8) {
                        width = (int)r3sz;
                        t_signed = !type_is_unsigned(r3);
                    }
                }
            }
            /* Real overflow detection (replaces the old conservative 0).
             * Each operand is normalized to 64 bits - zero-extended when
             * unsigned, sign-extended when signed - and the mathematical
             * a OP b is formed with 64-bit wrap; the overflow flag is 1
             * when that value cannot be represented in the type of *r
             * (width bytes, signedness t_signed). Mixed signed/unsigned
             * operand pairs use signed 64-bit math (exact for the operand
             * widths that occur in practice). */
            int au = 0, bu = 0;
            int aw = zero_cc_expr_int_info(c, arena, arg_a_ref, &au);
            int bw = zero_cc_expr_int_info(c, arena, arg_b_ref, &bu);
            /* GCC's overflow-builtin model (calibrated against gcc-15 on
             * the c-torture corpus): each operand keeps its OWN signedness
             * (unsigned zero-extends, signed sign-extends to 64 bits),
             * the math domain is unsigned only when BOTH operands are
             * unsigned (pr71554: 2u32 * -1073741824 with an int r must
             * compute the signed product == INT_MIN, which fits), and the
             * exact mathematical a OP b is checked against the RANGE OF
             * r - an unsigned r rejects P < 0 (pr91450-1: -4 * 2 == -8
             * overflows the u64 r), a signed r rejects |P| past its
             * symmetric bounds (pr30314 qux: 42 * 439208192231179800 ==
             * 2^64-16 fits the u64 r even though the 64-bit wrap is
             * negative). */
            int use_unsigned_math = (au && bu);
            int t_a64 = zc_alloc_temp(c);
            ir_emit_mov_reg(c, t_a64, t_a);
            if (aw < 8) {
                if (au) {
                    int m = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, m, (int64_t)((1ULL << (aw * 8)) - 1));
                    ir_emit_and(c, t_a64, t_a64, m);
                } else {
                    ir_emit_sext(c, t_a64, aw);
                }
            }
            int t_b64 = zc_alloc_temp(c);
            ir_emit_mov_reg(c, t_b64, t_b);
            if (bw < 8) {
                if (bu) {
                    int m = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, m, (int64_t)((1ULL << (bw * 8)) - 1));
                    ir_emit_and(c, t_b64, t_b64, m);
                } else {
                    ir_emit_sext(c, t_b64, bw);
                }
            }
            int t_res = zc_alloc_temp(c);
            if (is_add) {
                ir_emit(c, (Instr){.op=OP_ADD,.temp_dest=(uint32_t)t_res,
                    .temp_src1=(uint32_t)t_a64,.temp_src2=(uint32_t)t_b64});
            } else if (is_sub) {
                ir_emit(c, (Instr){.op=OP_SUB,.temp_dest=(uint32_t)t_res,
                    .temp_src1=(uint32_t)t_a64,.temp_src2=(uint32_t)t_b64});
            } else {
                ir_emit(c, (Instr){.op=OP_MUL,.temp_dest=(uint32_t)t_res,
                    .temp_src1=(uint32_t)t_a64,.temp_src2=(uint32_t)t_b64});
            }
            if (!is_pred) {
                if (width == 1) ir_emit_store_byte(c, t_res, t_ptr);
                else if (width == 2) ir_emit_store_16(c, t_res, t_ptr);
                else if (width == 8) ir_emit_store_64(c, t_res, t_ptr);
                else ir_emit_store_32(c, t_res, t_ptr);
            }
            int t_ovf;
            if (use_unsigned_math) {
                uint64_t target_hi = (width == 8)
                    ? (t_signed ? (uint64_t)1 << 63 : UINT64_MAX)
                    : ((uint64_t)1 << (t_signed ? (width * 8 - 1) : (width * 8)));
                uint64_t target_lo = t_signed
                    ? ((width == 8) ? (uint64_t)1 << 63 : (uint64_t)(UINT64_MAX - (((uint64_t)1 << (width * 8 - 1)) - 1)))
                    : 0;
                if (is_mul) {
                    /* P = a*b (a,b >= 0) fits the target iff
                     * (b == 0) || (a <= LIMIT / b); the division is
                     * branch-guarded against a zero divisor. */
                    uint64_t limit = t_signed ? (target_hi - 1)
                                              : (width == 8 ? UINT64_MAX : ((uint64_t)1 << (width * 8)) - 1);
                    int t_lim = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, t_lim, (int64_t)limit);
                    int t_bz = zc_ovf_cmp_set_imm(c, t_b64, 0, CC_EQ);
                    int lab_no = c->ir_next_label++;
                    ir_emit_jne(c, (uint32_t)t_bz, (uint32_t)lab_no);
                    int t_q = zc_alloc_temp(c);
                    ir_emit_div(c, t_q, t_lim, t_b64);
                    ir_mark_last_unsigned(c, 1);
                    int t_gt = zc_ovf_cmp_set(c, t_a64, t_q, CC_FLT_GT);
                    ir_emit_label(c, lab_no);
                    t_ovf = zc_ovf_and(c, zc_ovf_not1(c, t_bz), t_gt);
                } else if (is_add) {
                    /* carry64 = (res < a) unsigned; the mathematical sum
                     * exceeds the target when carry64 or the top bits
                     * (above the target width) are set. */
                    int t_carry = zc_ovf_cmp_set(c, t_res, t_a64, CC_FLT_LT);
                    if (width < 8) {
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE);
                        t_ovf = zc_ovf_or(c, t_carry, t_hi);
                    } else {
                        t_ovf = (t_signed ? zc_ovf_or(c, t_carry, zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE)) : t_carry);
                    }
                } else {
                    /* borrow = (a < b) unsigned; the mathematical
                     * difference a-b (negative when borrow) fits the
                     * target per the target's range. */
                    int t_borrow = zc_ovf_cmp_set(c, t_a64, t_b64, CC_FLT_LT);
                    if (!t_signed) {
                        if (width < 8) {
                            t_ovf = zc_ovf_or(c, t_borrow, zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE));
                        } else {
                            t_ovf = t_borrow;
                        }
                    } else if (width < 8) {
                        /* -2^(W-1) <= a-b < 2^(W-1): with borrow, res
                         * must be >= 2^64 - 2^(W-1); without, < 2^(W-1). */
                        int t_ge = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE);
                        int t_lt = zc_ovf_cmp_set_imm(c, t_res, target_lo, CC_FLT_LT);
                        t_ovf = zc_ovf_or(c, zc_ovf_and(c, t_borrow, t_lt),
                                          zc_ovf_and(c, zc_ovf_not1(c, t_borrow), t_ge));
                    } else {
                        int t_ge = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_GE);
                        int t_lt = zc_ovf_cmp_set_imm(c, t_res, target_hi, CC_FLT_LT);
                        t_ovf = zc_ovf_or(c, zc_ovf_and(c, t_borrow, t_lt),
                                          zc_ovf_and(c, zc_ovf_not1(c, t_borrow), t_ge));
                    }
                }
            } else {
                /* Signed 64-bit math (a mixed or all-signed pair): t_res
                 * is the 2^64 wrap of the exact P = a OP b, which lies
                 * in (-2^126, 2^126).  The wrap loses information in
                 * [2^63, 2^64) and (-2^64, -2^63) - pr30314 qux's
                 * 42 * 439208192231179800 == 2^64-16 wraps to -16 but
                 * fits the u64 r - so the checks below use magnitude
                 * limit tests (|A| > BOUND/|B|, exact for |B| >= 1 via
                 * the floor-division argument) instead of the wrapped
                 * value, plus a 64-bit-overflow (OF) analysis for
                 * add/sub. */
                int t_an = zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT);
                int t_bn = zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT);
                int t_az = zc_ovf_cmp_set_imm(c, t_a64, 0, CC_EQ);
                int t_bz = zc_ovf_cmp_set_imm(c, t_b64, 0, CC_EQ);
                int t_zero = zc_ovf_or(c, t_az, t_bz);  /* P == 0: fits */
                int t_opp = zc_ovf_or(c,
                            zc_ovf_and(c, t_an, zc_ovf_not1(c, t_bn)),
                            zc_ovf_and(c, zc_ovf_not1(c, t_an), t_bn));
                if (is_mul) {
                    /* P == 0 (either operand zero) always fits - and the
                     * limit division below needs |B| != 0, so skip the
                     * whole magnitude check on the zero path.  Both
                     * paths assign the same t_ovf temp. */
                    t_ovf = zc_alloc_temp(c);
                    int lab_zero = c->ir_next_label++, lab_md = c->ir_next_label++;
                    ir_emit_jne(c, (uint32_t)t_zero, (uint32_t)lab_zero);
                    /* |A|, |B| via branch select (the IR has no CMOV). */
                    int t_Aabs = zc_alloc_temp(c);
                    int lab_ad = c->ir_next_label++, lab_aneg = c->ir_next_label++;
                    ir_emit_jne(c, (uint32_t)t_an, (uint32_t)lab_aneg);
                    ir_emit_mov_reg(c, t_Aabs, t_a64);
                    ir_emit_jmp(c, (uint32_t)lab_ad);
                    ir_emit_label(c, lab_aneg);
                    ir_emit_neg(c, t_Aabs, t_a64);
                    ir_emit_label(c, lab_ad);
                    int t_Babs = zc_alloc_temp(c);
                    int lab_bd = c->ir_next_label++, lab_bneg = c->ir_next_label++;
                    ir_emit_jne(c, (uint32_t)t_bn, (uint32_t)lab_bneg);
                    ir_emit_mov_reg(c, t_Babs, t_b64);
                    ir_emit_jmp(c, (uint32_t)lab_bd);
                    ir_emit_label(c, lab_bneg);
                    ir_emit_neg(c, t_Babs, t_b64);
                    ir_emit_label(c, lab_bd);
                    /* overflow iff P < 0 (unsigned r) or |P| past r's
                     * bound: the NEGATIVE side reaches -2^(8W-1)
                     * (inclusive), the positive side stops at
                     * 2^(8W-1) - 1 (signed r) / 2^(8W) - 1 (unsigned). */
                    int t_q = zc_alloc_temp(c);
                    int t_lim = zc_alloc_temp(c);
                    int t_gt_neg = -1;
                    if (t_signed) {
                        /* negative-side bound 2^(8W-1) (used when t_opp) */
                        ir_emit_mov_imm(c, t_lim, (int64_t)((uint64_t)1 << (width * 8 - 1)));
                        ir_emit_div(c, t_q, t_lim, t_Babs);
                        ir_mark_last_unsigned(c, 1);
                        t_gt_neg = zc_ovf_cmp_set(c, t_Aabs, t_q, CC_FLT_GT);
                    }
                    /* positive-side bound 2^(8W-1)-1 / 2^(8W)-1 */
                    uint64_t b_pos = t_signed ? (((uint64_t)1 << (width * 8 - 1)) - 1)
                                              : (width == 8 ? UINT64_MAX
                                                            : ((uint64_t)1 << (width * 8)) - 1);
                    ir_emit_mov_imm(c, t_lim, (int64_t)b_pos);
                    ir_emit_div(c, t_q, t_lim, t_Babs);
                    ir_mark_last_unsigned(c, 1);
                    int t_gt_pos = zc_ovf_cmp_set(c, t_Aabs, t_q, CC_FLT_GT);
                    int t_gt;
                    if (!t_signed) {
                        /* P < 0 overflows an unsigned r outright. */
                        t_gt = zc_ovf_or(c, t_opp, t_gt_pos);
                    } else {
                        t_gt = zc_ovf_or(c,
                                zc_ovf_and(c, t_opp, t_gt_neg),
                                zc_ovf_and(c, zc_ovf_not1(c, t_opp), t_gt_pos));
                    }
                    int t_final = zc_ovf_and(c, zc_ovf_not1(c, t_zero), t_gt);
                    ir_emit_mov_reg(c, t_ovf, t_final);
                    ir_emit_jmp(c, (uint32_t)lab_md);
                    ir_emit_label(c, lab_zero);
                    ir_emit_mov_imm(c, t_ovf, 0);
                    ir_emit_label(c, lab_md);
                } else {
                    int t_res_neg = zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT);
                    int t_of, t_of_posP;  /* OF = 64-bit wrap; P's sign */
                    if (is_add) {
                        int t1 = zc_ovf_and(c, zc_ovf_and(c,
                                    zc_ovf_cmp_set_imm(c, t_a64, 0, CC_GT),
                                    zc_ovf_cmp_set_imm(c, t_b64, 0, CC_GT)),
                                    t_res_neg);                 /* P in (2^63, 2^64) */
                        int t2 = zc_ovf_and(c, zc_ovf_and(c,
                                    zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT),
                                    zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT)),
                                    zc_ovf_not1(c, t_res_neg));  /* P in (-2^64, -2^63) */
                        t_of = zc_ovf_or(c, t1, t2);
                        t_of_posP = t1;
                    } else {
                        int t1 = zc_ovf_and(c, zc_ovf_and(c,
                                    zc_ovf_cmp_set_imm(c, t_a64, 0, CC_GE),
                                    zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT)),
                                    t_res_neg);                 /* P in (2^63, 2^64) */
                        int t2 = zc_ovf_and(c, zc_ovf_and(c,
                                    zc_ovf_not1(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT)),
                                    zc_ovf_cmp_set_imm(c, t_b64, 0, CC_GE)),
                                    zc_ovf_not1(c, t_res_neg));  /* P in (-2^64, -2^63) */
                        t_of = zc_ovf_or(c, t1, t2);
                        t_of_posP = t1;
                    }
                    /* OF puts |P| in [2^63, 2^64): that overflows every
                     * r EXCEPT an unsigned 64-bit one with P > 0
                     * (then P < 2^64). */
                    int t_of_ovf;
                    if (!t_signed && width == 8)
                        t_of_ovf = zc_ovf_and(c, t_of, zc_ovf_not1(c, t_of_posP));
                    else
                        t_of_ovf = t_of;
                    /* No OF: t_res is the exact P - plain range check. */
                    int t_exact;
                    if (!t_signed) {
                        t_exact = t_res_neg;
                        if (width < 8)
                            t_exact = zc_ovf_or(c, t_exact,
                                zc_ovf_cmp_set_imm(c, t_res, ((uint64_t)1 << (width * 8)), CC_FLT_GE));
                    } else if (width < 8) {
                        int t_lo = zc_ovf_cmp_set_imm(c, t_res, (-((uint64_t)1 << (width * 8 - 1))), CC_LT);
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, (((uint64_t)1 << (width * 8 - 1)) - 1), CC_GT);
                        t_exact = zc_ovf_or(c, t_lo, t_hi);
                    } else {
                        int t_c0 = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, t_c0, 0);   /* i64 P: always in range */
                        t_exact = t_c0;
                    }
                    t_ovf = zc_ovf_or(c, t_of_ovf,
                            zc_ovf_and(c, zc_ovf_not1(c, t_of), t_exact));
                }
            }
            c->last_expr_is_float = 0;
            *out_temp = t_ovf;
            return true;
        }

        if (callee_is_ident) {
            /* Intercept __zerocc_syscall (zero_rt OS boundary):
             * __zerocc_syscall(number, a, b, c, d, e, f) - 1..7 integer
             * arguments, returns the kernel result. Lowers to OP_SYSCALL;
             * the backend moves the arguments into the target's syscall
             * argument registers and issues the instruction. The result
             * is in VREG_0 (the ABI return register). */
            if (strcmp(name, "__zerocc_syscall") == 0) {
                int sys_arg_temps[8] = {0};
                int sys_arg_count = 0;
                ASTRef sys_arg = node->right;
                while (sys_arg != AST_NULL && sys_arg_count < 8) {
                    sys_arg_temps[sys_arg_count++] = (int)lower_expr(c, arena, sys_arg);
                    ZeroCCASTNode *an = zero_cc_ast_get(arena, sys_arg);
                    sys_arg = (an && an->next != AST_NULL) ? an->next : AST_NULL;
                }
                if (sys_arg_count == 0) {
                    diag_report_error_at_token(c,
                        "__zerocc_syscall requires a syscall number argument");
                    *out_temp = VREG_0;
                    return true;
                }
                if (sys_arg_count > 7) {
                    diag_report_error_at_token(c,
                        "__zerocc_syscall takes at most 7 arguments (number + 6 operands)");
                    *out_temp = VREG_0;
                    return true;
                }
                ir_emit_syscall(c, sys_arg_count, sys_arg_temps);
                c->last_expr_is_float = 0;
                *out_temp = VREG_0;
                return true;
            }

            /* Intercept __zerocc_clone (zero_rt thread boundary):
             * __zerocc_clone(flags, child_sp, ptid, tls, ctid) - 5 integer
             * arguments, returns child TID in parent (0 in child). Lowers
             * to OP_CLONE; backend sets up SYS_clone arguments and emits
             * the child entry trampoline. */
            if (strcmp(name, "__zerocc_clone") == 0) {
                int clone_arg_temps[8] = {0};
                int clone_arg_count = 0;
                ASTRef clone_arg = node->right;
                while (clone_arg != AST_NULL && clone_arg_count < 8) {
                    clone_arg_temps[clone_arg_count++] = (int)lower_expr(c, arena, clone_arg);
                    ZeroCCASTNode *an = zero_cc_ast_get(arena, clone_arg);
                    clone_arg = (an && an->next != AST_NULL) ? an->next : AST_NULL;
                }
                if (clone_arg_count < 2) {
                    diag_report_error_at_token(c,
                        "__zerocc_clone requires at least flags and child_sp arguments");
                    *out_temp = VREG_0;
                    return true;
                }
                ir_emit_clone(c, clone_arg_count, clone_arg_temps);
                c->last_expr_is_float = 0;
                *out_temp = VREG_0;
                return true;
            }

            /* C11/GNU atomic-builtin intercept (table: zc_atomic_builtin_table).
             * Access width comes from the pointer operand's pointee type;
             * narrow integer value operands are extended at the value
             * boundary (C99 6.3.1.2, see lower_extend_narrow). */
            for (size_t zc_at_i = 0;
                 zc_at_i < sizeof(zc_atomic_builtin_table) / sizeof(zc_atomic_builtin_table[0]);
                 zc_at_i++) {
                if (strcmp(name, zc_atomic_builtin_table[zc_at_i].name) != 0) continue;
                ZCAtomicKind kind = zc_atomic_builtin_table[zc_at_i].kind;
                ASTRef a0 = node->right;
                ASTRef a1 = (a0 != AST_NULL) ? zero_cc_ast_get(arena, a0)->next : AST_NULL;
                ASTRef a2 = (a1 != AST_NULL) ? zero_cc_ast_get(arena, a1)->next : AST_NULL;
                ZeroCCASTNode *a0_node = zero_cc_ast_get(arena, a0);
                ZeroCCASTNode *a1_node = (a1 != AST_NULL) ? zero_cc_ast_get(arena, a1) : NULL;
                ZeroCCASTNode *a2_node = (a2 != AST_NULL) ? zero_cc_ast_get(arena, a2) : NULL;
                int width = zc_atomic_access_width(c, a0_node);
                int res_temp = -1;

                if (kind == ZC_AT_FENCE) {
                    /* __sync_synchronize / __atomic_thread_fence: x86-64 TSO
                     * gives full ordering, so MFENCE is a valid (over-strong)
                     * lowering for every memory order, incl. seq_cst. */
                    ir_emit_mfence(c);
                    res_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, res_temp, 0);
                } else if (kind == ZC_AT_IS_LOCK_FREE) {
                    /* C11 7.17.2: aligned integer atomics up to 8 bytes are
                     * lock-free on x86-64. */
                    res_temp = zc_alloc_temp(c);
                    ir_emit_mov_imm(c, res_temp, 1);
                } else {
                    int ptr_temp = lower_expr(c, arena, a0);
                    switch (kind) {
                    case ZC_AT_LOAD: {
                        res_temp = zc_alloc_temp(c);
                        if (width == 1) ir_emit_load_byte(c, res_temp, ptr_temp);
                        else if (width == 2) ir_emit_load_16(c, res_temp, ptr_temp);
                        else if (width == 8) ir_emit_deref(c, res_temp, ptr_temp);
                        else ir_emit_load_32(c, res_temp, ptr_temp);
                        TypeInfo *pti = ast_node_get_typeinfo(c, a0_node);
                        if (pti && pti->kind == TYPE_KIND_POINTER && pti->details.pointer.base) {
                            lower_extend_narrow(c, res_temp, pti->details.pointer.base);
                        }
                        break;
                    }
                    case ZC_AT_STORE:
                    case ZC_AT_EXCHANGE: {
                        /* (ptr, val, order) for both __atomic_store_n and the
                         * bare __atomic_store (GCC's documented convention).
                         * xchg returns the old value (exchange semantics);
                         * store discards it (returned as a dead temp). */
                        int val_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, val_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        res_temp = zc_alloc_temp(c);
                        ir_emit_atomic_xchg(c, res_temp, ptr_temp, val_temp, width);
                        break;
                    }
                    case ZC_AT_STORE_0: {
                        /* __sync_lock_release: store 0 at the pointee. */
                        int zero_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, zero_temp, 0);
                        if (width == 1) ir_emit_store_byte(c, zero_temp, ptr_temp);
                        else if (width == 2) ir_emit_store_16(c, zero_temp, ptr_temp);
                        else if (width == 8) ir_emit_store_64(c, zero_temp, ptr_temp);
                        else ir_emit_store_32(c, zero_temp, ptr_temp);
                        res_temp = zero_temp;
                        break;
                    }
                    case ZC_AT_FETCH_ADD:
                    case ZC_AT_ADD_FETCH:
                    case ZC_AT_FETCH_SUB:
                    case ZC_AT_SUB_FETCH: {
                        int is_sub = (kind == ZC_AT_FETCH_SUB || kind == ZC_AT_SUB_FETCH);
                        int val_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, val_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        if (is_sub) {
                            int neg_temp = zc_alloc_temp(c);
                            ir_emit_neg(c, neg_temp, val_temp);
                            val_temp = neg_temp;
                        }
                        int val_copy = zc_alloc_temp(c);
                        ir_emit_mov_reg(c, val_copy, val_temp);
                        int old_temp = zc_alloc_temp(c);
                        ir_emit_atomic_xadd(c, old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_FETCH_ADD || kind == ZC_AT_FETCH_SUB) {
                            res_temp = old_temp;
                        } else {
                            res_temp = zc_alloc_temp(c);
                            if (kind == ZC_AT_ADD_FETCH) ir_emit_add(c, res_temp, old_temp, val_copy);
                            else if (kind == ZC_AT_SUB_FETCH) ir_emit_add(c, res_temp, old_temp, val_copy);
                            else ir_emit_sub(c, res_temp, old_temp, val_copy);
                        }
                        break;
                    }
                    case ZC_AT_FETCH_OR:
                    case ZC_AT_OR_FETCH:
                    case ZC_AT_FETCH_AND:
                    case ZC_AT_AND_FETCH:
                    case ZC_AT_FETCH_XOR:
                    case ZC_AT_XOR_FETCH: {
                        int val_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, val_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int old_temp = zc_alloc_temp(c);
                        if (kind == ZC_AT_FETCH_OR || kind == ZC_AT_OR_FETCH) ir_emit_atomic_or(c, old_temp, ptr_temp, val_temp, width);
                        else if (kind == ZC_AT_FETCH_AND || kind == ZC_AT_AND_FETCH) ir_emit_atomic_and(c, old_temp, ptr_temp, val_temp, width);
                        else ir_emit_atomic_xor(c, old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_OR_FETCH || kind == ZC_AT_AND_FETCH || kind == ZC_AT_XOR_FETCH) {
                            res_temp = zc_alloc_temp(c);
                            if (kind == ZC_AT_OR_FETCH) ir_emit_or(c, res_temp, old_temp, val_temp);
                            else if (kind == ZC_AT_AND_FETCH) ir_emit_and(c, res_temp, old_temp, val_temp);
                            else ir_emit_xor(c, res_temp, old_temp, val_temp);
                        } else {
                            res_temp = old_temp;
                        }
                        break;
                    }
                    case ZC_AT_CAS_OLD: {
                        /* __sync_val_compare_and_swap(ptr, expected, desired) */
                        int exp_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, exp_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        res_temp = zc_alloc_temp(c);
                        ir_emit_atomic_cmpxchg(c, res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL_SYNC: {
                        /* __sync_bool_compare_and_swap(ptr, expected, desired) */
                        int exp_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, exp_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        res_temp = zc_alloc_temp(c);
                        ir_emit_atomic_cmpxchg_bool(c, res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL: {
                        /* __atomic_compare_exchange[_n](ptr, &expected, desired, ...) */
                        int exp_ptr_temp = lower_expr(c, arena, a1);
                        int exp_temp = zc_alloc_temp(c);
                        if (width == 1) ir_emit_load_byte(c, exp_temp, exp_ptr_temp);
                        else if (width == 2) ir_emit_load_16(c, exp_temp, exp_ptr_temp);
                        else if (width == 8) ir_emit_deref(c, exp_temp, exp_ptr_temp);
                        else ir_emit_load_32(c, exp_temp, exp_ptr_temp);
                        TypeInfo *epti = ast_node_get_typeinfo(c, a1_node);
                        if (epti && epti->kind == TYPE_KIND_POINTER && epti->details.pointer.base) {
                            lower_extend_narrow(c, exp_temp, epti->details.pointer.base);
                        }
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        int actual_old = zc_alloc_temp(c);
                        ir_emit_atomic_cmpxchg(c, actual_old, ptr_temp, exp_temp, des_temp, width);
                        if (width == 1) ir_emit_store_byte(c, actual_old, exp_ptr_temp);
                        else if (width == 2) ir_emit_store_16(c, actual_old, exp_ptr_temp);
                        else if (width == 8) ir_emit_store_64(c, actual_old, exp_ptr_temp);
                        else ir_emit_store_32(c, actual_old, exp_ptr_temp);
                        res_temp = zc_alloc_temp(c);
                        ir_emit_cmp(c, actual_old, exp_temp);
                        ir_emit_cond_set(c, res_temp, CC_EQ);
                        break;
                    }
                    case ZC_AT_TEST_AND_SET: {
                        /* exchange(ptr, 1), return (old != 0). */
                        int one_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, one_temp, 1);
                        int zero_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, zero_temp, 0);
                        int old_temp = zc_alloc_temp(c);
                        ir_emit_atomic_xchg(c, old_temp, ptr_temp, one_temp, width);
                        res_temp = zc_alloc_temp(c);
                        ir_emit_cmp(c, old_temp, zero_temp);
                        ir_emit_cond_set(c, res_temp, CC_NE);
                        break;
                    }
                    case ZC_AT_CLEAR: {
                        /* xchg(ptr, 0); the old value is dead (void op). */
                        int zero_temp = zc_alloc_temp(c);
                        ir_emit_mov_imm(c, zero_temp, 0);
                        res_temp = zc_alloc_temp(c);
                        ir_emit_atomic_xchg(c, res_temp, ptr_temp, zero_temp, width);
                        break;
                    }
                    default:
                        break;
                    }
                }
                c->last_expr_is_float = 0;
                *out_temp = res_temp;
                return true;
            }
        }

    if (strncmp(name, "__builtin_zerocc_v", 18) == 0 &&
        zc_lower_simd_builtin(c, arena, node, name, out_temp)) {
        return true;
    }

    if (strncmp(name, "__builtin_zerocc_sve_", 21) == 0 &&
        zc_lower_sve_builtin(c, arena, node, name + 21, out_temp)) {
        return true;
    }

    if (strncmp(name, "__builtin_zerocc_rvv_", 21) == 0 &&
        zc_lower_rvv_builtin(c, arena, node, name + 21, out_temp)) {
        return true;
    }

    if (strncmp(name, "__builtin_zerocc_k", 18) == 0 &&
        zc_lower_kmask_builtin(c, arena, node, name + 17, out_temp)) {
        return true;
    }

    return false;
}
