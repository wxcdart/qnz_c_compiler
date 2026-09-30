/* AST Lowering - AST to IR/Machine Code */
#ifndef ZERO_CC_PARSER_AST_LOWERING_H
#define ZERO_CC_PARSER_AST_LOWERING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../parser.h"
#include "regalloc.h"
#include "ir.h"
#include "machine_emitter.h"
#include "../types/global_vars.h"

/* =========================================================================
 * Temporary Variable Tracking
 * ========================================================================= */

typedef struct {
    int temp_id;
    size_t live_start;
    size_t live_end;
    RegClass reg_class;
    int assigned_reg;
    int spill_slot;
} TempVar;

static TempVar temp_vars[256];
static size_t temp_var_count = 0;

static int alloc_temp(ZeroCCCompiler *c, RegClass reg_class) {
    int temp_id = c->next_temp_id++;
    if (temp_var_count < 256) {
        TempVar *tv = &temp_vars[temp_var_count++];
        tv->temp_id = temp_id;
        tv->live_start = me_get_offset(c);
        tv->live_end = 0;
        tv->reg_class = reg_class;
        tv->assigned_reg = -1;
        tv->spill_slot = -1;
    }
    return temp_id;
}

static int get_temp_reg(int temp_id) __attribute__((unused));
static int get_temp_reg(int temp_id) {
    for (size_t i = 0; i < temp_var_count; i++) {
        if (temp_vars[i].temp_id == temp_id) {
            return temp_vars[i].assigned_reg;
        }
    }
    return -1;
}

static bool is_temp_spilled(int temp_id) __attribute__((unused));
static bool is_temp_spilled(int temp_id) {
    for (size_t i = 0; i < temp_var_count; i++) {
        if (temp_vars[i].temp_id == temp_id) {
            return temp_vars[i].assigned_reg < 0;
        }
    }
    return true;
}

/* =========================================================================
 * Loop Control Flow Support
 * ========================================================================= */

#define MAX_LOOP_NESTING 16
#define MAX_LOOP_PATCHES 64

static size_t break_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int break_patch_count[MAX_LOOP_NESTING];
static size_t continue_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int continue_patch_count[MAX_LOOP_NESTING];
static size_t loop_start_pos[MAX_LOOP_NESTING];
static int loop_depth = 0;

/* =========================================================================
 * Label/Goto Support
 * ========================================================================= */

#define MAX_LABELS 256
#define MAX_LABEL_NAME 64

static struct {
    char name[MAX_LABEL_NAME];
    size_t position;
} label_table[MAX_LABELS];

static int label_count = 0;
static size_t* goto_patch_list[MAX_LABELS];
static int goto_patch_count[MAX_LABELS];
static int goto_patch_list_count = 0;

static size_t epilogue_patches[MAX_EPILOGUE_PATCHES];
static int epilogue_patch_count = 0;

static void reset_label_state(void) {
    label_count = 0;
    goto_patch_list_count = 0;
    for (int i = 0; i < MAX_LABELS; i++) {
        goto_patch_count[i] = 0;
    }
}

static int find_label(const char *name) {
    for (int i = 0; i < label_count; i++) {
        if (strcmp(label_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_label(const char *name, size_t position) {
    if (label_count < MAX_LABELS) {
        strncpy(label_table[label_count].name, name, MAX_LABEL_NAME - 1);
        label_table[label_count].name[MAX_LABEL_NAME - 1] = '\0';
        label_table[label_count].position = position;
        return label_count++;
    }
    return -1;
}

static void patch_goto(size_t label_idx) {
    if (label_idx < (size_t)MAX_LABELS && goto_patch_count[label_idx] > 0) {
        for (int i = 0; i < goto_patch_count[label_idx]; i++) {
            size_t patch_pos = goto_patch_list[label_idx][i];
            int32_t rel = (int32_t)(label_table[label_idx].position - (patch_pos + 4));
            memcpy((uint8_t*)patch_pos, &rel, 4);
        }
    }
}

/* =========================================================================
 * Forward Declarations
 * ========================================================================= */

static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
static void lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);

/* =========================================================================
 * Statement Handlers (each under 24 lines)
 * ========================================================================= */

static void lower_stmt_block(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    ASTRef stmt = ref;
    while (stmt != AST_NULL) {
        lower_node(c, arena, stmt);
        ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
        stmt = snode ? snode->next : AST_NULL;
    }
}

static void lower_stmt_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        lower_expr(c, arena, node->left);
    }
}

static void lower_stmt_decl(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->left != AST_NULL) {
        lower_expr(c, arena, node->left);
        const char *var = node->s.str;
        ZeroCCSym *sym = zero_cc_find_local(c, var);
        if (sym) {
            me_mov_mem_rax(c, sym->stack_offset);
        }
    }
}

