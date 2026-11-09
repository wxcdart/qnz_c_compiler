/* AST Lowering - Statement Handlers and Dispatcher */

#include "zero_cc/ir/ast_lowering_internal.h"

#define ZERO_CC_PARSER_AST_LOWERING_STMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/ir/ir_emit_ops.h"
#include "zero_cc/ir/machine_emitter.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/types/vregs.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/common/arena.h"
#include "zero_cc/ir/abi_classify.h"
#include "zero_cc/ir/label_patcher.h"

void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
static void lower_asm_stmt(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node);

void lower_stmt_block(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    c->lowering.vla.block_depth++;

    if (c->lowering.vla.block_depth == 1 && c->lowering.inlining.active_depth == 0) {
        /* Initialize per-VLA saved-SP slots to 0 at function entry so any
         * loop or backward goto re-entering a VLA declaration restores SP
         * to the pre-allocation watermark instead of leaking stack space. */
        int zero_sp = TEMP_NONE;
        for (size_t li = 0; li < c->local_count; li++) {
            if (c->locals[li].is_vla && c->locals[li].stack_offset >= 16) {
                if (zero_sp == TEMP_NONE) {
                    zero_sp = c->next_temp_id++;
                    ir_emit_mov_imm(c, zero_sp, 0);
                }
                ir_emit_store(c, zero_sp, BE(c)->fp_reg, -(c->locals[li].stack_offset - 8));
            }
        }
    }

    ASTRef stmt = ref;
    while (stmt != AST_NULL) {
        lower_node(c, arena, stmt);
        ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
        stmt = snode ? snode->next : AST_NULL;
    }

    c->lowering.vla.block_depth--;
}

/* Sprint 135 (inline asm Phase 1): x86-64 fixed-register constraint letters
 * map to hardware register numbers (the LRA index space, matching
 * get_hw_reg_name: 0=rax 1=rcx 2=rdx 3=rbx 4=rsp 5=rbp 6=rsi 7=rdi
 * 8-15=r8-r15). */
static int zc_asm_fixed_letter_reg(const char ch) {
    switch (ch) {
        case 'a': return 0;  /* rax */
        case 'b': return 3;  /* rbx */
        case 'c': return 1;  /* rcx */
        case 'd': return 2;  /* rdx */
        case 'S': return 6;  /* rsi */
        case 'D': return 7;  /* rdi */
        default: return -1;
    }
}

/* Resolve a fixed-register constraint letter to an allocatable hardware
 * register. 'c' is rejected: RCX is reserved for the shift count, so it can
 * never hold an asm operand. Returns 1 on success (pool set), 0 after
 * reporting a clean diagnostic. */
static int zc_asm_fixed_letter_ok(ZeroCCCompiler *c, char body, int *pool) {
    int p = zc_asm_fixed_letter_reg(body);
    if (p < 0) {
        diag_report_error_at_token(c, "asm constraint not supported in Phase 1 (r/m/a/b/d/S/D)");
        return 0;
    }
    if (p == 1) {
        diag_report_error_at_token(c,
            "asm constraint 'c' (rcx) not supported: rcx is reserved for shift counts");
        return 0;
    }
    *pool = p;
    return 1;
}

/* Width (bits) of an r-constraint operand from its C type; -1 on a
 * Phase-1-unsupported operand type (FP, struct, 128-bit, unknown). */
static int zc_asm_r_operand_width(ZeroCCCompiler *c, ZeroCCASTNode *expr, int *ok) {
    *ok = 1;
    if (!expr) { *ok = 0; return -1; }
    TypeInfo *ti = ast_node_get_typeinfo(c, expr);
    if (!ti) { *ok = 0; return -1; }
    if (type_is_struct_or_union(ti)) { *ok = 0; return -1; }
    if (ti->kind == TYPE_KIND_FLOAT || ti->kind == TYPE_KIND_DOUBLE) { *ok = 0; return -1; }
    size_t sz = type_size(ti);
    if (sz == 1) return 8;
    if (sz == 2) return 16;
    if (sz == 4) return 32;
    if (sz == 8) return 64;
    *ok = 0;  /* 128-bit / incomplete / odd */
    return -1;
}

/* Phase 1: an asm operand that names a location (m input/output, or an
 * r/fixed output stored back after the asm) must be directly addressable -
 * the same lvalue forms `&` accepts (semantic/expr.c expr_is_lvalue),
 * minus the __real/__imag part extractors. A value operand like `a + b`
 * would otherwise be dereferenced as a pointer by the memory form. */
static bool zc_asm_expr_is_lvalue(const ZeroCCASTNode *node) {
    if (!node) return false;
    switch (node->tag) {
        case NODE_IDENT:
        case NODE_MEMBER:
        case NODE_INDEX:
        case NODE_COMPOUND_LIT:
            return true;
        case NODE_UNOP:
            return (node->aux.op == '*' || node->aux.op == TK_STAR);
        default:
            return false;
    }
}

/* Record an asm operand temp (no_spill) with optional fixed-register
 * pre-coloring for the current function. */
static void zc_asm_record_operand_temp(ZeroCCCompiler *c, int temp, int fixed_reg) {
    if (c->asm_operand_count >= c->asm_operand_cap) {
        size_t ncap = c->asm_operand_cap ? c->asm_operand_cap * 2 : 16;
        int *nt = realloc(c->asm_operand_temps, ncap * sizeof(int));
        if (!nt) return;
        int *nr = realloc(c->asm_fixed_regs, ncap * sizeof(int));
        if (!nr) { free(nt); return; }
        c->asm_operand_temps = nt;
        c->asm_fixed_regs = nr;
        c->asm_operand_cap = ncap;
    }
    c->asm_operand_temps[c->asm_operand_count] = temp;
    c->asm_fixed_regs[c->asm_operand_count] = fixed_reg;
    c->asm_operand_count++;
}

/* GNU inline asm (Sprint 135, Phase 1: x86-64 AOT text path).
 * Constraint subset: r/m with = / + prefixes, matching digits 0-7, and the
 * x86 fixed-register letters a/b/c/d/S/D. Clobbers: "memory", "cc" and
 * GPR names. Everything else is a clean reject with a clean diagnostic. */
