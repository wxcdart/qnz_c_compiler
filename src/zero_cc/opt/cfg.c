/*
 * CFG layer over the flat IR (Sprint 35 Phase 2).
 * See cfg.h for the invariants.  The CFG is a pure view: it reads the
 * IRArena, never writes to it.
 */
#include "zero_cc/opt/cfg.h"
#include "zero_cc/types/types.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/types/symbols.h"
#include <stdlib.h>
#include <string.h>

/* External reference to the global compiler pointer (defined in ir.c). */
extern __thread ZeroCCCompiler *g_compiler;

/* Terminal instructions end a basic block. */
static bool cfg_is_terminal(int op) {
    switch (op) {
        case OP_JMP:
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
        case OP_RET:
        case OP_EPILOGUE:
            return true;
        default:
            return false;
    }
}

/* Conditional/branch instructions with an explicit label target. */
static bool cfg_is_branch(int op) {
    switch (op) {
        case OP_JE:
        case OP_JNE:
        case OP_JL:
        case OP_JLE:
        case OP_JG:
        case OP_JCOND:
            return true;
        default:
            return false;
    }
}

void zero_cc_cfg_bump_version(void) {
    if (g_compiler) g_compiler->cfg_version++;
}

uint64_t zero_cc_cfg_version(void) {
    return g_compiler ? g_compiler->cfg_version : 0;
}

void cfg_invalidate(void) {
    zero_cc_cfg_bump_version();
}

static void cfg_free(ZeroCCCFG *cfg) {
    if (!cfg->blocks) return;
    for (int b = 0; b < cfg->block_count; b++) {
        free(cfg->blocks[b].preds);
    }
    free(cfg->blocks);
    cfg->blocks = NULL;
    free(cfg->loops);
    cfg->loops = NULL;
    cfg->block_count = 0;
    cfg->loop_count = 0;
    cfg->valid = false;
}

static void cfg_add_pred(ZeroCCBlock *blk, int pred) {
    if (blk->pred_count >= blk->pred_cap) {
        int nc = blk->pred_cap ? blk->pred_cap * 2 : 4;
        int *np = (int *)realloc(blk->preds, (size_t)nc * sizeof(int));
        if (!np) return;
        blk->preds = np;
        blk->pred_cap = nc;
    }
    blk->preds[blk->pred_count++] = pred;
}

static void cfg_add_succ(ZeroCCBlock *blk, int succ, int nblocks) {
    if (succ < 0 || succ >= nblocks || blk->succ_count >= CFG_MAX_SUCCS) return;
    for (int i = 0; i < blk->succ_count; i++) {
        if (blk->succ[i] == succ) return; /* dedup (branch target == fallthrough) */
    }
    blk->succ[blk->succ_count++] = succ;
}

