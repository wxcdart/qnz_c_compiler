/* expressions.h — Legacy Expression Parser Barrel: REMOVED (Sprint 5.5 Task 40)
 *
 * The legacy single-pass expression compiler has been eliminated.
 * All expression compilation now goes through the AST/IR pipeline:
 *   src/zero_cc/parser/ast_expr.h → ast_lowering.h → IR → emit_ir_to_machine_code()
 *
 * Pure utilities (sizeof/alignof type-inference, zero_cc_eval_const_expr,
 * zero_cc_parse_type_name, zero_cc_type_find_member) are now in:
 *   src/zero_cc/parser/const_eval.h
 */
#ifndef ZERO_CC_EXPRESSIONS_H
#define ZERO_CC_EXPRESSIONS_H
#endif /* ZERO_CC_EXPRESSIONS_H */