static void lower_asm_stmt(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node->int_val < 0 || (size_t)node->int_val >= c->asm_stmt_count) return;
    ZeroCCAsmStmt *as = &c->asm_stmts[node->int_val];

    if (!c->emit_asm && !BE(c)->emit_inline_asm_mc) {
        diag_report_error_at_token(c,
            "inline asm machine-code emission is not supported on this target");
        c->had_error = 1;
        return;
    }
    if (BE(c)->isa != ISA_X86_64) {
        diag_report_error_at_token(c,
            "inline asm Phase 1 targets x86-64 (template text is ISA-specific)");
        c->had_error = 1;
        return;
    }
    if (!as->template_str) return;

    IRAsmArgs aa;
    memset(&aa, 0, sizeof(aa));
    aa.template_str = as->template_str;
    aa.op_count = 0;
    aa.has_memory = as->is_volatile;  /* volatile asm: full memory barrier */
    aa.is_goto = as->is_goto;
    aa.goto_label_count = as->goto_label_count;
    for (int i = 0; i < as->goto_label_count; i++) {
        snprintf(aa.goto_label_names[i], sizeof(aa.goto_label_names[i]), "%s", as->goto_labels[i]);
        aa.goto_labels[i] = get_or_create_user_ir_label(c, as->goto_labels[i]);
    }
    if (as->is_goto) {
        aa.has_memory = 1;
    }

    /* --- Clobbers --- */
    for (int i = 0; i < as->clobber_count; i++) {
        const char *cb = as->clobber_names[i];
        if (strcmp(cb, "memory") == 0) { aa.has_memory = 1; continue; }
        if (strcmp(cb, "cc") == 0) continue;  /* IR keeps no implicit flags */
        int pool = -1;
        for (int r = 0; r < BE(c)->regs->num_int_regs; r++) {
            if (strcmp(cb, BE(c)->get_hw_reg_name(r)) == 0) { pool = r; break; }
        }
        if (pool < 0) {
            diag_report_error_at_token(c,
                "inline asm clobber not supported in Phase 1 (memory/cc/rax..r15)");
            c->had_error = 1;
            return;
        }
        if (pool == 4 || pool == 5) {  /* rsp / rbp */
            diag_report_error_at_token(c, "inline asm cannot clobber rsp/rbp");
            c->had_error = 1;
            return;
        }
        aa.clobber_mask |= (1u << pool);
    }

    /* --- Parse constraints (outputs first, then inputs) --- */
    int out_kind[ZC_ASM_MAX_OPS];   /* ZC_ASM_OP_* for r/m */
    int out_width[ZC_ASM_MAX_OPS];  /* bits for r ops */
    int out_fixed[ZC_ASM_MAX_OPS];  /* pool index or -1 */
    int in_kind[ZC_ASM_MAX_OPS];
    int in_width[ZC_ASM_MAX_OPS];
    int in_fixed[ZC_ASM_MAX_OPS];
    int in_digit[ZC_ASM_MAX_OPS];   /* 0..7 or -1 */
    for (int i = 0; i < as->out_count; i++) out_fixed[i] = -1;
    for (int i = 0; i < as->in_count; i++) { in_fixed[i] = -1; in_digit[i] = -1; }

    for (int i = 0; i < as->out_count; i++) {
        const char *cs = as->out_constraints[i];
        size_t off = 0;
        if (cs[0] == '=') off = 1;
        else if (cs[0] == '+') off = 1;
        else {
            diag_report_error_at_token(c, "asm output operand requires '=' or '+' prefix");
            c->had_error = 1;
            return;
        }
        if (cs[off] == '&') {
            off++;
        }
        char body = cs[off];
        if (cs[off + 1] != '\0' && body != 'r' && body != 'm') {
            diag_report_error_at_token(c, "asm constraint not supported in Phase 1 (r/m/a/b/c/d/S/D)");
            c->had_error = 1;
            return;
        }
        int ok = 1;
        if (body == 'r') {
            if (!zc_asm_expr_is_lvalue(zero_cc_ast_get(arena, as->out_exprs[i]))) {
                diag_report_error_at_token(c, "asm r output operand must be an lvalue");
                c->had_error = 1;
                return;
            }
            int w = zc_asm_r_operand_width(c, zero_cc_ast_get(arena, as->out_exprs[i]), &ok);
            if (!ok) {
                diag_report_error_at_token(c, "asm r operand must be an integer or pointer scalar");
                c->had_error = 1;
                return;
            }
            out_width[i] = w;
            out_kind[i] = (cs[0] == '+') ? ZC_ASM_OP_INOUT_R : ZC_ASM_OP_OUT_R;
        } else if (body == 'm') {
            if (!zc_asm_expr_is_lvalue(zero_cc_ast_get(arena, as->out_exprs[i]))) {
                diag_report_error_at_token(c, "asm m operand must be directly addressable (an lvalue)");
                c->had_error = 1;
                return;
            }
            out_width[i] = 0;
            out_kind[i] = ZC_ASM_OP_OUT_M;
        } else {
            int pool;
            if (!zc_asm_fixed_letter_ok(c, body, &pool)) {
                c->had_error = 1;
                return;
            }
            if (!zc_asm_expr_is_lvalue(zero_cc_ast_get(arena, as->out_exprs[i]))) {
                diag_report_error_at_token(c, "asm r output operand must be an lvalue");
                c->had_error = 1;
                return;
            }
            int w = zc_asm_r_operand_width(c, zero_cc_ast_get(arena, as->out_exprs[i]), &ok);
            if (!ok) {
                diag_report_error_at_token(c, "asm r operand must be an integer or pointer scalar");
                c->had_error = 1;
                return;
            }
            out_width[i] = w;
            out_fixed[i] = pool;
            out_kind[i] = (cs[0] == '+') ? ZC_ASM_OP_INOUT_R : ZC_ASM_OP_OUT_R;
        }
    }

    for (int i = 0; i < as->in_count; i++) {
        const char *cs = as->in_constraints[i];
        char body = cs[0];
        if (body >= '0' && body <= '7') {
            in_digit[i] = body - '0';
            if (in_digit[i] >= as->out_count ||
                (out_kind[in_digit[i]] != ZC_ASM_OP_OUT_R &&
                 out_kind[in_digit[i]] != ZC_ASM_OP_INOUT_R)) {
                diag_report_error_at_token(c, "asm matching-digit input requires an r output");
                c->had_error = 1;
                return;
            }
            in_kind[i] = ZC_ASM_OP_IN_R;
            in_width[i] = 0;  /* materialized into the output register */
            continue;
        }
        if (body == '&') {
            diag_report_error_at_token(c, "asm earlyclobber '&' operand not supported in Phase 1");
            c->had_error = 1;
            return;
        }
        if (cs[1] != '\0' && body != 'r' && body != 'm') {
            diag_report_error_at_token(c, "asm constraint not supported in Phase 1 (r/m/a/b/c/d/S/D/0-7)");
            c->had_error = 1;
            return;
        }
        int ok = 1;
        if (body == 'r') {
            int w = zc_asm_r_operand_width(c, zero_cc_ast_get(arena, as->in_exprs[i]), &ok);
            if (!ok) {
                diag_report_error_at_token(c, "asm r operand must be an integer or pointer scalar");
                c->had_error = 1;
                return;
            }
            in_width[i] = w;
            in_kind[i] = ZC_ASM_OP_IN_R;
        } else if (body == 'm') {
            if (!zc_asm_expr_is_lvalue(zero_cc_ast_get(arena, as->in_exprs[i]))) {
                diag_report_error_at_token(c, "asm m operand must be directly addressable (an lvalue)");
                c->had_error = 1;
                return;
            }
            in_width[i] = 0;
            in_kind[i] = ZC_ASM_OP_IN_M;
        } else {
            int pool;
            if (!zc_asm_fixed_letter_ok(c, body, &pool)) {
                c->had_error = 1;
                return;
            }
            int w = zc_asm_r_operand_width(c, zero_cc_ast_get(arena, as->in_exprs[i]), &ok);
            if (!ok) {
                diag_report_error_at_token(c, "asm r operand must be an integer or pointer scalar");
                c->had_error = 1;
                return;
            }
            in_width[i] = w;
            in_fixed[i] = pool;
            in_kind[i] = ZC_ASM_OP_IN_R;
        }
    }

    /* --- Materialize operand temps (all BEFORE the asm). Outputs first so
     * digit-matched inputs can seed their "=r" output register; input
     * values are pure loads/LEAs and their order is unobservable. --- */
    int in_temp[ZC_ASM_MAX_OPS];
    int out_temp[ZC_ASM_MAX_OPS];
    int out_addr[ZC_ASM_MAX_OPS];   /* lvalue address temp (r outputs / m outputs) */
    for (int i = 0; i < as->out_count; i++) {
        out_addr[i] = TEMP_NONE;
        if (out_kind[i] == ZC_ASM_OP_OUT_M) {
            /* Address operand, live at the asm */
            out_addr[i] = zero_cc_lower_lvalue_addr(c, arena, as->out_exprs[i]);
            c->last_expr_is_float = 0;
            zc_asm_record_operand_temp(c, out_addr[i], -1);
            out_temp[i] = out_addr[i];
        } else {
            /* r output: the lvalue address is materialized before the asm
             * (operand-address evaluation order) and the value stored after */
            out_addr[i] = zero_cc_lower_lvalue_addr(c, arena, as->out_exprs[i]);
            c->last_expr_is_float = 0;
            out_temp[i] = c->next_temp_id++;
            zc_asm_record_operand_temp(c, out_temp[i], out_fixed[i]);
            if (out_kind[i] == ZC_ASM_OP_INOUT_R) {
                /* '+r': the lvalue's current value is the operand's initial
                * value - load it into the output temp so the template sees
                * it in the operand register (OP_ASM then redefines it). */
                switch (out_width[i]) {
                    case 8:  ir_emit(c, (Instr){.op = OP_LOAD_BYTE, .temp_dest = (uint16_t)out_temp[i], .temp_src1 = (uint16_t)out_addr[i], .temp_src2= TEMP_NONE, .imm = 0, .label_idx = 0}); break;
                    case 16: ir_emit_load_16(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
                    case 32: ir_emit_load_32(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
                    default: ir_emit_deref(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
                }
            }
        }
    }

    for (int i = 0; i < as->in_count; i++) {
        if (in_digit[i] >= 0) {
            /* Digit-matched: shares the referenced output's register. For
             * "=r" the input expression seeds the register (GCC semantics);
             * for "+r" the register already holds the lvalue's value. */
            if (out_kind[in_digit[i]] == ZC_ASM_OP_OUT_R) {
                int t = lower_expr(c, arena, as->in_exprs[i]);
                c->last_expr_is_float = 0;
                ir_emit(c, (Instr){.op = OP_MOV_REG, .temp_dest = (uint16_t)out_temp[in_digit[i]],
                                   .temp_src1 = (uint16_t)t, .temp_src2= TEMP_NONE,
                                   .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF,
                                   .imm = 0, .label_idx = 0});
            }
            in_temp[i] = out_temp[in_digit[i]];
            in_width[i] = out_width[in_digit[i]];
            continue;
        }
        if (in_kind[i] == ZC_ASM_OP_IN_R) {
            in_temp[i] = lower_expr(c, arena, as->in_exprs[i]);
            c->last_expr_is_float = 0;
        } else {
            in_temp[i] = zero_cc_lower_lvalue_addr(c, arena, as->in_exprs[i]);
            c->last_expr_is_float = 0;
        }
        zc_asm_record_operand_temp(c, in_temp[i], in_fixed[i]);
    }

    /* --- Record operands: outputs first (GCC %0..%n-1), then inputs --- */
    for (int i = 0; i < as->out_count; i++) {
        if (aa.op_count >= 16) {
            diag_report_error_at_token(c, "too many asm operands (max 16)");
            c->had_error = 1;
            return;
        }
        IRAsmOperand *op = &aa.ops[aa.op_count++];
        memset(op, 0, sizeof(*op));
        op->kind = (uint8_t)out_kind[i];
        op->width = (uint8_t)out_width[i];
        op->digit = (out_kind[i] == ZC_ASM_OP_INOUT_R) ? (int16_t)i : -1;
        op->fixed_reg = (int16_t)out_fixed[i];
        op->temp = out_temp[i];
    }

    /* --- Record inputs after outputs: GCC template numbering is %0 = first
     * output, ..., then inputs. --- */
    for (int i = 0; i < as->in_count; i++) {
        if (aa.op_count >= 16) {
            diag_report_error_at_token(c, "too many asm operands (max 16)");
            c->had_error = 1;
            return;
        }
        IRAsmOperand *op = &aa.ops[aa.op_count++];
        memset(op, 0, sizeof(*op));
        op->kind = (uint8_t)in_kind[i];
        op->width = (uint8_t)in_width[i];
        op->digit = (int16_t)in_digit[i];
        op->fixed_reg = (int16_t)in_fixed[i];
        op->temp = in_temp[i];
    }

    /* Validate the template's operand references (the text emitter cannot
     * report a clean diagnostic for a bad %N). */
    for (const char *tp = as->template_str; *tp; tp++) {
        if (*tp != '%' || !tp[1]) continue;
        if (tp[1] == '%') { tp++; continue; }
        if (tp[1] == 'l') {
            if (tp[2] >= '0' && tp[2] <= '9') {
                int n = tp[2] - '0';
                if (n >= aa.op_count + aa.goto_label_count && n >= aa.goto_label_count) {
                    diag_report_error_at_token(c, "asm template references an out-of-range label operand");
                    c->had_error = 1;
                    return;
                }
                tp += 2;
                continue;
            } else if (tp[2] == '[') {
                const char *close_b = strchr(tp + 3, ']');
                if (!close_b) {
                    diag_report_error_at_token(c, "asm template has unclosed '%l['");
                    c->had_error = 1;
                    return;
                }
                char lname[64];
                size_t llen = (size_t)(close_b - (tp + 3));
                if (llen >= sizeof(lname)) llen = sizeof(lname) - 1;
                memcpy(lname, tp + 3, llen);
                lname[llen] = '\0';
                bool found = false;
                for (int li = 0; li < aa.goto_label_count; li++) {
                    if (strcmp(aa.goto_label_names[li], lname) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    diag_report_error_at_token(c, "asm template references an unknown label operand '%s'", lname);
                    c->had_error = 1;
                    return;
                }
                tp = close_b;
                continue;
            }
        }
        if (tp[1] < '0' || tp[1] > '9') continue;
        if (tp[1] - '0' >= aa.op_count + aa.goto_label_count) {
            diag_report_error_at_token(c, "asm template references an out-of-range operand");
            c->had_error = 1;
            return;
        }
    }

    /* --- The OP_ASM itself --- */
    ir_emit(c, (Instr){.op = OP_ASM, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE,
                       .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF,
                       .flags_in = 0xFF, .imm = 0, .label_idx = 0});
    ir_asm_args_record(c, ir_get_count(c) - 1, &aa);

    /* --- Post-asm: store r outputs back to their lvalues --- */
    for (int i = 0; i < as->out_count; i++) {
        if (out_kind[i] != ZC_ASM_OP_OUT_R && out_kind[i] != ZC_ASM_OP_INOUT_R) continue;
        switch (out_width[i]) {
            case 8:  ir_emit_store_byte(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
            case 16: ir_emit_store_16(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
            case 32: ir_emit_store_32(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
            default: ir_emit_store_64(c, (uint16_t)out_temp[i], (uint16_t)out_addr[i]); break;
        }
    }
    c->last_expr_is_float = 0;
}

void lower_stmt_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        (void)lower_expr(c, arena, node->left);  /* Result not used */
    }
}

void lower_stmt_decl(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    /* GNU variably-modified struct definition: capture the member
     * dimensions NOW - GCC evaluates the dimension expressions when the
     * definition executes, and every later use (runtime sizeof, object
     * allocation, by-value argument size word) reads the captured value
     * (c-torture execute/20041218-2: `n++; sizeof(struct s)` uses the
     * pre-increment dimension). Runs before any size computation in this
     * declaration (VLA local alloc, array-of-VLA-struct alloc). */
    if ((node->flags & NODE_DECL_VLA_STRUCT_DEF)) {
        /* Resolve the defined struct's tag: the declaration's type
         * (through a constant-array chain for `struct s {...} arr[N];`),
         * falling back to s.str (synthetic definition node). */
        const char *def_tag = node->s.str;
        TypeInfo *def_ti = ast_node_get_typeinfo(c, node);
        if (def_ti) {
            if (def_ti->kind == TYPE_KIND_STRUCT || def_ti->kind == TYPE_KIND_UNION) {
                if (def_ti->details.record.tag) def_tag = def_ti->details.record.tag;
            } else if (type_is_array(def_ti) && def_ti->details.array.base &&
                       (def_ti->details.array.base->kind == TYPE_KIND_STRUCT ||
                        def_ti->details.array.base->kind == TYPE_KIND_UNION)) {
                if (def_ti->details.array.base->details.record.tag)
                    def_tag = def_ti->details.array.base->details.record.tag;
            }
        }
        if (!def_tag[0]) return;
        ZeroCCType *dreg = zero_cc_find_struct(c, def_tag);
        if (dreg && dreg->is_vla_struct && dreg->members &&
            c->lowering.vla_struct_capture_count < 8) {
            int ndim = 0;
            for (size_t mi = 0; mi < dreg->member_count; mi++)
                if (dreg->members[mi].is_vla && dreg->members[mi].vla_dim) ndim++;
            if (ndim > 0) {
                struct ZeroCCVlaStructCapture *cap =
                    &c->lowering.vla_struct_captures[c->lowering.vla_struct_capture_count];
                snprintf(cap->tag, sizeof(cap->tag), "%s", def_tag);
                cap->count = 0;
                for (size_t mi = 0; mi < dreg->member_count; mi++) {
                    ZeroCCStructMember *m = &dreg->members[mi];
                    if (!m->is_vla || !m->vla_dim) continue;
                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                    c->current_stack_frame += 8;
                    cap->slots[cap->count] = (int32_t)c->current_stack_frame;
                    cap->count++;
                    ASTRef dim = zero_cc_ast_clone((ZeroCCASTArena *)c->tu_ast_arena,
                                                   (ASTRef)m->vla_dim, arena);
                    int dv = (dim != AST_NULL) ? lower_expr(c, arena, dim) : TEMP_NONE;
                    if (dv == TEMP_NONE) {
                        dv = c->next_temp_id++;
                        ir_emit_mov_imm(c, dv, (int32_t)m->vla_elem_size);
                    }
                    ir_emit_store(c, dv, BE(c)->fp_reg, -cap->slots[cap->count - 1]);
                }
                if (cap->count > 0) c->lowering.vla_struct_capture_count++;
            }
        }
    }
    if (node->flags & NODE_DECL_IS_VLA) {
        c->lowering.vla.has_runtime_vla = true;
        c->lowering.vla.vla_count++;

        const char *var = node->s.str;
        ZeroCCSym *sym = lowering_local_sym(c, node, var);
        if (sym && node->right != AST_NULL) {
            /* Invalidate saved-SP watermarks of any lexically-later VLAs so
             * they re-anchor below this VLA's new allocation. */
            size_t self_idx = (size_t)(sym - c->locals);
            int zero_sp = TEMP_NONE;
            for (size_t li = self_idx + 1; li < c->local_count; li++) {
                if (c->locals[li].is_vla && c->locals[li].stack_offset >= 16) {
                    if (zero_sp == TEMP_NONE) {
                        zero_sp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_sp, 0);
                    }
                    ir_emit_store(c, zero_sp, BE(c)->fp_reg, -(c->locals[li].stack_offset - 8));
                }
            }

            /* 1. Evaluate outermost dimension expression */
            int count_temp = lower_expr(c, arena, node->right);

            /* 2. Multiply count by element/row size (handling runtime inner VLA dims) */
            int size_temp;
            if (zc_find_vla_struct(&sym->type) != NULL) {
                /* Array of a struct with VLA members: the element size is
                 * runtime (re-laid out with the real dimension). */
                const ZeroCCType *es = zc_find_vla_struct(&sym->type);
                if (es && !es->members && es->struct_name[0]) {
                    ZeroCCType *reg = zero_cc_find_struct(c, es->struct_name);
                    if (reg) es = reg;
                }
                ASTRef sz_ast = zc_vla_struct_size_ast(arena, (ZeroCCASTArena *)c->tu_ast_arena, es, es->struct_name);
                int elem_temp = (sz_ast != AST_NULL) ? lower_expr(c, arena, sz_ast) : count_temp;
                size_temp = c->next_temp_id++;
                ir_emit_mul(c, size_temp, count_temp, elem_temp);
            } else if (sym->type.base && sym->type.base->tag == TYPE_ARRAY && sym->type.base->is_vla) {
                const ZeroCCType *leaf = sym->type.base;
                while (leaf && leaf->tag == TYPE_ARRAY && leaf->is_vla) {
                    leaf = leaf->base;
                }
                size_t leaf_sz = (leaf && leaf->size > 0) ? leaf->size : 4;
                int stride_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, stride_temp, (int32_t)leaf_sz);
                const ZeroCCType *it = sym->type.base;
                while (it && it->tag == TYPE_ARRAY && it->is_vla) {
                    if (it->vla_dim != 0) {
                        int d_temp = lower_expr(c, arena, (ASTRef)it->vla_dim);
                        int prod = c->next_temp_id++;
                        ir_emit_mul(c, prod, stride_temp, d_temp);
                        stride_temp = prod;
                    }
                    it = it->base;
                }
                ir_emit_store(c, stride_temp, BE(c)->fp_reg, -(sym->stack_offset - 16));
                size_temp = c->next_temp_id++;
                ir_emit_mul(c, size_temp, count_temp, stride_temp);
            } else {
                size_t elem_size = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 4;
                if (elem_size == 1) {
                    size_temp = count_temp;
                } else {
                    int sz_imm = c->next_temp_id++;
                    ir_emit_mov_imm(c, sz_imm, (int32_t)elem_size);
                    size_temp = c->next_temp_id++;
                    ir_emit_mul(c, size_temp, count_temp, sz_imm);
                }
            }

            /* 2b. C99 6.5.3.4: remember the exact object size in a frame
             * slot so `sizeof` of this VLA local is a runtime value (the
             * value at the declaration, per C99 6.7.6.2). */
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            sym->vla_slot_off = (int32_t)c->current_stack_frame;
            ir_emit_store(c, size_temp, BE(c)->fp_reg, -sym->vla_slot_off);

            /* 3. Align size to 16 bytes: (size + 15) & ~15 */
            int add15 = c->next_temp_id++;
            ir_emit_mov_imm(c, add15, 15);
            int size_p15 = c->next_temp_id++;
            ir_emit_add(c, size_p15, size_temp, add15);
            int mask16 = c->next_temp_id++;
            ir_emit_mov_imm(c, mask16, -16);
            int aligned_size = c->next_temp_id++;
            ir_emit_and(c, aligned_size, size_p15, mask16);

            /* 4. Allocate dynamically on stack via sub rsp, reg (with saved-SP watermark) */
            int vla_ptr = c->next_temp_id++;
            ir_emit(c, (Instr){.op = OP_VLA_ALLOC, .temp_dest = (uint16_t)vla_ptr,
                               .temp_src1 = (uint16_t)aligned_size, .temp_src2 = TEMP_NONE,
                               .imm = -(sym->stack_offset - 8), .label_idx = 0});

            /* 5. Store the buffer pointer in the local variable's stack slot */
            ir_emit_store(c, vla_ptr, BE(c)->fp_reg, -sym->stack_offset);
        }
        return;
    }
    /* Sprint 99: GNU `__label__ x;` - initialize the slot with the address
     * of the label named x (exactly &&x), so computed gotos through x
     * work. No initializer expression follows the declaration. */
    if (node->left == AST_NULL && node->s.str) {
        ZeroCCSym *lsym = lowering_local_sym(c, node, node->s.str);
        if (lsym && lsym->is_label_value) {
            uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
            int addr_temp = c->next_temp_id++;
            ir_emit_lea_label(c, (uint16_t)addr_temp, lbl);
            ir_emit_store(c, addr_temp, BE(c)->fp_reg, -lsym->stack_offset);
            c->last_expr_is_float = 0;
            return;
        }
    }
    /* GNU: a local of struct/union type with VLA members (no array VLA
     * dimension on the object itself - that is the NODE_DECL_IS_VLA path
     * above): runtime-sized object, allocated like a VLA local. */
    {
        ZeroCCSym *vls = lowering_local_sym(c, node, node->s.str);
        if (vls && zc_type_is_vla_struct_obj(&vls->type)) {
            if (node->left != AST_NULL) {
                diag_report_error_at_token(c,
                    "variable-sized object may not be initialized except with an empty initializer");
                return;
            }
            c->lowering.vla.has_runtime_vla = true;
            c->lowering.vla.vla_count++;

            /* Invalidate saved-SP watermarks of any lexically-later VLAs.
             * (Same pattern as the NODE_DECL_IS_VLA block above.) */
            size_t self_idx = (size_t)(vls - c->locals);
            int zero_sp = TEMP_NONE;
            for (size_t li = self_idx + 1; li < c->local_count; li++) {
                if (c->locals[li].is_vla && c->locals[li].stack_offset >= 16) {
                    if (zero_sp == TEMP_NONE) {
                        zero_sp = c->next_temp_id++;
                        ir_emit_mov_imm(c, zero_sp, 0);
                    }
                    ir_emit_store(c, zero_sp, BE(c)->fp_reg, -(c->locals[li].stack_offset - 8));
                }
            }

            /* Runtime size slot (C99 6.7.6.2 / sizeof at runtime). */
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            vls->vla_slot_off = (int32_t)c->current_stack_frame;

            const ZeroCCType *vs = zc_find_vla_struct(&vls->type);
            if (vs && !vs->members && vs->struct_name[0]) {
                ZeroCCType *reg = zero_cc_find_struct(c, vs->struct_name);
                if (reg) vs = reg; /* TypeInfo round-trip drops the member table */
            }
            ASTRef sz_ast = zc_vla_struct_size_ast(arena, (ZeroCCASTArena *)c->tu_ast_arena, vs, vs->struct_name);
            int size_temp = (sz_ast != AST_NULL) ? lower_expr(c, arena, sz_ast) : -1;
            if (size_temp < 0) {
                size_temp = c->next_temp_id++;
                ir_emit_mov_imm(c, size_temp, (int32_t)vs->size);
            }
            /* Constant array of VLA structs: multiply by the element count. */
            if (vls->type.tag == TYPE_ARRAY && vls->type.base && vls->type.base->size > 0) {
                size_t cnt = vls->type.size / vls->type.base->size;
                if (cnt > 1) {
                    int k = c->next_temp_id++;
                    ir_emit_mov_imm(c, k, (int32_t)cnt);
                    int t2 = c->next_temp_id++;
                    ir_emit_mul(c, t2, size_temp, k);
                    size_temp = t2;
                }
            }

            ir_emit_store(c, size_temp, BE(c)->fp_reg, -vls->vla_slot_off);

            /* Align size to 16 bytes: (size + 15) & ~15 */
            int add15 = c->next_temp_id++;
            ir_emit_mov_imm(c, add15, 15);
            int size_p15 = c->next_temp_id++;
            ir_emit_add(c, size_p15, size_temp, add15);
            int mask16 = c->next_temp_id++;
            ir_emit_mov_imm(c, mask16, -16);
            int aligned_size = c->next_temp_id++;
            ir_emit_and(c, aligned_size, size_p15, mask16);

            int vla_ptr = c->next_temp_id++;
            ir_emit(c, (Instr){.op = OP_VLA_ALLOC, .temp_dest = (uint16_t)vla_ptr,
                               .temp_src1 = (uint16_t)aligned_size, .temp_src2 = TEMP_NONE,
                               .imm = -(vls->stack_offset - 8), .label_idx = 0});

            ir_emit_store(c, vla_ptr, BE(c)->fp_reg, -vls->stack_offset);
            c->last_expr_is_float = 0;
            return;
        }
    }
    if (node->left != AST_NULL) {
        /* C11 6.7.9: a string literal initializing a local char array.
         * Zero-fill the whole slot, then store the literal's bytes (all
         * compile-time constants) as byte immediates; the generic path
         * below would lower the literal to its rodata pointer and store
         * the pointer value into the array.  Mirrors the NODE_COMPOUND_LIT
         * zero-fill / width-chunk pattern. */
        ZeroCCASTNode *str_init_node = zero_cc_ast_get(arena, node->left);
        ZeroCCSym *str_sym = lowering_local_sym(c, node, node->s.str);
        if (str_init_node && str_init_node->tag == NODE_STRING && str_init_node->s.str &&
            str_sym && str_sym->type.tag == TYPE_ARRAY && str_sym->type.base &&
            ((str_sym->type.base->tag == TYPE_INT8 || str_sym->type.base->tag == TYPE_UINT8) ||
             ((str_sym->type.base->tag == TYPE_INT32 || str_sym->type.base->tag == TYPE_UINT32) && (str_init_node->flags & NODE_FLAG_WIDE)))) {
            bool is_wide = (str_init_node->flags & NODE_FLAG_WIDE) != 0;
            size_t term_sz = is_wide ? 4 : 1;
            const char *lit = str_init_node->s.str;
            size_t total_size = str_sym->type.size;
            size_t copy_len = str_init_node->s_len + term_sz; /* may contain embedded NULs */
            if (copy_len > total_size) copy_len = total_size; /* C11 6.7.9p12: truncate */
            int zero_temp = c->next_temp_id++;
            ir_emit_mov_imm(c, zero_temp, 0);
            size_t zoff = 0;
            while (zoff < total_size) {
                size_t chunk = total_size - zoff;
                if (chunk >= 8) chunk = 8;
                else if (chunk >= 4) chunk = 4;
                else if (chunk >= 2) chunk = 2;
                else chunk = 1;
                int32_t store_offset = -(str_sym->stack_offset - (int32_t)zoff);
                if (chunk == 8) {
                    ir_emit_store(c, zero_temp, BE(c)->fp_reg, store_offset);
                } else {
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, store_offset);
                    if (chunk == 4) {
                        ir_emit_store_32(c, zero_temp, addr_temp);
                    } else if (chunk == 2) {
                        ir_emit_store_16(c, zero_temp, addr_temp);
                    } else {
                        ir_emit_store_byte(c, zero_temp, addr_temp);
                    }
                }
                zoff += chunk;
            }
            if (is_wide) {
                size_t n_words = str_init_node->s_len / 4;
                const uint32_t *wlit = (const uint32_t *)lit;
                for (size_t i = 0; i < n_words && (i + 1) * 4 <= total_size; i++) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, val_temp, (int32_t)wlit[i]);
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, -(int32_t)(str_sym->stack_offset - (int32_t)(i * 4)));
                    ir_emit_store_32(c, val_temp, addr_temp);
                }
            } else {
                for (size_t i = 0; i < copy_len; i++) {
                    int val_temp = c->next_temp_id++;
                    ir_emit_mov_imm(c, val_temp, (int32_t)(unsigned char)lit[i]);
                    int addr_temp = c->next_temp_id++;
                    ir_emit_lea(c, addr_temp, TEMP_NONE, -(int32_t)(str_sym->stack_offset - (int32_t)i));
                    ir_emit_store_byte(c, val_temp, addr_temp);
                }
            }
            c->last_expr_is_float = 0;
            return;
        }
        int result_temp = lower_expr(c, arena, node->left);
        int result_is128 = c->last_expr_is_128;  /* GNU __int128 (Sprint 109) */
        const char *var = node->s.str;
        ZeroCCSym *sym = lowering_local_sym(c, node, var);
        if (sym) {
            /* C99 complex local initializer: the RHS is either an
             * address-form complex object (binop/call/ident) or a scalar
             * value (converted to (val, 0)). */
            if (zero_cc_type_is_complex(&sym->type)) {
                size_t cesz = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 8;
                bool is_elem_fp = sym->type.base && (sym->type.base->tag == TYPE_FLOAT || sym->type.base->tag == TYPE_DOUBLE);
                if (is_elem_fp && cesz != 4 && cesz != 8) cesz = 8;
                int32_t soff = -sym->stack_offset;
                /* Brace initializer (`z = { re, im }`): the compound-lit
                 * lowering already zeroed the object and stamped both
                 * halves in place - nothing more to do. */
                if (node->left != AST_NULL) {
                    ZeroCCASTNode *init_node_cx = zero_cc_ast_get(arena, node->left);
                    if (init_node_cx && init_node_cx->tag == NODE_COMPOUND_LIT) {
                        c->last_expr_is_float = 0;
                        return;
                    }
                }
                ZeroCCASTNode *rhs_n = (node->left != AST_NULL) ? zero_cc_ast_get(arena, node->left) : NULL;
                TypeInfo *r_ti = rhs_n ? ast_node_get_typeinfo(c, rhs_n) : NULL;
                bool rhs_is_cx = (r_ti && r_ti->kind == TYPE_KIND_COMPLEX);
                const TypeInfo *r_el = (rhs_is_cx && r_ti->details.array.base) ? r_ti->details.array.base : NULL;
                const ZeroCCType *r_zt = NULL;
                if (!rhs_is_cx && rhs_n && rhs_n->tag == NODE_IDENT) {
                    ZeroCCSym *rs = zero_cc_find_local(c, rhs_n->s.str);
                    if (rs && zero_cc_type_is_complex(&rs->type)) { r_zt = rs->type.base; rhs_is_cx = true; }
                    else {
                        ZeroCCGlobalVar *rg = find_global_var(c, rhs_n->s.str);
                        if (rg && zero_cc_type_is_complex(&rg->type)) { r_zt = rg->type.base; rhs_is_cx = true; }
                    }
                }
                bool r_is_fp = r_el ? type_is_floating(r_el) :
                               (r_zt ? (r_zt->tag == TYPE_FLOAT || r_zt->tag == TYPE_DOUBLE) : true);
                size_t r_esz = r_el ? type_size(r_el) : (r_zt ? r_zt->size : 8);
                if (r_esz == 0) r_esz = 8;
                if (r_is_fp && r_esz != 4 && r_esz != 8) r_esz = 8;

                int rh = c->next_temp_id++, li = c->next_temp_id++;
                if (rhs_is_cx) {
                    if (r_is_fp) {
                        if (r_esz == 4) {
                            ir_emit_vloadss(c, rh, (uint16_t)result_temp);
                            int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, 4);
                            int a2 = c->next_temp_id++; ir_emit_add(c, a2, result_temp, o2);
                            ir_emit_vloadss(c, li, (uint16_t)a2);
                        } else {
                            ir_emit_vloadsd(c, rh, (uint16_t)result_temp);
                            int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, 8);
                            int a2 = c->next_temp_id++; ir_emit_add(c, a2, result_temp, o2);
                            ir_emit_vloadsd(c, li, (uint16_t)a2);
                        }
                    } else {
                        int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, (int)r_esz);
                        int a2 = c->next_temp_id++; ir_emit_add(c, a2, result_temp, o2);
                        if (r_esz == 1) { ir_emit_load_byte(c, rh, result_temp); ir_emit_load_byte(c, li, a2); }
                        else if (r_esz == 2) { ir_emit_load_16(c, rh, result_temp); ir_emit_load_16(c, li, a2); }
                        else if (r_esz == 4) { ir_emit_load_32(c, rh, result_temp); ir_emit_load_32(c, li, a2); }
                        else { ir_emit_deref(c, rh, result_temp); ir_emit_deref(c, li, a2); }
                        if (r_el) { lower_extend_narrow(c, rh, r_el); lower_extend_narrow(c, li, r_el); }
                        else if (r_zt) { lower_assign_conv_zt(c, rh, r_zt); lower_assign_conv_zt(c, li, r_zt); }
                        else if (r_esz < 8) { ir_emit_sext(c, rh, (int)r_esz); ir_emit_sext(c, li, (int)r_esz); }
                    }
                } else {
                    /* Scalar RHS: (val, 0) */
                    if (c->last_expr_is_float) {
                        r_is_fp = true;
                        rh = result_temp;
                        li = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)li, (int64_t)zc_rodata_double_offset(c, 0.0));
                    } else {
                        r_is_fp = false;
                        rh = result_temp;
                        li = c->next_temp_id++;
                        ir_emit_mov_imm(c, li, 0);
                    }
                }

                /* Convert (rh, li) from (r_is_fp, r_esz) to (is_elem_fp, cesz) */
                if (r_is_fp && !is_elem_fp) {
                    int irh = c->next_temp_id++;
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)irh,
                                       .temp_src1 = (uint16_t)rh, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    int ili = c->next_temp_id++;
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)ili,
                                       .temp_src1 = (uint16_t)li, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    rh = irh;
                    li = ili;
                } else if (!r_is_fp && is_elem_fp) {
                    int frh = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)frh, (uint16_t)rh);
                    int fli = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)fli, (uint16_t)li);
                    rh = frh;
                    li = fli;
                } else if (!r_is_fp && !is_elem_fp) {
                    if (sym->type.base) lower_assign_conv_zt(c, rh, sym->type.base);
                    if (sym->type.base) lower_assign_conv_zt(c, li, sym->type.base);
                }

                /* Store (rh, li) to destination */
                if (is_elem_fp) {
                    if (cesz == 4) {
                        int a1 = c->next_temp_id++;
                        ir_emit_lea(c, a1, BE(c)->fp_reg, soff);
                        ir_emit_vstoress(c, (uint16_t)a1, (uint16_t)rh);
                        int a1b = c->next_temp_id++;
                        ir_emit_lea(c, a1b, BE(c)->fp_reg, soff + 4);
                        ir_emit_vstoress(c, (uint16_t)a1b, (uint16_t)li);
                    } else {
                        ir_emit_fstore(c, (uint16_t)rh, TEMP_NONE, soff);
                        ir_emit_fstore(c, (uint16_t)li, TEMP_NONE, soff + (int)cesz);
                    }
                } else {
                    int sa = c->next_temp_id++;
                    ir_emit_lea(c, sa, BE(c)->fp_reg, soff);
                    int sa2 = c->next_temp_id++;
                    ir_emit_mov_imm(c, sa2, (int)cesz);
                    int sa3 = c->next_temp_id++;
                    ir_emit_add(c, sa3, sa, sa2);
                    if (cesz == 1) { ir_emit_store_byte(c, rh, sa); ir_emit_store_byte(c, li, sa3); }
                    else if (cesz == 2) { ir_emit_store_16(c, rh, sa); ir_emit_store_16(c, li, sa3); }
                    else if (cesz == 4) { ir_emit_store_32(c, rh, sa); ir_emit_store_32(c, li, sa3); }
                    else { ir_emit_store_64(c, rh, sa); ir_emit_store_64(c, li, sa3); }
                }
                c->last_expr_is_float = 0;
                return;
            }
            bool target_is_float = (sym->type.tag == TYPE_FLOAT || sym->type.tag == TYPE_DOUBLE);
            if (target_is_float) {
                int float_temp = result_temp;
                if (!c->last_expr_is_float) {
                    /* Convert int to float via IR */
                    float_temp = c->next_temp_id++;
                    ir_emit_cvtsi2sd(c, (uint16_t)float_temp, (uint16_t)result_temp);
                }
                /* Float assignment: store via IR */
                if (sym->type.tag == TYPE_FLOAT) {
                    int addr = c->next_temp_id++;
                    ir_emit_lea(c, addr, BE(c)->fp_reg, -sym->stack_offset);
                    ir_emit_vstoress(c, (uint16_t)addr, (uint16_t)float_temp);
                } else {
                    ir_emit_fstore(c, (uint16_t)float_temp, TEMP_NONE, -sym->stack_offset);
                }
                c->last_expr_is_float = 1;
                return;
            } else {
                if (c->last_expr_is_float) {
                    /* Convert float to int via IR */
                    int int_temp = c->next_temp_id++;
                    ir_emit_cvttsd2si(c, (uint16_t)int_temp, (uint16_t)result_temp);
                    ir_emit_store(c, (uint16_t)int_temp, BE(c)->fp_reg, -sym->stack_offset);
                    c->last_expr_is_float = 0;
                    return;
                } else {
                    /* If initializer was a brace initializer storing directly to sym in-place, don't overwrite */
                    ZeroCCASTNode *init_node = zero_cc_ast_get(arena, node->left);
                    bool is_inplace_brace_init = (init_node && init_node->tag == NODE_COMPOUND_LIT &&
                                                  init_node->s.str && strcmp(init_node->s.str, sym->name) == 0);
                    if (!zero_cc_type_is_array(&sym->type) || !init_node || init_node->tag != NODE_COMPOUND_LIT) {
                        ZeroCCSym *src_sym = (init_node && init_node->tag == NODE_IDENT) ? zero_cc_find_local(c, init_node->s.str) : NULL;
                        if (zero_cc_type_is_struct_or_union(&sym->type) && sym->type.size > 16) {
                            if (!is_inplace_brace_init) {
                                int num_chunks = (sym->type.size + 7) / 8;
                                if (src_sym && zero_cc_type_is_struct_or_union(&src_sym->type)) {
                                    for (int i = 0; i < num_chunks; i++) {
                                        size_t rem = sym->type.size - (size_t)(i * 8);
                                        if (rem >= 8) {
                                            int chunk_temp = c->next_temp_id++;
                                            ir_emit_load(c, chunk_temp, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
                                            ir_emit_store(c, chunk_temp, BE(c)->fp_reg, -(sym->stack_offset - i * 8));
                                        } else {
                                            /* Tail chunk (rem 1..7): copy exactly
                                             * rem bytes - a fixed 8-byte chunk
                                             * overwrites the bytes adjacent to
                                             * the destination slot (c-torture
                                             * execute/va-arg-22). */
                                            int tail_a = c->next_temp_id++;
                                            ir_emit_lea(c, tail_a, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
                                            int dst_a = c->next_temp_id++;
                                            ir_emit_lea(c, dst_a, BE(c)->fp_reg, -(sym->stack_offset - i * 8));
                                            for (size_t b = 0; b < rem; b++) {
                                                int bv = c->next_temp_id++;
                                                int sa = tail_a, da = dst_a;
                                                if (b) {
                                                    int t = c->next_temp_id++;
                                                    ir_emit_mov_imm(c, t, (int)b);
                                                    sa = c->next_temp_id++;
                                                    ir_emit_add(c, sa, tail_a, t);
                                                    int t2 = c->next_temp_id++;
                                                    ir_emit_mov_imm(c, t2, (int)b);
                                                    da = c->next_temp_id++;
                                                    ir_emit_add(c, da, dst_a, t2);
                                                }
                                                ir_emit_load_byte(c, bv, sa);
                                                ir_emit_store_byte(c, bv, da);
                                            }
                                        }
                                    }
                                } else {
                                    /* System V AMD64 ABI: sret buffer pointer in result_temp.
                                     * Spill result_temp to a stack slot so offset additions don't clobber it.
                                     * Copy chunks from [result_temp + i*8] to local stack -(sym->stack_offset - i*8) */
                                    c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
                                    c->current_stack_frame += 8;
                                    int32_t buf_ptr_off = c->current_stack_frame;
                                    ir_emit_store(c, result_temp, BE(c)->fp_reg, -buf_ptr_off);

                                    for (int i = 0; i < num_chunks; i++) {
                                        size_t rem = sym->type.size - (size_t)(i * 8);
                                        int chunk_temp = c->next_temp_id++;
                                        int addr_temp;
                                        if (i == 0) {
                                            addr_temp = c->next_temp_id++;
                                            ir_emit_load(c, addr_temp, BE(c)->fp_reg, -buf_ptr_off);
                                        } else {
                                            int base_temp = c->next_temp_id++;
                                            ir_emit_load(c, base_temp, BE(c)->fp_reg, -buf_ptr_off);
                                            int off_temp = c->next_temp_id++;
                                            ir_emit_mov_imm(c, off_temp, i * 8);
                                            addr_temp = c->next_temp_id++;
                                            ir_emit_add(c, addr_temp, base_temp, off_temp);
                                        }
                                        int32_t dst_off = -(sym->stack_offset - i * 8);
                                        if (rem >= 8) {
                                            ir_emit_deref(c, chunk_temp, addr_temp);
                                            ir_emit_store(c, chunk_temp, BE(c)->fp_reg, dst_off);
                                        } else if (rem == 4) {
                                            ir_emit_load_32(c, chunk_temp, addr_temp);
                                            int dst_addr = c->next_temp_id++;
                                            ir_emit_lea(c, dst_addr, TEMP_NONE, dst_off);
                                            ir_emit_store_32(c, chunk_temp, dst_addr);
                                        } else if (rem == 2) {
                                            ir_emit_load_16(c, chunk_temp, addr_temp);
                                            int dst_addr = c->next_temp_id++;
                                            ir_emit_lea(c, dst_addr, TEMP_NONE, dst_off);
                                            ir_emit_store_16(c, chunk_temp, dst_addr);
                                        } else if (rem == 1) {
                                            ir_emit_load_byte(c, chunk_temp, addr_temp);
                                            int dst_addr = c->next_temp_id++;
                                            ir_emit_lea(c, dst_addr, TEMP_NONE, dst_off);
                                            ir_emit_store_byte(c, chunk_temp, dst_addr);
                                        } else {
                                            /* Tail of 3,5,6,7 bytes: byte-by-byte -
                                             * a single narrow load/store drops
                                             * the extra bytes (c-torture
                                             * execute/va-arg-22: `A35 c = *p;`
                                             * lost the 35th byte). */
                                            for (size_t b = 0; b < rem; b++) {
                                                int bv = c->next_temp_id++;
                                                int sa = addr_temp;
                                                if (b) {
                                                    int t = c->next_temp_id++;
                                                    ir_emit_mov_imm(c, t, (int)b);
                                                    sa = c->next_temp_id++;
                                                    ir_emit_add(c, sa, addr_temp, t);
                                                }
                                                ir_emit_load_byte(c, bv, sa);
                                                int dst_addr = c->next_temp_id++;
                                                ir_emit_lea(c, dst_addr, TEMP_NONE, dst_off);
                                                int da = dst_addr;
                                                if (b) {
                                                    int t = c->next_temp_id++;
                                                    ir_emit_mov_imm(c, t, (int)b);
                                                    da = c->next_temp_id++;
                                                    ir_emit_add(c, da, dst_addr, t);
                                                }
                                                ir_emit_store_byte(c, bv, da);
                                            }
                                        }
                                    }
                                }
                            }
                        } else if (zero_cc_type_is_struct_or_union(&sym->type) &&
                                   is_inplace_brace_init) {
                            /* Struct/union already initialized in-place by brace initializer */
                        } else if (zero_cc_type_is_vector(&sym->type)) {
                            /* GNU vector declaration initializer. The RHS form:
                             *  - NODE_CALL: value in the VREG_0/VREG_2 pair;
                             *  - vector local ident: value in its frame slot;
                             *  - anything else (in-place brace init, binop
                             *    temp, compound literal, deref): ADDRESS of
                             *    a vector object. Copy 8-byte chunks into
                             *    the new slot. */
                            if (is_inplace_brace_init) {
                                /* Already initialized in place. */
                            } else {
                                int num_chunks = (int)((sym->type.size + 7) / 8);
                                ZeroCCASTNode *lnode = zero_cc_ast_get(arena, node->left);
                                ZeroCCSym *src_sym = (lnode && lnode->tag == NODE_IDENT) ? zero_cc_find_local(c, lnode->s.str) : NULL;
                                bool src_is_vec_ident = src_sym && src_sym->type.tag == TYPE_USER_0 &&
                                                        zero_cc_type_is_vector(&src_sym->type);
                                /* A by-value call result is already
                                 * materialized into a stack object; its
                                 * result_temp is the object's ADDRESS, so
                                 * it falls into the generic address form. */
                                for (int i = 0; i < num_chunks; i++) {
                                    int ch_val = c->next_temp_id++;
                                    if (src_is_vec_ident) {
                                        if (src_sym->is_promoted) {
                                            ch_val = src_sym->promoted_vreg;  /* 8B single-reg */
                                        } else {
                                            ir_emit_load(c, ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - i * 8));
                                        }
                                    } else {
                                        if (i == 0) {
                                            ir_emit_deref(c, ch_val, result_temp);
                                        } else {
                                            int o8 = c->next_temp_id++;
                                            ir_emit_mov_imm(c, o8, i * 8);
                                            int a2 = c->next_temp_id++;
                                            ir_emit_add(c, a2, result_temp, o8);
                                            ir_emit_deref(c, ch_val, a2);
                                        }
                                    }
                                    ir_emit_store(c, ch_val, BE(c)->fp_reg, -(sym->stack_offset - i * 8));
                                }
                            }
                        } else {
                            /* Integer or small struct assignment: use IR store */
                            bool rhs_is_struct_addr = (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) &&
                                                      ast_expr_is_struct_addr(c, arena, node->left);
                            if (rhs_is_struct_addr) {
                                if (sym->type.size <= 8) {
                                    int val_temp = c->next_temp_id++;
                                    if (sym->type.size == 1) ir_emit_load_byte(c, val_temp, result_temp);
                                    else if (sym->type.size == 2) ir_emit_load_16(c, val_temp, result_temp);
                                    else if (sym->type.size == 4) ir_emit_load_32(c, val_temp, result_temp);
                                    else ir_emit_deref(c, val_temp, result_temp);
                                    ir_emit_store(c, val_temp, BE(c)->fp_reg, -sym->stack_offset);
                                } else {
                                    int val1 = c->next_temp_id++;
                                    ir_emit_deref(c, val1, result_temp);
                                    ir_emit_store(c, val1, BE(c)->fp_reg, -sym->stack_offset);

                                    int off8 = c->next_temp_id++;
                                    ir_emit_mov_imm(c, off8, 8);
                                    int addr2 = c->next_temp_id++;
                                    ir_emit_add(c, addr2, result_temp, off8);

                                    int val2 = c->next_temp_id++;
                                    size_t rem = sym->type.size - 8;
                                    if (rem == 8) {
                                        ir_emit_deref(c, val2, addr2);
                                        ir_emit_store(c, val2, BE(c)->fp_reg, -(sym->stack_offset - 8));
                                    } else {
                                        if (rem == 1) ir_emit_load_byte(c, val2, addr2);
                                        else if (rem == 2) ir_emit_load_16(c, val2, addr2);
                                        else if (rem == 4) ir_emit_load_32(c, val2, addr2);
                                        else ir_emit_deref(c, val2, addr2);
                                        int dst_a = c->next_temp_id++;
                                        ir_emit_lea(c, dst_a, TEMP_NONE, -(sym->stack_offset - 8));
                                        zc_store_low_bytes(c, val2, dst_a, rem);
                                    }
                                }
                            } else {
                                ZeroCCASTNode *lnode = zero_cc_ast_get(arena, node->left);
                                ZeroCCSym *src_sym = (lnode && lnode->tag == NODE_IDENT) ? zero_cc_find_local(c, lnode->s.str) : NULL;
                                if (sym->type.tag == TYPE_INT128 || sym->type.tag == TYPE_UINT128) {
                                    /* GNU __int128 (Sprint 109): store the full 16 bytes.
                                     * Widen a non-128-bit initializer (e.g. a literal) first. */
                                    bool unsigned_128 = zero_cc_type_is_unsigned(&sym->type);
                                    if (!result_is128) {
                                        int d = c->next_temp_id++;
                                        if (unsigned_128) ir_emit_zext64_128(c, d, result_temp);
                                        else ir_emit_sext64_128(c, d, result_temp);
                                        result_temp = d;
                                    }
                                    int addr128 = c->next_temp_id++;
                                    ir_emit_lea(c, addr128, BE(c)->fp_reg, -sym->stack_offset);
                                    ir_emit_store128(c, result_temp, addr128);
                                    c->last_expr_is_float = 0;
                                    return;
                                }
                                if (src_sym && (src_sym->type.tag == TYPE_STRUCT || src_sym->type.tag == TYPE_UNION) && sym->type.size > 16) {
                                    int num_chunks = (int)((sym->type.size + 7) / 8);
                                    for (int ch = 0; ch < num_chunks; ch++) {
                                        int ch_val = c->next_temp_id++;
                                        ir_emit_load(c, ch_val, BE(c)->fp_reg, -(src_sym->stack_offset - ch * 8));
                                        ir_emit_store(c, ch_val, BE(c)->fp_reg, -(sym->stack_offset - ch * 8));
                                    }
                                } else {
                                    ir_emit_store(c, result_temp, BE(c)->fp_reg, -sym->stack_offset);
                                    if (c->isa == ISA_RISCV32 && (sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 4 && sym->type.size <= 8) {
                                        /* RV32 ILP32 ABI: second 4-byte chunk returned in a1 (VREG_2) */
                                        ir_emit_store(c, VREG_2, BE(c)->fp_reg, -(sym->stack_offset - 4));
                                    } else if ((sym->type.tag == TYPE_STRUCT || sym->type.tag == TYPE_UNION) && sym->type.size > 8 && sym->type.size <= 16) {
                                        /* System V AMD64 ABI: second eightbyte returned in rdx (VREG_2)
                                         * Canonical secondary value register: VREG_2 maps to rdx/x1/a1
                                         * via regalloc_get_reg_for_temp. Store only the remaining
                                         * (size-8) bytes - a full 8-byte store scribbles the bytes
                                         * adjacent to the slot (c-torture execute/va-arg-22). */
                                        int dst_addr = c->next_temp_id++;
                                        ir_emit_lea(c, dst_addr, TEMP_NONE, -(sym->stack_offset - 8));
                                        zc_store_low_bytes(c, VREG_2, dst_addr, sym->type.size - 8);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            c->last_expr_is_float = 0;
        }
    }
}

void lower_stmt_static_assert(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        lower_expr(c, arena, node->left);
    }
}

void lower_stmt_return(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* If currently expanding an inlined function call, redirect return to the target temp and jump to exit label */
    if (c->lowering.inlining.active_depth > 0) {
        if (node && node->left != AST_NULL) {
            int res = lower_expr(c, arena, node->left);
            if (c->lowering.inlining.return_type.tag != TYPE_VOID) {
                /* Sprint 110: 128-bit return. Mirror the non-inlined path
                 * (below): widen a narrow expression to GPR128 per the
                 * return type's signedness, and copy the FULL 16-byte value
                 * into the target temp (OP_MOV_REG moves one 64-bit half).
                 * Without the widen, `return 5;` in an inlined
                 * `__int128 f()` leaves a 64-bit temp that the caller
                 * misclassifies as GPR128 (garbage high half). */
                bool ret128 = (c->lowering.inlining.return_type.tag == TYPE_INT128 ||
                               c->lowering.inlining.return_type.tag == TYPE_UINT128);
                if (!c->last_expr_is_float) {
                    if (ret128) {
                        if (!c->last_expr_is_128) {
                            int d = c->next_temp_id++;
                            if (zero_cc_type_is_unsigned(&c->lowering.inlining.return_type))
                                ir_emit_zext64_128(c, d, res);
                            else
                                ir_emit_sext64_128(c, d, res);
                            res = d;
                        }
                    } else {
                        zero_cc_emit_int_extend(c, res, &c->lowering.inlining.return_type);
                    }
                }
                if (c->lowering.inlining.return_target_temp >= 0 && res >= 0 && res != c->lowering.inlining.return_target_temp) {
                    if (ret128) ir_emit_mov128(c, c->lowering.inlining.return_target_temp, res);
                    else ir_emit_mov_reg(c, c->lowering.inlining.return_target_temp, res);
                }
            }
        }
        if (c->lowering.inlining.return_target_label != 0) {
            ir_emit_jmp(c, c->lowering.inlining.return_target_label);
        }
        return;
    }

    if (node && node->left != AST_NULL) {
        ZeroCCASTNode *ret_expr = zero_cc_ast_get(arena, node->left);
        ZeroCCSym *sym = (ret_expr && ret_expr->tag == NODE_IDENT) ? zero_cc_find_local(c, ret_expr->s.str) : NULL;
        ZeroCCFunction *cur_fn2 = zero_cc_get_current_fn(c);
        if (cur_fn2 && cur_fn2->return_type.tag == TYPE_VOID) {
            (void)lower_expr(c, arena, node->left);
            c->last_expr_is_float = 0;
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }
        /* GNU 128-bit return: a __int128/__uint128_t result is returned across
         * the (VREG_0, VREG_2) = (rax, rdx) pair, low then high (System V AMD64
         * ABI, same pair as the 9-16 byte struct return). */
        if (cur_fn2 && (cur_fn2->return_type.tag == TYPE_INT128 ||
                        cur_fn2->return_type.tag == TYPE_UINT128)) {
            int res = lower_expr(c, arena, node->left);
            if (!c->last_expr_is_128) {
                int d = c->next_temp_id++;
                if (zero_cc_type_is_unsigned(&cur_fn2->return_type)) ir_emit_zext64_128(c, d, res);
                else ir_emit_sext64_128(c, d, res);
                res = d;
            }
            ir_emit_mov_reg(c, VREG_0, res);   /* lo -> rax */
            ir_emit_hi128(c, VREG_2, res);     /* hi -> rdx */
            c->last_expr_is_float = 0;
            c->last_expr_is_128 = 0;
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }
        bool ret_cx_type = (cur_fn2 && zero_cc_type_is_complex(&cur_fn2->return_type));
        if (ret_cx_type) {
            bool ret_cx_is_fp = (cur_fn2->return_type.base &&
                                 (cur_fn2->return_type.base->tag == TYPE_FLOAT ||
                                  cur_fn2->return_type.base->tag == TYPE_DOUBLE ||
                                  cur_fn2->return_type.base->is_long_double));
            size_t ret_esz = (cur_fn2->return_type.base && cur_fn2->return_type.base->size > 0)
                             ? cur_fn2->return_type.base->size : (ret_cx_is_fp ? 8 : 4);
            size_t ret_csz = cur_fn2->return_type.size ? cur_fn2->return_type.size : ret_esz * 2;

            int caddr = TEMP_NONE;
            if (sym && zero_cc_type_is_complex(&sym->type)) {
                caddr = c->next_temp_id++;
                ir_emit_lea(c, caddr, BE(c)->fp_reg, -sym->stack_offset);
            } else {
                int cval = lower_expr(c, arena, node->left);
                TypeInfo *r_ti = ast_node_get_typeinfo(c, ret_expr);
                bool rhs_is_cx = (r_ti && r_ti->kind == TYPE_KIND_COMPLEX);
                if (!rhs_is_cx && ret_expr && ret_expr->tag == NODE_IDENT) {
                    ZeroCCGlobalVar *gv = find_global_var(c, ret_expr->s.str);
                    if (gv && zero_cc_type_is_complex(&gv->type)) rhs_is_cx = true;
                }
                if (c->last_expr_is_float) {
                    /* Scalar float operand */
                    if (ret_cx_is_fp) {
                        /* Sprint 115m: float-complex return narrows the
                         * model double into the low 32 bits of xmm0 (the
                         * ABI float pattern) instead of a raw fmov. */
                        if (ret_esz == 4)
                            ir_emit_cvttsd2ss(c, (uint16_t)VREG_F0, (uint16_t)cval);
                        else
                            ir_emit_fmov(c, (uint16_t)VREG_F0, (uint16_t)cval);
                        int rz = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)rz, (int64_t)zc_rodata_double_offset(c, 0.0));
                        ir_emit_fmov(c, (uint16_t)VREG_F1, (uint16_t)rz);
                    } else {
                        int ival = c->next_temp_id++;
                        ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = (uint16_t)ival,
                                           .temp_src1 = (uint16_t)cval, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                        ir_emit_mov_reg(c, VREG_0, ival);
                        if (ret_csz > 8) ir_emit_mov_imm(c, VREG_2, 0);
                    }
                    c->last_expr_is_float = 0;
                    ir_emit_jmp(c, c->lowering.epilogue_label);
                    return;
                } else if (!rhs_is_cx) {
                    /* Scalar integer operand */
                    if (ret_cx_is_fp) {
                        int fv = c->next_temp_id++;
                        ir_emit_cvtsi2sd(c, (uint16_t)fv, (uint16_t)cval);
                        if (ret_esz == 4)
                            ir_emit_cvttsd2ss(c, (uint16_t)VREG_F0, (uint16_t)fv);
                        else
                            ir_emit_fmov(c, (uint16_t)VREG_F0, (uint16_t)fv);
                        int rz = c->next_temp_id++;
                        ir_emit_fmov_imm(c, (uint16_t)rz, (int64_t)zc_rodata_double_offset(c, 0.0));
                        ir_emit_fmov(c, (uint16_t)VREG_F1, (uint16_t)rz);
                    } else {
                        ir_emit_mov_reg(c, VREG_0, cval);
                        if (ret_csz > 8) ir_emit_mov_imm(c, VREG_2, 0);
                    }
                    c->last_expr_is_float = 0;
                    ir_emit_jmp(c, c->lowering.epilogue_label);
                    return;
                }
                caddr = cval;
            }

            if (ret_cx_is_fp) {
                int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, (int)ret_esz);
                int a2 = c->next_temp_id++; ir_emit_add(c, a2, caddr, o2);
                if (ret_esz == 4 && c->isa != ISA_RISCV32) {
                    /* psABI: a _Complex float return is ONE register - real
                     * in the low 32 bits, imaginary in the HIGH 32 bits.
                     * The stack object already holds that exact 8-byte bit
                     * pattern ({real @+0, imag @+4}), so one raw 8-byte
                     * load (vloadsd = movsd/ldr d/fld) builds the register;
                     * gcc does the same memory round-trip. */
                    ir_emit_vloadsd(c, (uint16_t)VREG_F0, (uint16_t)caddr);
                } else if (ret_esz == 4) {
                    /* RV32: two registers, float in each low 32. */
                    ir_emit_vloadss_raw(c, (uint16_t)VREG_F0, (uint16_t)caddr);
                    ir_emit_vloadss_raw(c, (uint16_t)VREG_F1, (uint16_t)a2);
                } else {
                    ir_emit_vloadsd(c, (uint16_t)VREG_F0, (uint16_t)caddr);
                    ir_emit_vloadsd(c, (uint16_t)VREG_F1, (uint16_t)a2);
                }
            } else {
                if (ret_csz <= 8) {
                    if (ret_csz == 1 || ret_csz == 2) ir_emit_load_16(c, VREG_0, caddr);
                    else if (ret_csz == 4) ir_emit_load_32(c, VREG_0, caddr);
                    else ir_emit_deref(c, VREG_0, caddr);
                } else {
                    ir_emit_deref(c, VREG_0, caddr);
                    int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, 8);
                    int a2 = c->next_temp_id++; ir_emit_add(c, a2, caddr, o2);
                    ir_emit_deref(c, VREG_2, a2);
                }
            }
            c->last_expr_is_float = 0;
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }

        if (sym && zero_cc_type_is_complex(&sym->type)) {
            /* C99 6.8.6.4: return complex value in a scalar-returning function */
            bool sym_cx_is_fp = (sym->type.base &&
                                 (sym->type.base->tag == TYPE_FLOAT ||
                                  sym->type.base->tag == TYPE_DOUBLE ||
                                  sym->type.base->is_long_double));
            int32_t cesz2 = (sym->type.base && sym->type.base->size > 0) ? sym->type.base->size : 8;
            int caddr2 = c->next_temp_id++;
            ir_emit_lea(c, caddr2, BE(c)->fp_reg, -sym->stack_offset);

            bool ret_float = (cur_fn2 &&
                              (cur_fn2->return_type.tag == TYPE_FLOAT ||
                               cur_fn2->return_type.tag == TYPE_DOUBLE ||
                               cur_fn2->return_type.is_long_double));
            if (sym_cx_is_fp) {
                int rt = c->next_temp_id++;
                if (cesz2 == 4) {
                    ir_emit_vloadss(c, (uint16_t)rt, (uint16_t)caddr2);
                } else {
                    ir_emit_fload(c, rt, TEMP_NONE, -sym->stack_offset);
                }
                if (ret_float) {
                    /* Sprint 115m: a `float` return narrows the real part to
                     * the ABI float pattern in the low 32 of xmm0. */
                    if (cur_fn2->return_type.tag == TYPE_FLOAT) {
                        ir_emit_cvtsd2ss(c, (uint16_t)VREG_0, (uint16_t)rt);
                    } else {
                        ir_emit_fmov(c, VREG_0, (uint16_t)rt);
                    }
                    c->last_expr_is_float = 1;
                } else {
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = VREG_0,
                        .temp_src1 = (uint16_t)rt, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    c->last_expr_is_float = 0;
                }
            } else {
                int rt = c->next_temp_id++;
                if (cesz2 == 1) ir_emit_load_byte(c, rt, caddr2);
                else if (cesz2 == 2) ir_emit_load_16(c, rt, caddr2);
                else if (cesz2 == 4) ir_emit_load_32(c, rt, caddr2);
                else ir_emit_deref(c, rt, caddr2);

                if (ret_float) {
                    if (cur_fn2->return_type.tag == TYPE_FLOAT)
                        ir_emit_cvttsd2ss(c, (uint16_t)VREG_0, (uint16_t)VREG_0);
                    else
                        ir_emit_cvtsi2sd(c, (uint16_t)VREG_0, (uint16_t)rt);
                    if (cur_fn2->return_type.tag != TYPE_FLOAT)
                        c->last_expr_is_float = 1;
                    else
                        c->last_expr_is_float = 0;
                } else {
                    ir_emit_mov_reg(c, VREG_0, rt);
                    c->last_expr_is_float = 0;
                }
            }
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }

        if (sym && (zero_cc_type_is_struct_or_union(&sym->type) || zero_cc_type_is_vector(&sym->type))) {
            if (sym->is_promoted) {
                ir_emit_mov_reg(c, VREG_0, sym->promoted_vreg);
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            }
            int32_t struct_offset = -sym->stack_offset;
            if (c->isa == ISA_RISCV32 && sym->type.size > 4 && sym->type.size <= 8) {
                /* RV32 ILP32 ABI: 5-8 byte struct returned across a0 (VREG_0) and a1 (VREG_2) */
                ir_emit_load(c, VREG_0, BE(c)->fp_reg, struct_offset);
                ir_emit_load(c, VREG_2, BE(c)->fp_reg, struct_offset + 4);
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            } else if (sym->type.size <= 8) {
                /* System V AMD64 ABI: <=8 byte struct returned in rax (VREG_0) */
                ir_emit_load(c, VREG_0, BE(c)->fp_reg, struct_offset);
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            } else if (sym->type.size <= 16) {
                /* System V AMD64 ABI: 9-16 byte struct returned across rax and rdx
                 * Canonical secondary value register: VREG_2 maps to rdx/x1/a1
                 * via regalloc_get_reg_for_temp. */
                // 1. Load lower 8 bytes into rax (VREG_0) via IR
                ir_emit_load(c, VREG_0, BE(c)->fp_reg, struct_offset);

                // 2. Load upper 8 bytes into rdx (VREG_2) via IR
                ir_emit_load(c, VREG_2, BE(c)->fp_reg, struct_offset + 8);

                // 3. Jump to epilogue (which will emit leave; ret)
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            }

            /* System V AMD64 ABI: >16 byte struct returned via hidden pointer (sret) */
            /* RDI contains the hidden pointer to caller's buffer */
            else {
                ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
                int32_t sret_off = cur_fn ? cur_fn->sret_stack_offset : 0;

                /* Load hidden pointer directly into VREG_0 (RAX) from stack slot (RDI saved at -sret_off) */
                ir_emit_load(c, VREG_0, BE(c)->fp_reg, -sret_off);

                /* Store each 8-byte chunk to [hidden_ptr + i*8] */
                int num_chunks = (sym->type.size + 7) / 8;
                for (int i = 0; i < num_chunks; i++) {
                    size_t rem = sym->type.size - (size_t)(i * 8);
                    int chunk_temp = c->next_temp_id++;
                    int addr_temp;
                    if (i == 0) {
                        addr_temp = c->next_temp_id++;
                        ir_emit_load(c, addr_temp, BE(c)->fp_reg, -sret_off);
                    } else {
                        int base_temp = c->next_temp_id++;
                        ir_emit_load(c, base_temp, BE(c)->fp_reg, -sret_off);
                        int off_temp = c->next_temp_id++;
                        ir_emit_mov_imm(c, off_temp, i * 8);
                        addr_temp = c->next_temp_id++;
                        ir_emit_add(c, addr_temp, base_temp, off_temp);
                    }

                    /* Load chunk from local stack: local offset is -(sym->stack_offset - i*8) */
                    int32_t chunk_stack_off = -(sym->stack_offset - i * 8);
                    if (rem >= 8) {
                        ir_emit_load(c, chunk_temp, BE(c)->fp_reg, chunk_stack_off);
                        ir_emit_store_64(c, chunk_temp, addr_temp);
                    } else if (rem >= 4) {
                        int src_addr = c->next_temp_id++;
                        ir_emit_lea(c, src_addr, TEMP_NONE, chunk_stack_off);
                        ir_emit_load_32(c, chunk_temp, src_addr);
                        ir_emit_store_32(c, chunk_temp, addr_temp);
                    } else if (rem >= 2) {
                        int src_addr = c->next_temp_id++;
                        ir_emit_lea(c, src_addr, TEMP_NONE, chunk_stack_off);
                        ir_emit_load_16(c, chunk_temp, src_addr);
                        ir_emit_store_16(c, chunk_temp, addr_temp);
                    } else {
                        int src_addr = c->next_temp_id++;
                        ir_emit_lea(c, src_addr, TEMP_NONE, chunk_stack_off);
                        ir_emit_load_byte(c, chunk_temp, src_addr);
                        ir_emit_store_byte(c, chunk_temp, addr_temp);
                    }
                }

                /* Return hidden pointer in RAX (VREG_0) */
                ir_emit_load(c, VREG_0, BE(c)->fp_reg, -sret_off);
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            }
        }

        /* Global struct/union return (`return x;` where x is a file-scope
         * struct): the local-ident branch above only sees function-scope
         * symbols, so a 9-16B global struct would fall through to the
         * generic path and return only its first 8 bytes in VREG_0
         * (c-torture pr60017). Load the full register pair / sret chunks
         * directly from the global. */
        ZeroCCGlobalVar *gsym = (ret_expr && ret_expr->tag == NODE_IDENT) ? find_global_var(c, ret_expr->s.str) : NULL;
        if (gsym && (gsym->type.tag == TYPE_STRUCT || gsym->type.tag == TYPE_UNION) &&
            gsym->type.size > 0) {
            int gaddr = c->next_temp_id++;
            if (gsym->is_tls && !c->jit_mode) ir_emit_tls_lea(c, gaddr, gsym->name);
            else ir_emit_mov_global(c, gaddr, gsym->name);
            if (gsym->type.size <= 16) {
                ir_emit_deref(c, VREG_0, gaddr);
                if (gsym->type.size > 8) {
                    int off_t = c->next_temp_id++;
                    ir_emit_mov_imm(c, off_t, 8);
                    int a2 = c->next_temp_id++;
                    ir_emit_add(c, a2, gaddr, off_t);
                    ir_emit_deref(c, VREG_2, a2);
                }
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            }
            /* >16B: sret chunk copy into the caller's hidden-pointer buffer */
            ZeroCCFunction *gf = zero_cc_get_current_fn(c);
            int32_t gs_off = gf ? gf->sret_stack_offset : 0;
            int g_chunks = (int)((gsym->type.size + 7) / 8);
            for (int i = 0; i < g_chunks; i++) {
                int s_base = gaddr;
                if (i > 0) {
                    int s_off = c->next_temp_id++;
                    ir_emit_mov_imm(c, s_off, i * 8);
                    s_base = c->next_temp_id++;
                    ir_emit_add(c, s_base, gaddr, s_off);
                }
                int d_base = c->next_temp_id++;
                ir_emit_load(c, d_base, BE(c)->fp_reg, -gs_off);
                if (i > 0) {
                    int d_off = c->next_temp_id++;
                    ir_emit_mov_imm(c, d_off, i * 8);
                    int d_addr = c->next_temp_id++;
                    ir_emit_add(c, d_addr, d_base, d_off);
                    int gchunk = c->next_temp_id++;
                    ir_emit_deref(c, gchunk, s_base);
                    ir_emit_store_64(c, gchunk, d_addr);
                } else {
                    int gchunk = c->next_temp_id++;
                    ir_emit_deref(c, gchunk, s_base);
                    ir_emit_store_64(c, gchunk, d_base);
                }
            }
            ir_emit_load(c, VREG_0, BE(c)->fp_reg, -gs_off);
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }

        int result_temp = lower_expr(c, arena, node->left);
        ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
        TypeInfo *ret_expr_ti = ast_node_get_typeinfo(c, ret_expr);
        if (ret_expr_ti && ret_expr_ti->kind == TYPE_KIND_COMPLEX) {
            bool r_fp = ret_expr_ti->details.array.base && type_is_floating(ret_expr_ti->details.array.base);
            size_t r_esz = ret_expr_ti->details.array.base ? type_size(ret_expr_ti->details.array.base) : 8;
            bool ret_float = (cur_fn &&
                              (cur_fn->return_type.tag == TYPE_FLOAT ||
                               cur_fn->return_type.tag == TYPE_DOUBLE ||
                               cur_fn->return_type.is_long_double));
            if (r_fp) {
                int rt = c->next_temp_id++;
                if (r_esz == 4) ir_emit_vloadss(c, (uint16_t)rt, (uint16_t)result_temp);
                else ir_emit_vloadsd(c, (uint16_t)rt, (uint16_t)result_temp);
                if (ret_float) {
                    /* Sprint 115m: a `float` return narrows the real part to
                     * the ABI float pattern in the low 32 of xmm0. */
                    if (cur_fn->return_type.tag == TYPE_FLOAT) {
                        ir_emit_cvtsd2ss(c, (uint16_t)VREG_0, (uint16_t)rt);
                    } else {
                        ir_emit_fmov(c, VREG_0, (uint16_t)rt);
                    }
                    c->last_expr_is_float = 1;
                } else {
                    ir_emit(c, (Instr){.op = OP_CVTTSD2SI, .temp_dest = VREG_0,
                        .temp_src1 = (uint16_t)rt, .temp_src2 = TEMP_NONE, .imm = 0, .label_idx = 0});
                    c->last_expr_is_float = 0;
                }
            } else {
                int rt = c->next_temp_id++;
                if (r_esz == 1) ir_emit_load_byte(c, rt, result_temp);
                else if (r_esz == 2) ir_emit_load_16(c, rt, result_temp);
                else if (r_esz == 4) ir_emit_load_32(c, rt, result_temp);
                else ir_emit_deref(c, rt, result_temp);
                if (ret_float) {
                    if (cur_fn->return_type.tag == TYPE_FLOAT)
                        ir_emit_cvttsd2ss(c, (uint16_t)VREG_0, (uint16_t)VREG_0);
                    else
                        ir_emit_cvtsi2sd(c, (uint16_t)VREG_0, (uint16_t)rt);
                    if (cur_fn->return_type.tag != TYPE_FLOAT)
                        c->last_expr_is_float = 1;
                    else
                        c->last_expr_is_float = 0;
                } else {
                    ir_emit_mov_reg(c, VREG_0, rt);
                    c->last_expr_is_float = 0;
                }
            }
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }
        {
            ZeroCCType fn_ret = zero_cc_get_current_fn_ret_type(c);
            if ((fn_ret.tag == TYPE_USER_0 && zero_cc_type_is_vector(&fn_ret)) &&
                fn_ret.size <= 16 && !c->last_expr_is_float) {
                if (ret_expr->tag == NODE_CALL) {
                    /* Nested call returning a vector: the result is already
                     * in the VREG_0/VREG_2 register pair. */
                    ir_emit_jmp(c, c->lowering.epilogue_label);
                    return;
                }
                /* Vector return value (cast / binop / compound literal): the
                 * expression lowered to the address of a 16/8B stack object;
                 * load the 8-byte chunks into the register pair. */
                ir_emit_deref(c, VREG_0, result_temp);
                if (fn_ret.size > 8) {
                    int off = c->next_temp_id++;
                    ir_emit_mov_imm(c, off, 8);
                    int a2 = c->next_temp_id++;
                    ir_emit_add(c, a2, result_temp, off);
                    int ch2 = c->next_temp_id++;
                    ir_emit_deref(c, ch2, a2);
                    ir_emit_mov_reg(c, VREG_2, ch2);
                }
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            }
        }
        if (cur_fn && cur_fn->ret_struct_sz > 16) {
            /* Return expression (e.g., NODE_CALL or NODE_COMPOUND_LIT) evaluated to a pointer to a >16B struct buffer.
             * Copy chunks from [result_temp + i*8] to caller's sret buffer at -sret_off. */
            int32_t sret_off = cur_fn->sret_stack_offset;
            c->current_stack_frame = (c->current_stack_frame + 7) & ~7;
            c->current_stack_frame += 8;
            int32_t src_ptr_off = c->current_stack_frame;
            ir_emit_store(c, result_temp, BE(c)->fp_reg, -src_ptr_off);

            size_t st_size = (size_t)cur_fn->ret_struct_sz;
            int num_chunks = (int)((st_size + 7) / 8);
            for (int i = 0; i < num_chunks; i++) {
                size_t rem = st_size - (size_t)(i * 8);
                int chunk_temp = c->next_temp_id++;
                int src_addr;
                int dst_addr;
                if (i == 0) {
                    src_addr = c->next_temp_id++;
                    ir_emit_load(c, src_addr, BE(c)->fp_reg, -src_ptr_off);
                    dst_addr = c->next_temp_id++;
                    ir_emit_load(c, dst_addr, BE(c)->fp_reg, -sret_off);
                } else {
                    int s_base = c->next_temp_id++;
                    ir_emit_load(c, s_base, BE(c)->fp_reg, -src_ptr_off);
                    int s_off = c->next_temp_id++;
                    ir_emit_mov_imm(c, s_off, i * 8);
                    src_addr = c->next_temp_id++;
                    ir_emit_add(c, src_addr, s_base, s_off);

                    int d_base = c->next_temp_id++;
                    ir_emit_load(c, d_base, BE(c)->fp_reg, -sret_off);
                    int d_off = c->next_temp_id++;
                    ir_emit_mov_imm(c, d_off, i * 8);
                    dst_addr = c->next_temp_id++;
                    ir_emit_add(c, dst_addr, d_base, d_off);
                }
                if (rem >= 8) {
                    ir_emit_deref(c, chunk_temp, src_addr);
                    ir_emit_store_64(c, chunk_temp, dst_addr);
                } else if (rem >= 4) {
                    ir_emit_load_32(c, chunk_temp, src_addr);
                    ir_emit_store_32(c, chunk_temp, dst_addr);
                } else if (rem >= 2) {
                    ir_emit_load_16(c, chunk_temp, src_addr);
                    ir_emit_store_16(c, chunk_temp, dst_addr);
                } else {
                    ir_emit_load_byte(c, chunk_temp, src_addr);
                    ir_emit_store_byte(c, chunk_temp, dst_addr);
                }
            }
            ir_emit_load(c, VREG_0, BE(c)->fp_reg, -sret_off);
            ir_emit_jmp(c, c->lowering.epilogue_label);
            return;
        }
        /* Struct/union return from a non-ident expression (array element,
         * deref, member, compound literal): the expression lowers to the
         * ADDRESS of the struct object - storing result_temp would put the
         * pointer in rax, and a 9-16B return also needs VREG_2. A nested
         * call already placed the value in the ABI registers/sret buffer.
         * (c-torture execute/pr82388: `return h[29];` on a 12B struct.) */
        {
            ZeroCCType fn_ret_s = zero_cc_get_current_fn_ret_type(c);
            if ((fn_ret_s.tag == TYPE_STRUCT || fn_ret_s.tag == TYPE_UNION) && fn_ret_s.size > 0) {
                if (ret_expr->tag == NODE_CALL) {
                    ir_emit_jmp(c, c->lowering.epilogue_label);
                    return;
                }
                if (ret_expr->tag == NODE_UNOP &&
                    (ret_expr->aux.op == '*' || ret_expr->aux.op == TK_STAR) &&
                    fn_ret_s.size > 8 && fn_ret_s.size <= 16) {
                    /* `return *p;` on a 9-16B struct: the deref lowering
                     * only loads the first 8 bytes into result_temp; load
                     * the full ABI pair straight from the pointee. */
                    int ptr_t = lower_expr(c, arena, ret_expr->left);
                    ir_emit_deref(c, VREG_0, ptr_t);
                    int roff = c->next_temp_id++;
                    ir_emit_mov_imm(c, roff, 8);
                    int ra2 = c->next_temp_id++;
                    ir_emit_add(c, ra2, ptr_t, roff);
                    int rch2 = c->next_temp_id++;
                    ir_emit_deref(c, rch2, ra2);
                    ir_emit_mov_reg(c, VREG_2, rch2);
                    ir_emit_jmp(c, c->lowering.epilogue_label);
                    return;
                }
                if (ast_expr_is_struct_addr(c, arena, node->left)) {
                    if (fn_ret_s.size <= 8) {
                        ir_emit_deref(c, VREG_0, result_temp);
                    } else if (fn_ret_s.size <= 16) {
                        ir_emit_deref(c, VREG_0, result_temp);
                        int roff = c->next_temp_id++;
                        ir_emit_mov_imm(c, roff, 8);
                        int ra2 = c->next_temp_id++;
                        ir_emit_add(c, ra2, result_temp, roff);
                        int rch2 = c->next_temp_id++;
                        ir_emit_deref(c, rch2, ra2);
                        ir_emit_mov_reg(c, VREG_2, rch2);
                    }
                } else if (result_temp != VREG_0) {
                    ir_emit_mov_reg(c, VREG_0, result_temp);
                }
                ir_emit_jmp(c, c->lowering.epilogue_label);
                return;
            }
        }
        ZeroCCType fn_ret_type = zero_cc_get_current_fn_ret_type(c);
        int final_float_temp = result_temp;
        if (!c->last_expr_is_float) {
            /* C99 width semantics: truncate/extend the result to the declared
             * return width (e.g. `return a + a;` from an `int` function must
             * wrap to 32 bits, not return the 64-bit residue). */
            if (fn_ret_type.tag != TYPE_FLOAT && fn_ret_type.tag != TYPE_DOUBLE) {
                zero_cc_emit_int_extend(c, result_temp, &fn_ret_type);
            }
            /* Move result to rax if not already there - emit IR */
            if (result_temp != VREG_0) {
                ir_emit_mov_reg(c, VREG_0, result_temp);
            }
            /* If function returns float/double, convert int to float
             * (u64-aware: cvtsi2sd misreads unsigned 64-bit sources). */
            if (fn_ret_type.tag == TYPE_FLOAT || fn_ret_type.tag == TYPE_DOUBLE) {
                ZeroCCASTNode *ret_node = zero_cc_ast_get(arena, node->left);
                TypeInfo *src_ti = ret_node ? ast_node_get_typeinfo(c, ret_node) : NULL;
                final_float_temp = zc_emit_int_to_double(c, result_temp, src_ti);
            }
        } else {
            /* Expression is float/double. If function returns integer, convert float to int */
            if (fn_ret_type.tag != TYPE_FLOAT && fn_ret_type.tag != TYPE_DOUBLE) {
                int int_res = c->next_temp_id++;
                ir_emit_cvttsd2si(c, (uint16_t)int_res, (uint16_t)result_temp);
                zero_cc_emit_int_extend(c, int_res, &fn_ret_type);
                ir_emit_mov_reg(c, VREG_0, int_res);
                c->last_expr_is_float = 0;
            }
        }
        /* Float result must be in the FP return register (xmm0/d0/fa0).
         * VREG_F0 (temp 0) always maps to physical register 0, which the FP
         * emitters treat as the FP return register, so move the result there
         * when the computed value lives in another FP temp. */
        if (c->last_expr_is_float) {
            if (fn_ret_type.tag == TYPE_FLOAT) {
                /* Sprint 115m: `float` return - narrow the model double into
                 * the low 32 bits of xmm0 (the ABI float pattern). Valid
                 * in-place (final_float_temp == VREG_F0) too. */
                ir_emit_cvttsd2ss(c, (uint16_t)VREG_F0, (uint16_t)final_float_temp);
            } else if (final_float_temp != VREG_F0) {
                ir_emit_fmov(c, (uint8_t)VREG_F0, (uint16_t)final_float_temp);
            }
        }
    } else {
        /* Bare `return;`. In a void function this is the normal shape. In a
         * non-void function it is the C89 gnu89 leniency accepted with a
         * warning (see stmt_handle_return) - return 0 for scalar types so
         * the value is deterministic (c-torture 920415-1). */
        ZeroCCFunction *br_fn = zero_cc_get_current_fn(c);
        if (br_fn && br_fn->return_type.tag != TYPE_VOID &&
            br_fn->return_type.tag != TYPE_STRUCT &&
            br_fn->return_type.tag != TYPE_UNION &&
            br_fn->return_type.tag != TYPE_USER_0 &&
            !zero_cc_type_is_complex(&br_fn->return_type)) {
            ir_emit_mov_imm(c, VREG_0, 0);
        }
        c->last_expr_is_float = 0;
    }
    ir_emit_jmp(c, c->lowering.epilogue_label);
}

/* Emit return jumps after IR-to-MC */
void emit_return_jumps(ZeroCCCompiler *c) {
    (void)c;
}

static int lower_condition_as_int(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef expr_ref) {
    TypeInfo *cond_ti = (expr_ref != AST_NULL) ? ast_node_get_typeinfo(c, zero_cc_ast_get(arena, expr_ref)) : NULL;
    int cond_temp = lower_expr(c, arena, expr_ref);
    if (cond_ti && cond_ti->kind == TYPE_KIND_COMPLEX) {
        size_t esz = (cond_ti->details.array.base) ? type_size(cond_ti->details.array.base) : 8;
        if (esz != 4 && esz != 8) esz = 8;
        int rh = c->next_temp_id++;
        int li = c->next_temp_id++;
        if (esz == 4) {
            ir_emit_vloadss(c, rh, (uint16_t)cond_temp);
            int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, 4);
            int a2 = c->next_temp_id++; ir_emit_add(c, a2, cond_temp, o2);
            ir_emit_vloadss(c, li, (uint16_t)a2);
        } else {
            ir_emit_vloadsd(c, rh, (uint16_t)cond_temp);
            int o2 = c->next_temp_id++; ir_emit_mov_imm(c, o2, 8);
            int a2 = c->next_temp_id++; ir_emit_add(c, a2, cond_temp, o2);
            ir_emit_vloadsd(c, li, (uint16_t)a2);
        }
        int zf = c->next_temp_id++;
        ir_emit_fmov_imm(c, (uint16_t)zf, (int64_t)zc_rodata_double_offset(c, 0.0));
        int cr = c->next_temp_id++;
        ir_emit_cmp(c, rh, zf);
        ir_emit_cond_set(c, cr, CC_NE);
        int ci = c->next_temp_id++;
        ir_emit_cmp(c, li, zf);
        ir_emit_cond_set(c, ci, CC_NE);
        int res = c->next_temp_id++;
        ir_emit_or(c, res, cr, ci);
        c->last_expr_is_float = 0;
        return res;
    }
    if (c->last_expr_is_float) {
        int zero_i = c->next_temp_id++;
        ir_emit_mov_imm(c, zero_i, 0);
        int zero_f = c->next_temp_id++;
        ir_emit_cvtsi2sd(c, (uint16_t)zero_f, (uint16_t)zero_i);
        int cmp_i = c->next_temp_id++;
        ir_emit_cmp(c, cond_temp, zero_f);
        ir_emit_cond_set(c, cmp_i, CC_NE);
        cond_temp = cmp_i;
        c->last_expr_is_float = 0;
    }
    return cond_temp;
}

