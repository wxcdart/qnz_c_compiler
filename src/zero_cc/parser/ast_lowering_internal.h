/* AST Lowering - Internal Declarations
 *
 * Private internal header for AST lowering module.
 * Contains forward declarations and shared types.
 */

#ifndef ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H
#define ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../types/types.h"
#include "parser.h"
#include "ir.h"
#include "ir_emitter.h"
#include "ir_emit_ops.h"
#include "../isa/isa_backend.h"
#include "../types/vregs.h"
#include "../types/global_vars.h"
#include "../semantic/type.h"
#include "../common/arena.h"
#include "abi_classify.h"
#include "regalloc.h"
#include "live_range.h"
#include "lowering_state.h"
#include "label_patcher.h"

/* Forward declarations */
void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset);
void emit_return_jumps(ZeroCCCompiler *c);

/* Helper: Get current function's return type */
static inline ZeroCCType zero_cc_get_current_fn_ret_type(ZeroCCCompiler *c) {
    if (c->function_count > 0) {
        return c->functions[c->function_count - 1].return_type;
    }
    return zero_cc_type_int();
}

/* Callee-save register helpers */
void emit_callee_save_stores(ZeroCCCompiler *c);
void emit_callee_save_loads(ZeroCCCompiler *c);

#endif /* ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H */
