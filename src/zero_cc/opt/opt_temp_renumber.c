/*
 * opt_temp_renumber.c - Temp-ID renumbering by liveness (Sprint 151,
 * development plan item 9).
 *
 * See opt_temp_renumber.h for the contract. The pass runs once per
 * function, after lowering (and the lowering-time epilogue / static-local
 * emission) and before the optimizer pipeline.
 *
 * Liveness source of truth: the LRA itself. A throwaway
 * LiveRangeAnalyzer + RegAllocator is run over the churned IR
 * (live_range_scan_ir + live_range_compute) so the recycling decision
 * uses exactly the intervals the real register allocator will use -
 * including the loop-backedge extension, the param pre-coloring
 * start=0 rule, and the call-position data. Recycling on raw
 * first/last references is NOT sound: the LRA extends ranges (a temp
 * live in a loop extends to the backedge; params live from entry), and
 * two raw-disjoint temps whose extended ranges overlap must never
 * share an id.
 *
 * Recycling pools: a new id is handed back to a (reg class, type tag)
 * pool when its last extended-range end is passed, and is reused only
 * by a temp of the SAME class and SAME tag. This keeps both LRA
 * invariants intact:
 *   - the LRA keys one reg class per id (a GPR and an XMM temp must
 *     never share an id, even at disjoint times - temp_reg_class[]
 *     holds a single value per id);
 *   - the advisory type-tag table is static per id (a recycled id must
 *     carry the same tag for every owner - zc_div_is_unsigned treats
 *     an unknown tag as signed, so erasing tags would miscompile
 *     unsigned divides).
 *
 * The sweep assigns at a position BEFORE freeing at that position, so
 * an id whose last reference and another id's first reference are the
 * same instruction never share a new id (the instruction reads the old
 * value and writes the new one - distinct home registers).
 *
 * After the rewrite (Instr fields - including the in-stream OP_ASM_ARG
 * operand temps, Sprint 154 - and c->asm_operand_temps, type-tag table),
 * c->next_temp_id is reset to the top of the compact space so
 * optimizer-phase allocations stay inside the <=65,534 budget.
 *
 * An independent verification (pairwise disjoint extended ranges per
 * new id) runs BEFORE the rewrite; any violation is an internal error
 * - we refuse to emit, never guess.
 */

#include "zero_cc/opt/opt_temp_renumber.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/live_range.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/semantic/diagnostics.h"

#define TR_ID_LIMIT 65536  /* new ids must stay < this (16-bit user temp budget) */
#define TR_NO_POS    ((size_t)-1)

/* Free-list pool key: (reg class, type tag). The class field indexes up to
 * REG_CLASS_GPR64 (=4); size the pool for 8 classes to leave headroom for
 * future classes (a class >= TR_POOL_COUNT/256 would overflow pool_head). */
#define TR_POOL_KEY(cls, tag) ((size_t)(cls) * 256 + (size_t)(tag))
#define TR_POOL_COUNT (8 * 256)

