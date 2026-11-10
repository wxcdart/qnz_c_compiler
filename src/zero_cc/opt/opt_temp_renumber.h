/*
 * opt_temp_renumber.h - Temp-ID renumbering by liveness (Sprint 151).
 *
 * One-shot per-function pass that runs after lowering (and the lowering-
 * time epilogue/static-local emission) and BEFORE the optimizer pipeline:
 * compacts the function's churned temp-id space into the peak-live set.
 *
 * Why: temp ids are allocated per function from c->next_temp_id and are
 * never recycled, so the id budget was charged against TOTAL allocation
 * churn (~3 ids per `t = t + 1` statement -> the 65,534 C2b cap tripped
 * at ~22,000 such statements in one function) instead of peak-live usage.
 * The IR temp fields are 32-bit (Sprint 151) so lowering may churn past
 * the old 16-bit space; this pass remaps every temp id to a dense
 * <=65,534 space (sound: temps are a consistent 1:1 relabel, and two old
 * ids may share a new id only when their reference ranges are disjoint),
 * resets c->next_temp_id to the top of the compact space, and turns the
 * per-function backstop from "total churn" into "peak live".
 *
 * All temp-keyed structures are remapped: Instr temp fields, IRAsmArgs,
 * c->asm_operand_temps, and the advisory per-temp type-tag
 * table (conflicting owners of a recycled id -> ZC_TAG_NONE, which every
 * tag consumer treats as "unknown" and falls back conservatively).
 */
#ifndef ZERO_CC_OPT_TEMP_RENUMBER_H
#define ZERO_CC_OPT_TEMP_RENUMBER_H

#include "zero_cc/core/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Renumber the current function's temp ids by liveness.
 *
 * Returns 1 on success (c->next_temp_id reset to max_new_id + 1), 0 on
 * failure (peak-live temps >= 65535, or an internal verification failure
 * - in both cases a diagnostic has been reported; the caller must not
 * proceed to optimization/regalloc).
 *
 * fn_name is used only for diagnostics.
 */
int opt_temp_renumber(ZeroCCCompiler *c, const char *fn_name);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_OPT_TEMP_RENUMBER_H */
