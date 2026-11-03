/* AST Lowering - GCC/Clang Builtins, Intrinsics, and Atomics (AST to IR) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/ir/ast_lowering_internal.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/semantic/diagnostics.h"

static int zc_bitop_width(ZeroCCCompiler *c, const char *name) {
    size_t len = strlen(name);
    if (len >= 2 && strcmp(name + len - 2, "ll") == 0) return 64;
    if (len >= 1 && name[len - 1] == 'l') return (c && c->isa == ISA_RISCV32) ? 32 : 64;
    return 32;
}

static bool zc_is_bitop_builtin_name(const char *name) {
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

static int zc_eval_bitop_const(const char *name, uint64_t uval, int nbits) {
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

static int zc_emit_ctz_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
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

static int zc_emit_clz_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
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

static int zc_emit_clrsb_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
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

static int zc_emit_popcount32(ZeroCCCompiler *c, int t_in) {
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

static int zc_emit_popcount_nbits(ZeroCCCompiler *c, int t_in, int nbits) {
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

static bool zc_node_is_const_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    (void)c;
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    switch (n->tag) {
        case NODE_INT:
        case NODE_FLOAT:
        case NODE_STRING:
        case NODE_NULLPTR:
        case NODE_BOOL:
            return true;
        case NODE_CAST:
            return zc_node_is_const_expr(c, arena, n->left);
        case NODE_UNOP:
            switch (n->aux.op) {
                case TK_PLUS: case TK_MINUS: case TK_BANG: case TK_TILDE:
                    return zc_node_is_const_expr(c, arena, n->left);
                default:
                    return false;
            }
        case NODE_BINOP:
            switch (n->aux.op) {
                case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH:
                case TK_PERCENT: case TK_AMP: case TK_PIPE: case TK_CARET:
                case TK_SHL: case TK_SHR:
                case TK_EQ: case TK_NE: case TK_LT: case TK_GT:
                case TK_LE: case TK_GE:
                case TK_LOGAND: case TK_LOGOR:
                    return zc_node_is_const_expr(c, arena, n->left) &&
                           zc_node_is_const_expr(c, arena, n->right);
                default:
                    return false;
            }
        case NODE_TERNARY:
            return zc_node_is_const_expr(c, arena, n->left) &&
                   zc_node_is_const_expr(c, arena, n->right) &&
                   zc_node_is_const_expr(c, arena, (ASTRef)n->aux.alt);
        case NODE_INDEX: {
            ZeroCCASTNode *base = zero_cc_ast_get(arena, n->left);
            return (base && base->tag == NODE_STRING) &&
                   zc_node_is_const_expr(c, arena, n->right);
        }
        default:
            return false;
    }
}

/* Evaluate an AST node as a compile-time integer constant, recognizing
 * the __builtin_constant_p / __builtin_choose_expr builtins (which are
 * themselves constant expressions).  Returns 1 (with *out set) when the
 * node is a constant, 0 otherwise.  Used by __builtin_choose_expr's
 * runtime lowering to decide which operand to evaluate. */