void lower_stmt_if(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    int saved_temp_id = c->next_temp_id;
    int cond_temp = lower_condition_as_int(c, arena, node->left);
    uint16_t else_label = c->ir_next_label++;
    uint16_t end_label = c->ir_next_label++;
    ir_emit_je(c, (uint16_t)cond_temp, else_label);
    lower_node(c, arena, node->right);
    ir_emit_jmp(c, end_label);
    ir_emit_label(c, else_label);
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(c, end_label);
    if (saved_temp_id > 32768 && c->lowering.vla.loop_depth == 0) {
        c->next_temp_id = saved_temp_id;
    }
}

void lower_stmt_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* Track loop depth for VLA hybrid rule enforcement */
    c->lowering.vla.loop_depth++;

    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_start);
    ir_emit_label(c, loop_start);
    int cond_temp = lower_condition_as_int(c, arena, node->left);
    ir_emit_je(c, (uint16_t)cond_temp, loop_exit);
    lower_node(c, arena, node->right);
    ir_emit_jmp(c, loop_start);
    ir_emit_label(c, loop_exit);
    label_patcher_exit_loop(&c->label_patcher);

    c->lowering.vla.loop_depth--;
}

void lower_stmt_do_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* Track loop depth for VLA hybrid rule enforcement */
    c->lowering.vla.loop_depth++;

    if (!node) return;
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_cond = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_cond);
    ir_emit_label(c, loop_start);
    lower_node(c, arena, node->right);
    ir_emit_label(c, loop_cond);
    int cond_temp = lower_condition_as_int(c, arena, node->left);
    ir_emit_jne(c, (uint16_t)cond_temp, loop_start);
    ir_emit_label(c, loop_exit);
    label_patcher_exit_loop(&c->label_patcher);

    c->lowering.vla.loop_depth--;
}

