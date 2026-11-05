/* AST Lowering - builtin emit helpers: bitops, overflow, atomics
 * (Sprint 114: extracted from ast_lowering_builtins.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

int zc_bitop_width(ZeroCCCompiler *c, const char *name) {
    size_t len = strlen(name);
    if (len >= 2 && strcmp(name + len - 2, "ll") == 0) return 64;
    if (len >= 1 && name[len - 1] == 'l') return (c && c->isa == ISA_RISCV32) ? 32 : 64;
    return 32;
}

bool zc_is_bitop_builtin_name(const char *name) {
    if (!name) return false;
    return strcmp(name, "__builtin_ffs") == 0 ||
           strcmp(name, "__builtin_ffsl") == 0 ||
           strcmp(name, "__builtin_ffsll") == 0 ||
           strcmp(name, "__builtin_ctz") == 0 ||
           strcmp(name, "__builtin_ctzl") == 0 ||
           strcmp(name, "__builtin_ctzll") == 0 ||
           strcmp(name, "__builtin_clz") == 0 ||
           strcmp(name, "__builtin_clzl") == 0 ||
           strcmp(name, "__builtin_clzll") == 0 ||
           strcmp(name, "__builtin_clrsb") == 0 ||
           strcmp(name, "__builtin_clrsbl") == 0 ||
           strcmp(name, "__builtin_clrsbll") == 0 ||
           strcmp(name, "__builtin_popcount") == 0 ||
           strcmp(name, "__builtin_popcountl") == 0 ||
           strcmp(name, "__builtin_popcountll") == 0 ||
           strcmp(name, "__builtin_parity") == 0 ||
           strcmp(name, "__builtin_parityl") == 0 ||
           strcmp(name, "__builtin_parityll") == 0;
}

int zc_eval_bitop_const(const char *name, uint64_t uval, int nbits) {
    if (nbits == 32) uval &= 0xFFFFFFFFULL;
    if (strncmp(name, "__builtin_ffs", 13) == 0) {
        return uval == 0 ? 0 : (__builtin_ctzll(uval) + 1);
    }
    if (strncmp(name, "__builtin_ctz", 13) == 0) {
        return uval == 0 ? nbits : __builtin_ctzll(uval);
    }
    if (strncmp(name, "__builtin_clz", 13) == 0) {
        return uval == 0 ? nbits : (__builtin_clzll(uval) - (64 - nbits));
    }
    if (strncmp(name, "__builtin_clrsb", 15) == 0) {
        int leading = (int)((uval >> (nbits - 1)) & 1ULL);
        int cnt = 0;
        for (int i = 1; i < nbits; i++) {
            if ((int)((uval >> (nbits - i - 1)) & 1ULL) != leading) break;
            cnt++;
        }
        return cnt;
    }
    if (strncmp(name, "__builtin_popcount", 18) == 0) {
        return __builtin_popcountll(uval);
    }
    if (strncmp(name, "__builtin_parity", 16) == 0) {
        return __builtin_popcountll(uval) & 1;
    }
    return 0;
}

int zc_emit_ctz_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
    int t_x = c->next_temp_id++;
    ir_emit_mov_reg(c, t_x, t_in);
    if (nbits == 32) ir_emit_zext(c, t_x, 4);
    int t_zero = c->next_temp_id++;
    ir_emit_mov_imm(c, t_zero, 0);
    int t_res = c->next_temp_id++;
    ir_emit_mov_imm(c, t_res, 0);
    int k_max = (nbits == 64) ? 5 : 4;
    for (int k = k_max; k >= 0; k--) {
        int step = 1 << k;
        int t_low = c->next_temp_id++;
        if (step == 32) {
            ir_emit_mov_reg(c, t_low, t_x);
            ir_emit_zext(c, t_low, 4);
        } else {
            int t_mask = c->next_temp_id++;
            ir_emit_mov_imm(c, t_mask, (1 << step) - 1);
            ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_low,
                               .temp_src1 = (uint16_t)t_x, .temp_src2 = (uint16_t)t_mask});
        }
        int t_isz = c->next_temp_id++;
        ir_emit_cmp(c, t_low, t_zero);
        ir_emit(c, (Instr){.op = OP_COND_SET, .temp_dest = (uint16_t)t_isz,
                           .temp_src1 = (uint16_t)t_low, .temp_src2 = (uint16_t)t_zero,
                           .cond = CC_EQ});
        int t_sh = t_isz;
        if (k > 0) {
            int t_k = c->next_temp_id++;
            ir_emit_mov_imm(c, t_k, k);
            t_sh = c->next_temp_id++;
            ir_emit(c, (Instr){.op = OP_SHL, .temp_dest = (uint16_t)t_sh,
                               .temp_src1 = (uint16_t)t_isz, .temp_src2 = (uint16_t)t_k});
        }
        ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_res,
                           .temp_src1 = (uint16_t)t_res, .temp_src2 = (uint16_t)t_sh});
        if (k > 0) {
            ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_x,
                               .temp_src1 = (uint16_t)t_x, .temp_src2 = (uint16_t)t_sh});
        }
    }
    return t_res;
}

int zc_emit_clz_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
    int t_x = c->next_temp_id++;
    ir_emit_mov_reg(c, t_x, t_in);
    if (nbits == 32) ir_emit_zext(c, t_x, 4);
    int t_zero = c->next_temp_id++;
    ir_emit_mov_imm(c, t_zero, 0);
    int t_res = c->next_temp_id++;
    ir_emit_mov_imm(c, t_res, 0);
    int k_max = (nbits == 64) ? 5 : 4;
    for (int k = k_max; k >= 0; k--) {
        int step = 1 << k;
        int t_step = c->next_temp_id++;
        ir_emit_mov_imm(c, t_step, step);
        int t_hi = c->next_temp_id++;
        ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_hi,
                           .temp_src1 = (uint16_t)t_x, .temp_src2 = (uint16_t)t_step});
        int t_isz = c->next_temp_id++;
        ir_emit_cmp(c, t_hi, t_zero);
        ir_emit(c, (Instr){.op = OP_COND_SET, .temp_dest = (uint16_t)t_isz,
                           .temp_src1 = (uint16_t)t_hi, .temp_src2 = (uint16_t)t_zero,
                           .cond = CC_EQ});
        int t_add = t_isz;
        if (k > 0) {
            int t_k = c->next_temp_id++;
            ir_emit_mov_imm(c, t_k, k);
            t_add = c->next_temp_id++;
            ir_emit(c, (Instr){.op = OP_SHL, .temp_dest = (uint16_t)t_add,
                               .temp_src1 = (uint16_t)t_isz, .temp_src2 = (uint16_t)t_k});
        }
        ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_res,
                           .temp_src1 = (uint16_t)t_res, .temp_src2 = (uint16_t)t_add});
        if (k > 0) {
            int t_isnz = c->next_temp_id++;
            ir_emit_cmp(c, t_hi, t_zero);
            ir_emit(c, (Instr){.op = OP_COND_SET, .temp_dest = (uint16_t)t_isnz,
                               .temp_src1 = (uint16_t)t_hi, .temp_src2 = (uint16_t)t_zero,
                               .cond = CC_NE});
            int t_k2 = c->next_temp_id++;
            ir_emit_mov_imm(c, t_k2, k);
            int t_sh = c->next_temp_id++;
            ir_emit(c, (Instr){.op = OP_SHL, .temp_dest = (uint16_t)t_sh,
                               .temp_src1 = (uint16_t)t_isnz, .temp_src2 = (uint16_t)t_k2});
            ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_x,
                               .temp_src1 = (uint16_t)t_x, .temp_src2 = (uint16_t)t_sh});
        }
    }
    return t_res;
}

int zc_emit_clrsb_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
    int t_x = c->next_temp_id++;
    ir_emit_mov_reg(c, t_x, t_in);
    if (nbits == 32) ir_emit_zext(c, t_x, 4);
    int t_nm1 = c->next_temp_id++;
    ir_emit_mov_imm(c, t_nm1, nbits - 1);
    int t_msb = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_msb,
                       .temp_src1 = (uint16_t)t_x, .temp_src2 = (uint16_t)t_nm1});
    int t_one = c->next_temp_id++;
    ir_emit_mov_imm(c, t_one, 1);
    ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_msb,
                       .temp_src1 = (uint16_t)t_msb, .temp_src2 = (uint16_t)t_one});
    int t_zero = c->next_temp_id++;
    ir_emit_mov_imm(c, t_zero, 0);
    int t_mask = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint16_t)t_mask,
                       .temp_src1 = (uint16_t)t_zero, .temp_src2 = (uint16_t)t_msb});
    int t_u = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_XOR, .temp_dest = (uint16_t)t_u,
                       .temp_src1 = (uint16_t)t_x, .temp_src2 = (uint16_t)t_mask});
    int t_w = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHL, .temp_dest = (uint16_t)t_w,
                       .temp_src1 = (uint16_t)t_u, .temp_src2 = (uint16_t)t_one});
    ir_emit(c, (Instr){.op = OP_OR, .temp_dest = (uint16_t)t_w,
                       .temp_src1 = (uint16_t)t_w, .temp_src2 = (uint16_t)t_one});
    if (nbits == 32) ir_emit_zext(c, t_w, 4);
    return zc_emit_clz_nbits(c, t_w, nbits);
}

int zc_emit_popcount32(ZeroCCCompiler *c, int t_in) {
    int t_v = c->next_temp_id++;
    ir_emit_mov_reg(c, t_v, t_in);
    ir_emit_zext(c, t_v, 4);
    int t_1 = c->next_temp_id++; ir_emit_mov_imm(c, t_1, 1);
    int t_s1 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_s1,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_1});
    int t_m55 = c->next_temp_id++; ir_emit_mov_imm(c, t_m55, 0x55555555);
    int t_a1 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_a1,
                       .temp_src1 = (uint16_t)t_s1, .temp_src2 = (uint16_t)t_m55});
    ir_emit(c, (Instr){.op = OP_SUB, .temp_dest = (uint16_t)t_v,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_a1});
    int t_2 = c->next_temp_id++; ir_emit_mov_imm(c, t_2, 2);
    int t_s2 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_s2,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_2});
    int t_m33 = c->next_temp_id++; ir_emit_mov_imm(c, t_m33, 0x33333333);
    int t_a2 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_a2,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_m33});
    int t_b2 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_b2,
                       .temp_src1 = (uint16_t)t_s2, .temp_src2 = (uint16_t)t_m33});
    ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_v,
                       .temp_src1 = (uint16_t)t_a2, .temp_src2 = (uint16_t)t_b2});
    int t_4 = c->next_temp_id++; ir_emit_mov_imm(c, t_4, 4);
    int t_s4 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_s4,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_4});
    ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_v,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_s4});
    int t_m0f = c->next_temp_id++; ir_emit_mov_imm(c, t_m0f, 0x0F0F0F0F);
    ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_v,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_m0f});
    int t_8 = c->next_temp_id++; ir_emit_mov_imm(c, t_8, 8);
    int t_s8 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_s8,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_8});
    ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_v,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_s8});
    int t_16 = c->next_temp_id++; ir_emit_mov_imm(c, t_16, 16);
    int t_s16 = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_s16,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_16});
    ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_v,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_s16});
    int t_m3f = c->next_temp_id++; ir_emit_mov_imm(c, t_m3f, 0x3F);
    int t_res = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_res,
                       .temp_src1 = (uint16_t)t_v, .temp_src2 = (uint16_t)t_m3f});
    return t_res;
}

int zc_emit_popcount_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
    int t_lo = zc_emit_popcount32(c, t_in);
    if (nbits <= 32) return t_lo;
    int t_32 = c->next_temp_id++; ir_emit_mov_imm(c, t_32, 32);
    int t_hi_in = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_SHR, .temp_dest = (uint16_t)t_hi_in,
                       .temp_src1 = (uint16_t)t_in, .temp_src2 = (uint16_t)t_32});
    int t_hi = zc_emit_popcount32(c, t_hi_in);
    int t_res = c->next_temp_id++;
    ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_res,
                       .temp_src1 = (uint16_t)t_lo, .temp_src2 = (uint16_t)t_hi});
    return t_res;
}

/* --- __builtin_{add,sub,mul}_overflow helpers ------------------------- */
/* 0/1 temp holding (a <cc> b) over already-computed values. */
int zc_ovf_cmp_set(ZeroCCCompiler *c, int a, int b, int cc) {
    int t = c->next_temp_id++;
    ir_emit_cmp(c, a, b);
    ir_emit_cond_set(c, t, cc);
    return t;
}