int opt_temp_renumber(ZeroCCCompiler *c, const char *fn_name) {
    if (!c) return 0;
    const char *fn = fn_name ? fn_name : "<anon>";
    size_t n = ir_get_count(c);
    if (n == 0) return 1;  /* empty function: nothing to do */

    uint32_t max_id = (uint32_t)c->next_temp_id;  /* ids are 16..max_id-1 */
    if (max_id <= VREG_USER_START) return 1;

    /* ---- 1. the LRA's own view: extended intervals per temp --------- */
    LiveRangeAnalyzer lra;
    live_range_init(&lra);
    live_range_scan_ir(&lra, ir_get(c, 0), n, c);
    RegAllocator *scratch = (RegAllocator *)calloc(1, sizeof(RegAllocator));
    if (!scratch) {
        live_range_free(&lra);
        return 1;  /* OOM: leave churned ids in place */
    }
    regalloc_init(scratch);
    ZeroCCISA isa = c->regalloc.target_isa ? c->regalloc.target_isa : (c->isa ? c->isa : ISA_X86_64);
    regalloc_set_isa(scratch, isa);
    live_range_compute(&lra, scratch, c);

    size_t *r_start = (size_t *)malloc(max_id * sizeof(size_t));
    size_t *r_end = (size_t *)malloc(max_id * sizeof(size_t));
    int8_t *r_class = (int8_t *)malloc(max_id);
    uint8_t *r_tag = (uint8_t *)malloc(max_id);
    int32_t *head_start = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *head_end = (int32_t *)malloc(n * sizeof(int32_t));
    int32_t *next_start = (int32_t *)malloc(max_id * sizeof(int32_t));
    int32_t *next_end = (int32_t *)malloc(max_id * sizeof(int32_t));
    uint32_t *ids = (uint32_t *)malloc(max_id * sizeof(uint32_t));
    uint32_t *new_id = (uint32_t *)calloc(max_id, sizeof(uint32_t));
    int32_t *pool_next = (int32_t *)malloc(max_id * sizeof(int32_t));
    int32_t *pool_head = (int32_t *)malloc(TR_POOL_COUNT * sizeof(int32_t));
    uint16_t *newid_pool = (uint16_t *)malloc(max_id * sizeof(uint16_t));
    if (!r_start || !r_end || !r_class || !r_tag || !head_start || !head_end ||
        !next_start || !next_end || !ids || !new_id ||
        !pool_next || !pool_head || !newid_pool) {
        goto oom;  /* leave the (valid) churned ids in place */
    }
    for (uint32_t t = 0; t < max_id; t++) {
        r_start[t] = TR_NO_POS;
        r_end[t] = 0;
        r_class[t] = -1;
        r_tag[t] = 0;
    }
    memset(pool_head, 0xFF, TR_POOL_COUNT * sizeof(int32_t));  /* -1 */

    /* Union of all intervals per temp (a param pre-coloring guard adds
     * a second, subsumed interval for the same temp - the union is the
     * allocator's exact view). */
    for (size_t i = 0; i < scratch->interval_count; i++) {
        const LiveInterval *iv = &scratch->intervals[i];
        uint32_t t = (uint32_t)iv->temp_id;
        if (t < VREG_USER_START || t >= max_id) continue;  /* fixed vregs */
        if (r_start[t] == TR_NO_POS) {
            r_start[t] = iv->start;
            r_end[t] = iv->end;
            r_class[t] = (int8_t)iv->reg_class;
        } else {
            if (iv->start < r_start[t]) r_start[t] = iv->start;
            if (iv->end > r_end[t]) r_end[t] = iv->end;
            if (r_class[t] != (int8_t)iv->reg_class) {
                diag_report_error_at_token(c,
                    "internal error: conflicting reg classes for temp %u in "
                    "function '%s' - refusing to emit", t, fn);
                goto fail;
            }
        }
    }

    /* Tag per old temp from the advisory table (high ids were never
     * tagged - tag 0 = unknown, which every consumer handles
     * conservatively). */
    for (uint32_t t = VREG_USER_START; t < max_id; t++)
        r_tag[t] = (t < 65536 && c->temp_type_tags) ? c->temp_type_tags[t] : 0;

    /* Bucket referenced ids by extended-range start / end. */
    memset(head_start, 0xFF, n * sizeof(int32_t));  /* -1 sentinels */
    memset(head_end, 0xFF, n * sizeof(int32_t));
    uint32_t id_count = 0;
    for (uint32_t t = VREG_USER_START; t < max_id; t++) {
        if (r_start[t] == TR_NO_POS) continue;
        if (r_start[t] >= n || r_end[t] >= n) {
            diag_report_error_at_token(c,
                "internal error: temp %u range [%zu..%zu] outside function "
                "'%s' - refusing to emit", t, r_start[t], r_end[t], fn);
            goto fail;
        }
        ids[id_count] = t;
        next_start[id_count] = (int32_t)head_start[r_start[t]];
        head_start[r_start[t]] = (int32_t)id_count;
        next_end[id_count] = (int32_t)head_end[r_end[t]];
        head_end[r_end[t]] = (int32_t)id_count;
        id_count++;
    }
    if (id_count == 0) {
        goto done_clean;
    }

    /* ---- 2. linear scan: (class, tag) pools -------------------------- */
    uint32_t next_fresh = VREG_USER_START;
    for (size_t p = 0; p < n; p++) {
        /* Allocate at this position FIRST (see header comment). */
        for (int32_t k = head_start[p]; k >= 0; k = next_start[k]) {
            uint32_t t = ids[k];
            size_t pool = TR_POOL_KEY(r_class[t], r_tag[t]);
            uint32_t ni;
            if (pool_head[pool] >= 0) {
                ni = (uint32_t)pool_head[pool];
                pool_head[pool] = pool_next[ni];
            } else {
                ni = next_fresh++;
            }
            new_id[t] = ni;
            newid_pool[ni] = (uint16_t)pool;
        }
        /* Free at this position AFTER (usable from p+1 on). */
        for (int32_t k = head_end[p]; k >= 0; k = next_end[k]) {
            uint32_t t = ids[k];
            uint32_t ni = new_id[t];
            size_t pool = TR_POOL_KEY(r_class[t], r_tag[t]);
            pool_next[ni] = pool_head[pool];
            pool_head[pool] = (int32_t)ni;
        }
    }
    /* ZC_RENUMBER_DEBUG=1: report the churn -> peak-live compaction so
     * churn-stress tests can assert they actually exceed the 65,534
     * 16-bit-era budget (test/jit/temp_renumber_churn.c). */
    if (getenv("ZC_RENUMBER_DEBUG"))
        fprintf(stderr, "[renumber] %s: %zu instrs, churn %u -> peak %u\n",
                fn, n, max_id - VREG_USER_START, next_fresh - VREG_USER_START);

    if (next_fresh >= TR_ID_LIMIT) {
        /* Peak-live temps exceeded the 16-bit-era budget - the same
         * resource limit the C2b guard enforced, now on PEAK live
         * usage rather than total churn. */
        diag_report_error_at_token(c,
            "function '%s' keeps %u temporaries simultaneously live; max is "
            "%d (16-bit temp id space). Split the function into smaller "
            "helper functions.",
            fn, next_fresh - 1, TR_ID_LIMIT - 1);
        goto fail;
    }

    /* ---- 3. independent verification BEFORE the rewrite ------------- */
    {
        uint32_t max_new = next_fresh - 1;
        int32_t *vhead = (int32_t *)malloc((max_new + 1) * sizeof(int32_t));
        int32_t *vnext = (int32_t *)malloc(max_id * sizeof(int32_t));
        size_t *vstart = (size_t *)malloc(max_id * sizeof(size_t));
        size_t *vend = (size_t *)malloc(max_id * sizeof(size_t));
        if (!vhead || !vnext || !vstart || !vend) goto oom;
        memset(vhead, 0xFF, (max_new + 1) * sizeof(int32_t));
        for (uint32_t k = 0; k < id_count; k++) {
            uint32_t t = ids[k];
            uint32_t ni = new_id[t];
            vstart[k] = r_start[t];
            vend[k] = r_end[t];
            vnext[k] = vhead[ni];
            vhead[ni] = (int32_t)k;
        }
        int bad = 0;
        size_t *a_start = (size_t *)malloc((id_count + 1) * sizeof(size_t));
        size_t *a_end = (size_t *)malloc((id_count + 1) * sizeof(size_t));
        if (!a_start || !a_end) bad = 1;
        for (uint32_t ni = VREG_USER_START; ni <= max_new && !bad; ni++) {
            uint32_t m = 0;
            for (int32_t k = vhead[ni]; k >= 0; k = vnext[k]) {
                a_start[m] = vstart[k];
                a_end[m] = vend[k];
                m++;
            }
            for (uint32_t a = 0; a < m && !bad; a++) {
                for (uint32_t b = a + 1; b < m; b++) {
                    if (!(a_end[a] < a_start[b] || a_end[b] < a_start[a])) {
                        bad = 1;  /* overlapping extended ranges share an id */
                        break;
                    }
                }
            }
        }
        free(a_start); free(a_end);
        free(vhead); free(vnext); free(vstart); free(vend);
        if (bad) {
            diag_report_error_at_token(c,
                "internal error: temp renumbering verification failed for "
                "function '%s' - refusing to emit", fn);
            goto fail;
        }
    }

    /* ---- 4. rewrite all temp references ------------------------------ */
    for (size_t i = 0; i < n; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        uint32_t f;
        f = ins->temp_dest;
        /* OP_CALL/OP_SYSCALL/OP_CLONE pack arg_count in temp_dest; OP_ASM
         * packs the operand count (Sprint 154) - neither is a temp id. */
        if (ins->op != OP_CALL && ins->op != OP_SYSCALL && ins->op != OP_CLONE &&
            ins->op != OP_ASM &&
            zc_temp_is_user(f)) {
            if (f >= max_id || !new_id[f]) {
                diag_report_error_at_token(c,
                    "internal error: untracked temp %u in function '%s' - "
                    "refusing to emit", f, fn);
                goto fail;
            }
            ins->temp_dest = new_id[f];
        }
        f = ins->temp_src1;
        if (zc_temp_is_user(f)) {
            if (f >= max_id || !new_id[f]) {
                diag_report_error_at_token(c,
                    "internal error: untracked temp %u in function '%s' - "
                    "refusing to emit", f, fn);
                goto fail;
            }
            ins->temp_src1 = new_id[f];
        }
        f = ins->temp_src2;
        if (zc_temp_is_user(f)) {
            if (f >= max_id || !new_id[f]) {
                diag_report_error_at_token(c,
                    "internal error: untracked temp %u in function '%s' - "
                    "refusing to emit", f, fn);
                goto fail;
            }
            ins->temp_src2 = new_id[f];
        }
        if (ins->op == OP_ATOMIC_CMPXCHG || ins->op == OP_ATOMIC_CMPXCHG_BOOL) {
            f = (uint32_t)ins->imm64;
            if (zc_temp_is_user(f)) {
                if (f >= max_id || !new_id[f]) {
                    diag_report_error_at_token(c,
                        "internal error: untracked temp %u in atomic cmpxchg of "
                        "function '%s' - refusing to emit", f, fn);
                    goto fail;
                }
                ins->imm64 = (int64_t)new_id[f];
            }
        }
    }
    for (size_t a = 0; a < c->asm_operand_count; a++) {
        int t = c->asm_operand_temps[a];
        if (t >= 0 && zc_temp_is_user((uint32_t)t)) {
            if ((uint32_t)t >= max_id || !new_id[(uint32_t)t]) {
                diag_report_error_at_token(c,
                    "internal error: untracked temp %d in asm operands of "
                    "function '%s' - refusing to emit", t, fn);
                goto fail;
            }
            c->asm_operand_temps[a] = (int)new_id[(uint32_t)t];
        }
    }

    /* Remap the type-tag table. Pooling by (class, tag) guarantees every
     * new id has exactly one tag (its pool's), so the remap is exact. */
    if (c->temp_type_tags) {
        uint8_t *merged = (uint8_t *)calloc(1, 65536);
        if (merged) {
            for (uint32_t k = 0; k < id_count; k++) {
                uint32_t t = ids[k];
                uint32_t ni = new_id[t];
                if (ni < TR_ID_LIMIT) merged[ni] = r_tag[t];
            }
            memcpy(c->temp_type_tags, merged, 65536);
            uint32_t hi = next_fresh < 65536 ? next_fresh : 65536;
            memset(c->temp_type_tags + hi, 0, 65536 - hi);
            c->temp_type_tags_hi = hi;
            free(merged);
        }
    }

    /* Resume the id counter at the top of the compact space so later
     * allocations (optimizer passes) stay inside the <=65,534 budget. */
    c->next_temp_id = (int)next_fresh;

done_clean:
    live_range_free(&lra);
    if (scratch) { regalloc_destroy(scratch); free(scratch); }
    free(r_start); free(r_end); free(r_class); free(r_tag);
    free(head_start); free(head_end);
    free(next_start); free(next_end);
    free(ids); free(new_id);
    free(pool_next); free(pool_head); free(newid_pool);
    return 1;

oom:
    live_range_free(&lra);
    if (scratch) { regalloc_destroy(scratch); free(scratch); }
    free(r_start); free(r_end); free(r_class); free(r_tag);
    free(head_start); free(head_end);
    free(next_start); free(next_end);
    free(ids); free(new_id);
    free(pool_next); free(pool_head); free(newid_pool);
    return 0;

fail:
    live_range_free(&lra);
    if (scratch) { regalloc_destroy(scratch); free(scratch); }
    free(r_start); free(r_end); free(r_class); free(r_tag);
    free(head_start); free(head_end);
    free(next_start); free(next_end);
    free(ids); free(new_id);
    free(pool_next); free(pool_head); free(newid_pool);
    return 0;
}