void lower_stmt_for(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    /* Track loop depth for VLA hybrid rule enforcement */
    c->lowering.vla.loop_depth++;

    if (!node) return;
    /* For-scope (C11 6.8.5): the init-declared loop variable is visible for
     * the whole for statement (init/cond/step/body).  Cap name lookup at the
     * symbol count recorded by the parser after the init, so a later sibling
     * for redeclaring the same name doesn't shadow this loop's header. */
    int32_t saved_for_bound = c->local_lookup_bound;
    if (node->aux.op != 0) c->local_lookup_bound = (int32_t)(node->aux.op - 1);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
    uint16_t loop_start = c->ir_next_label++;
    uint16_t loop_step = c->ir_next_label++;
    uint16_t loop_exit = c->ir_next_label++;
    label_patcher_enter_loop(&c->label_patcher, loop_exit, loop_step);
    ir_emit_label(c, loop_start);
    if (node->right != AST_NULL) {
        int cond_temp = lower_condition_as_int(c, arena, node->right);
        ir_emit_je(c, (uint16_t)cond_temp, loop_exit);
    }
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    ir_emit_label(c, loop_step);
    if (node->aux.step != AST_NULL) {
        lower_expr(c, arena, node->aux.step);
    }
    ir_emit_jmp(c, loop_start);
    ir_emit_label(c, loop_exit);
    label_patcher_exit_loop(&c->label_patcher);

    c->local_lookup_bound = saved_for_bound;
    c->lowering.vla.loop_depth--;
}