/* 0/1 temp holding (a <cc> imm). */
int zc_ovf_cmp_set_imm(ZeroCCCompiler *c, int a, int64_t imm, int cc) {
    int b = c->next_temp_id++;
    ir_emit_mov_imm(c, b, imm);
    return zc_ovf_cmp_set(c, a, b, cc);
}

int zc_ovf_or(ZeroCCCompiler *c, int a, int b) {
    int t = c->next_temp_id++;
    ir_emit_or(c, t, a, b);
    return t;
}

int zc_ovf_and(ZeroCCCompiler *c, int a, int b) {
    int t = c->next_temp_id++;
    ir_emit_and(c, t, a, b);
    return t;
}

/* 1 - a for a 0/1 temp (logical NOT). */
int zc_ovf_not1(ZeroCCCompiler *c, int a) {
    int one = c->next_temp_id++;
    ir_emit_mov_imm(c, one, 1);
    int t = c->next_temp_id++;
    ir_emit_xor(c, t, a, one);
    return t;
}


/* ---- C11/GCC atomic-builtin dispatch (C11 7.6 / GNU __sync + __atomic) ----
 *
 * Every __sync_* / __atomic_* builtin is a flat name -> fixed-arity -> IR
 * mapping: there is no context-dependent token stream, so the dispatch is a
 * static table + one switch (a state machine would track state that never
 * transitions). Previously ~14 of the builtins were individual
 * strcmp if-blocks hardcoding a 32-bit access width, and every name that
 * was not listed (e.g. __atomic_thread_fence, __atomic_fetch_or, the
 * non-_n variants) fell through to the generic call emitter and became an
 * external call to a nonexistent symbol — a runtime segfault.
 *
 * Width: the access width is derived from the pointer operand's semantic
 * TypeInfo (C11 requires every integer width, not just 32-bit). Memory-
 * order arguments are accepted and ignored: x86-64 TSO makes every locked
 * RMW seq_cst-strength, and OP_MFENCE is a valid (over-strong) lowering of
 * any fence order. */
