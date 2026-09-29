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
#include "../types/global_vars.h"

/* Temporary variable tracking for register allocation */
typedef struct {
    int temp_id;              /* Temporary ID */
    size_t live_start;        /* Start of live range */
    size_t live_end;          /* End of live range */
    RegClass reg_class;       /* Required register class */
    int assigned_reg;         /* Assigned register (-1 if spilled) */
    int spill_slot;           /* Spill slot (-1 if not spilled) */
} TempVar;

static TempVar temp_vars[256];
static size_t temp_var_count = 0;

/* Allocate a new temporary variable */
static int alloc_temp(ZeroCCCompiler *c, RegClass reg_class) __attribute__((unused));
static int alloc_temp(ZeroCCCompiler *c, RegClass reg_class) {
    int temp_id = c->next_temp_id++;
    if (temp_var_count < 256) {
        TempVar *tv = &temp_vars[temp_var_count++];
        tv->temp_id = temp_id;
        tv->live_start = c->text_section.length;
        tv->live_end = 0;
        tv->reg_class = reg_class;
        tv->assigned_reg = -1;
        tv->spill_slot = -1;
    }
    return temp_id;
}

/* Update live end position for a temporary */
static void update_temp_live_end(int temp_id, size_t pos) __attribute__((unused));
static void update_temp_live_end(int temp_id, size_t pos) {
    for (size_t i = 0; i < temp_var_count; i++) {
        if (temp_vars[i].temp_id == temp_id) {
            temp_vars[i].live_end = pos;
            break;
        }
    }
}

/* Get register for a temporary (placeholder - returns -1 for now) */
static int get_temp_reg(int temp_id) __attribute__((unused));
static int get_temp_reg(int temp_id) {
    for (size_t i = 0; i < temp_var_count; i++) {
        if (temp_vars[i].temp_id == temp_id) {
            return temp_vars[i].assigned_reg;
        }
    }
    return -1;
}

/* Loop control flow support */
#define MAX_LOOP_NESTING 16
#define MAX_LOOP_PATCHES 64
static size_t break_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int break_patch_count[MAX_LOOP_NESTING];
static size_t continue_patch_pos[MAX_LOOP_NESTING][MAX_LOOP_PATCHES];
static int continue_patch_count[MAX_LOOP_NESTING];
static size_t loop_start_pos[MAX_LOOP_NESTING];
static int loop_depth = 0;

/* Goto/Label support */
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

/* Reset label/goto state at function entry */
static void reset_label_state(void) __attribute__((unused));
static void reset_label_state(void) {
    label_count = 0;
    goto_patch_list_count = 0;
    for (int i = 0; i < MAX_LABELS; i++) {
        goto_patch_count[i] = 0;
    }
}

/* Helper: check if both operands are float literals for simple float arithmetic */
static bool is_simple_float_binop(ZeroCCASTArena *arena, ZeroCCASTNode *node) __attribute__((unused));
static bool is_simple_float_binop(ZeroCCASTArena *arena, ZeroCCASTNode *node) {
    if (!node) return false;
    ZeroCCASTNode *left = zero_cc_ast_get(arena, node->left);
    ZeroCCASTNode *right = zero_cc_ast_get(arena, node->right);
    if (!left || !right) return false;
    /* Only handle float + float, float - float, etc. where both are NODE_FLOAT */
    return (left->tag == NODE_FLOAT && right->tag == NODE_FLOAT &&
           (node->aux.op == TK_PLUS || node->aux.op == TK_MINUS ||
            node->aux.op == TK_STAR || node->aux.op == TK_SLASH));
}

