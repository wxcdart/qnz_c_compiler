#ifndef ZERO_CC_OPT_CFG_H
#define ZERO_CC_OPT_CFG_H

/*
 * CFG layer over the flat, non-SSA IR (Sprint 35 Phase 2).
 *
 * A basic block is a contiguous instruction range [start, end):
 *   leaders are instruction 0, every OP_LABEL, and the instruction
 *   immediately following a terminal (OP_JMP/OP_JE/OP_JNE/OP_JL/OP_JLE/
 *   OP_JG/OP_JCOND/OP_RET/OP_EPILOGUE).  Blocks tile the instruction
 *   range exactly once, so labels belong to the block that starts at
 *   them.
 *
 * The CFG is a VIEW over the existing IRArena: no new IR, no copying.
 * It is built lazily and cached on the ZeroCCCompiler, invalidated by an
 * explicit version bump (cfg_invalidate / zero_cc_cfg_bump_version) from
 * ir_reset, the optimizer pipeline, and the callee-save append.  It is
 * never built at -O0 unless requested (e.g. -dump-cfg).
 *
 * Dominator data (Sprint 149, plan item 8a): the immediate-dominator and
 * immediate-post-dominator arrays are a second, LAZIER layer on top of the
 * cached CFG snapshot (cfg_idom / cfg_post_idom).  They are computed on
 * demand by a meet-over-predecessors dataflow (see cfg.c) and freed with
 * the snapshot, so they inherit the same versioning/invalidation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CFG_MAX_SUCCS 4

typedef struct ZeroCCBlock {
    size_t start;            /* first instruction index (inclusive) */
    size_t end;              /* one past the last instruction index */
    int succ[CFG_MAX_SUCCS]; /* successor block indices */
    int succ_count;
    int *preds;              /* predecessor block indices (dynamic) */
    int pred_count;
    int pred_cap;
    bool reachable;
    bool is_loop_header;     /* target of at least one backedge */
} ZeroCCBlock;

typedef struct ZeroCCLoop {
    int header;        /* header block (backedge target) */
    int backedge_from; /* source block of the backedge */
} ZeroCCLoop;

typedef struct ZeroCCCFG {
    ZeroCCBlock *blocks;
    int block_count;
    ZeroCCLoop *loops;
    int loop_count;
    uint64_t version;  /* IR version this snapshot was built at */
    bool valid;
    int *idom;         /* idom[b] = immediate dominator block, or -1
                        * (unreachable from entry); idom[entry] == entry.
                        * Lazy: computed by cfg_idom, freed by cfg_free. */
    int *post_idom;    /* post-dominator analogue over the reverse CFG;
                        * -1 = block cannot reach any exit; an exit block
                        * (or a block with no closer post-dominator)
                        * reports itself.  Lazy: cfg_post_idom. */
} ZeroCCCFG;

typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Build (or return the cached) CFG for the current function's IR.
 * Returns false on OOM. */
bool cfg_build(ZeroCCCompiler *c);

/* Explicit invalidation - call after mutating the IR outside the
 * version-bump hooks (ir_reset, optimizer passes, callee-save append). */
void cfg_invalidate(ZeroCCCompiler *c);

/* Release the cached CFG snapshot (blocks, per-block pred arrays, loops).
 * Public so zero_cc_compiler_free can tear down the last-built cache. */
void zero_cc_cfg_cache_free(ZeroCCCFG *cfg);

/* Bump the IR version counter (used by the invalidation hooks). */
void zero_cc_cfg_bump_version(ZeroCCCompiler *c);

/* Current IR version counter. */
uint64_t zero_cc_cfg_version(ZeroCCCompiler *c);

/* Stable plain-text dump:
 *   fn <name>
 *   bb<i>: [<start>..<end-1>] succ=<bb,...> pred=<bb,...> loop_header=<0|1>
 *          idom=<bb|- > post_idom=<bb|- >        (idom fields appended when
 *                                                  the dataflow succeeded)
 *   loops:
 *     bb<from>->bb<header>
 *   <n> blocks, <r> reachable, <l> loops
 */
void cfg_dump(ZeroCCCompiler *c, FILE *out);

/* Immediate dominators: builds (or returns the cached) CFG, then computes
 * idom lazily on the snapshot.  idom[b] = index of b's immediate-dominator
 * block; idom[0] == 0; -1 for blocks unreachable from entry.  Returns NULL
 * on OOM (the snapshot itself is still usable). */
const int *cfg_idom(ZeroCCCompiler *c);

/* Immediate post-dominators (the same dataflow over the reverse CFG with a
 * synthetic entry above all exit blocks).  post_idom[b] = the block that
 * strictly follows b on every path to any exit; -1 if b cannot reach any
 * exit; an exit block reports itself (a sink post-dominates itself). */
const int *cfg_post_idom(ZeroCCCompiler *c);

/* Block-level dominance queries over a computed snapshot (the caller must
 * have obtained the array first via cfg_idom / cfg_post_idom, e.g. after a
 * successful cfg_build in a pass).  a dominates b iff a lies on every
 * entry->b path (a == b counts); post-dominates is the same over exit
 * paths.  Both return false for out-of-range blocks and when b is
 * unreachable from entry (dominates) / cannot reach an exit (post).
 * O(chain depth) walks of the idom tree. */
bool cfg_block_dominates(const ZeroCCCFG *cfg, int a, int b);
bool cfg_block_post_dominates(const ZeroCCCFG *cfg, int a, int b);

#endif /* ZERO_CC_OPT_CFG_H */
