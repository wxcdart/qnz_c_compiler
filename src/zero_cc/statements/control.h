/* statements/control.h — REMOVED (Sprint 5.5 Task 40)
 * The legacy single-pass code-emitting control flow statement compiler
 * (if/else, while, for, do-while, switch/case, break, continue, return)
 * has been eliminated. All control flow compilation now goes through the
 * AST/IR pipeline:
 *   src/zero_cc/parser/ast_stmt.h → ast_lowering.h → IR → emit_ir_to_machine_code()
 *
 * The only non-emitting function from this file has been extracted to:
 *   src/zero_cc/statements/static_assert.h → zero_cc_stmt_static_assert()
 */
#ifndef ZERO_CC_STATEMENTS_CONTROL_H
#define ZERO_CC_STATEMENTS_CONTROL_H
#endif /* ZERO_CC_STATEMENTS_CONTROL_H */