bool cfg_build(void) {
    ZeroCCCompiler *c = g_compiler;
    if (!c) return false;
    if (c->cfg_cache.valid && c->cfg_cache.version == c->cfg_version) {
        return true;
    }

    size_t count = ir_get_count();
    ZeroCCCFG *cfg = &c->cfg_cache;
    cfg_free(cfg);

    if (count == 0) {
        cfg->valid = true;
        cfg->version = c->cfg_version;
        return true;
    }

    /* ---- label id -> instruction position (first occurrence) ---- */
    int max_label = 0;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx > max_label) {
            max_label = (int)ins->label_idx;
        }
    }
    int *label_to_pos = (int *)malloc(((size_t)max_label + 1) * sizeof(int));
    if (!label_to_pos) return false;
    for (int i = 0; i <= max_label; i++) label_to_pos[i] = -1;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (ins && ins->op == OP_LABEL) {
            int t = (int)ins->label_idx;
            if (t >= 0 && t <= max_label && label_to_pos[t] < 0) label_to_pos[t] = (int)i;
        }
    }

    /* ---- leaders: 0, every OP_LABEL, instruction after a terminal ---- */
    bool *leader = (bool *)calloc(count, sizeof(bool));
    ZeroCCBlock *blocks = NULL;
    ZeroCCLoop *loops = NULL;
    if (!leader) { free(label_to_pos); return false; }
    leader[0] = true;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(i);
        if (!ins) continue;
        if (ins->op == OP_LABEL) leader[i] = true;
        if (i + 1 < count && cfg_is_terminal((int)ins->op)) leader[i + 1] = true;
    }

    /* ---- build blocks (contiguous leader..leader-1 ranges) ---- */
    int nblocks = 0;
    for (size_t i = 0; i < count; i++) {
        if (leader[i]) nblocks++;
    }
    if (nblocks == 0) nblocks = 1;
    blocks = (ZeroCCBlock *)calloc((size_t)nblocks, sizeof(ZeroCCBlock));
    if (!blocks) { free(leader); free(label_to_pos); return false; }
    int bi = 0;
    for (size_t i = 0; i < count;) {
        size_t start = i;
        i++;
        while (i < count && !leader[i]) i++;
        blocks[bi].start = start;
        blocks[bi].end = i;
        blocks[bi].succ_count = 0;
        blocks[bi].pred_count = 0;
        blocks[bi].preds = NULL;
        blocks[bi].pred_cap = 0;
        blocks[bi].reachable = false;
        blocks[bi].is_loop_header = false;
        bi++;
    }

    /* ---- label id -> block index ---- */
    int *label_to_block = (int *)malloc(((size_t)max_label + 1) * sizeof(int));
    if (!label_to_block) { free(leader); free(label_to_pos); cfg_free(cfg); return false; }
    for (int i = 0; i <= max_label; i++) label_to_block[i] = -1;
    for (int b = 0; b < nblocks; b++) {
        Instr *first = ir_get(blocks[b].start);
        if (first && first->op == OP_LABEL) {
            int t = (int)first->label_idx;
            if (t >= 0 && t <= max_label) label_to_block[t] = b;
        }
    }

    /* ---- edges ---- */
    for (int b = 0; b < nblocks; b++) {
        size_t last = blocks[b].end - 1;
        Instr *t = ir_get(last);
        if (!t) continue;
        int op = (int)t->op;
        if (op == OP_JMP) {
            int target = -1;
            int lid = (int)t->label_idx;
            if (lid >= 0 && lid <= max_label) {
                int pos = label_to_pos[lid];
                if (pos >= 0) target = label_to_block[lid];
            }
            cfg_add_succ(&blocks[b], target, nblocks);
        } else if (cfg_is_branch(op)) {
            int lid = (int)t->label_idx;
            if (lid >= 0 && lid <= max_label) {
                int pos = label_to_pos[lid];
                if (pos >= 0) cfg_add_succ(&blocks[b], label_to_block[lid], nblocks);
            }
            cfg_add_succ(&blocks[b], b + 1, nblocks); /* fallthrough (clamped/deduped) */
        } else if (op == OP_RET || op == OP_EPILOGUE) {
            /* no successors */
        } else {
            cfg_add_succ(&blocks[b], b + 1, nblocks); /* fallthrough (clamped/deduped) */
        }
    }
    /* pred lists */
    for (int b = 0; b < nblocks; b++) {
        for (int s = 0; s < blocks[b].succ_count; s++) {
            cfg_add_pred(&blocks[blocks[b].succ[s]], b);
        }
    }

    /* ---- reachability from block 0 (BFS) ---- */
    int *queue = (int *)malloc((size_t)nblocks * sizeof(int));
    if (queue) {
        int qh = 0, qt = 0;
        blocks[0].reachable = true;
        queue[qt++] = 0;
        while (qh < qt) {
            int u = queue[qh++];
            for (int s = 0; s < blocks[u].succ_count; s++) {
                int v = blocks[u].succ[s];
                if (v >= 0 && v < nblocks && !blocks[v].reachable) {
                    blocks[v].reachable = true;
                    queue[qt++] = v;
                }
            }
        }
    }
    free(queue);

    /* ---- backedges and loops: an edge u->v with blocks[v].start <=
     *      blocks[u].start is a backedge; its target is a loop header.
     *      (Matches the natural-loop detection the legacy passes do.) ---- */
    int max_loops = 0;
    for (int b = 0; b < nblocks; b++) {
        for (int s = 0; s < blocks[b].succ_count; s++) {
            int v = blocks[b].succ[s];
            if (v >= 0 && v < nblocks && blocks[v].start <= blocks[b].start) max_loops++;
        }
    }
    if (max_loops > 0) {
        loops = (ZeroCCLoop *)calloc((size_t)max_loops, sizeof(ZeroCCLoop));
        if (loops) {
            int lc = 0;
            for (int b = 0; b < nblocks; b++) {
                for (int s = 0; s < blocks[b].succ_count; s++) {
                    int v = blocks[b].succ[s];
                    if (v >= 0 && v < nblocks && blocks[v].start <= blocks[b].start) {
                        loops[lc].header = v;
                        loops[lc].backedge_from = b;
                        blocks[v].is_loop_header = true;
                        lc++;
                    }
                }
            }
            cfg->loop_count = lc;
        }
    }

    free(leader);
    free(label_to_pos);
    free(label_to_block);

    cfg->blocks = blocks;
    cfg->block_count = nblocks;
    cfg->loops = loops;
    cfg->valid = true;
    cfg->version = c->cfg_version;
    return true;
}

void cfg_dump(FILE *out) {
    if (!out) return;
    ZeroCCCompiler *c = g_compiler;
    if (!c || !cfg_build()) return;
    ZeroCCCFG *cfg = &c->cfg_cache;
    const char *fn_name = "<anon>";
    if (c->function_count > 0) fn_name = c->functions[c->function_count - 1].name;
    fprintf(out, "fn %s\n", fn_name);
    int reachable = 0;
    for (int b = 0; b < cfg->block_count; b++) {
        ZeroCCBlock *blk = &cfg->blocks[b];
        if (blk->reachable) reachable++;
        size_t last = blk->end > blk->start ? blk->end - 1 : blk->start;
        fprintf(out, "bb%d: [%zu..%zu]", b, blk->start, last);
        fprintf(out, " succ=");
        for (int s = 0; s < blk->succ_count; s++) {
            fprintf(out, "%sbb%d", s ? "," : "", blk->succ[s]);
        }
        fprintf(out, " pred=");
        for (int p = 0; p < blk->pred_count; p++) {
            fprintf(out, "%sbb%d", p ? "," : "", blk->preds[p]);
        }
        fprintf(out, " loop_header=%d\n", (int)blk->is_loop_header);
    }
    fprintf(out, "loops:\n");
    for (int l = 0; l < cfg->loop_count; l++) {
        fprintf(out, "  bb%d->bb%d\n", cfg->loops[l].backedge_from, cfg->loops[l].header);
    }
    fprintf(out, "%d blocks, %d reachable, %d loops\n",
            cfg->block_count, reachable, cfg->loop_count);
}
