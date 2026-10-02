/* expressions/primary.h — REMOVED (Sprint 5.5 Task 40)
 * The legacy single-pass code-emitting primary expression parser has been
 * eliminated. All expression compilation now goes through the AST/IR pipeline:
 *   src/zero_cc/parser/ast_expr.h → ast_lowering.h → IR → emit_ir_to_machine_code()
 *
 * Pure utilities (sizeof/alignof type-inference, zero_cc_eval_const_expr) have
 * been moved to: src/zero_cc/parser/const_eval.h
 */
#ifndef ZERO_CC_EXPRESSIONS_PRIMARY_H
#define ZERO_CC_EXPRESSIONS_PRIMARY_H
#endif /* ZERO_CC_EXPRESSIONS_PRIMARY_H */