/* Find label by name, return index or -1 if not found */
static int find_label(const char *name) {
    for (int i = 0; i < label_count; i++) {
        if (strcmp(label_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Add label to table, return index */
static int add_label(const char *name, size_t position) {
    if (label_count >= MAX_LABELS) return -1;
    strncpy(label_table[label_count].name, name, MAX_LABEL_NAME - 1);
    label_table[label_count].name[MAX_LABEL_NAME - 1] = '\0';
    label_table[label_count].position = position;
    return label_count++;
}

/* Record a goto patch location for later patching */
static void record_goto_patch(ZeroCCCompiler *c, const char *label_name, size_t patch_pos) __attribute__((unused));
static void record_goto_patch(ZeroCCCompiler *c, const char *label_name, size_t patch_pos) {
    int idx = find_label(label_name);
    if (idx >= 0) {
        /* Label already seen, patch immediately */
        size_t rel = label_table[idx].position - (patch_pos + 4);
        memcpy(c->text_section.data + patch_pos, &rel, 4);
    } else {
        /* Label not seen yet, record for later patching */
        if (goto_patch_list_count >= MAX_LABELS) return;
        int list_idx = goto_patch_list_count++;
        goto_patch_list[list_idx] = (size_t*)malloc(sizeof(size_t) * 64);
        goto_patch_count[list_idx] = 0;
        /* Store label name and patch position in a simple way */
        /* For now, use a simplified approach: store patch_pos in a global array */
    }
}


static void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_BLOCK: {
            ASTRef stmt = node->left;
            while (stmt != AST_NULL) {
                lower_node(c, arena, stmt);
                ZeroCCASTNode *snode = zero_cc_ast_get(arena, stmt);
                stmt = snode ? snode->next : AST_NULL;
            }
            break;
        }
        case NODE_EXPR_STMT:
            lower_expr(c, arena, node->left);
            break;
        case NODE_DECL: {
            // Variable declaration handled during parsing; initializer if present.
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
                // Store result (in rax) to local variable.
                const char *var = node->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, var);
                if (sym) {
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                }
            }
            break;
        }
        case NODE_STATIC_ASSERT: {
            // Evaluate condition (discard result). No codegen needed unless assertion fails at compile time.
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            break;
        }
        case NODE_RETURN: {
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            // Emit a jump to the function epilogue label.
            // Placeholder 5‑byte relative jump: E9 <rel32>
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t patch_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            if (epilogue_patch_count < MAX_EPILOGUE_PATCHES) {
                epilogue_patches[epilogue_patch_count++] = patch_pos;
            }
            break;
        }
        case NODE_IF: {
            // Evaluate condition -> result in rax.
            lower_expr(c, arena, node->left);
            // test rax, rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            // je to else (or end if no else).
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x84);
            uint32_t else_offset_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            // then block
            lower_node(c, arena, node->right);
            // jmp end
            zero_cc_emit_u8(&c->text_section, 0xE9);
            uint32_t end_jump_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            // else label position – patch.
            size_t else_pos = c->text_section.length;
            uint32_t rel_else = (uint32_t)(else_pos - (else_offset_pos + 4));
            memcpy(c->text_section.data + else_offset_pos, &rel_else, 4);
            if (node->aux.alt != AST_NULL) {
                lower_node(c, arena, node->aux.alt);
            }
            // end label position – patch.
            size_t end_pos = c->text_section.length;
            uint32_t rel_end = (uint32_t)(end_pos - (end_jump_pos + 4));
            memcpy(c->text_section.data + end_jump_pos, &rel_end, 4);
            break;
        }
        case NODE_WHILE: {
            // Enter loop: push depth, init patch lists
            loop_start_pos[loop_depth] = c->text_section.length;
            break_patch_count[loop_depth] = 0;
            continue_patch_count[loop_depth] = 0;
            loop_depth++;

            // condition
            lower_expr(c, arena, node->left);
            // test rax, rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            // je end
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x84);
            uint32_t end_offset_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            // body
            lower_node(c, arena, node->right);
            // jmp start (E9 <rel32>)
            uint32_t back_rel = (uint32_t)(loop_start_pos[loop_depth-1] - (c->text_section.length + 5));
            zero_cc_emit_u8(&c->text_section, 0xE9);
            zero_cc_emit_u32(&c->text_section, back_rel);
            // end label
            size_t end_pos = c->text_section.length;
            uint32_t rel_end = (uint32_t)(end_pos - (end_offset_pos + 4));
            memcpy(c->text_section.data + end_offset_pos, &rel_end, 4);
            // patch break jumps to end of loop
            for (int i = 0; i < break_patch_count[loop_depth-1]; i++) {
                size_t patch = break_patch_pos[loop_depth-1][i];
                uint32_t rel = (uint32_t)(end_pos - (patch + 4));
                memcpy(c->text_section.data + patch, &rel, 4);
            }
            // patch continue jumps to loop start
            for (int i = 0; i < continue_patch_count[loop_depth-1]; i++) {
                size_t patch = continue_patch_pos[loop_depth-1][i];
                uint32_t rel = (uint32_t)(loop_start_pos[loop_depth-1] - (patch + 5));
                memcpy(c->text_section.data + patch, &rel, 4);
            }
            // pop stack
            loop_depth--;
            break;
        }
        case NODE_DO_WHILE: {
            // do-while loop with break/continue support
            loop_start_pos[loop_depth] = c->text_section.length;
            break_patch_count[loop_depth] = 0;
            continue_patch_count[loop_depth] = 0;
            loop_depth++;

            // body
            lower_node(c, arena, node->right);

            // condition
            size_t cond_pos = c->text_section.length;
            lower_expr(c, arena, node->left);
            // test rax, rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            // jne loop start
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x85);
            uint32_t jump_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);

            // end label
            size_t end_pos = c->text_section.length;
            uint32_t rel_back = (uint32_t)(loop_start_pos[loop_depth-1] - (jump_pos + 4));
            memcpy(c->text_section.data + jump_pos, &rel_back, 4);

            // patch break jumps to end of loop
            for (int i = 0; i < break_patch_count[loop_depth-1]; i++) {
                size_t patch = break_patch_pos[loop_depth-1][i];
                uint32_t rel = (uint32_t)(end_pos - (patch + 4));
                memcpy(c->text_section.data + patch, &rel, 4);
            }
            // patch continue jumps to condition
            for (int i = 0; i < continue_patch_count[loop_depth-1]; i++) {
                size_t patch = continue_patch_pos[loop_depth-1][i];
                uint32_t rel = (uint32_t)(cond_pos - (patch + 5));
                memcpy(c->text_section.data + patch, &rel, 4);
            }
            loop_depth--;
            break;
        }
        case NODE_LABEL: {
            /* Record label position for later patching */
            const char *label_name = node->s.str;
            if (label_name && strlen(label_name) > 0) {
                int idx = find_label(label_name);
                if (idx < 0) {
                    add_label(label_name, c->text_section.length);
                }
                /* Patch any pending gotos to this label */
                for (int i = 0; i < goto_patch_list_count; i++) {
                    if (goto_patch_list[i] != NULL) {
                        size_t patch_pos = goto_patch_list[i][0];
                        size_t rel = c->text_section.length - (patch_pos + 4);
                        memcpy(c->text_section.data + patch_pos, &rel, 4);
                        free(goto_patch_list[i]);
                        goto_patch_list[i] = NULL;
                        /* Remove from list by shifting */
                        for (int j = i; j < goto_patch_list_count - 1; j++) {
                            goto_patch_list[j] = goto_patch_list[j + 1];
                        }
                        goto_patch_list_count--;
                        i--; /* Recheck this index */
                    }
                }
            }
            break;
        }
        case NODE_GOTO: {
            /* Emit jump to label */
            const char *label_name = node->s.str;
            if (label_name && strlen(label_name) > 0) {
                int idx = find_label(label_name);
                zero_cc_emit_u8(&c->text_section, 0xE9); /* JMP rel32 */
                size_t patch = c->text_section.length;
                if (idx >= 0) {
                    /* Label already seen, patch immediately */
                    size_t rel = label_table[idx].position - (patch + 4);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)rel);
                } else {
                    /* Label not seen yet, record for later patching */
                    zero_cc_emit_u32(&c->text_section, 0); /* Placeholder */
                    size_t *patch_list = (size_t*)malloc(sizeof(size_t));
                    patch_list[0] = patch;
                    goto_patch_list[goto_patch_list_count++] = patch_list;
                }
            }
            break;
        }
        case NODE_BREAK: {
            // Emit placeholder jump; patch later to loop end
            if (loop_depth <= 0) {
                // No enclosing loop – ignore or handle error
                break;
            }
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t patch = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            break_patch_pos[loop_depth-1][break_patch_count[loop_depth-1]++] = patch;
            break;
        }
        case NODE_CONTINUE: {
            // Emit placeholder jump; patch later to loop start
            if (loop_depth <= 0) {
                break;
            }
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t patch = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            continue_patch_pos[loop_depth-1][continue_patch_count[loop_depth-1]++] = patch;
            break;
        }
        case NODE_SWITCH: {
            // Evaluate switch expression
            lower_expr(c, arena, node->left);
            // Move switch value to rbx
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xD8); // mov rbx, rax
            // Walk through case/default linked list
            ASTRef stmt = node->right;
            // removed unused default_body_pos
            while (stmt != AST_NULL) {
                ZeroCCASTNode *case_node = zero_cc_ast_get(arena, stmt);
                if (!case_node) break;
                if (case_node->tag == NODE_CASE) {
                    // Evaluate case constant
                    lower_expr(c, arena, case_node->left);
                    // cmp rbx, rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x39);
                    zero_cc_emit_u8(&c->text_section, 0xD8);
                    // je placeholder
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    zero_cc_emit_u8(&c->text_section, 0x84);
                    size_t je_patch = c->text_section.length;
                    zero_cc_emit_u32(&c->text_section, 0);
                    // case body start
                    size_t case_body_start = c->text_section.length;
                    uint32_t rel = (uint32_t)(case_body_start - (je_patch + 4));
                    memcpy(c->text_section.data + je_patch, &rel, 4);
                    if (case_node->right != AST_NULL) {
                        lower_node(c, arena, case_node->right);
                    }
                } else if (case_node->tag == NODE_DEFAULT) {
                    // default case body already emitted
                    if (case_node->right != AST_NULL) {
                        lower_node(c, arena, case_node->right);
                    }
                }
                stmt = case_node->next;
            }
            // execution falls through to default if present
            break;
        }
        case NODE_FOR: {
            if (node->left != AST_NULL) lower_node(c, arena, node->left);
            size_t start_pos = c->text_section.length;
            // condition
            if (node->right != AST_NULL) {
                lower_expr(c, arena, node->right);
                // test rax, rax
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u8(&c->text_section, 0xC0);
                // je end
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0x84);
                uint32_t end_offset_pos = c->text_section.length;
                zero_cc_emit_u32(&c->text_section, 0);
                // body
                if (node->aux.alt != AST_NULL) lower_node(c, arena, node->aux.alt);
                // step
                if (node->aux.step != AST_NULL) lower_node(c, arena, node->aux.step);
                // jmp start
                uint32_t back_rel = (uint32_t)(start_pos - (c->text_section.length + 5));
                zero_cc_emit_u8(&c->text_section, 0xE9);
                zero_cc_emit_u32(&c->text_section, back_rel);
                // end label patch
                size_t end_pos = c->text_section.length;
                uint32_t rel_end = (uint32_t)(end_pos - (end_offset_pos + 4));
                memcpy(c->text_section.data + end_offset_pos, &rel_end, 4);
            } else {
                // No condition -> infinite loop.
                if (node->aux.alt != AST_NULL) lower_node(c, arena, node->aux.alt);
                if (node->aux.step != AST_NULL) lower_node(c, arena, node->aux.step);
                // jmp start
                uint32_t back_rel = (uint32_t)(start_pos - (c->text_section.length + 5));
                zero_cc_emit_u8(&c->text_section, 0xE9);
                zero_cc_emit_u32(&c->text_section, back_rel);
            }
            break;
        }

    }
}