const struct ZCAtomicBuiltinEntry zc_atomic_builtin_table[37] = {  /* type in ast_lowering_internal.h; count must match */
    { "__sync_fetch_and_add",          ZC_AT_FETCH_ADD },
    { "__atomic_fetch_add",            ZC_AT_FETCH_ADD },
    { "__sync_add_and_fetch",          ZC_AT_ADD_FETCH },
    { "__atomic_add_fetch",            ZC_AT_ADD_FETCH },
    { "__sync_fetch_and_sub",          ZC_AT_FETCH_SUB },
    { "__atomic_fetch_sub",            ZC_AT_FETCH_SUB },
    { "__sync_sub_and_fetch",          ZC_AT_SUB_FETCH },
    { "__atomic_sub_fetch",            ZC_AT_SUB_FETCH },
    { "__sync_fetch_and_or",           ZC_AT_FETCH_OR },
    { "__atomic_fetch_or",             ZC_AT_FETCH_OR },
    { "__sync_or_and_fetch",           ZC_AT_OR_FETCH },
    { "__atomic_or_fetch",             ZC_AT_OR_FETCH },
    { "__sync_fetch_and_and",          ZC_AT_FETCH_AND },
    { "__atomic_fetch_and",            ZC_AT_FETCH_AND },
    { "__sync_and_and_fetch",          ZC_AT_AND_FETCH },
    { "__atomic_and_fetch",            ZC_AT_AND_FETCH },
    { "__sync_fetch_and_xor",          ZC_AT_FETCH_XOR },
    { "__atomic_fetch_xor",            ZC_AT_FETCH_XOR },
    { "__sync_xor_and_fetch",          ZC_AT_XOR_FETCH },
    { "__atomic_xor_fetch",            ZC_AT_XOR_FETCH },
    { "__sync_val_compare_and_swap",   ZC_AT_CAS_OLD },
    { "__sync_bool_compare_and_swap",  ZC_AT_CAS_BOOL_SYNC },
    { "__atomic_compare_exchange_n",   ZC_AT_CAS_BOOL },
    { "__atomic_compare_exchange",     ZC_AT_CAS_BOOL },
    { "__sync_lock_test_and_set",      ZC_AT_EXCHANGE },
    { "__atomic_exchange_n",           ZC_AT_EXCHANGE },
    { "__atomic_exchange",             ZC_AT_EXCHANGE },
    { "__atomic_store_n",              ZC_AT_STORE },
    { "__atomic_store",                ZC_AT_STORE },
    { "__atomic_load_n",               ZC_AT_LOAD },
    { "__atomic_load",                 ZC_AT_LOAD },
    { "__sync_lock_release",           ZC_AT_STORE_0 },
    { "__sync_synchronize",            ZC_AT_FENCE },
    { "__atomic_thread_fence",         ZC_AT_FENCE },
    { "__atomic_test_and_set",         ZC_AT_TEST_AND_SET },
    { "__atomic_clear",                ZC_AT_CLEAR },
    { "__atomic_is_lock_free",         ZC_AT_IS_LOCK_FREE },
};

