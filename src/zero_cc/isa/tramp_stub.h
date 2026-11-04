/* Trampoline stub support - shared unit model (Phase 3b cross-ISA port)
 *
 * A capturing nested function's mangled symbol has the hidden capture
 * parameters in its ABI, so calls through a function pointer must go
 * through a trampoline stub (see the per-ISA emitters:
 * zero_cc_x86_emit_tramp_stub, zero_cc_aarch64_emit_tramp_stub,
 * zero_cc_riscv_emit_tramp_stub). The stub takes the USER arguments,
 * loads the captures from the static ctx object and re-places everything
 * in the real function's argument slots.
 *
 * The unit model below MUST match the prologue's param-save stream
 * (ast_lowering.c) and the caller-side marshalling (ast_lowering_call.c):
 *   - float/double          : 1 FP unit (complex: 2 consecutive FP units)
 *   - struct/union > 16B    : 1 GPR unit (hidden pointer convention)
 *   - struct/union 9..16B   : 2 GPR units (8-byte chunks)
 *   - vector args           : 8-byte chunks (GPR units), never hidden ptr
 *   - everything else       : 1 GPR unit
 *
 * The >16B hidden-pointer rule (zerocc's own convention, see
 * ast_lowering_call.c "big_struct_ptr_conv") is what the real nested
 * function's prologue expects for its struct captures/params, so the
 * stub must use it too - a 17..32B struct capture is copied into the
 * stub's frame and its address is passed.
 */
#ifndef ZERO_CC_ISA_TRAMP_STUB_H
#define ZERO_CC_ISA_TRAMP_STUB_H

#include <stdint.h>
#include "zero_cc/types/types.h"

/* Hard cap on the total 8-byte units (sret + captures + user params) a
 * stub can re-marshal. Checked at registration time so an overflowing
 * nested function falls back to the "no trampoline" diagnostic instead
 * of a truncated stub. */
#define ZERO_CC_TRAMP_MAX_UNITS 24

static inline bool zero_cc_tramp_unit_is_fp(const ZeroCCType *t) {
    return (t->tag == TYPE_FLOAT || t->tag == TYPE_DOUBLE);
}

/* Number of 8-byte ABI units this type occupies in the argument stream. */
static inline int zero_cc_tramp_unit_slots(const ZeroCCType *t) {
    if (zero_cc_tramp_unit_is_fp(t)) return t->is_complex ? 2 : 1;
    if ((t->tag == TYPE_STRUCT || t->tag == TYPE_UNION) &&
        !(t->tag == TYPE_USER_0 && zero_cc_type_is_vector(t)) &&
        t->size > 16)
        return 1; /* hidden pointer convention */
    size_t s = t->size ? t->size : 8;
    return (int)((s + 7) / 8);
}

/* Total units the function's trampoline stub would need: captures (by-ref
 * = 1 pointer word; by-value = unit slots of the captured type) plus the
 * user parameters. A >16B struct RETURN adds nothing (x86-64/RISC-V: the
 * sret pointer is a pass-through of the entry's first integer slot;
 * AArch64: it travels in x8 and never touches the streams). */
static inline int zero_cc_tramp_total_units(const ZeroCCFunction *nf) {
    int n = 0;
    for (size_t ci = 0; ci < nf->nested_captured_count; ci++) {
        n += nf->nested_capture_byref[ci] ? 1 :
             zero_cc_tramp_unit_slots(&nf->nested_capture_types[ci]);
    }
    for (size_t p = nf->nested_captured_count; p < nf->param_count; p++) {
        n += zero_cc_tramp_unit_slots(&nf->param_types[p]);
    }
    return n;
}

/* Value source of a stub unit:
 *   0 = ctx object (src_off = 32*ci + 8*w)
 *   1 = entry GPR-arg save in the stub frame
 *   2 = entry FP-arg save in the stub frame
 *   3 = entry stack-arg save in the stub frame
 *   5 = address of a >16B by-value capture copied into the stub frame
 */
typedef struct {
    bool is_fp;      /* FP stream vs GPR stream */
    int  fin_slot;   /* final register slot (0..cap-1), -1 = final stack */
    int  fin_stack;  /* final stack unit index, -1 = final register */
    int  src;        /* one of the TRAMP_UNIT_SRC_* values above */
    int  src_off;    /* frame/ctx offset for the source */
} ZeroCCTrampUnit;

#endif /* ZERO_CC_ISA_TRAMP_STUB_H */