static void lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref) {
    if (ref == AST_NULL) return;
    ZeroCCASTNode *node = zero_cc_ast_get(arena, ref);
    if (!node) return;
    switch (node->tag) {
        case NODE_INT:
            // mov rax, imm64
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, (uint64_t)node->int_val);
            break;
        case NODE_FLOAT: {
            // Emit SSE2 movsd xmm0, [rip + rodata_offset] for float literal
            double val = node->float_val;
            size_t rodata_offset = c->rodata_section.length;
            zero_cc_buf_put_bytes(&c->rodata_section, (const uint8_t*)&val, sizeof(val));
            // movsd xmm0, [rip + offset]
            zero_cc_emit_u8(&c->text_section, 0xF2);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x10);
            zero_cc_emit_u8(&c->text_section, 0x05);
            size_t patch_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
                c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
                c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
                c->str_reloc_count++;
            }
            break;
        }
        case NODE_BOOL:
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, (uint64_t)node->int_val);
            break;
        case NODE_NULLPTR:
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC0); // xor rax, rax
            break;
        case NODE_STRING:
            // Emit lea rax, [rip + rodata_offset] for string literal
            if (node->s.str) {
                // Len may be corrupted by semantic passes, recompute safely
                size_t len = strlen(node->s.str);
                size_t rodata_offset = c->rodata_section.length;
                zero_cc_buf_put_bytes(&c->rodata_section, node->s.str, len);
                uint8_t zero = 0;
                zero_cc_buf_put_bytes(&c->rodata_section, &zero, 1);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8D);
                zero_cc_emit_u8(&c->text_section, 0x05);
                size_t patch_pos = c->text_section.length;
                zero_cc_emit_u32(&c->text_section, 0);
                if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
                    c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
                    c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
                    c->str_reloc_count++;
                }
            } else {
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x31);
                zero_cc_emit_u8(&c->text_section, 0xC0);
            }
            break;
        case NODE_IDENT: {
            const char *var = node->s.str;
            /* First check if it's a local variable */
            ZeroCCSym *sym = zero_cc_find_local(c, var);
            if (sym) {
                // mov rax, [rbp - offset]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
            } else {
                /* Check if it's a global variable */
                ZeroCCGlobalVar *gv = find_global_var(var);
                if (gv) {
                    /* Load global variable from data section using RIP-relative addressing */
                    /* lea rax, [rip + data_offset + gv->data_offset] */
                    /* First, emit the address as a relocation */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8D);
                    zero_cc_emit_u8(&c->text_section, 0x05);
                    size_t patch_pos = c->text_section.length;
                    zero_cc_emit_u32(&c->text_section, 0); /* Placeholder for RIP-relative offset */
                    /* Add relocation for global variable access */
                    if (c->reloc_count < 512) {
                        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                        r->text_patch_offset = patch_pos;
                        r->is_call = false; /* Not a call, it's a data reference */
                        snprintf(r->target_symbol, sizeof(r->target_symbol), "__data_%s", var);
                        r->data_offset = gv->data_offset;
                    }
                    /* Now load the value: mov rax, [rax] */
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x00);
                }
            }
            break;
        }
        case NODE_CALL: {
            // Evaluate arguments left to right and push them to stack
            ASTRef arg = node->right;
            int arg_count = 0;
            while (arg != AST_NULL) {
                lower_expr(c, arena, arg);
                zero_cc_emit_u8(&c->text_section, 0x50); // push rax
                arg_count++;
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                arg = arg_node ? arg_node->next : AST_NULL;
            }

            // Pop into AMD64 argument registers
            static const uint8_t arg_regs[6][3] = {
                {0x48, 0x89, 0xC7}, /* mov rdi, rax */
                {0x48, 0x89, 0xC6}, /* mov rsi, rax */
                {0x48, 0x89, 0xC2}, /* mov rdx, rax */
                {0x48, 0x89, 0xC1}, /* mov rcx, rax */
                {0x49, 0x89, 0xC0}, /* mov r8,  rax */
                {0x49, 0x89, 0xC1}  /* mov r9,  rax */
            };
            int reg_args = (arg_count > 6) ? 6 : arg_count;
            for (int i = reg_args - 1; i >= 0; i--) {
                zero_cc_emit_u8(&c->text_section, 0x58); /* pop rax */
                zero_cc_emit_u8(&c->text_section, arg_regs[i][0]);
                zero_cc_emit_u8(&c->text_section, arg_regs[i][1]);
                zero_cc_emit_u8(&c->text_section, arg_regs[i][2]);
            }
            // Clear al for ABI variadic compatibility
            zero_cc_emit_u8(&c->text_section, 0x31);
            zero_cc_emit_u8(&c->text_section, 0xC0);

            // Callee is left child
            ZeroCCASTNode *callee = zero_cc_ast_get(arena, node->left);
            if (callee && callee->tag == NODE_IDENT) {
                const char *name = callee->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, name);
                if (sym) {
                    // Indirect call: mov r11, [rbp - offset]; call r11
                    zero_cc_emit_u8(&c->text_section, 0x4C);
                    zero_cc_emit_u8(&c->text_section, 0x8B);
                    zero_cc_emit_u8(&c->text_section, 0x9D);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    zero_cc_emit_u8(&c->text_section, 0x41);
                    zero_cc_emit_u8(&c->text_section, 0xFF);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                } else {
                    ZeroCCFunction *fn = zero_cc_find_function(c, name);
                    void *host_fn = zero_cc_resolve_symbol(name);
                    if (fn && !fn->is_prototype) {
                        /* Internal function: generate call with relocation */
                        zero_cc_emit_u8(&c->text_section, 0xE8);
                        size_t patch_pos = c->text_section.length;
                        int32_t rel = (int32_t)(fn->text_offset - (patch_pos + 4));
                        zero_cc_emit_u32(&c->text_section, (uint32_t)rel);
                        if (c->reloc_count < 512) {
                            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                            r->text_patch_offset = patch_pos;
                            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                            r->is_call = true;
                        }
                    } else if (host_fn) {
                        /* External function resolved at compile time: generate indirect call */
                        zero_cc_emit_u8(&c->text_section, 0x49);
                        zero_cc_emit_u8(&c->text_section, 0xBB);
                        zero_cc_emit_u64(&c->text_section, (uint64_t)host_fn);
                        zero_cc_emit_u8(&c->text_section, 0x41);
                        zero_cc_emit_u8(&c->text_section, 0xFF);
                        zero_cc_emit_u8(&c->text_section, 0xD3);
                    } else if (fn && fn->is_prototype) {
                        /* External function prototype: generate indirect call with placeholder */
                        zero_cc_emit_u8(&c->text_section, 0x49);
                        zero_cc_emit_u8(&c->text_section, 0xBB);
                        uint64_t placeholder = 0;
                        zero_cc_emit_u64(&c->text_section, placeholder);
                        zero_cc_emit_u8(&c->text_section, 0x41);
                        zero_cc_emit_u8(&c->text_section, 0xFF);
                        zero_cc_emit_u8(&c->text_section, 0xD3);
                        /* Record relocation for runtime address patching */
                        if (c->reloc_count < 512) {
                            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                            r->text_patch_offset = c->text_section.length - 9; /* Point to the 8-byte address */
                            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                            r->is_call = false; /* Address patch, not call relocation */
                        }
                    } else {
                        /* Unknown function: generate call with relocation */
                        zero_cc_emit_u8(&c->text_section, 0xE8);
                        size_t patch_pos = c->text_section.length;
                        zero_cc_emit_u32(&c->text_section, 0);
                        if (c->reloc_count < 512) {
                            ZeroCCReloc *r = &c->relocs[c->reloc_count++];
                            r->text_patch_offset = patch_pos;
                            snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", name);
                            r->is_call = true;
                        }
                    }
                }
            }
            break;
        }
        case NODE_COMPOUND_LIT:
            // Compound literal: evaluate first initializer expression
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            break;
        case NODE_MEMBER:
            // Member access: evaluate base, add offset, load value
            lower_expr(c, arena, node->left);
            // For arrow ->, rax is already a pointer to struct
            // For dot ., rax contains the struct value (not address)
            // Current implementation assumes pointer base (arrow case)
            // Load member from [rax + offset]
            size_t offset = node->aux.extra;
            if (offset == 0) {
                // No offset: mov rax, [rax]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x00);  // mov rax, [rax]
            } else if (offset < 128) {
                // Small offset: mov rax, [rax + offset]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x40);  // mov rax, [rax + disp8]
                zero_cc_emit_u8(&c->text_section, (uint8_t)offset);
            } else if (offset < 256) {
                // Offset fits in 8 bits
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x80);  // mov rax, [rax + disp32]
                zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
            } else {
                // Large offset: mov rax, [rax + offset]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x80);  // mov rax, [rax + disp32]
                zero_cc_emit_u32(&c->text_section, (uint32_t)offset);
            }
            break;
        case NODE_INDEX: {
            // Array subscript: base[index]
            // For char/byte elements (element size = 1)
            // 1. Evaluate base pointer -> rax
            lower_expr(c, arena, node->left);
            // Save base to rdx
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x89);
            zero_cc_emit_u8(&c->text_section, 0xC2);  // mov rdx, rax
            
            // 2. Evaluate index -> rax
            lower_expr(c, arena, node->right);
            
            // 3. Calculate address: rax = rdx + rax (base + index)
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x01);
            zero_cc_emit_u8(&c->text_section, 0xD0);  // add rax, rdx
            
            // 4. Load byte from address and zero-extend -> rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0xB6);
            zero_cc_emit_u8(&c->text_section, 0x00);  // movzx rax, byte [rax]
            break;
        }
        case NODE_CAST:
            // Casts are no-ops for integer types in this stub
            // Float to int conversion requires SSE2 cvttsd2si (not yet implemented)
            if (node->left != AST_NULL) {
                lower_expr(c, arena, node->left);
            }
            break;
        case NODE_ASSIGN: {
            // Right-hand side evaluated into rax
            lower_expr(c, arena, node->right);
            // Left-hand side variable target
            ZeroCCASTNode *target = zero_cc_ast_get(arena, node->left);
            if (target && target->tag == NODE_IDENT) {
                const char *var = target->s.str;
                ZeroCCSym *sym = zero_cc_find_local(c, var);
                if (sym) {
                    // mov [rbp - offset], rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0x85);
                    zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                }
            }
            break;
        }
        case NODE_TERNARY: {
            // Evaluate condition
            lower_expr(c, arena, node->left);
            // test rax, rax
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0x85);
            zero_cc_emit_u8(&c->text_section, 0xC0);
            // je else
            zero_cc_emit_u8(&c->text_section, 0x0F);
            zero_cc_emit_u8(&c->text_section, 0x84);
            size_t else_pos = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            // then
            lower_expr(c, arena, node->right);
            // jmp end
            zero_cc_emit_u8(&c->text_section, 0xE9);
            size_t end_pos_patch = c->text_section.length;
            zero_cc_emit_u32(&c->text_section, 0);
            // else
            size_t else_addr = c->text_section.length;
            uint32_t rel_else = (uint32_t)(else_addr - (else_pos + 4));
            memcpy(c->text_section.data + else_pos, &rel_else, 4);
            lower_expr(c, arena, node->aux.alt);
            size_t end_addr = c->text_section.length;
            uint32_t rel_end = (uint32_t)(end_addr - (end_pos_patch + 4));
            memcpy(c->text_section.data + end_pos_patch, &rel_end, 4);
            break;
        }
        case NODE_BINOP: {
            // Evaluate left operand, push to stack.
            lower_expr(c, arena, node->left);
            zero_cc_emit_u8(&c->text_section, 0x50); // push rax
            // Evaluate right operand.
            lower_expr(c, arena, node->right);
            zero_cc_emit_u8(&c->text_section, 0x59); // pop rcx (left operand)
            // Left is now in rcx, right is in rax.
            // Perform operation based on opcode.
            switch (node->aux.op) {
                case TK_PLUS:
                    // add rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x01);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_MINUS:
                    // sub rcx, rax ; mov rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x29);
                    zero_cc_emit_u8(&c->text_section, 0xC1);
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_STAR:
                    // imul rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    zero_cc_emit_u8(&c->text_section, 0xAF);
                    zero_cc_emit_u8(&c->text_section, 0xC1);
                    break;
                case TK_SLASH:
                    // rcx (left) / rax (right) -> quotient in rax
                    zero_cc_emit_u8(&c->text_section, 0x53);       // push rbx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC3);       // mov rbx, rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // mov rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x99);       // cqo (sign extend rax into rdx:rax)
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xF7);
                    zero_cc_emit_u8(&c->text_section, 0xFB);       // idiv rbx
                    zero_cc_emit_u8(&c->text_section, 0x5B);       // pop rbx
                    break;
                case TK_PERCENT:
                    // rcx (left) % rax (right) -> remainder in rdx, move to rax
                    zero_cc_emit_u8(&c->text_section, 0x53);       // push rbx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC3);       // mov rbx, rax
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // mov rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x99);       // cqo
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xF7);
                    zero_cc_emit_u8(&c->text_section, 0xFB);       // idiv rbx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x89);
                    zero_cc_emit_u8(&c->text_section, 0xD0);       // mov rax, rdx
                    zero_cc_emit_u8(&c->text_section, 0x5B);       // pop rbx
                    break;
                case TK_AMP:
                    // and rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x21);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_PIPE:
                    // or rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x09);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_CARET:
                    // xor rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x31);
                    zero_cc_emit_u8(&c->text_section, 0xC8);
                    break;
                case TK_SHL:
                    // rcx (left) << rax (right: count in cl)
                    // xchg rax, rcx ; shl rax, cl
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x87);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // xchg rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                    zero_cc_emit_u8(&c->text_section, 0xE0);       // shl rax, cl
                    break;
                case TK_SHR:
                    // rcx (left) >> rax (right: count in cl)
                    // xchg rax, rcx ; sar rax, cl
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x87);
                    zero_cc_emit_u8(&c->text_section, 0xC8);       // xchg rax, rcx
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xD3);
                    zero_cc_emit_u8(&c->text_section, 0xF8);       // sar rax, cl
                    break;
                case TK_EQ:
                case TK_NE:
                case TK_LT:
                case TK_GT:
                case TK_LE:
                case TK_GE:
                    // cmp rcx, rax (compare left with right)
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x39);
                    zero_cc_emit_u8(&c->text_section, 0xC1);
                    // setcc al
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    if (node->aux.op == TK_EQ) zero_cc_emit_u8(&c->text_section, 0x94);      // sete al
                    else if (node->aux.op == TK_NE) zero_cc_emit_u8(&c->text_section, 0x95); // setne al
                    else if (node->aux.op == TK_LT) zero_cc_emit_u8(&c->text_section, 0x9C); // setl al
                    else if (node->aux.op == TK_GE) zero_cc_emit_u8(&c->text_section, 0x9D); // setge al
                    else if (node->aux.op == TK_LE) zero_cc_emit_u8(&c->text_section, 0x9E); // setle al
                    else if (node->aux.op == TK_GT) zero_cc_emit_u8(&c->text_section, 0x9F); // setg al
                    zero_cc_emit_u8(&c->text_section, 0xC0); // ModR/M for AL register
                    // movzx rax, al
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0x0F);
                    zero_cc_emit_u8(&c->text_section, 0xB6);
                    zero_cc_emit_u8(&c->text_section, 0xC0);
                    break;
                default:
                    // Unsupported binary operator – emit zero.
                    zero_cc_emit_u8(&c->text_section, 0x48);
                    zero_cc_emit_u8(&c->text_section, 0xB8);
                    zero_cc_emit_u64(&c->text_section, 0);
                    break;
            }
            break;
        }
        case NODE_UNOP: {
            if (node->aux.op == TK_AMP) {
                // Address-of operator &var
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        // lea rax, [rbp - offset]
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8D);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    }
                }
            } else if (node->aux.op == TK_STAR) {
                // Dereference operator *ptr
                lower_expr(c, arena, node->left);
                // mov rax, [rax]
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x8B);
                zero_cc_emit_u8(&c->text_section, 0x00);
            } else if (node->aux.op == TK_MINUS) {
                // Negation -x
                lower_expr(c, arena, node->left);
                // neg rax
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0xF7);
                zero_cc_emit_u8(&c->text_section, 0xD8);
            } else if (node->aux.op == TK_BANG) {
                // Logical NOT !x
                lower_expr(c, arena, node->left);
                // test rax, rax; sete al; movzx rax, al
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x85);
                zero_cc_emit_u8(&c->text_section, 0xC0);
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0x94);
                zero_cc_emit_u8(&c->text_section, 0xC0);
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0x0F);
                zero_cc_emit_u8(&c->text_section, 0xB6);
                zero_cc_emit_u8(&c->text_section, 0xC0);
            } else if (node->aux.op == TK_TILDE) {
                // Bitwise NOT ~x
                lower_expr(c, arena, node->left);
                // not rax
                zero_cc_emit_u8(&c->text_section, 0x48);
                zero_cc_emit_u8(&c->text_section, 0xF7);
                zero_cc_emit_u8(&c->text_section, 0xD0);
            } else if (node->flags == 0 && (node->aux.op == TK_INC || node->aux.op == TK_DEC)) {
                // Handle prefix ++/-- on identifiers.
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        // Load current value.
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                        // inc or dec rax.
                        if (node->aux.op == TK_INC) {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0xFF);
                            zero_cc_emit_u8(&c->text_section, 0xC0); // inc rax
                        } else {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0xFF);
                            zero_cc_emit_u8(&c->text_section, 0xC8); // dec rax
                        }
                        // Store back.
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                    }
                }
            } else if (node->flags != 0 && (node->aux.op == TK_INC || node->aux.op == TK_DEC)) {
                // Handle postfix ++/-- on identifiers.
                ZeroCCASTNode *operand = zero_cc_ast_get(arena, node->left);
                if (operand && operand->tag == NODE_IDENT) {
                    const char *var = operand->s.str;
                    ZeroCCSym *sym = zero_cc_find_local(c, var);
                    if (sym) {
                        // Load current value (to return).
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x8B);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                        // Save original value to rcx.
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0xC1); // mov rcx, rax
                        // Increment/decrement rax.
                        if (node->aux.op == TK_INC) {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0xFF);
                            zero_cc_emit_u8(&c->text_section, 0xC0); // inc rax
                        } else {
                            zero_cc_emit_u8(&c->text_section, 0x48);
                            zero_cc_emit_u8(&c->text_section, 0xFF);
                            zero_cc_emit_u8(&c->text_section, 0xC8); // dec rax
                        }
                        // Store back.
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0x85);
                        zero_cc_emit_u32(&c->text_section, (uint32_t)(-sym->stack_offset));
                        // Restore original value to rax (for postfix semantics).
                        zero_cc_emit_u8(&c->text_section, 0x48);
                        zero_cc_emit_u8(&c->text_section, 0x89);
                        zero_cc_emit_u8(&c->text_section, 0xC8); // mov rax, rcx
                    }
                }
            }
            break;
        }
        default:
            // Emit zero for other expression types (placeholder).
            zero_cc_emit_u8(&c->text_section, 0x48);
            zero_cc_emit_u8(&c->text_section, 0xB8);
            zero_cc_emit_u64(&c->text_section, 0);
            break;
    }
}

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    epilogue_patch_count = 0;
    reset_label_state();
    
    /* Reset temp variable tracking */
    temp_var_count = 0;
    
    /* Lower AST to machine code */
    lower_node(c, arena, root);
    
    /* Update live end positions for all temporaries */
    size_t end_pos = c->text_section.length;
    for (size_t i = 0; i < temp_var_count; i++) {
        if (temp_vars[i].live_end == 0) {
            temp_vars[i].live_end = end_pos;
        }
    }
    
    /* Run register allocation */
    for (size_t i = 0; i < temp_var_count; i++) {
        regalloc_add_interval(&c->regalloc, temp_vars[i].temp_id, 
                              temp_vars[i].live_start, temp_vars[i].live_end,
                              temp_vars[i].reg_class);
    }
    regalloc_allocate(&c->regalloc);
    
    /* Backpatch all return epilogue jumps to the current offset */
    size_t epilogue_target = c->text_section.length;
    for (size_t i = 0; i < epilogue_patch_count; i++) {
        size_t patch_pos = epilogue_patches[i];
        int32_t rel = (int32_t)(epilogue_target - (patch_pos + 4));
        memcpy(c->text_section.data + patch_pos, &rel, 4);
    }
    epilogue_patch_count = 0;
    
    /* Emit function epilogue */
    zero_cc_emit_u8(&c->text_section, 0xC9);           /* leave (mov rsp, rbp; pop rbp) */
    zero_cc_emit_u8(&c->text_section, 0xC3);           /* ret */
    
    /* Reset temp variables for next function */
    temp_var_count = 0;
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_H */
