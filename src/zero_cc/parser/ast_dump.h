#ifndef ZERO_CC_PARSER_AST_DUMP_H
#define ZERO_CC_PARSER_AST_DUMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../parser.h"

static const char *op_to_str(uint32_t op) {
    switch (op) {
        case TK_PLUS: return "+";
        case TK_MINUS: return "-";
        case TK_STAR: return "*";
        case TK_SLASH: return "/";
        case TK_PERCENT: return "%";
        case TK_AMP: return "&";
        case TK_PIPE: return "|";
        case TK_CARET: return "^";
        case TK_SHL: return "<<";
        case TK_SHR: return ">>";
        case TK_EQ: return "==";
        case TK_NE: return "!=";
        case TK_LT: return "<";
        case TK_GT: return ">";
        case TK_LE: return "<=";
        case TK_GE: return ">=";
        case TK_LOGAND: return "&&";
        case TK_LOGOR: return "||";
        case TK_ASSIGN: return "=";
        case TK_ADD_ASSIGN: return "+=";
        case TK_SUB_ASSIGN: return "-=";
        case TK_MUL_ASSIGN: return "*=";
        case TK_DIV_ASSIGN: return "/=";
        case TK_MOD_ASSIGN: return "%=";
        case TK_AND_ASSIGN: return "&=";
        case TK_OR_ASSIGN: return "|=";
        case TK_XOR_ASSIGN: return "^=";
        case TK_SHL_ASSIGN: return "<<=";
        case TK_SHR_ASSIGN: return ">>=";
        case TK_BANG: return "!";
        case TK_TILDE: return "~";
        case TK_INC: return "++";
        case TK_DEC: return "--";
        default: return "?";
    }
}

void zero_cc_ast_dump(ZeroCCASTArena *arena, ASTRef root, int indent) {
    if (root == AST_NULL) return;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, root);
    if (!n) return;

    for (int i = 0; i < indent; i++) printf("  ");

    switch (n->tag) {
        case NODE_INT:
            printf("IntLiteral: %ld\n", (long)n->int_val);
            break;
        case NODE_FLOAT:
            printf("FloatLiteral: %f\n", n->float_val);
            break;
        case NODE_STRING:
            printf("StringLiteral: \"%.*s\"\n", (int)n->s.len, n->s.str ? n->s.str : "");
            break;
        case NODE_IDENT:
            printf("Identifier: %.*s\n", (int)n->s.len, n->s.str ? n->s.str : "");
            break;
        case NODE_NULLPTR:
            printf("NullptrLiteral\n");
            break;
        case NODE_BOOL:
            printf("BoolLiteral: %s\n", n->int_val ? "true" : "false");
            break;
        case NODE_UNOP:
            printf("UnaryOp: %s %s\n", op_to_str(n->aux.op), n->flags ? "(postfix)" : "(prefix)");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_BINOP:
            printf("BinaryOp: %s\n", op_to_str(n->aux.op));
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            break;
        case NODE_ASSIGN:
            printf("Assign: %s\n", op_to_str(n->aux.op));
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            break;
        case NODE_TERNARY:
            printf("TernaryOp (? :)\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            zero_cc_ast_dump(arena, n->aux.alt, indent + 1);
            break;
        case NODE_MEMBER:
            printf("MemberAccess: %s%.*s\n", n->flags ? "->" : ".", (int)n->s.len, n->s.str ? n->s.str : "");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_INDEX:
            printf("ArrayIndex\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            zero_cc_ast_dump(arena, n->right, indent + 1);
            break;
        case NODE_COMPOUND_LIT:
            printf("CompoundLiteral\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_CALL:
            printf("FunctionCall\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            ASTRef arg = n->right;
            while (arg != AST_NULL) {
                zero_cc_ast_dump(arena, arg, indent + 2);
                ZeroCCASTNode *arg_node = zero_cc_ast_get(arena, arg);
                arg = arg_node ? arg_node->next : AST_NULL;
            }
            break;
        case NODE_BLOCK:
            printf("Block\n");
            ASTRef stmt = n->left;
            while (stmt != AST_NULL) {
                zero_cc_ast_dump(arena, stmt, indent + 1);
                ZeroCCASTNode *stmt_node = zero_cc_ast_get(arena, stmt);
                stmt = stmt_node ? stmt_node->next : AST_NULL;
            }
            break;
        case NODE_EXPR_STMT:
            printf("ExprStmt\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        case NODE_IF:
            printf("IfStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".cond:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".then:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            if (n->aux.alt != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".else:\n");
                zero_cc_ast_dump(arena, n->aux.alt, indent + 2);
            }
            break;
        case NODE_WHILE:
            printf("WhileStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".cond:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".body:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            break;
        case NODE_DO_WHILE:
            printf("DoWhileStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".body:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".cond:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            break;
        case NODE_FOR:
            printf("ForStmt\n");
            if (n->left != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".init:\n");
                zero_cc_ast_dump(arena, n->left, indent + 2);
            }
            if (n->right != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".cond:\n");
                zero_cc_ast_dump(arena, n->right, indent + 2);
            }
            if (n->aux.step != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".step:\n");
                zero_cc_ast_dump(arena, n->aux.step, indent + 2);
            }
            if (n->aux.alt != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".body:\n");
                zero_cc_ast_dump(arena, n->aux.alt, indent + 2);
            }
            break;
        case NODE_SWITCH:
            printf("SwitchStmt\n");
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".expr:\n");
            zero_cc_ast_dump(arena, n->left, indent + 2);
            for (int i = 0; i <= indent; i++) printf("  ");
            printf(".body:\n");
            zero_cc_ast_dump(arena, n->right, indent + 2);
            break;
        case NODE_CASE:
            printf("CaseStmt\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            if (n->right != AST_NULL) {
                zero_cc_ast_dump(arena, n->right, indent + 1);
            }
            break;
        case NODE_DEFAULT:
            printf("DefaultStmt\n");
            if (n->left != AST_NULL) {
                zero_cc_ast_dump(arena, n->left, indent + 1);
            }
            break;
        case NODE_BREAK:
            printf("BreakStmt\n");
            break;
        case NODE_CONTINUE:
            printf("ContinueStmt\n");
            break;
        case NODE_RETURN:
            printf("ReturnStmt\n");
            if (n->left != AST_NULL) {
                zero_cc_ast_dump(arena, n->left, indent + 1);
            }
            break;
        case NODE_DECL:
            printf("Decl: %.*s\n", (int)n->s.len, n->s.str ? n->s.str : "");
            if (n->left != AST_NULL) {
                for (int i = 0; i <= indent; i++) printf("  ");
                printf(".init:\n");
                zero_cc_ast_dump(arena, n->left, indent + 2);
            }
            break;
        case NODE_STATIC_ASSERT:
            printf("StaticAssert\n");
            zero_cc_ast_dump(arena, n->left, indent + 1);
            break;
        default:
            printf("UnknownNode (tag=%d)\n", n->tag);
            break;
    }
}

/* ======================================================================
 *  AST Lowering to AMD64 Machine Code
 * ======================================================================
 */

static void lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);

#define MAX_EPILOGUE_PATCHES 256
static size_t epilogue_patches[MAX_EPILOGUE_PATCHES];
static size_t epilogue_patch_count = 0;


#endif /* ZERO_CC_PARSER_AST_DUMP_H */
