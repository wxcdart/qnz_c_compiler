/* statements.c — Legacy Statement Parser: REMOVED (Sprint 5.5 Task 40)
 *
 * The single-pass code-emitting statement compiler has been eliminated.
 * All statement compilation now goes through the AST/IR pipeline:
 *
 *   zero_cc_parse_stmt_ast()  [src/zero_cc/parser/ast_stmt.h]
 *       → IR emission         [src/zero_cc/parser/ir.h]
 *       → Code generation     [src/zero_cc/parser/ast_lowering.h]
 *       → Backend             [src/zero_cc/isa_backend.h]
 *
 * Top-level typedef/struct/enum registration is still handled in:
 *   src/zero_cc/statements/definitions.h (no code emission, type/symbol mgmt only)
 *
 * _Static_assert handling (no code emission) is in:
 *   src/zero_cc/statements/static_assert.h
 */
#include "zero_cc/statements/definitions.h"
