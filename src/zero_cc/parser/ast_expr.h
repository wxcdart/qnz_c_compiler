/* Expression parsing (AST) - declaration header (Sprint 114 Tier C: implementation
 * moved to ast_expr.c; consumers link the single copy). */
#ifndef ZERO_CC_PARSER_AST_EXPR_H
#define ZERO_CC_PARSER_AST_EXPR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/parser/init_common.h"
#include "zero_cc/parser/vla_struct.h"
#include "zero_cc/jit/type_info.h"

bool is_primary_token(ZeroCCTokType t);
bool is_assign_op(ZeroCCTokType t);
ASTRef parse_brace_init_list_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ZeroCCType lit_type, size_t base_off, size_t *leaf_cursor, size_t *out_elem_count);
ASTRef parse_primary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_postfix_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_unary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_multiplicative_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_additive_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_shift_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_relational_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_equality_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_bit_and_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_bit_xor_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_bit_or_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_logical_and_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_logical_or_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_ternary_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_assignment_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
void skip_c23_attributes(ZeroCCCompiler *c);
bool is_type_specifier_at(ZeroCCCompiler *c, size_t pos, ZeroCCTokType t);
bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t);

#endif /* ZERO_CC_PARSER_AST_EXPR_H */