static void collect_switch_cases(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref, int cond_temp, uint16_t *default_label) {
    while (ref != AST_NULL) {
        ZeroCCASTNode *sn = zero_cc_ast_get(arena, ref);
        if (!sn) break;
        if (sn->tag == NODE_SWITCH) {
            /* Do not recurse into inner switch statements (their cases belong to the inner switch) */
            ref = sn->next;
            continue;
        }
        if (sn->tag == NODE_CASE) {
            sn->aux.alt = c->ir_next_label++;
            uint16_t case_label = (uint16_t)sn->aux.alt;
            if (sn->aux.step != 0) {
                /* Range case: low in sn->left, high in sn->aux.step */
                ZeroCCASTNode *low_n = zero_cc_ast_get(arena, sn->left);
                ZeroCCASTNode *high_n = zero_cc_ast_get(arena, (ASTRef)sn->aux.step);
                bool range_unsigned = zero_cc_binop_is_unsigned(c, arena, sn->left, (ASTRef)sn->aux.step) ||
                                      (low_n && high_n && low_n->tag == NODE_INT &&
                                       high_n->tag == NODE_INT &&
                                       low_n->int_val >= 0 && high_n->int_val < 0);
                int low_temp = lower_expr(c, arena, sn->left);
                int high_temp = lower_expr(c, arena, (ASTRef)sn->aux.step);
                int flag_low = c->next_temp_id++;
                ir_emit_cmp(c, cond_temp, low_temp);
                ir_emit_cond_set(c, flag_low, range_unsigned ? CC_FLT_GE : CC_GE);
                int flag_high = c->next_temp_id++;
                ir_emit_cmp(c, cond_temp, high_temp);
                ir_emit_cond_set(c, flag_high, range_unsigned ? CC_FLT_LE : CC_LE);
                int in_range = c->next_temp_id++;
                ir_emit_and(c, in_range, flag_low, flag_high);
                ir_emit_jne(c, (uint16_t)in_range, case_label);
            } else {
                int val_temp = lower_expr(c, arena, sn->left);
                int flag_temp = c->next_temp_id++;
                ir_emit_cmp(c, cond_temp, val_temp);
                ir_emit_cond_set(c, flag_temp, CC_EQ);
                /* JE = jump-if-zero, so take the case when the flag is non-zero */
                ir_emit_jne(c, (uint16_t)flag_temp, case_label);
            }
            if (sn->right != AST_NULL) {
                collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_DEFAULT) {
            sn->aux.alt = c->ir_next_label++;
            *default_label = (uint16_t)sn->aux.alt;
            if (sn->left != AST_NULL) {
                collect_switch_cases(c, arena, sn->left, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_BLOCK) {
            collect_switch_cases(c, arena, sn->left, cond_temp, default_label);
        } else if (sn->tag == NODE_IF) {
            collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
            if (sn->aux.alt != AST_NULL) {
                collect_switch_cases(c, arena, (ASTRef)sn->aux.alt, cond_temp, default_label);
            }
        } else if (sn->tag == NODE_WHILE || sn->tag == NODE_DO_WHILE || sn->tag == NODE_FOR) {
            collect_switch_cases(c, arena, sn->right, cond_temp, default_label);
        } else if (sn->tag == NODE_LABEL) {
            collect_switch_cases(c, arena, sn->left, cond_temp, default_label);
        }
        ref = sn->next;
    }
}

void lower_stmt_switch(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;

    uint16_t switch_exit = c->ir_next_label++;

    /* break inside a switch jumps to the switch exit */
    label_patcher_enter_loop(&c->label_patcher, switch_exit, 0);

    int cond_temp = lower_expr(c, arena, node->left);

    /* Dispatch: recursively scan the switch body for all case and default labels
     * (supporting Duff's device, cases nested in if/while/for/blocks).
     * C3 fix: no case cap - dispatch is O(n) CMP/JNE and the per-function
     * temp guard (C2b, 65534 temps) bounds the case count (~2 temps per
     * case -> ~32k cases), which stays under the 16-bit label space. */
    uint16_t default_label = 0;
    if (node->right != AST_NULL) {
        collect_switch_cases(c, arena, node->right, cond_temp, &default_label);
    }

    /* No matching case: jump to default, or skip the whole body.
     * The unconditional jump makes the body's start unreachable from the
     * dispatch path, so a non-matching value executes no body statement
     * (per C semantics). */
    if (default_label) {
        ir_emit_jmp(c, default_label);
    } else {
        ir_emit_jmp(c, switch_exit);
    }
    /* Fallthrough between case labels is automatic: each case's body
     * continues into the next statement until break/return. */

    lower_node(c, arena, node->right);

    ir_emit_label(c, switch_exit);
    label_patcher_exit_loop(&c->label_patcher);
}

void lower_stmt_break(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    uint16_t lbl = label_patcher_get_break_label(&c->label_patcher);
    if (lbl) {
        ir_emit_jmp(c, lbl);
    }
}

void lower_stmt_continue(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    uint16_t lbl = label_patcher_get_continue_label(&c->label_patcher);
    if (lbl) {
        ir_emit_jmp(c, lbl);
    }
}

uint16_t get_or_create_user_ir_label(ZeroCCCompiler *c, const char *name) {
    uint16_t lbl = label_patcher_find_or_create_user_label(&c->label_patcher, name);
    if (lbl == 0) {
        lbl = c->ir_next_label++;
        /* Update the label_patcher with the new label */
        for (int i = 0; i < c->label_patcher.user_label_count; i++) {
            if (strcmp(c->label_patcher.user_labels[i].name, name) == 0) {
                c->label_patcher.user_labels[i].ir_label = lbl;
                break;
            }
        }
    }
    return lbl;
}

void lower_stmt_label(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node || !node->s.str) return;
    uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
    ir_emit_label(c, lbl);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
}

void lower_stmt_goto(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->left != AST_NULL) {
        /* Computed goto: goto *expr; */
        int target_temp = lower_expr(c, arena, node->left);
        /* Computed goto in a nested function with nonlocal-goto pairs:
         * only targets in THIS function's frame are supported (plain
         * indirect jump, no frame restore) - matching GCC, which does not
         * support computed gotos to ancestor frames. */
        ir_emit_jmp_ind(c, target_temp);
        return;
    }
    if (!node->s.str) return;

    /* Sprint 99: nonlocal goto - the label is not in THIS function but in
     * an ANCESTOR (a __label__ variable of this function names a label of
     * the same name, so the name check covers it). Restore the target
     * scope's (rbp, rsp) from the hidden pair params, then jump. */
    ZeroCCFunction *cur = &c->functions[c->lowering.current_func_idx];
    if (cur->is_nested && cur->goto_pair_count > 0) {
        const char *nm = node->s.str;
        /* Local label wins (innermost scope): check this function's own
         * registered labels first. */
        bool own = false;
        for (int li = 0; li < cur->nl_label_count; li++) {
            if (cur->nl_labels[li] && strcmp(cur->nl_labels[li], nm) == 0) own = true;
        }
        if (!own) {
            /* Walk the ancestor chain: chain[0] = direct parent (innermost
             * ancestor), chain[chain_n-1] = outermost. Pair index i (0 =
             * outermost) therefore belongs to chain position
             * chain_n - 1 - i. */
            size_t chain[64];
            int chain_n = 0;
            for (size_t ai = cur->nested_enclosing_fn_idx;
                 ai != (size_t)-1 && chain_n < 64; ) {
                chain[chain_n++] = ai;
                if (!c->functions[ai].is_nested) break;
                ai = c->functions[ai].nested_enclosing_fn_idx;
            }
            for (int ci = 0; ci < chain_n; ci++) {
                size_t ai = chain[ci];
                ZeroCCFunction *af = &c->functions[ai];
                for (int li = 0; li < af->nl_label_count; li++) {
                    if (!af->nl_labels[li] || strcmp(af->nl_labels[li], nm) != 0) continue;
                    int pair = chain_n - 1 - ci;
                    char rn[48], sn[48];
                    snprintf(rn, sizeof(rn), "__nlgbp_%d", pair);
                    snprintf(sn, sizeof(sn), "__nlbsp_%d", pair);
                    ZeroCCSym *rb = zero_cc_find_local(c, rn);
                    ZeroCCSym *rs = zero_cc_find_local(c, sn);
                    if (rb && rs) {
                        int t1 = c->next_temp_id++;
                        zc_sym_load(c, rb, 0, t1);
                        int t2 = c->next_temp_id++;
                        zc_sym_load(c, rs, 0, t2);
                        ir_emit_nonlocal_goto(c, t1, t2, (int)ai,
                                              af->nl_label_ir[li]);
                        return;
                    }
                    break;
                }
            }
        }
    }

    uint16_t lbl = get_or_create_user_ir_label(c, node->s.str);
    ir_emit_jmp(c, lbl);
}

/* =========================================================================
 * Main Statement Dispatcher
 * ========================================================================= */

void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_BLOCK: {
            if (c->lowering.inlining.active_depth > 0) {
                lower_stmt_block(c, arena, node->left);
                break;
            }
            /* Block scope (C11 6.2.1): a statement in this list sees the
             * declarations made before it in this block plus everything in
             * enclosing scope - not the block's own declarations made after
             * it (a name redeclared later in a nested sibling block would
             * otherwise shadow earlier references in the flat latest-wins
             * table).  Start the list at the entry bound (aux.alt - 1, or
             * the enclosing bound when larger) and let each lowered
             * declaration raise it; on exit restore the entry bound - the
             * block's own declarations were marked scope-ended below, so
             * later outer statements skip them. */
            int32_t saved_bound = c->local_lookup_bound;
            int32_t entry_bound = (node->aux.alt != 0) ? (int32_t)(node->aux.alt - 1) : saved_bound;
            int32_t list_bound = (saved_bound < 0) ? entry_bound : ((entry_bound > saved_bound) ? entry_bound : saved_bound);
            c->local_lookup_bound = list_bound;
            lower_stmt_block(c, arena, node->left);
            c->local_lookup_bound = list_bound;
            /* C11 6.2.1 scope end: this block's own declarations (indices
             * [aux.alt-1, aux.step-2]) go out of scope at the closing brace.
             * The flat latest-wins table + single numeric bound cannot
             * express that for shadowed names (an outer-scope reference
             * before or after the block would still resolve to the inner
             * declaration), so mark them and let zero_cc_find_local skip
             * them.  Index-based access (lowering_local_sym, the prologue /
             * epilogue static-local mirrors) is unaffected. */
            if (node->aux.alt != 0 && node->aux.step != 0 && node->aux.step > node->aux.alt) {
                for (size_t si = node->aux.alt - 1; si + 1 < node->aux.step && si < c->local_count; si++) {
                    c->locals[si].scope_ended = true;
                }
            }
            break;
        }
        case NODE_EXPR_STMT:
            lower_stmt_expr(c, arena, node);
            break;
        case NODE_ASM:
            lower_asm_stmt(c, arena, node);
            break;
        case NODE_DECL:
            /* If this is a function definition (has body in right), lower the body */
            if (!(node->flags & NODE_DECL_IS_VLA) && node->right != AST_NULL) {
                lower_stmt_block(c, arena, node->right);
            } else {
                /* Make this declaration's own symbol visible while lowering
                 * its initializer: self-referential aggregate initializers
                 * like `struct xx s = {0, &s, 23};` must resolve `&s` to
                 * the NEW object (C99 6.7.9), and `int x = x;` reads the
                 * new indeterminate object, not a shadowed outer x.  The
                 * bound is raised again after the lowering regardless. */
                int32_t pre_bound = c->local_lookup_bound;
                if (pre_bound >= 0 && node->aux.step != 0 &&
                    (int32_t)node->aux.step > pre_bound) {
                    c->local_lookup_bound = (int32_t)node->aux.step;
                }
                lower_stmt_decl(c, arena, node);
                if (c->local_lookup_bound >= 0 && node->aux.step != 0) {
                    if ((int32_t)node->aux.step > c->local_lookup_bound) {
                        c->local_lookup_bound = (int32_t)node->aux.step;
                    }
                }
            }
            break;
        case NODE_STATIC_ASSERT:
            lower_stmt_static_assert(c, arena, node);
            break;
        case NODE_RETURN:
            lower_stmt_return(c, arena, node);
            break;
        case NODE_IF:
            lower_stmt_if(c, arena, node);
            break;
        case NODE_WHILE:
            lower_stmt_while(c, arena, node);
            break;
        case NODE_DO_WHILE:
            lower_stmt_do_while(c, arena, node);
            break;
        case NODE_FOR:
            lower_stmt_for(c, arena, node);
            break;
        case NODE_SWITCH:
            lower_stmt_switch(c, arena, node);
            break;
        case NODE_CASE:
            if (node->aux.alt) ir_emit_label(c, (uint16_t)node->aux.alt);
            if (node->right != AST_NULL) lower_node(c, arena, node->right);
            break;
        case NODE_DEFAULT:
            if (node->aux.alt) ir_emit_label(c, (uint16_t)node->aux.alt);
            if (node->left != AST_NULL) lower_node(c, arena, node->left);
            break;
        case NODE_BREAK:
            lower_stmt_break(c, arena);
            break;
        case NODE_CONTINUE:
            lower_stmt_continue(c, arena);
            break;
        case NODE_LABEL:
            lower_stmt_label(c, arena, node);
            break;
        case NODE_GOTO:
            lower_stmt_goto(c, arena, node);
            break;
        default:
            break;
    }
}