static bool zc_ast_const_eval(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                              ASTRef ref, int64_t *out) {
    (void)c;
    if (ref == AST_NULL) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    switch (n->tag) {
        case NODE_INT:
        case NODE_BOOL:
            *out = n->int_val;
            return true;
        case NODE_FLOAT:
            *out = (int64_t)n->float_val;
            return true;
        case NODE_CAST:
            return zc_ast_const_eval(c, arena, n->left, out);
        case NODE_UNOP:
            if (!zc_ast_const_eval(c, arena, n->left, out)) return false;
            switch (n->aux.op) {
                case TK_PLUS: break;
                case TK_MINUS: *out = -*out; break;
                case TK_BANG: *out = !*out; break;
                case TK_TILDE: *out = ~*out; break;
                default: return false;
            }
            return true;
        case NODE_BINOP: {
            int64_t l, r;
            if (!zc_ast_const_eval(c, arena, n->left, &l)) return false;
            if (!zc_ast_const_eval(c, arena, n->right, &r)) return false;
            switch (n->aux.op) {
                case TK_PLUS: *out = l + r; break;
                case TK_MINUS: *out = l - r; break;
                case TK_STAR: *out = l * r; break;
                case TK_SLASH: if (r == 0) return false; *out = l / r; break;
                case TK_PERCENT: if (r == 0) return false; *out = l % r; break;
                case TK_AMP: *out = l & r; break;
                case TK_PIPE: *out = l | r; break;
                case TK_CARET: *out = l ^ r; break;
                case TK_SHL: *out = l << r; break;
                case TK_SHR: *out = l >> r; break;
                case TK_EQ: *out = (l == r); break;
                case TK_NE: *out = (l != r); break;
                case TK_LT: *out = (l < r); break;
                case TK_GT: *out = (l > r); break;
                case TK_LE: *out = (l <= r); break;
                case TK_GE: *out = (l >= r); break;
                case TK_LOGAND: *out = (l && r); break;
                case TK_LOGOR: *out = (l || r); break;
                default: return false;
            }
            return true;
        }
        case NODE_TERNARY: {
            int64_t cond;
            if (!zc_ast_const_eval(c, arena, n->left, &cond)) return false;
            if (cond) return zc_ast_const_eval(c, arena, n->right, out);
            return zc_ast_const_eval(c, arena, (ASTRef)n->aux.alt, out);
        }
        case NODE_CALL: {
            ZeroCCASTNode *cal = zero_cc_ast_get(arena, n->left);
            if (!cal || cal->tag != NODE_IDENT || !cal->s.str) return false;
            if (strcmp(cal->s.str, "__builtin_constant_p") == 0) {
                *out = zc_node_is_const_expr(c, arena, n->right) ? 1 : 0;
                return true;
            }
            if (strcmp(cal->s.str, "__builtin_choose_expr") == 0) {
                ASTRef r1 = n->right;
                if (r1 == AST_NULL) return false;
                ZeroCCASTNode *an1 = zero_cc_ast_get(arena, r1);
                ASTRef r2 = an1 ? an1->next : AST_NULL;
                ZeroCCASTNode *an2 = (r2 != AST_NULL) ? zero_cc_ast_get(arena, r2) : NULL;
                ASTRef r3 = an2 ? an2->next : AST_NULL;
                if (r2 == AST_NULL || r3 == AST_NULL) return false;
                int64_t cond;
                if (!zc_ast_const_eval(c, arena, r1, &cond)) return false;
                return zc_ast_const_eval(c, arena, cond ? r2 : r3, out);
            }
            if (zc_is_bitop_builtin_name(cal->s.str)) {
                int64_t arg_v = 0;
                if (!zc_ast_const_eval(c, arena, n->right, &arg_v)) return false;
                int nbits = zc_bitop_width(c, cal->s.str);
                *out = (int64_t)zc_eval_bitop_const(cal->s.str, (uint64_t)arg_v, nbits);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}


/* --- __builtin_{add,sub,mul}_overflow helpers ------------------------- */
/* 0/1 temp holding (a <cc> b) over already-computed values. */
static int zc_ovf_cmp_set(ZeroCCCompiler *c, int a, int b, int cc) {
    int t = c->next_temp_id++;
    ir_emit_cmp(c, a, b);
    ir_emit_cond_set(c, t, cc);
    return t;
}

/* 0/1 temp holding (a <cc> imm). */
static int zc_ovf_cmp_set_imm(ZeroCCCompiler *c, int a, int64_t imm, int cc) {
    int b = c->next_temp_id++;
    ir_emit_mov_imm(c, b, imm);
    return zc_ovf_cmp_set(c, a, b, cc);
}

static int zc_ovf_or(ZeroCCCompiler *c, int a, int b) {
    int t = c->next_temp_id++;
    ir_emit_or(c, t, a, b);
    return t;
}

static int zc_ovf_and(ZeroCCCompiler *c, int a, int b) {
    int t = c->next_temp_id++;
    ir_emit_and(c, t, a, b);
    return t;
}

/* 1 - a for a 0/1 temp (logical NOT). */
static int zc_ovf_not1(ZeroCCCompiler *c, int a) {
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
typedef enum ZCAtomicKind {
    ZC_AT_FETCH_ADD, ZC_AT_ADD_FETCH,
    ZC_AT_FETCH_SUB, ZC_AT_SUB_FETCH,
    ZC_AT_FETCH_OR,  ZC_AT_OR_FETCH,
    ZC_AT_FETCH_AND, ZC_AT_AND_FETCH,
    ZC_AT_FETCH_XOR, ZC_AT_XOR_FETCH,
    ZC_AT_CAS_OLD,   ZC_AT_CAS_BOOL,   ZC_AT_CAS_BOOL_SYNC,
    ZC_AT_EXCHANGE,  ZC_AT_STORE,
    ZC_AT_LOAD,      ZC_AT_STORE_0,
    ZC_AT_FENCE,     ZC_AT_TEST_AND_SET,
    ZC_AT_CLEAR,     ZC_AT_IS_LOCK_FREE
} ZCAtomicKind;

static const struct {
    const char   *name;
    ZCAtomicKind  kind;
} zc_atomic_builtin_table[] = {
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
static int zc_atomic_access_width(ZeroCCCompiler *c, const ZeroCCASTNode *arg0_node) {
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
static void zc_atomic_extend_value(ZeroCCCompiler *c, int temp,
                                   const ZeroCCASTNode *node, const TypeInfo *ti) {
    if (node && node->tag == NODE_INT) {
        int64_t v = node->int_val;
        if (v < INT32_MIN || v > INT32_MAX) return;  /* genuine 64-bit literal */
    }
    lower_extend_narrow(c, temp, ti);
}


int lower_expr_va_start(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* va_start(ap, last) - initialize the ABI-correct va_list.
         *
         * The callee prologue spilled ALL argument registers into a
         * 16-slot save area above the locals (see the variadic spill in
         * zero_cc_lower_ast): slots 0..5 = integer arg regs in order,
         * slots 6..13 = FP arg regs in order. Named parameters consume
         * the streams in order, so the streams start just past the
         * named count. Once a stream is exhausted (ptr == end) reads
         * continue from ovf - the shared stack-overflow area (the
         * caller's stack args, in argument order).
         *
         * va_list layout (5 x 8-byte pointers):
         *   +0 gp, +8 gp_end, +16 fp, +24 fp_end, +32 ovf. */
        int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
        ZeroCCFunction *fn = zero_cc_get_current_fn(c);

        if (ap_addr != TEMP_NONE && fn) {

            /* Save area base: above all named locals (mirrors the
             * prologue's base_spill_offset = locals-top + 8). */
            int32_t top_local = (int32_t)BE(c)->callee_save_frame_reserve;
            for (size_t i = 0; i < c->local_count; i++) {
                if (c->locals[i].stack_offset > top_local) top_local = c->locals[i].stack_offset;
            }
            int32_t save_base = top_local + 8;

            /* Named params consume the GP/FP streams in order (mirror
             * the prologue's param-save loop, incl. the hidden sret
             * pointer and multi-chunk struct params). */
            int gp_regs = BE(c)->conv->int_arg_reg_count;
            int fp_regs = BE(c)->conv->fp_arg_reg_count;
            int gp_idx = (fn->ret_struct_sz > 16 && c->isa != ISA_AARCH64) ? 1 : 0;
            int fp_idx = 0;
            int stack_named = 0;  /* stack-passed named params (ovf base) */
            for (size_t p = 0; p < fn->param_count && p < c->local_count; p++) {
                ZeroCCSym *ps = &c->locals[p];
                if (!ps->is_param) continue;
                bool pfp = (ps->type.tag == TYPE_FLOAT || ps->type.tag == TYPE_DOUBLE);
                /* >16B struct/union params take ONE hidden-pointer slot
                 * (SysV ABI), not one slot per 8-byte chunk. */
                bool phidden = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 16;
                bool pmulti = (ps->type.tag == TYPE_STRUCT || ps->type.tag == TYPE_UNION) && ps->type.size > 8 && !phidden;
                if (phidden) {
                    if (gp_idx < gp_regs) gp_idx++; else stack_named++;
                } else if (pmulti) {
                    int chunks = (int)((ps->type.size + 7) / 8);
                    for (int ch = 0; ch < chunks; ch++) { if (gp_idx < gp_regs) gp_idx++; else stack_named++; }
                } else if (pfp) {
                    if (fp_idx < fp_regs) fp_idx++; else stack_named++;
                } else {
                    if (gp_idx < gp_regs) gp_idx++; else stack_named++;
                }
            }

            int t;
            t = c->next_temp_id++;
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + gp_idx * 8));
            ir_emit_store_64(c, t, ap_addr);             /* +0 gp */

            t = c->next_temp_id++;
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + gp_regs * 8));
            int off8 = c->next_temp_id++;
            ir_emit_mov_imm(c, off8, 8);
            int ap_8 = c->next_temp_id++;
            ir_emit_add(c, ap_8, ap_addr, off8);
            ir_emit_store_64(c, t, ap_8);                /* +8 gp_end */

            t = c->next_temp_id++;
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + (gp_regs + fp_idx) * 8));
            int off16 = c->next_temp_id++;
            ir_emit_mov_imm(c, off16, 16);
            int ap_16 = c->next_temp_id++;
            ir_emit_add(c, ap_16, ap_addr, off16);
            ir_emit_store_64(c, t, ap_16);               /* +16 fp */

            t = c->next_temp_id++;
            ir_emit_lea(c, t, TEMP_NONE, -(save_base + (gp_regs + fp_regs) * 8));
            int off24 = c->next_temp_id++;
            ir_emit_mov_imm(c, off24, 24);
            int ap_24 = c->next_temp_id++;
            ir_emit_add(c, ap_24, ap_addr, off24);
            ir_emit_store_64(c, t, ap_24);               /* +24 fp_end */

            t = c->next_temp_id++;
            ir_emit_lea(c, t, TEMP_NONE, 16 + 8 * stack_named);
            int off32 = c->next_temp_id++;
            ir_emit_mov_imm(c, off32, 32);
            int ap_32 = c->next_temp_id++;
            ir_emit_add(c, ap_32, ap_addr, off32);
            ir_emit_store_64(c, t, ap_32);               /* +32 ovf */
        }

        c->last_expr_is_float = 0;
        return VREG_0;
}

int lower_expr_va_arg(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
        /* va_arg(ap, type) - extract the next variadic argument.
         * Integer/pointer targets read the GP stream (va_list +0/+8),
         * float/double targets the FP stream (+16/+24); when a stream
         * is exhausted (ptr == end) both continue from the shared
         * overflow pointer (+32). Values are read through the stream
         * pointer (frame-independent dereference). */
        TypeInfo *t_t = ast_node_get_typeinfo(c, node);
        bool is_fp = t_t && (t_t->kind == TYPE_KIND_FLOAT || t_t->kind == TYPE_KIND_DOUBLE);
        bool use_fp_stream = is_fp && !(c->isa == ISA_RISCV64 || c->isa == ISA_RISCV32);

        int result_temp = c->next_temp_id++;
        int ap_addr = zero_cc_lower_lvalue_addr(c, arena, node->left);
        if (ap_addr != TEMP_NONE) {
            int32_t ptr_field = use_fp_stream ? 16 : 0;    /* va_list member offset of the stream ptr */
            int32_t end_field = use_fp_stream ? 24 : 8;   /* ...and of its end sentinel */
            uint16_t ovf_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;

            int addr_ptr_field = ap_addr;
            if (ptr_field != 0) {
                int off_pf = c->next_temp_id++;
                ir_emit_mov_imm(c, off_pf, ptr_field);
                addr_ptr_field = c->next_temp_id++;
                ir_emit_add(c, addr_ptr_field, ap_addr, off_pf);
            }
            int off_ef = c->next_temp_id++;
            ir_emit_mov_imm(c, off_ef, end_field);
            int addr_end_field = c->next_temp_id++;
            ir_emit_add(c, addr_end_field, ap_addr, off_ef);

            int off_ovf = c->next_temp_id++;
            ir_emit_mov_imm(c, off_ovf, 32);
            int addr_ovf_field = c->next_temp_id++;
            ir_emit_add(c, addr_ovf_field, ap_addr, off_ovf);

            size_t struct_sz = (t_t && (t_t->kind == TYPE_KIND_STRUCT || t_t->kind == TYPE_KIND_UNION)) ? type_size(t_t) : 0;
            bool is_struct_hidden = (!is_fp && struct_sz > 16);
            bool is_struct2 = (!is_fp && struct_sz > 8 && struct_sz <= 16);
            if (is_struct_hidden) {
                /* >16B struct va_arg: the caller passed ONE hidden
                 * pointer slot (SysV ABI) - read it like a pointer;
                 * the result is the address of the caller's copy. */
                int hp = c->next_temp_id++;
                ir_emit_deref(c, hp, addr_ptr_field);    /* fresh stream ptr */
                int he = c->next_temp_id++;
                ir_emit_deref(c, he, addr_end_field);    /* fresh end sentinel */
                int hd = c->next_temp_id++;
                ir_emit_sub(c, hd, hp, he);
                uint16_t ho_lbl = c->ir_next_label++;
                uint16_t ha_lbl = c->ir_next_label++;
                ir_emit_je(c, (uint16_t)hd, ho_lbl);
                /* hp = ap[0] = the ADDRESS of the save slot; the slot
                 * holds the hidden POINTER to the caller's copy. The
                 * va_arg value for a >16B struct is that hidden pointer
                 * (an address the consumers copy from; see
                 * ast_expr_is_struct_addr) - one dereference of the slot. */
                ir_emit_deref(c, result_temp, hp);
                int hn8 = c->next_temp_id++;
                ir_emit_mov_imm(c, hn8, -8);
                int hp2 = c->next_temp_id++;
                ir_emit_add(c, hp2, hp, hn8);
                ir_emit_store_64(c, hp2, addr_ptr_field);
                ir_emit_jmp(c, ha_lbl);
                ir_emit_label(c, ho_lbl);
                int ho = c->next_temp_id++;
                ir_emit_deref(c, ho, addr_ovf_field);
                ir_emit_deref(c, result_temp, ho);
                int hp8 = c->next_temp_id++;
                ir_emit_mov_imm(c, hp8, 8);
                int ho2 = c->next_temp_id++;
                ir_emit_add(c, ho2, ho, hp8);
                ir_emit_store_64(c, ho2, addr_ovf_field);
                ir_emit_label(c, ha_lbl);
            } else if (is_struct2) {
                /* 16B (or 9-16B) struct va_arg: two 8-byte chunks, but
                 * each chunk needs its OWN GP-exhaustion check - the
                 * chunks can straddle the register-stack boundary (the
                 * last register chunk and the first stack chunk, e.g.
                 * 4 x 16B structs passed variadically: chunks 0-4 in
                 * registers, chunks 5-7 on the caller's stack). */
                for (int ch = 0; ch < 2; ch++) {
                    int cp = c->next_temp_id++;
                    ir_emit_deref(c, cp, addr_ptr_field);    /* fresh stream ptr */
                    int ce = c->next_temp_id++;
                    ir_emit_deref(c, ce, addr_end_field);    /* fresh end sentinel */
                    int cd = c->next_temp_id++;
                    ir_emit_sub(c, cd, cp, ce);
                    uint16_t o_lbl = c->ir_next_label++;
                    uint16_t a_lbl = c->ir_next_label++;
                    ir_emit_je(c, (uint16_t)cd, o_lbl);
                    int dst = (ch == 0) ? result_temp : VREG_2;
                    ir_emit_deref(c, dst, cp);
                    int n8 = c->next_temp_id++;
                    ir_emit_mov_imm(c, n8, -8);
                    int cp2 = c->next_temp_id++;
                    ir_emit_add(c, cp2, cp, n8);
                    ir_emit_store_64(c, cp2, addr_ptr_field);
                    ir_emit_jmp(c, a_lbl);
                    ir_emit_label(c, o_lbl);
                    int co = c->next_temp_id++;
                    ir_emit_deref(c, co, addr_ovf_field);
                    ir_emit_deref(c, dst, co);
                    int p8 = c->next_temp_id++;
                    ir_emit_mov_imm(c, p8, 8);
                    int co2 = c->next_temp_id++;
                    ir_emit_add(c, co2, co, p8);
                    ir_emit_store_64(c, co2, addr_ovf_field);
                    ir_emit_label(c, a_lbl);
                }
            } else {
            int t_ptr = c->next_temp_id++;
            ir_emit_deref(c, t_ptr, addr_ptr_field);
            int t_end = c->next_temp_id++;
            ir_emit_deref(c, t_end, addr_end_field);
            int t_diff = c->next_temp_id++;
            ir_emit_sub(c, t_diff, t_ptr, t_end);
            ir_emit_je(c, (uint16_t)t_diff, ovf_label);

            /* Save-area path. The save slots extend toward MORE
             * negative offsets (slot i at -(base + i*8)), so the
             * stream pointer DECREASES by 8 per argument; it is
             * exhausted when it reaches the end sentinel. */
            if (is_fp) {
                if (t_t->kind == TYPE_KIND_FLOAT) ir_emit_vloadss(c, (uint16_t)result_temp, (uint16_t)t_ptr);
                else ir_emit_vloadsd(c, (uint16_t)result_temp, (uint16_t)t_ptr);
                int t_neg8 = c->next_temp_id++;
                ir_emit_mov_imm(c, t_neg8, -8);
                int t_ptr2 = c->next_temp_id++;
                ir_emit_add(c, t_ptr2, t_ptr, t_neg8);
                ir_emit_store_64(c, t_ptr2, addr_ptr_field);
            } else {
                ir_emit_deref(c, result_temp, t_ptr);
                int t_neg8 = c->next_temp_id++;
                ir_emit_mov_imm(c, t_neg8, -8);
                int t_ptr2 = c->next_temp_id++;
                ir_emit_add(c, t_ptr2, t_ptr, t_neg8);
                ir_emit_store_64(c, t_ptr2, addr_ptr_field);
            }
            ir_emit_jmp(c, end_label);

            /* Stack-overflow path: the caller's stack args sit at
             * fp+16, fp+24, ... - the pointer INCREASES by 8. */
            ir_emit_label(c, ovf_label);
            int t_ovf = c->next_temp_id++;
            ir_emit_deref(c, t_ovf, addr_ovf_field);
            if (is_fp) {
                if (t_t->kind == TYPE_KIND_FLOAT) ir_emit_vloadss(c, (uint16_t)result_temp, (uint16_t)t_ovf);
                else ir_emit_vloadsd(c, (uint16_t)result_temp, (uint16_t)t_ovf);
                int t_pos8 = c->next_temp_id++;
                ir_emit_mov_imm(c, t_pos8, 8);
                int t_ovf2 = c->next_temp_id++;
                ir_emit_add(c, t_ovf2, t_ovf, t_pos8);
                ir_emit_store_64(c, t_ovf2, addr_ovf_field);
            } else {
                ir_emit_deref(c, result_temp, t_ovf);
                int t_pos8 = c->next_temp_id++;
                ir_emit_mov_imm(c, t_pos8, 8);
                int t_ovf2 = c->next_temp_id++;
                ir_emit_add(c, t_ovf2, t_ovf, t_pos8);
                ir_emit_store_64(c, t_ovf2, addr_ovf_field);
            }
            ir_emit_label(c, end_label);
            }

            /* Narrow/widen integer targets to their true C width
             * (values arrived int-promoted / zero-extended). */
            if (!is_fp && t_t) {
                size_t tsz = type_size(t_t);
                bool tu = (t_t->modifiers & TYPE_MOD_UNSIGNED) != 0;
                if (tsz == 4) { if (tu) ir_emit_zext(c, result_temp, 4); else ir_emit_sext(c, result_temp, 4); }
                else if (tsz == 2) { if (tu) ir_emit_zext(c, result_temp, 2); else ir_emit_sext(c, result_temp, 2); }
                else if (tsz == 1) { if (tu) ir_emit_zext(c, result_temp, 1); else ir_emit_sext(c, result_temp, 1); }
            }
            c->last_expr_is_float = is_fp;
        } else {
            /* No identifiable va_list - legacy single-pointer
             * behaviour (dereference and bump) as a fallback. */
            int ap_temp = lower_expr(c, arena, node->left);
            ir_emit_deref(c, result_temp, ap_temp);
            int eight_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, eight_temp, 8);
            int new_ap_temp = c->next_temp_id++;
            ir_emit_add(c, new_ap_temp, ap_temp, eight_temp);
            c->last_expr_is_float = is_fp;
        }
        return result_temp;
}