/* Access width for an atomic builtin: the size of the pointer operand's
 * pointee, clamped to the x86-64 atomic widths (1/2/4/8). Falls back to 4
 * when the operand type is unavailable (matches the pre-table behavior). */
int zc_atomic_access_width(ZeroCCCompiler *c, const ZeroCCASTNode *arg0_node) {
    TypeInfo *ti = ast_node_get_typeinfo(c, arg0_node);
    if (ti && ti->kind == TYPE_KIND_POINTER && ti->details.pointer.base) {
        size_t sz = type_size(ti->details.pointer.base);
        if (sz == 1 || sz == 2 || sz == 4 || sz == 8) return (int)sz;
    }
    return 4;
}

/* Extend a value operand for a wider atomic access (C99 6.3.1.2). Skips
 * 64-bit integer literals: the semantic model types every NODE_INT as
 * 32-bit int, so SEXT32-ing a literal whose value needs 64 bits would
 * corrupt it (e.g. __atomic_store_n(&l, 0x1122334455667788LL)). A literal
 * whose int64 value fits in signed int32 is exactly C's int-typed literal
 * (decimal in-range, or hex/unsigned within uint32 where the mathematical
 * value already equals its C-converted form), so normal extension applies. */
void zc_atomic_extend_value(ZeroCCCompiler *c, int temp,
                                   const ZeroCCASTNode *node, const TypeInfo *ti) {
    if (node && node->tag == NODE_INT) {
        int64_t v = node->int_val;
        if (v < INT32_MIN || v > INT32_MAX) return;  /* genuine 64-bit literal */
    }
    lower_extend_narrow(c, temp, ti);
}
