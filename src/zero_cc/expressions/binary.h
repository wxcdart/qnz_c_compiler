/* expressions/binary.h — REMOVED (Sprint 5.5 Task 40)
 * The constant-expression evaluator and type-inference helpers previously in
 * this file have been migrated to:
 *   src/zero_cc/parser/const_eval.h
 *
 * The code-emitting binary operator parser has been eliminated. All binary
 * expression compilation goes through the AST/IR pipeline:
 *   src/zero_cc/parser/ast_expr.h → ast_lowering.h → IR → emit_ir_to_machine_code()
 */
#ifndef ZERO_CC_EXPRESSIONS_BINARY_H
#define ZERO_CC_EXPRESSIONS_BINARY_H
#endif /* ZERO_CC_EXPRESSIONS_BINARY_H */