static void lower_stmt_static_assert(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        lower_expr(c, arena, node->left);
    }
}

static void lower_stmt_return(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node && node->left != AST_NULL) {
        lower_expr(c, arena, node->left);
    }
    me_emit_u8(c, 0xE9);
    size_t patch_pos = me_get_offset(c);
    me_emit_u32(c, 0);
    if (epilogue_patch_count < MAX_EPILOGUE_PATCHES) {
        epilogue_patches[epilogue_patch_count++] = patch_pos;
    }
}

static void lower_stmt_if(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    lower_expr(c, arena, node->left);
    me_test_rax_rax(c);
    size_t else_patch = me_emit_je_rel32(c);
    lower_node(c, arena, node->right);
    if (node->aux.alt != AST_NULL) {
        size_t end_patch = me_emit_jmp_rel32(c);
        me_patch_jmp_rel32(c, else_patch, me_get_offset(c));
        lower_node(c, arena, node->aux.alt);
        me_patch_jmp_rel32(c, end_patch, me_get_offset(c));
    } else {
        me_patch_jmp_rel32(c, else_patch, me_get_offset(c));
    }
}

static void lower_stmt_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    size_t loop_start = me_get_offset(c);
    loop_start_pos[loop_depth] = loop_start;
    break_patch_count[loop_depth] = 0;
    continue_patch_count[loop_depth] = 0;
    lower_expr(c, arena, node->left);
    me_test_rax_rax(c);
    size_t exit_patch = me_emit_je_rel32(c);
    loop_depth++;
    lower_node(c, arena, node->right);
    loop_depth--;
    size_t back_patch = me_emit_jmp_rel32(c);
    me_patch_jmp_rel32(c, exit_patch, me_get_offset(c));
    me_patch_jmp_rel32(c, back_patch, loop_start);
}

static void lower_stmt_do_while(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    size_t loop_start = me_get_offset(c);
    loop_start_pos[loop_depth] = loop_start;
    break_patch_count[loop_depth] = 0;
    continue_patch_count[loop_depth] = 0;
    loop_depth++;
    lower_node(c, arena, node->right);
    loop_depth--;
    lower_expr(c, arena, node->left);
    me_test_rax_rax(c);
    size_t back_patch = me_emit_jne_rel32(c);
    me_patch_jmp_rel32(c, back_patch, loop_start);
}

static void lower_stmt_for(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
    size_t loop_start = me_get_offset(c);
    loop_start_pos[loop_depth] = loop_start;
    break_patch_count[loop_depth] = 0;
    continue_patch_count[loop_depth] = 0;
    if (node->right != AST_NULL) {
        lower_expr(c, arena, node->right);
    } else {
        me_xor_rax_rax(c);
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x83);
        me_emit_u8(c, 0xC0);
        me_emit_u8(c, 0x00);
    }
    me_test_rax_rax(c);
    size_t exit_patch = me_emit_je_rel32(c);
    loop_depth++;
    if (node->aux.alt != AST_NULL) {
        lower_node(c, arena, node->aux.alt);
    }
    if (node->aux.step != AST_NULL) {
        lower_expr(c, arena, node->aux.step);
    }
    size_t back_patch = me_emit_jmp_rel32(c);
    me_patch_jmp_rel32(c, back_patch, loop_start);
    loop_depth--;
    me_patch_jmp_rel32(c, exit_patch, me_get_offset(c));
}

static void lower_stmt_switch(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (node) {
        lower_expr(c, arena, node->left);
    }
    /* TODO: Implement switch statement */
}

static void lower_stmt_break(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    if (loop_depth > 0) {
        size_t patch __attribute__((unused)) = me_emit_jmp_rel32(c);
        if (break_patch_count[loop_depth - 1] < MAX_LOOP_PATCHES) {
            break_patch_pos[loop_depth - 1][break_patch_count[loop_depth - 1]++] = patch;
        }
    }
}

static void lower_stmt_continue(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused))) {
    if (loop_depth > 0) {
        size_t patch __attribute__((unused)) = me_emit_jmp_rel32(c);
        if (continue_patch_count[loop_depth - 1] < MAX_LOOP_PATCHES) {
            continue_patch_pos[loop_depth - 1][continue_patch_count[loop_depth - 1]++] = patch;
        }
    }
}

