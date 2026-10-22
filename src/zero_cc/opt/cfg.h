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
} ZeroCCCFG;

/* Build (or return the cached) CFG for the current function's IR.
 * Returns false on OOM. */
bool cfg_build(void);

/* Explicit invalidation - call after mutating the IR outside the
 * version-bump hooks (ir_reset, optimizer passes, callee-save append). */
void cfg_invalidate(void);

/* Release the cached CFG snapshot (blocks, per-block pred arrays, loops).
 * Public so zero_cc_compiler_free can tear down the last-built cache. */
void zero_cc_cfg_cache_free(ZeroCCCFG *cfg);

/* Bump the IR version counter (used by the invalidation hooks). */
void zero_cc_cfg_bump_version(void);

/* Current IR version counter. */
uint64_t zero_cc_cfg_version(void);

/* Stable plain-text dump:
 *   fn <name>
 *   bb<i>: [<start>..<end-1>] succ=<bb,...> pred=<bb,...> loop_header=<0|1>
 *   loops:
 *     bb<from>->bb<header>
 *   <n> blocks, <r> reachable, <l> loops
 */
void cfg_dump(FILE *out);

#endif /* ZERO_CC_OPT_CFG_H */