bool lower_expr_builtin(ZeroCCCompiler *c, ZeroCCASTArena *arena,
                        ZeroCCASTNode *node, const char *name,
                        int *out_temp) {
    if (!name || !out_temp) return false;
    ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
    bool callee_is_ident = (callee && callee->tag == NODE_IDENT);
    (void)callee_is_ident;

        /* __builtin_return_address(level) / __builtin_frame_address(level) */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_return_address") == 0 ||
             strcmp(callee->s.str, "__builtin_frame_address") == 0)) {
            bool is_ret_addr = (strcmp(callee->s.str, "__builtin_return_address") == 0);
            c->lowering.has_frame_address = true;

            int cur_fp = c->next_temp_id++;
            ir_emit_lea(c, cur_fp, BE(c)->fp_reg, 0);

            int fp = cur_fp;
            int64_t const_lvl = 0;
            if (node->right != AST_NULL && zc_ast_const_eval(c, arena, node->right, &const_lvl)) {
                if (const_lvl < 0) const_lvl = 0;
                if (const_lvl > 100) const_lvl = 100;
                for (int64_t i = 0; i < const_lvl; i++) {
                    int next_fp = c->next_temp_id++;
                    ir_emit_deref(c, next_fp, fp);
                    fp = next_fp;
                }
            } else if (node->right != AST_NULL) {
                int lvl_temp = lower_expr(c, arena, node->right);
                int cnt_temp = c->next_temp_id++;
                ir_emit_mov_reg(c, cnt_temp, lvl_temp);
                uint16_t loop_lbl = (uint16_t)c->ir_next_label++;
                uint16_t end_lbl = (uint16_t)c->ir_next_label++;

                ir_emit_label(c, loop_lbl);
                int zero_t = c->next_temp_id++;
                ir_emit_mov_imm(c, zero_t, 0);
                ir_emit_cmp(c, cnt_temp, zero_t);
                int is_le = c->next_temp_id++;
                ir_emit_cond_set(c, is_le, CC_LE);
                ir_emit_jne(c, (uint16_t)is_le, end_lbl);

                int next_fp = c->next_temp_id++;
                ir_emit_deref(c, next_fp, fp);
                fp = next_fp;

                int one = c->next_temp_id++;
                ir_emit_mov_imm(c, one, 1);
                ir_emit_sub(c, cnt_temp, cnt_temp, one);
                ir_emit_jmp(c, loop_lbl);

                ir_emit_label(c, end_lbl);
            }

            int res = fp;
            if (is_ret_addr) {
                int ra_off = (c->isa == ISA_RISCV32) ? 4 : 8;
                int off_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, off_temp, ra_off);
                int ra_ptr = c->next_temp_id++;
                ir_emit_add(c, ra_ptr, fp, off_temp);
                res = c->next_temp_id++;
                ir_emit_deref(c, res, ra_ptr);
            }

            c->last_expr_is_float = 0;
            *out_temp = res;
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
                int is_zero = c->next_temp_id++;
                ir_emit_cmp_zero(c, val_raw);
                ir_emit_cond_set(c, is_zero, CC_EQ);
                int val_norm = c->next_temp_id++;
                ir_emit_or(c, val_norm, val_raw, is_zero);

                ir_emit_longjmp(c, (uint16_t)buf_temp, (uint16_t)val_norm);
                /* longjmp never returns: block fall-through with an infinite
                 * loop (the setjmp site in the target function resumes). */
                uint16_t loop_lbl = (uint16_t)c->ir_next_label++;
                ir_emit_label(c, loop_lbl);
                ir_emit_jmp(c, loop_lbl);
                *out_temp = VREG_0;
                return true;
            }

            /* setjmp: emit inline OP_SETJMP (sets VREG_0 = 0 on first pass,
             * or lands at resume label with VREG_0 = val from OP_LONGJMP),
             * then capture VREG_0 into a fresh allocatable temp. */
            uint16_t resume_lbl = (uint16_t)c->ir_next_label++;
            ir_emit_setjmp(c, (uint16_t)buf_temp, resume_lbl);
            int res = c->next_temp_id++;
            ir_emit_mov_reg(c, res, VREG_0);
            *out_temp = res;
            return true;
        }

        /* __builtin_alloca(size) / alloca(size) - dynamic stack allocation */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_alloca") == 0 ||
             strcmp(callee->s.str, "alloca") == 0)) {
            int size_temp = lower_expr(c, arena, node->right);
            int add15 = c->next_temp_id++;
            ir_emit_mov_imm(c, add15, 15);
            int size_p15 = c->next_temp_id++;
            ir_emit_add(c, size_p15, size_temp, add15);
            int mask16 = c->next_temp_id++;
            ir_emit_mov_imm(c, mask16, -16);
            int aligned_size = c->next_temp_id++;
            ir_emit_and(c, aligned_size, size_p15, mask16);
            int ptr_temp = c->next_temp_id++;
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
            int res = c->next_temp_id++;
            int la = addr;
            if (is_imag) {
                int off = c->next_temp_id++;
                ir_emit_mov_imm(c, off, (int)esz);
                la = c->next_temp_id++;
                ir_emit_add(c, la, addr, off);
            }
            if (is_elem_fp) {
                if (esz == 4) ir_emit_vloadss(c, res, (uint16_t)la);
                else ir_emit_vloadsd(c, res, (uint16_t)la);
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

        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_conj") == 0 ||
             strcmp(callee->s.str, "__builtin_conjf") == 0 ||
             strcmp(callee->s.str, "__builtin_conjl") == 0)) {
            ZeroCCASTNode *cx_node = zero_cc_ast_get(arena, node->right);
            TypeInfo *cx_ti = cx_node ? ast_node_get_typeinfo(c, cx_node) : NULL;
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
            size_t esz = el_ti ? type_size(el_ti) : (el_zt ? el_zt->size :
                         (strcmp(callee->s.str, "__builtin_conjf") == 0 ? 4 : 8));
            if (esz == 0) esz = 8;
            bool is_elem_fp = el_ti ? type_is_floating(el_ti) :
                              (el_zt ? (el_zt->tag == TYPE_FLOAT || el_zt->tag == TYPE_DOUBLE) : true);
            if (is_elem_fp && esz != 4 && esz != 8) esz = 8;
            int addr = lower_expr(c, arena, node->right);
            int rh = c->next_temp_id++, li = c->next_temp_id++;
            int off = c->next_temp_id++;
            ir_emit_mov_imm(c, off, (int)esz);
            int la = c->next_temp_id++;
            ir_emit_add(c, la, addr, off);
            if (is_elem_fp) {
                if (esz == 4) {
                    ir_emit_vloadss(c, (uint16_t)rh, (uint16_t)addr);
                    ir_emit_vloadss(c, (uint16_t)li, (uint16_t)la);
                } else {
                    ir_emit_vloadsd(c, (uint16_t)rh, (uint16_t)addr);
                    ir_emit_vloadsd(c, (uint16_t)li, (uint16_t)la);
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
            int neg_li = c->next_temp_id++;
            if (is_elem_fp) {
                int zero_i = c->next_temp_id++;
                ir_emit_mov_imm(c, zero_i, 0);
                int zero_f = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
                ir_emit_fsub(c, (uint16_t)neg_li, (uint16_t)zero_f, (uint16_t)li);
            } else {
                ir_emit_neg(c, neg_li, li);
            }

            size_t csz = esz * 2;
            c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
            c->current_stack_frame += (int32_t)csz;
            int32_t res_off = c->current_stack_frame;
            int res_addr = c->next_temp_id++;
            ir_emit_lea(c, res_addr, BE(c)->fp_reg, -res_off);
            int off_m = c->next_temp_id++;
            ir_emit_mov_imm(c, off_m, (int)esz);
            int res_a2 = c->next_temp_id++;
            ir_emit_add(c, res_a2, res_addr, off_m);
            if (is_elem_fp) {
                if (esz == 4) {
                    ir_emit_vstoress(c, (uint16_t)res_addr, (uint16_t)rh);
                    ir_emit_vstoress(c, (uint16_t)res_a2, (uint16_t)neg_li);
                } else {
                    ir_emit_vstoresd(c, (uint16_t)res_addr, (uint16_t)rh);
                    ir_emit_vstoresd(c, (uint16_t)res_a2, (uint16_t)neg_li);
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
                            int bi_off = c->next_temp_id++;
                            if (i == 0) { ir_emit_mov_reg(c, bi_off, b_addr); }
                            else {
                                int bo = c->next_temp_id++;
                                ir_emit_mov_imm(c, bo, (int32_t)(i * idsz));
                                ir_emit_add(c, bi_off, b_addr, bo);
                            }
                            int idx = c->next_temp_id++;
                            if (idsz == 1) { ir_emit_load_byte(c, idx, bi_off); ir_emit_zext(c, idx, 1); }
                            else if (idsz == 2) { ir_emit_load_16(c, idx, bi_off); ir_emit_zext(c, idx, 2); }
                            else if (idsz == 4) { ir_emit_load_32(c, idx, bi_off); ir_emit_zext(c, idx, 4); }
                            else { ir_emit_deref(c, idx, bi_off); }
                            /* m = idx & (n-1) */
                            int m_mask = c->next_temp_id++;
                            ir_emit_mov_imm(c, m_mask, mask_nm1);
                            int m = c->next_temp_id++;
                            ir_emit_and(c, m, idx, m_mask);
                            /* off = m * elsz */
                            int off = c->next_temp_id++;
                            if (elsz > 1) {
                                int es = c->next_temp_id++;
                                ir_emit_mov_imm(c, es, (int32_t)elsz);
                                ir_emit_mul(c, off, m, es);
                            } else {
                                ir_emit_mov_reg(c, off, m);
                            }
                            int aa = c->next_temp_id++;
                            ir_emit_add(c, aa, a_addr, off);
                            int ba = c->next_temp_id++;
                            ir_emit_add(c, ba, b_addr, off);
                            int av = c->next_temp_id++;
                            if (elsz == 1) { ir_emit_load_byte(c, av, aa); ir_emit_zext(c, av, 1); }
                            else if (elsz == 2) { ir_emit_load_16(c, av, aa); ir_emit_zext(c, av, 2); }
                            else if (elsz == 4) { ir_emit_load_32(c, av, aa); ir_emit_zext(c, av, 4); }
                            else { ir_emit_deref(c, av, aa); }
                            int bv = c->next_temp_id++;
                            if (elsz == 1) { ir_emit_load_byte(c, bv, ba); ir_emit_zext(c, bv, 1); }
                            else if (elsz == 2) { ir_emit_load_16(c, bv, ba); ir_emit_zext(c, bv, 2); }
                            else if (elsz == 4) { ir_emit_load_32(c, bv, ba); ir_emit_zext(c, bv, 4); }
                            else { ir_emit_deref(c, bv, ba); }
                            /* sel = (idx & n) == 0  ->  0/1 */
                            int n_bit = c->next_temp_id++;
                            ir_emit_mov_imm(c, n_bit, bit_n);
                            int ib = c->next_temp_id++;
                            ir_emit_and(c, ib, idx, n_bit);
                            int zero_t = c->next_temp_id++;
                            ir_emit_mov_imm(c, zero_t, 0);
                            ir_emit_cmp(c, (uint16_t)ib, (uint16_t)zero_t);
                            int cmp_t = c->next_temp_id++;
                            ir_emit_cond_set(c, (uint16_t)cmp_t, CC_EQ);
                            int sel = c->next_temp_id++;
                            ir_emit_mov_imm(c, sel, -1);
                            int selm = c->next_temp_id++;
                            ir_emit_mul(c, selm, cmp_t, sel);
                            int nv = c->next_temp_id++;
                            int all_ones = c->next_temp_id++;
                            ir_emit_mov_imm(c, all_ones, -1);
                            ir_emit_xor(c, nv, selm, all_ones);
                            int s1 = c->next_temp_id++;
                            ir_emit_and(c, s1, selm, av);
                            int s2 = c->next_temp_id++;
                            ir_emit_and(c, s2, nv, bv);
                            int lv = c->next_temp_id++;
                            ir_emit_or(c, lv, s1, s2);
                            /* store lane i into the result temp */
                            int ra = c->next_temp_id++;
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
                        int addr = c->next_temp_id++;
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
                int t_res = c->next_temp_id++;
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
                int t_one = c->next_temp_id++;
                ir_emit_mov_imm(c, t_one, 1);
                t_result = c->next_temp_id++;
                ir_emit(c, (Instr){.op = OP_AND, .temp_dest = (uint16_t)t_result,
                                   .temp_src1 = (uint16_t)t_pc, .temp_src2 = (uint16_t)t_one});
            } else {
                /* __builtin_ffs / ffsl / ffsll */
                int t_in = c->next_temp_id++;
                ir_emit_mov_reg(c, t_in, t_x);
                if (nbits == 32) ir_emit_zext(c, t_in, 4);
                int t_ctz = zc_emit_ctz_nbits(c, t_in, nbits);
                int t_one = c->next_temp_id++;
                ir_emit_mov_imm(c, t_one, 1);
                int t_zero = c->next_temp_id++;
                ir_emit_mov_imm(c, t_zero, 0);
                int t_plus1 = c->next_temp_id++;
                ir_emit(c, (Instr){.op = OP_ADD, .temp_dest = (uint16_t)t_plus1,
                                   .temp_src1 = (uint16_t)t_ctz, .temp_src2 = (uint16_t)t_one});
                int t_nz = c->next_temp_id++;
                ir_emit_cmp(c, t_in, t_zero);
                ir_emit(c, (Instr){.op = OP_COND_SET, .temp_dest = (uint16_t)t_nz,
                                   .temp_src1 = (uint16_t)t_in, .temp_src2 = (uint16_t)t_zero,
                                   .cond = CC_NE});
                t_result = c->next_temp_id++;
                ir_emit(c, (Instr){.op = OP_MUL, .temp_dest = (uint16_t)t_result,
                                   .temp_src1 = (uint16_t)t_plus1, .temp_src2 = (uint16_t)t_nz});
            }
            c->last_expr_is_float = 0;
            *out_temp = t_result;
            return true;
        }

        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_va_arg_pack") == 0 ||
             strcmp(callee->s.str, "__builtin_va_arg_pack_len") == 0)) {
            int temp = c->next_temp_id++;
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
            int temp = c->next_temp_id++;
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
            int temp = c->next_temp_id++;
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
            int sel = c->next_temp_id++;
            ir_emit_mov_reg(c, sel, av);
            int zt = c->next_temp_id++;
            ir_emit_mov_imm(c, zt, 0);
            ir_emit_cmp(c, cv, zt);
            int nz = c->next_temp_id++;
            ir_emit_cond_set(c, (uint16_t)nz, CC_NE);
            uint16_t done_lbl = (uint16_t)c->ir_next_label++;
            ir_emit_je(c, (uint16_t)nz, done_lbl);
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
            int temp = c->next_temp_id++;
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
            int u_ff = c->next_temp_id++; ir_emit_mov_imm(c, u_ff, 0xFF);
            int u_8  = c->next_temp_id++; ir_emit_mov_imm(c, u_8,  8);
            int u_b0 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)u_b0,
                .temp_src1=(uint16_t)u_a,.temp_src2=(uint16_t)u_ff});
            int u_a8 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)u_a8,
                .temp_src1=(uint16_t)u_a,.temp_src2=(uint16_t)u_8});
            int u_b1 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)u_b1,
                .temp_src1=(uint16_t)u_a8,.temp_src2=(uint16_t)u_ff});
            int u_b0s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)u_b0s,
                .temp_src1=(uint16_t)u_b0,.temp_src2=(uint16_t)u_8});
            int u_result = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)u_result,
                .temp_src1=(uint16_t)u_b0s,.temp_src2=(uint16_t)u_b1});
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
            int t_ff = c->next_temp_id++; ir_emit_mov_imm(c, t_ff, 0xFF);
            int t_8  = c->next_temp_id++; ir_emit_mov_imm(c, t_8,  8);
            int t_16 = c->next_temp_id++; ir_emit_mov_imm(c, t_16, 16);
            int t_24 = c->next_temp_id++; ir_emit_mov_imm(c, t_24, 24);
            int t_b0 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b0,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_ff});
            int t_a8 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a8,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_8});
            int t_b1 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b1,
                .temp_src1=(uint16_t)t_a8,.temp_src2=(uint16_t)t_ff});
            int t_a16 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a16,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_16});
            int t_b2 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b2,
                .temp_src1=(uint16_t)t_a16,.temp_src2=(uint16_t)t_ff});
            int t_a24 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)t_a24,
                .temp_src1=(uint16_t)t_a,.temp_src2=(uint16_t)t_24});
            int t_b3 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)t_b3,
                .temp_src1=(uint16_t)t_a24,.temp_src2=(uint16_t)t_ff});
            int t_b0s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b0s,
                .temp_src1=(uint16_t)t_b0,.temp_src2=(uint16_t)t_24});
            int t_b1s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b1s,
                .temp_src1=(uint16_t)t_b1,.temp_src2=(uint16_t)t_16});
            int t_b2s = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)t_b2s,
                .temp_src1=(uint16_t)t_b2,.temp_src2=(uint16_t)t_8});
            int t_r1 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)t_r1,
                .temp_src1=(uint16_t)t_b0s,.temp_src2=(uint16_t)t_b1s});
            int t_r2 = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)t_r2,
                .temp_src1=(uint16_t)t_r1,.temp_src2=(uint16_t)t_b2s});
            int t_result = c->next_temp_id++;
            ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)t_result,
                .temp_src1=(uint16_t)t_r2,.temp_src2=(uint16_t)t_b3});
            c->last_expr_is_float = 0;
            *out_temp = t_result;
            return true;
        }

        /* __builtin_inf() / __builtin_inff() / __builtin_nan(s) /
         * __builtin_nanf(s): FP special constants (c-torture
         * ieee/copysign1/2). The string argument (when present) is
         * ignored; ZeroCC models float as double, so the result is
         * always an 8-byte double constant - the caller's conversion
         * does any narrowing. Previously emitted as undefined external
         * calls (runtime crash). */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_inf") == 0 ||
             strcmp(callee->s.str, "__builtin_inff") == 0 ||
             strcmp(callee->s.str, "__builtin_infl") == 0 ||
             strcmp(callee->s.str, "__builtin_nan") == 0 ||
             strcmp(callee->s.str, "__builtin_nanf") == 0 ||
             strcmp(callee->s.str, "__builtin_nanl") == 0)) {
            bool is_nan = (strcmp(callee->s.str, "__builtin_nan") == 0 ||
                           strcmp(callee->s.str, "__builtin_nanf") == 0 ||
                           strcmp(callee->s.str, "__builtin_nanl") == 0);
            uint64_t fpbits = is_nan ? 0x7FF8000000000000ULL : 0x7FF0000000000000ULL;
            double fpc; memcpy(&fpc, &fpbits, sizeof(fpc));
            size_t fpad = (8 - (c->rodata_section.length % 8)) % 8;
            if (fpad) {
                uint8_t z8[8] = {0};
                zero_cc_buf_put_bytes(&c->rodata_section, z8, fpad);
            }
            size_t fro_off = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &fpc, sizeof(double));
            int ft = c->next_temp_id++;
            ir_emit_fmov_imm(c, (uint16_t)ft, (int64_t)fro_off);
            c->last_expr_is_float = 1;
            *out_temp = ft;
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
            int s_ff = c->next_temp_id++; ir_emit_mov_imm(c, s_ff, 0xFF);
            int s_b[8];
            int sh[8] = {0, 8, 16, 24, 32, 40, 48, 56};
            int shd[8] = {56, 48, 40, 32, 24, 16, 8, 0};
            for (int i = 0; i < 8; i++) {
                int s_s = c->next_temp_id++; ir_emit_mov_imm(c, s_s, sh[i]);
                int s_t = c->next_temp_id++;
                ir_emit(c, (Instr){.op=OP_SHR,.temp_dest=(uint16_t)s_t,
                    .temp_src1=(uint16_t)s_a,.temp_src2=(uint16_t)s_s});
                s_b[i] = c->next_temp_id++;
                ir_emit(c, (Instr){.op=OP_AND,.temp_dest=(uint16_t)s_b[i],
                    .temp_src1=(uint16_t)s_t,.temp_src2=(uint16_t)s_ff});
            }
            int s_r = c->next_temp_id++;
            for (int i = 0; i < 8; i++) {
                int s_s = c->next_temp_id++; ir_emit_mov_imm(c, s_s, shd[i]);
                int s_t = c->next_temp_id++;
                ir_emit(c, (Instr){.op=OP_SHL,.temp_dest=(uint16_t)s_t,
                    .temp_src1=(uint16_t)s_b[i],.temp_src2=(uint16_t)s_s});
                if (i == 0) {
                    ir_emit_mov_reg(c, s_r, s_t);
                    continue;
                }
                ir_emit(c, (Instr){.op=OP_OR,.temp_dest=(uint16_t)s_r,
                    .temp_src1=(uint16_t)s_r,.temp_src2=(uint16_t)s_t});
            }
            c->last_expr_is_float = 0;
            *out_temp = s_r;
            return true;
        }

        /* __builtin_inf() / __builtin_inff() - return infinity. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_inf") == 0 ||
             strcmp(callee->s.str, "__builtin_inff") == 0 ||
             strcmp(callee->s.str, "__builtin_infl") == 0 ||
             strcmp(callee->s.str, "__builtin_huge_val") == 0 ||
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
            int temp = c->next_temp_id++;
            ir_emit_fmov_imm(c, (uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            *out_temp = temp;
            return true;
        }

        /* __builtin_nan() / __builtin_nanf() - return quiet NaN. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_nan") == 0 ||
             strcmp(callee->s.str, "__builtin_nanf") == 0 ||
             strcmp(callee->s.str, "__builtin_nanl") == 0)) {
            union { double d; int64_t i; } u;
            u.i = 0x7FF8000000000000LL;
            double qnan = u.d;
            size_t pad = (8 - (c->rodata_section.length % 8)) % 8;
            if (pad) { uint8_t z8[8] = {0}; zero_cc_buf_put_bytes(&c->rodata_section, z8, pad); }
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, &qnan, sizeof(double));
            int temp = c->next_temp_id++;
            ir_emit_fmov_imm(c, (uint16_t)temp, (int64_t)rodata_offset);
            c->last_expr_is_float = 1;
            *out_temp = temp;
            return true;
        }

        /* llabs() / labs() / imaxabs() / abs() - integer absolute value */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_llabs") == 0 ||
             strcmp(callee->s.str, "llabs") == 0 ||
             strcmp(callee->s.str, "__builtin_labs") == 0 ||
             strcmp(callee->s.str, "labs") == 0 ||
             strcmp(callee->s.str, "__builtin_imaxabs") == 0 ||
             strcmp(callee->s.str, "imaxabs") == 0 ||
             strcmp(callee->s.str, "__builtin_abs") == 0 ||
             strcmp(callee->s.str, "abs") == 0)) {
            bool is_32bit = (strcmp(callee->s.str, "__builtin_abs") == 0 ||
                             strcmp(callee->s.str, "abs") == 0);
            int arg_temp = lower_expr(c, arena, node->right);
            if (is_32bit) {
                ir_emit_sext(c, arg_temp, 4);
            }
            int zero_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_temp, 0);
            int cond_temp = c->next_temp_id++;
            ir_emit_cmp(c, arg_temp, zero_temp);
            ir_emit_cond_set(c, cond_temp, CC_LT);
            uint16_t pos_label = c->ir_next_label++;
            uint16_t end_label = c->ir_next_label++;
            ir_emit_je(c, (uint16_t)cond_temp, pos_label);
            int neg_temp = c->next_temp_id++;
            ir_emit_sub(c, neg_temp, zero_temp, arg_temp);
            int res_temp = c->next_temp_id++;
            ir_emit_mov_reg(c, res_temp, neg_temp);
            ir_emit_jmp(c, end_label);
            ir_emit_label(c, pos_label);
            ir_emit_mov_reg(c, res_temp, arg_temp);
            ir_emit_label(c, end_label);
            if (is_32bit) {
                ir_emit_sext(c, res_temp, 4);
            }
            c->last_expr_is_float = 0;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_fabs() / __builtin_fabsf() - absolute value.
         * Clears the sign bit (bit 63) in IEEE 754 representation. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_fabs") == 0 ||
             strcmp(callee->s.str, "__builtin_fabsf") == 0 ||
             strcmp(callee->s.str, "__builtin_fabsl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint16_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = c->next_temp_id++;
            ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
            int mask_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, mask_temp, 0x7FFFFFFFFFFFFFFFLL);
            int abs_int = c->next_temp_id++;
            ir_emit_and(c, abs_int, int_temp, mask_temp);
            ir_emit_store(c, abs_int, BE(c)->fp_reg, -slot);
            int res_temp = c->next_temp_id++;
            ir_emit_fload(c, (uint16_t)res_temp, TEMP_NONE, -slot);
            c->last_expr_is_float = 1;
            *out_temp = res_temp;
            return true;
        }

        /* __builtin_isinf() / __builtin_isinff() - infinity test.
         * In IEEE 754, x != 0.0 && (x * 2.0 == x) holds iff x is +/-inf. */
        if (callee_is_ident && callee->s.str &&
            (strcmp(callee->s.str, "__builtin_isinf") == 0 ||
             strcmp(callee->s.str, "__builtin_isinff") == 0 ||
             strcmp(callee->s.str, "__builtin_isinfl") == 0)) {
            int arg_temp = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            int two_i = c->next_temp_id++;
            ir_emit_mov_imm(c, two_i, 2);
            int two_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)two_f, (uint16_t)two_i);

            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);

            int mul_temp = c->next_temp_id++;
            ir_emit_fmul(c, (uint16_t)mul_temp, (uint16_t)arg_temp, (uint16_t)two_f);

            int c1 = c->next_temp_id++;
            ir_emit_cmp(c, arg_temp, zero_f);
            ir_emit_cond_set(c, c1, CC_NE);

            int c2 = c->next_temp_id++;
            ir_emit_cmp(c, mul_temp, arg_temp);
            ir_emit_cond_set(c, c2, CC_EQ);

            int res_temp = c->next_temp_id++;
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
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)arg_temp);
                arg_temp = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint16_t)arg_temp, TEMP_NONE, -slot);
            int int_temp = c->next_temp_id++;
            ir_emit_load(c, int_temp, BE(c)->fp_reg, -slot);
            int shift_imm = c->next_temp_id++;
            ir_emit_mov_imm(c, shift_imm, 63);
            int res_temp = c->next_temp_id++;
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
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                    t_x = conv;
                }
                int t_y = lower_expr(c, arena, ia2);
                if (!c->last_expr_is_float) {
                    int conv = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_y);
                    t_y = conv;
                }
                int res_temp = c->next_temp_id++;
                if (strncmp(bn, "__builtin_isunordered", 19) == 0) {
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, res_temp, 12); /* CC_FP_UNORD */
                } else if (strncmp(bn, "__builtin_islessgreater", 21) == 0) {
                    int t_lt = c->next_temp_id++;
                    ir_emit_cmp(c, t_x, t_y);
                    ir_emit_cond_set(c, t_lt, CC_LT);
                    int t_gt = c->next_temp_id++;
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

        /* __builtin_isnan(x) - NaN is the only value that is not
         * equal to itself. */
        if (callee_is_ident && callee->s.str &&
            strncmp(callee->s.str, "__builtin_isnan", 13) == 0) {
            int t_x = lower_expr(c, arena, node->right);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int res_temp = c->next_temp_id++;
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
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
            int t_x0 = c->next_temp_id++;
            ir_emit_fmul(c, (uint16_t)t_x0, (uint16_t)t_x, (uint16_t)zero_f);
            int t_zero = c->next_temp_id++;
            ir_emit_cmp(c, t_x0, zero_f);
            ir_emit_cond_set(c, t_zero, CC_EQ);
            int t_eq = c->next_temp_id++;
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, t_eq, CC_EQ);
            int res_temp = c->next_temp_id++;
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
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int zero_i = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_i, 0);
            int zero_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
            int p230_i = c->next_temp_id++;
            ir_emit_mov_imm(c, p230_i, 1073741824); /* 2^30 */
            int p230_f = c->next_temp_id++;
            ir_emit_cvtsi2sd(c, (uint16_t)p230_f, (uint16_t)p230_i);
            int t_xd = c->next_temp_id++;
            ir_emit_fdiv(c, (uint16_t)t_xd, (uint16_t)t_x, (uint16_t)p230_f);
            int t_nz = c->next_temp_id++;
            ir_emit_cmp(c, t_x, zero_f);
            ir_emit_cond_set(c, t_nz, CC_NE);
            int t_ne = c->next_temp_id++;
            ir_emit_cmp(c, t_xd, t_x);
            ir_emit_cond_set(c, t_ne, CC_NE);
            int t_eq = c->next_temp_id++;
            ir_emit_cmp(c, t_x, t_x);
            ir_emit_cond_set(c, t_eq, CC_EQ);
            int t_a = c->next_temp_id++;
            ir_emit_and(c, t_a, t_nz, t_ne);
            int res_temp = c->next_temp_id++;
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
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_x);
                t_x = conv;
            }
            int t_y = lower_expr(c, arena, ca2);
            if (!c->last_expr_is_float) {
                int conv = c->next_temp_id++;
                ir_emit_cvtsi2sd(c, (uint16_t)conv, (uint16_t)t_y);
                t_y = conv;
            }
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t slot = c->current_stack_frame;
            ir_emit_fstore(c, (uint16_t)t_y, TEMP_NONE, -slot);
            int y_int = c->next_temp_id++;
            ir_emit_load(c, y_int, BE(c)->fp_reg, -slot);
            int sh63 = c->next_temp_id++;
            ir_emit_mov_imm(c, sh63, 63);
            int y_sign = c->next_temp_id++;
            ir_emit_shr(c, y_sign, y_int, sh63);
            int sign_mask = c->next_temp_id++;
            ir_emit_shl(c, sign_mask, y_sign, sh63);
            ir_emit_fstore(c, (uint16_t)t_x, TEMP_NONE, -slot);
            int x_int = c->next_temp_id++;
            ir_emit_load(c, x_int, BE(c)->fp_reg, -slot);
            int nosig_mask = c->next_temp_id++;
            ir_emit_mov_imm(c, nosig_mask, 0x7FFFFFFFFFFFFFFFLL);
            int res_int = c->next_temp_id++;
            ir_emit_and(c, res_int, x_int, nosig_mask);
            ir_emit_or(c, res_int, res_int, sign_mask);
            ir_emit_store(c, res_int, BE(c)->fp_reg, -slot);
            int res_temp = c->next_temp_id++;
            ir_emit_fload(c, (uint16_t)res_temp, TEMP_NONE, -slot);
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
            if (strstr(callee->s.str, "overflow_p") != NULL) {
                ASTRef mo_arg = node->right;
                while (mo_arg != AST_NULL) {
                    lower_expr(c, arena, mo_arg);
                    ZeroCCASTNode *an = zero_cc_ast_get(arena, mo_arg);
                    mo_arg = (an && an->next != AST_NULL) ? an->next : AST_NULL;
                }
                int t_ovf = c->next_temp_id++;
                ir_emit_mov_imm(c, t_ovf, 0);
                c->last_expr_is_float = 0;
                *out_temp = t_ovf;
                return true;
            }

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
            int use_unsigned_math = (au && bu);
            int t_a64 = c->next_temp_id++;
            ir_emit_mov_reg(c, t_a64, t_a);
            if (aw < 8) {
                if (au) {
                    int m = c->next_temp_id++;
                    ir_emit_mov_imm(c, m, (int64_t)((1ULL << (aw * 8)) - 1));
                    ir_emit_and(c, t_a64, t_a64, m);
                } else {
                    ir_emit_sext(c, t_a64, aw);
                }
            }
            int t_b64 = c->next_temp_id++;
            ir_emit_mov_reg(c, t_b64, t_b);
            if (bw < 8) {
                if (bu) {
                    int m = c->next_temp_id++;
                    ir_emit_mov_imm(c, m, (int64_t)((1ULL << (bw * 8)) - 1));
                    ir_emit_and(c, t_b64, t_b64, m);
                } else {
                    ir_emit_sext(c, t_b64, bw);
                }
            }
            int t_res = c->next_temp_id++;
            if (is_add) {
                ir_emit(c, (Instr){.op=OP_ADD,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            } else if (is_sub) {
                ir_emit(c, (Instr){.op=OP_SUB,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            } else {
                ir_emit(c, (Instr){.op=OP_MUL,.temp_dest=(uint16_t)t_res,
                    .temp_src1=(uint16_t)t_a64,.temp_src2=(uint16_t)t_b64});
            }
            if (width == 1) ir_emit_store_byte(c, t_res, t_ptr);
            else if (width == 2) ir_emit_store_16(c, t_res, t_ptr);
            else if (width == 8) ir_emit_store_64(c, t_res, t_ptr);
            else ir_emit_store_32(c, t_res, t_ptr);
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
                    int t_lim = c->next_temp_id++;
                    ir_emit_mov_imm(c, t_lim, (int64_t)limit);
                    int t_bz = zc_ovf_cmp_set_imm(c, t_b64, 0, CC_EQ);
                    int lab_no = c->ir_next_label++;
                    ir_emit_jne(c, (uint16_t)t_bz, (uint16_t)lab_no);
                    int t_q = c->next_temp_id++;
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
                /* Signed 64-bit math: t_res is the exact a OP b for the
                 * operand widths in use (|a|,|b| <= 2^32). */
                if (is_mul) {
                    /* Extra wrap guard: if the true product exceeded
                     * 64 bits, res/a != b catches it (division guarded). */
                    int t_az = zc_ovf_cmp_set_imm(c, t_a64, 0, CC_EQ);
                    int lab_no = c->ir_next_label++;
                    ir_emit_jne(c, (uint16_t)t_az, (uint16_t)lab_no);
                    int t_q = c->next_temp_id++;
                    ir_emit_div(c, t_q, t_res, t_a64);
                    int t_ne = zc_ovf_cmp_set(c, t_q, t_b64, CC_NE);
                    ir_emit_label(c, lab_no);
                    int t_wrap = zc_ovf_and(c, zc_ovf_not1(c, t_az), t_ne);
                    if (!t_signed) {
                        int t_neg = zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT);
                        t_ovf = zc_ovf_or(c, t_neg, t_wrap);
                        if (width < 8) {
                            t_ovf = zc_ovf_or(c, t_ovf, zc_ovf_cmp_set_imm(c, t_res, ((uint64_t)1 << (width * 8)), CC_FLT_GE));
                        }
                    } else if (width < 8) {
                        int t_lo = zc_ovf_cmp_set_imm(c, t_res, (-((uint64_t)1 << (width * 8 - 1))), CC_LT);
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, (((uint64_t)1 << (width * 8 - 1)) - 1), CC_GT);
                        t_ovf = zc_ovf_or(c, zc_ovf_or(c, t_lo, t_hi), t_wrap);
                    } else {
                        t_ovf = t_wrap;
                    }
                } else {
                    if (!t_signed) {
                        int t_neg = zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT);
                        t_ovf = t_neg;
                        if (width < 8) {
                            t_ovf = zc_ovf_or(c, t_ovf, zc_ovf_cmp_set_imm(c, t_res, ((uint64_t)1 << (width * 8)), CC_FLT_GE));
                        }
                    } else if (width < 8) {
                        int t_lo = zc_ovf_cmp_set_imm(c, t_res, (-((uint64_t)1 << (width * 8 - 1))), CC_LT);
                        int t_hi = zc_ovf_cmp_set_imm(c, t_res, (((uint64_t)1 << (width * 8 - 1)) - 1), CC_GT);
                        t_ovf = zc_ovf_or(c, t_lo, t_hi);
                    } else {
                        /* 64-bit target: the exact sum/diff fits unless
                         * both operands share the sign that res lost. */
                        if (is_add) {
                            int t1 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_GT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_GT));
                            t1 = zc_ovf_and(c, t1, zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT));
                            int t2 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT));
                            t2 = zc_ovf_and(c, t2, zc_ovf_cmp_set(c, t_res, 0, CC_GE));
                            t_ovf = zc_ovf_or(c, t1, t2);
                        } else {
                            int t1 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_GT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_LT));
                            t1 = zc_ovf_and(c, t1, zc_ovf_cmp_set_imm(c, t_res, 0, CC_LT));
                            int t2 = zc_ovf_and(c, zc_ovf_cmp_set_imm(c, t_a64, 0, CC_LT),
                                                zc_ovf_cmp_set_imm(c, t_b64, 0, CC_GT));
                            t2 = zc_ovf_and(c, t2, zc_ovf_cmp_set(c, t_res, 0, CC_GE));
                            t_ovf = zc_ovf_or(c, t1, t2);
                        }
                    }
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
                    res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, res_temp, 0);
                } else if (kind == ZC_AT_IS_LOCK_FREE) {
                    /* C11 7.17.2: aligned integer atomics up to 8 bytes are
                     * lock-free on x86-64. */
                    res_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, res_temp, 1);
                } else {
                    int ptr_temp = lower_expr(c, arena, a0);
                    switch (kind) {
                    case ZC_AT_LOAD: {
                        res_temp = c->next_temp_id++;
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
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(c, res_temp, ptr_temp, val_temp, width);
                        break;
                    }
                    case ZC_AT_STORE_0: {
                        /* __sync_lock_release: store 0 at the pointee. */
                        int zero_temp = c->next_temp_id++;
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
                            int neg_temp = c->next_temp_id++;
                            ir_emit_neg(c, neg_temp, val_temp);
                            val_temp = neg_temp;
                        }
                        int val_copy = c->next_temp_id++;
                        ir_emit_mov_reg(c, val_copy, val_temp);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xadd(c, old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_FETCH_ADD || kind == ZC_AT_FETCH_SUB) {
                            res_temp = old_temp;
                        } else {
                            res_temp = c->next_temp_id++;
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
                        int old_temp = c->next_temp_id++;
                        if (kind == ZC_AT_FETCH_OR || kind == ZC_AT_OR_FETCH) ir_emit_atomic_or(c, old_temp, ptr_temp, val_temp, width);
                        else if (kind == ZC_AT_FETCH_AND || kind == ZC_AT_AND_FETCH) ir_emit_atomic_and(c, old_temp, ptr_temp, val_temp, width);
                        else ir_emit_atomic_xor(c, old_temp, ptr_temp, val_temp, width);
                        if (kind == ZC_AT_OR_FETCH || kind == ZC_AT_AND_FETCH || kind == ZC_AT_XOR_FETCH) {
                            res_temp = c->next_temp_id++;
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
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg(c, res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL_SYNC: {
                        /* __sync_bool_compare_and_swap(ptr, expected, desired) */
                        int exp_temp = lower_expr(c, arena, a1);
                        zc_atomic_extend_value(c, exp_temp, a1_node, ast_node_get_typeinfo(c, a1_node));
                        int des_temp = lower_expr(c, arena, a2);
                        zc_atomic_extend_value(c, des_temp, a2_node, ast_node_get_typeinfo(c, a2_node));
                        res_temp = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg_bool(c, res_temp, ptr_temp, exp_temp, des_temp, width);
                        break;
                    }
                    case ZC_AT_CAS_BOOL: {
                        /* __atomic_compare_exchange[_n](ptr, &expected, desired, ...) */
                        int exp_ptr_temp = lower_expr(c, arena, a1);
                        int exp_temp = c->next_temp_id++;
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
                        int actual_old = c->next_temp_id++;
                        ir_emit_atomic_cmpxchg(c, actual_old, ptr_temp, exp_temp, des_temp, width);
                        if (width == 1) ir_emit_store_byte(c, actual_old, exp_ptr_temp);
                        else if (width == 2) ir_emit_store_16(c, actual_old, exp_ptr_temp);
                        else if (width == 8) ir_emit_store_64(c, actual_old, exp_ptr_temp);
                        else ir_emit_store_32(c, actual_old, exp_ptr_temp);
                        res_temp = c->next_temp_id++;
                        ir_emit_cmp(c, actual_old, exp_temp);
                        ir_emit_cond_set(c, res_temp, CC_EQ);
                        break;
                    }
                    case ZC_AT_TEST_AND_SET: {
                        /* exchange(ptr, 1), return (old != 0). */
                        int one_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, one_temp, 1);
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_temp, 0);
                        int old_temp = c->next_temp_id++;
                        ir_emit_atomic_xchg(c, old_temp, ptr_temp, one_temp, width);
                        res_temp = c->next_temp_id++;
                        ir_emit_cmp(c, old_temp, zero_temp);
                        ir_emit_cond_set(c, res_temp, CC_NE);
                        break;
                    }
                    case ZC_AT_CLEAR: {
                        /* xchg(ptr, 0); the old value is dead (void op). */
                        int zero_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_temp, 0);
                        res_temp = c->next_temp_id++;
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

    return false;
}