static void lower_stmt_label(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return;
    int idx = find_label(node->s.str);
    if (idx < 0) {
        idx = add_label(node->s.str, me_get_offset(c));
    } else {
        label_table[idx].position = me_get_offset(c);
    }
    patch_goto(idx);
    if (node->left != AST_NULL) {
        lower_node(c, arena, node->left);
    }
}

static void lower_stmt_goto(ZeroCCCompiler *c, ZeroCCASTArena *arena __attribute__((unused)), ZeroCCASTNode *node) {
    if (!node) return;
    int idx = find_label(node->s.str);
    if (idx >= 0) {
        size_t patch __attribute__((unused)) = me_emit_jmp_rel32(c);
        me_patch_jmp_rel32(c, patch, label_table[idx].position);
    } else {
        size_t patch __attribute__((unused)) = me_emit_jmp_rel32(c);
        /* TODO: Proper goto patching */
    }
}

/* =========================================================================
 * Main Statement Dispatcher
 * ========================================================================= */

static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_BLOCK:
            lower_stmt_block(c, arena, node->left);
            break;
        case NODE_EXPR_STMT:
            lower_stmt_expr(c, arena, node);
            break;
        case NODE_DECL:
            lower_stmt_decl(c, arena, node);
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
        case NODE_DEFAULT:
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

/* =========================================================================
 * Expression Lowering
 * ========================================================================= */

