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

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/ir/ir_emit_ops.h"
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/types/emit_helpers.h"
#include "zero_cc/types/vregs.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/semantic/type.h"
#include "zero_cc/common/arena.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/ir/abi_classify.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/ir/live_range.h"
#include "zero_cc/ir/lowering_state.h"
#include "zero_cc/ir/label_patcher.h"

/* Forward declarations */
void lower_node(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
int lower_expr(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef ref);
void emit_ir_to_machine_code(ZeroCCCompiler *c, int32_t spill_base_offset);

/* Emit function prologue directly (without IR instruction) */
void emit_prologue_mc(ZeroCCCompiler *c, size_t frame_size);
void emit_return_jumps(ZeroCCCompiler *c);

/* C99 narrow-integer width semantics. The machine word is 64 bits, so
 * 1/2/4-byte integer values must be explicitly extended to full width at
 * every load/use boundary, otherwise overflowed `int` results keep 64-bit
 * residue and compare/store wrong. Returns 1/2/4 for narrow int scalars,
 * 0 otherwise; *sign gets 1 for signed, 0 for unsigned. */
static inline int zero_cc_narrow_int_width(const ZeroCCType *t, int *sign) {
    if (!t) return 0;
    switch (t->tag) {
        case TYPE_INT8:   *sign = 1; return 1;
        case TYPE_UINT8:  *sign = 0; return 1;
        case TYPE_INT16:  *sign = 1; return 2;
        case TYPE_UINT16: *sign = 0; return 2;
        case TYPE_INT32:  *sign = 1; return 4;
        case TYPE_UINT32: *sign = 0; return 4;
        default: return 0;
    }
}

/* Emit in-place sext/zext for a narrow int temp (no-op for 8-byte/pointer types). */
static inline void zero_cc_emit_int_extend(ZeroCCCompiler *c, int temp, const ZeroCCType *t) {
    (void)c;
    int sign = 0;
    int w = zero_cc_narrow_int_width(t, &sign);
    if (w == 0) return;
    if (sign) ir_emit_sext(temp, w);
    else ir_emit_zext(temp, w);
}

/* Helper: Get current function's return type */
static inline ZeroCCType zero_cc_get_current_fn_ret_type(ZeroCCCompiler *c) {
    if (c->function_count > 0) {
        return c->functions[c->function_count - 1].return_type;
    }
    return zero_cc_type_int();
}

#endif /* ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H */