static void lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_INT:
            me_mov_rax_imm(c, node->int_val);
            break;
        case NODE_FLOAT: {
            double val = node->float_val;
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, (const uint8_t*)&val, sizeof(val));
            me_emit_u8(c, 0xF2);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0x10);
            me_emit_u8(c, 0x05);
            size_t patch_pos = me_get_offset(c);
            me_emit_u32(c, 0);
            if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
                c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
                c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
                c->str_reloc_count++;
            }
            break;
        }
        case NODE_BOOL:
            me_mov_rax_imm(c, node->int_val);
            break;
        case NODE_NULLPTR:
            me_xor_rax_rax(c);
            break;
        case NODE_STRING: {
            if (node->s.str) {
                size_t len = strlen(node->s.str);
                size_t rodata_offset = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                uint8_t zero = 0;
                zero_cc_buf_put_bytes(&c->rodata_section, &zero, 1);
                me_emit_u8(c, 0x48);
                me_emit_u8(c, 0x8D);
                me_emit_u8(c, 0x05);
                size_t patch_pos = me_get_offset(c);
                me_emit_u32(c, 0);
                if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
                    c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
                    c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
                    c->str_reloc_count++;
                }
            }
            break;
        }
        case NODE_IDENT: {
            const char *name = node->s.str;
            ZeroCCSym *sym = zero_cc_find_local(c, name);
            if (sym) {
                me_mov_rax_mem(c, sym->stack_offset);
            } else {
                ZeroCCGlobalVar *gv = find_global_var(name);
                if (gv) {
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0xB8);
                    me_emit_u64(c, 0);
                    size_t patch_pos = me_get_offset(c) - 8;
                    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
                        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                        r->text_patch_offset = patch_pos;
                        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                        r->is_call = false;
                    }
                }
            }
            break;
        }
        case NODE_BINOP: {
            alloc_temp(c, REG_CLASS_GPR); // unused for now
            lower_expr(c, arena, node->left);
            me_push_rax(c);
            alloc_temp(c, REG_CLASS_GPR); // unused for now
            lower_expr(c, arena, node->right);
            me_pop_rcx(c);
            switch (node->aux.op) {
                case TK_PLUS:
                    me_add_rax_rcx(c);
                    break;
                case TK_MINUS:
                    me_sub_rcx_rax(c);
                    me_mov_rax_rcx(c);
                    break;
                case TK_STAR:
                    me_imul_rax_rcx(c);
                    break;
                case TK_SLASH: {
                    me_push_rbx(c);
                    me_mov_rbx_rax(c);
                    me_mov_rax_rcx(c);
                    me_cqo(c);
                    me_idiv_rbx(c);
                    me_pop_rbx(c);
                    break;
                }
                case TK_PERCENT: {
                    me_push_rbx(c);
                    me_mov_rbx_rax(c);
                    me_mov_rax_rcx(c);
                    me_cqo(c);
                    me_idiv_rbx(c);
                    me_mov_rax_rdx(c);
                    me_pop_rbx(c);
                    break;
                }
                case TK_AMP:
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0x21);
                    me_emit_u8(c, 0xC8);
                    break;
                case TK_PIPE:
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0x09);
                    me_emit_u8(c, 0xC8);
                    break;
                case TK_CARET:
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0x31);
                    me_emit_u8(c, 0xC8);
                    break;
                case TK_SHL:
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0x89);
                    me_emit_u8(c, 0xC1);
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0xD3);
                    me_emit_u8(c, 0xE0);
                    break;
                case TK_SHR:
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0x89);
                    me_emit_u8(c, 0xC1);
                    me_emit_u8(c, 0x48);
                    me_emit_u8(c, 0xD3);
                    me_emit_u8(c, 0xE8);
                    break;
                case TK_EQ:
                    me_cmp_rcx_rax(c);
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x94);
                    me_emit_u8(c, 0xC0);
                    break;
                case TK_NE:
                    me_cmp_rcx_rax(c);
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x95);
                    me_emit_u8(c, 0xC0);
                    break;
                case TK_LT:
                    me_cmp_rcx_rax(c);
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x92);
                    me_emit_u8(c, 0xC0);
                    break;
                case TK_LE:
                    me_cmp_rcx_rax(c);
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x96);
                    me_emit_u8(c, 0xC0);
                    break;
                case TK_GT:
                    me_cmp_rcx_rax(c);
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x97);
                    me_emit_u8(c, 0xC0);
                    break;
                case TK_GE:
                    me_cmp_rcx_rax(c);
                    me_emit_u8(c, 0x0F);
                    me_emit_u8(c, 0x93);
                    me_emit_u8(c, 0xC0);
                    break;
                default:
                    break;
            }
            break;
        }
        case NODE_UNOP: {
            if (node->aux.op == TK_INC || node->aux.op == TK_DEC) {
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        me_emit_u8(c, 0x48);
                        me_emit_u8(c, 0x8B);
                        me_emit_u8(c, 0x85);
                        me_emit_u32(c, (uint32_t)(-sym->stack_offset));
                        if (node->aux.op == TK_INC) {
                            me_emit_u8(c, 0x48);
                            me_emit_u8(c, 0xFF);
                            me_emit_u8(c, 0xC0);
                        } else {
                            me_emit_u8(c, 0x48);
                            me_emit_u8(c, 0xFF);
                            me_emit_u8(c, 0xC8);
                        }
                        me_emit_u8(c, 0x48);
                        me_emit_u8(c, 0x89);
                        me_emit_u8(c, 0x85);
                        me_emit_u32(c, (uint32_t)(-sym->stack_offset));
                    }
                } else {
                    lower_expr(c, arena, node->left);
                    if (node->aux.op == TK_INC) {
                        me_emit_u8(c, 0xFF);
                        me_emit_u8(c, 0xC0);
                    } else {
                        me_emit_u8(c, 0xFF);
                        me_emit_u8(c, 0xC8);
                    }
                }
            } else {
                lower_expr(c, arena, node->left);
                switch (node->aux.op) {
                    case TK_STAR:
                        me_mov_rax_mem(c, 0);
                        break;
                    case TK_MINUS:
                        me_emit_u8(c, 0x48);
                        me_emit_u8(c, 0xF7);
                        me_emit_u8(c, 0xD8);
                        break;
                    case TK_BANG:
                        me_test_rax_rax(c);
                        me_emit_u8(c, 0x0F);
                        me_emit_u8(c, 0x94);
                        me_emit_u8(c, 0xC0);
                        break;
                    case TK_TILDE:
                        me_emit_u8(c, 0x48);
                        me_emit_u8(c, 0xF7);
                        me_emit_u8(c, 0xD8);
                        break;
                    default:
                        break;
                }
            }
            break;
        }
        case NODE_ASSIGN: {
            lower_expr(c, arena, node->right);
            ZeroCCASTNode *left_node = zero_cc_ast_get(arena, node->left);
            const char *var = left_node ? left_node->s.str : "";
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym) {
                me_mov_mem_rax(c, sym->stack_offset);
            }
            break;
        }
        case NODE_TERNARY: {
            lower_expr(c, arena, node->left);
            me_test_rax_rax(c);
            size_t else_patch = me_emit_je_rel32(c);
            lower_expr(c, arena, node->right);
            size_t end_patch = me_emit_jmp_rel32(c);
            me_patch_jmp_rel32(c, else_patch, me_get_offset(c));
            lower_expr(c, arena, node->aux.alt);
            me_patch_jmp_rel32(c, end_patch, me_get_offset(c));
            break;
        }
        case NODE_COMPOUND_LIT:
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            break;
        case NODE_INDEX: {
            lower_expr(c, arena, node->left);
            me_emit_u8(c, 0x48);
            me_emit_u8(c, 0x89);
            me_emit_u8(c, 0xC2);
            lower_expr(c, arena, node->right);
            me_emit_u8(c, 0x48);
            me_emit_u8(c, 0x01);
            me_emit_u8(c, 0xD0);
            me_emit_u8(c, 0x48);
            me_emit_u8(c, 0x0F);
            me_emit_u8(c, 0xB6);
            me_emit_u8(c, 0x00);
            break;
        }
        case NODE_CALL: {
            int temp __attribute__((unused)) = alloc_temp(c, REG_CLASS_GPR);
            ASTRef arg = node->right;
            int arg_count = 0;
            while (arg != AST_NULL) {
                lower_expr(c, arena, arg);
                me_emit_u8(c, 0x50);
                arg_count++;
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                arg = arg_node ? arg_node->next : AST_NULL;
            }
            static const uint8_t arg_regs[6][3] = {
                {0x48, 0x89, 0xC7}, {0x48, 0x89, 0xC6}, {0x48, 0x89, 0xC2},
                {0x48, 0x89, 0xC1}, {0x49, 0x89, 0xC0}, {0x49, 0x89, 0xC1}
            };
            int reg_args = (arg_count > 6) ? 6 : arg_count;
            for (int i = reg_args - 1; i >= 0; i--) {
                me_emit_u8(c, 0x58);
                me_emit_u8(c, arg_regs[i][0]);
                me_emit_u8(c, arg_regs[i][1]);
                me_emit_u8(c, arg_regs[i][2]);
            }
            me_emit_u8(c, 0x31);
            me_emit_u8(c, 0xC0);
            ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
            if (callee && callee->tag == NODE_IDENT) {
                const char *name = callee->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, name);
                if (sym) {
                    me_emit_u8(c, 0x4C);
                    me_emit_u8(c, 0x8B);
                    me_emit_u8(c, 0x9D);
                    me_emit_u32(c, (uint32_t)(-sym->stack_offset));
                    me_emit_u8(c, 0x41);
                    me_emit_u8(c, 0xFF);
                    me_emit_u8(c, 0xD3);
                } else {
                    ZeroCCFunction *fn = zero_cc_find_function(c, name);
                    void *host_fn = zero_cc_resolve_symbol(name);
                    if (fn && !fn->is_prototype) {
                        me_emit_u8(c, 0xE8);
                        size_t patch_pos = me_get_offset(c);
                        int32_t rel = (int32_t)(fn->text_offset - (patch_pos + 4));
                        me_emit_u32(c, (uint32_t)rel);
                        if (c->reloc_count < 512) {
                            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                            r->text_patch_offset = patch_pos;
                            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                            r->is_call = true;
                        }
                    } else if (host_fn) {
                        me_emit_u8(c, 0x49);
                        me_emit_u8(c, 0xBB);
                        me_emit_u64(c, (uint64_t)host_fn);
                        me_emit_u8(c, 0x41);
                        me_emit_u8(c, 0xFF);
                        me_emit_u8(c, 0xD3);
                    } else if (fn && fn->is_prototype) {
                        me_emit_u8(c, 0x49);
                        me_emit_u8(c, 0xBB);
                        me_emit_u64(c, 0);
                        me_emit_u8(c, 0x41);
                        me_emit_u8(c, 0xFF);
                        me_emit_u8(c, 0xD3);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    epilogue_patch_count = 0;
    reset_label_state();
    temp_var_count = 0;
    lower_node(c, arena, root);
    size_t end_pos = me_get_offset(c);
    for (size_t i = 0; i < temp_var_count; i++) {
        if (temp_vars[i].live_end == 0) {
            temp_vars[i].live_end = end_pos;
        }
    }
    for (size_t i = 0; i < temp_var_count; i++) {
        regalloc_add_interval(&c->regalloc, temp_vars[i].temp_id, 
                              temp_vars[i].live_start, temp_vars[i].live_end,
                              temp_vars[i].reg_class);
    }
    regalloc_allocate(&c->regalloc);
    size_t epilogue_target = me_get_offset(c);
    for (int i = 0; i < epilogue_patch_count; i++) {
        size_t patch_pos = epilogue_patches[i];
        int32_t rel = (int32_t)(epilogue_target - (patch_pos + 4));
        me_patch_u32(c, patch_pos, (uint32_t)rel);
    }
    epilogue_patch_count = 0;
    me_emit_leave(c);
    me_emit_ret(c);
    temp_var_count = 0;
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_H */
