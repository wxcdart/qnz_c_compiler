/*
 * CFG layer over the flat IR (Sprint 35 Phase 2).
 * See cfg.h for the invariants.  The CFG is a pure view: it reads the
 * IRArena, never writes to it.
 */
#include "zero_cc/opt/cfg.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/types/types.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/types/symbols.h"

/* Terminal instructions end a basic block. */
static bool cfg_is_terminal(int op) {
    switch (op) {
        case OP_JMP:
        case OP_JMP_INDIRECT:
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

void zero_cc_cfg_bump_version(ZeroCCCompiler *c) {
    if (c) c->cfg_version++;
}

uint64_t zero_cc_cfg_version(ZeroCCCompiler *c) {
    return c ? c->cfg_version : 0;
}

void cfg_invalidate(ZeroCCCompiler *c) {
    zero_cc_cfg_bump_version(c);
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
    free(cfg->idom);
    free(cfg->post_idom);
    cfg->idom = NULL;
    cfg->post_idom = NULL;
    cfg->valid = false;
}

void zero_cc_cfg_cache_free(ZeroCCCFG *cfg) {
    cfg_free(cfg);
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

bool cfg_build(ZeroCCCompiler *c) {
    if (!c) return false;
    if (c->cfg_cache.valid && c->cfg_cache.version == c->cfg_version) {
        return true;
    }

    size_t count = ir_get_count(c);
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
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx > max_label) {
            max_label = (int)ins->label_idx;
        }
    }
    int *label_to_pos = (int *)malloc(((size_t)max_label + 1) * sizeof(int));
    if (!label_to_pos) return false;
    for (int i = 0; i <= max_label; i++) label_to_pos[i] = -1;
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
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
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_LABEL) leader[i] = true;
        if (i + 1 < count) {
            if (cfg_is_terminal((int)ins->op)) {
                leader[i + 1] = true;
            } else if (ins->op == OP_ASM) {
                /* Sprint 154: is_goto is packed into the anchor's cond
                 * (bit 1). */
                if (ins->cond & 0x02) leader[i + 1] = true;
            }
        }
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
        Instr *first = ir_get(c, blocks[b].start);
        if (first && first->op == OP_LABEL) {
            int t = (int)first->label_idx;
            if (t >= 0 && t <= max_label) label_to_block[t] = b;
        }
    }

    /* ---- edges ---- */
    for (int b = 0; b < nblocks; b++) {
        size_t last = blocks[b].end - 1;
        Instr *t = ir_get(c, last);
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
        } else if (op == OP_JMP_INDIRECT) {
            /* Indirect jump can target any block that starts with an OP_LABEL */
            for (int tb = 0; tb < nblocks; tb++) {
                Instr *first = ir_get(c, blocks[tb].start);
                if (first && first->op == OP_LABEL) {
                    cfg_add_succ(&blocks[b], tb, nblocks);
                }
            }
        } else if (op == OP_ASM) {
            /* Sprint 154: goto labels are the OP_ASM_LABEL run following
             * the OP_ASM_ARG run. */
            int op_count = (int)t->temp_dest;
            int gcount = (int)t->width;
            for (int gi = 0; gi < gcount; gi++) {
                const Instr *lg = ir_get(c, last + 1 + (size_t)op_count + (size_t)gi);
                if (!lg || lg->op != OP_ASM_LABEL) break;
                int lid = (int)lg->label_idx;
                if (lid >= 0 && lid <= max_label) {
                    int pos = label_to_pos[lid];
                    if (pos >= 0) cfg_add_succ(&blocks[b], label_to_block[lid], nblocks);
                }
            }
            cfg_add_succ(&blocks[b], b + 1, nblocks); /* fallthrough */
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

/* ---- Dominator / post-dominator dataflow (Sprint 149, plan item 8a) ----
 *
 * Classic meet-over-predecessors dataflow run to fixpoint over dominator
 * SETS (one bitset per block):
 *
 *   D(entry) = {entry}
 *   D(b)     = {b} U (intersection of D(p) over the reachable preds of b)
 *   idom(b)  = the reverse-postorder-deepest member of D(b) \ {b}
 *
 * Dominators strictly precede their dominatees in reverse postorder, so
 * the deepest member of the dominator set (other than b itself) is b's
 * immediate dominator.  The worklist is seeded with the reachable blocks
 * in RPO order and requeues the successors of any block whose set
 * shrinks; sets only lose bits, so the pass terminates (O(V*E*V/64)
 * word ops worst case - negligible at function scale).
 *
 * Post-dominators are dominators on the REVERSE CFG with a synthetic entry
 * X (index = block_count): every exit block (no successors) gains X as a
 * reverse predecessor.  a post-dominates b in the original CFG iff a
 * dominates b in the reverse one.  A block whose only post-dominator is
 * itself (an exit block, or a block sitting directly above a fork to
 * several exits) has reverse idom X and is reported as itself, mirroring
 * the idom(entry) == entry convention.  Blocks that cannot reach any exit
 * (reverse-unreachable) get -1.
 */
typedef struct {
    int nb;               /* real blocks: 0..nb-1 */
    int entry;            /* entry of the graph analysed (0 or nb = X) */
    const int **preds;    /* preds[i] for i in [0, nb+1) (NULL ok if count 0) */
    const int *pred_counts;
} ZeroCCIdomGraph;

/* Compute the immediate-dominator array of the single-entry graph
 * described by g.  out must hold nb+1 ints.  Blocks unreachable from the
 * entry get -1; out[entry] == entry.  Returns false on OOM. */
static bool cfg_idom_pass(const ZeroCCIdomGraph *g, int *out) {
    int total = g->nb + 1;
    bool *reach = NULL;
    int *succ_cnt = NULL, *succ_cur = NULL;
    int **succs = NULL;
    int *rpo = NULL, *rpo_block = NULL;
    int *dfs_state = NULL, *dfs_next = NULL, *dfs_stack = NULL, *post = NULL;
    int rpo_count = 0;
    uint64_t *D = NULL, *tmp = NULL;
    int *wl = NULL;
    bool *in_wl = NULL;
    size_t words = 0;

    for (int b = 0; b < total; b++) out[b] = -1;

    /* --- successor lists: invert the predecessor lists --- */
    reach = (bool *)calloc((size_t)total, 1);
    succ_cnt = (int *)calloc((size_t)total, sizeof(int));
    succs = (int **)calloc((size_t)total, sizeof(int *));
    if (!reach || !succ_cnt || !succs) goto fail;
    for (int v = 0; v < total; v++) {
        for (int k = 0; k < g->pred_counts[v]; k++) {
            int p = g->preds[v][k];
            if (p < 0 || p >= total) goto fail; /* corrupt input */
            succ_cnt[p]++;
        }
    }
    for (int v = 0; v < total; v++) {
        if (succ_cnt[v] > 0) {
            succs[v] = (int *)malloc((size_t)succ_cnt[v] * sizeof(int));
            if (!succs[v]) goto fail;
        }
    }
    succ_cur = (int *)calloc((size_t)total, sizeof(int));
    if (!succ_cur) goto fail;
    for (int v = 0; v < total; v++) {
        for (int k = 0; k < g->pred_counts[v]; k++) {
            int p = g->preds[v][k];
            succs[p][succ_cur[p]++] = v;
        }
    }

    /* --- reachability from entry (BFS over successors) --- */
    {
        int *queue = (int *)malloc((size_t)total * sizeof(int));
        if (!queue) goto fail;
        int qh = 0, qt = 0;
        reach[g->entry] = true;
        queue[qt++] = g->entry;
        while (qh < qt) {
            int u = queue[qh++];
            for (int k = 0; k < succ_cnt[u]; k++) {
                int v = succs[u][k];
                if (!reach[v]) {
                    reach[v] = true;
                    queue[qt++] = v;
                }
            }
        }
        free(queue);
    }

    /* --- reverse postorder from entry (iterative DFS, then reverse) --- */
    rpo = (int *)malloc((size_t)total * sizeof(int));
    rpo_block = (int *)malloc((size_t)total * sizeof(int));
    dfs_state = (int *)calloc((size_t)total, sizeof(int));
    dfs_next = (int *)calloc((size_t)total, sizeof(int));
    dfs_stack = (int *)malloc((size_t)total * sizeof(int));
    post = (int *)malloc((size_t)total * sizeof(int));
    if (!rpo || !rpo_block || !dfs_state || !dfs_next || !dfs_stack || !post) {
        goto fail;
    }
    for (int b = 0; b < total; b++) rpo[b] = -1;
    {
        int sp = 0, pc = 0;
        dfs_state[g->entry] = 1;
        dfs_stack[sp++] = g->entry;
        while (sp > 0) {
            int u = dfs_stack[sp - 1];
            if (dfs_next[u] < succ_cnt[u]) {
                int v = succs[u][dfs_next[u]++];
                if (dfs_state[v] == 0) {
                    dfs_state[v] = 1;
                    dfs_stack[sp++] = v;
                }
            } else {
                dfs_state[u] = 2;
                sp--;
                post[pc++] = u;
            }
        }
        rpo_count = pc;
        for (int k = 0; k < pc; k++) {
            rpo[post[pc - 1 - k]] = k;
            rpo_block[k] = post[pc - 1 - k];
        }
    }

    /* --- dominator sets: meet over reachable preds to fixpoint --- */
    words = ((size_t)total + 63) / 64;
    if ((size_t)total * words * 8 > (64u << 20)) goto fail; /* soft cap */
    {
        uint64_t last_mask = ((size_t)total % 64 == 0) ? ~0ULL
                                                       : ((1ULL << (size_t)(total % 64)) - 1);
        D = (uint64_t *)calloc((size_t)total, words * 8);
        tmp = (uint64_t *)calloc(words, 8);
        if (!D || !tmp) goto fail;
        for (int b = 0; b < total; b++) {
            if (!reach[b]) continue;
            uint64_t *Db = D + (size_t)b * words;
            if (b == g->entry) {
                Db[b >> 6] |= 1ULL << (b & 63);
            } else {
                for (size_t w = 0; w < words; w++) Db[w] = ~0ULL;
                Db[words - 1] &= last_mask;
            }
        }
        /* Worklist: reachable blocks in RPO order.  Circular buffer of
         * capacity total; in_wl makes duplicates impossible, so it can
         * never fill up. */
        wl = (int *)malloc((size_t)total * sizeof(int));
        in_wl = (bool *)calloc((size_t)total, 1);
        if (!wl || !in_wl) goto fail;
        int wh = 0, wt = 0;
        for (int k = 0; k < rpo_count; k++) {
            int b = rpo_block[k];
            if (b == g->entry) continue;
            wl[wt % total] = b;
            in_wl[b] = true;
            wt++;
        }
        while (wh < wt) {
            int b = wl[wh % total];
            in_wl[b] = false;
            wh++;
            bool have = false;
            for (int k = 0; k < g->pred_counts[b]; k++) {
                int p = g->preds[b][k];
                if (!reach[p]) continue;
                if (!have) {
                    memcpy(tmp, D + (size_t)p * words, words * 8);
                    have = true;
                } else {
                    const uint64_t *Dp = D + (size_t)p * words;
                    for (size_t w = 0; w < words; w++) tmp[w] &= Dp[w];
                }
            }
            if (!have) continue; /* no reachable pred: keep current set */
            tmp[b >> 6] |= 1ULL << (b & 63);
            uint64_t *Db = D + (size_t)b * words;
            bool changed = false;
            for (size_t w = 0; w < words; w++) {
                if (tmp[w] != Db[w]) {
                    changed = true;
                    break;
                }
            }
            if (!changed) continue;
            memcpy(Db, tmp, words * 8);
            for (int k = 0; k < succ_cnt[b]; k++) {
                int s = succs[b][k];
                if (reach[s] && s != g->entry && !in_wl[s]) {
                    wl[wt % total] = s;
                    in_wl[s] = true;
                    wt++;
                }
            }
        }
        /* Extract idom: deepest (max RPO) member of D(b) \ {b}.  Every
         * reachable b != entry has at least the entry in D(b). */
        for (int b = 0; b < total; b++) {
            if (!reach[b]) continue;
            if (b == g->entry) {
                out[b] = g->entry;
                continue;
            }
            const uint64_t *Db = D + (size_t)b * words;
            int best = -1, best_rpo = -1;
            for (size_t w = 0; w < words; w++) {
                uint64_t x = Db[w];
                while (x) {
                    uint64_t low = x & (~x + 1);
                    int d = (int)(w * 64) + __builtin_ctzll(low);
                    x &= x - 1;
                    if (d == b) continue;
                    if (rpo[d] > best_rpo) {
                        best_rpo = rpo[d];
                        best = d;
                    }
                }
            }
            out[b] = best; /* >= 0: the entry is in every D(b) */
        }
    }
    goto done;

fail:
    for (int v = 0; v < total; v++) free(succs ? succs[v] : NULL);
    free(succs);
    free(reach);
    free(succ_cnt);
    free(succ_cur);
    free(rpo);
    free(rpo_block);
    free(dfs_state);
    free(dfs_next);
    free(dfs_stack);
    free(post);
    free(D);
    free(tmp);
    free(wl);
    free(in_wl);
    return false;
done:
    for (int v = 0; v < total; v++) free(succs ? succs[v] : NULL);
    free(succs);
    free(reach);
    free(succ_cnt);
    free(succ_cur);
    free(rpo);
    free(rpo_block);
    free(dfs_state);
    free(dfs_next);
    free(dfs_stack);
    free(post);
    free(D);
    free(tmp);
    free(wl);
    free(in_wl);
    return true;
}

static bool cfg_compute_idom(ZeroCCCFG *cfg, int *out) {
    int n = cfg->block_count;
    if (n == 0) {
        out[0] = -1;
        return true;
    }
    const int **pl = (const int **)malloc(((size_t)n + 1) * sizeof(int *));
    int *pc = (int *)malloc(((size_t)n + 1) * sizeof(int));
    if (!pl || !pc) {
        free(pl);
        free(pc);
        return false;
    }
    for (int b = 0; b < n; b++) {
        pl[b] = cfg->blocks[b].preds;
        pc[b] = cfg->blocks[b].pred_count;
    }
    pl[n] = NULL; /* the forward graph has no synthetic block */
    pc[n] = 0;
    ZeroCCIdomGraph g = { n, 0, pl, pc };
    bool ok = cfg_idom_pass(&g, out);
    free((void *)pl);
    free(pc);
    return ok;
}

static bool cfg_compute_post_idom(ZeroCCCFG *cfg, int *out) {
    int n = cfg->block_count;
    if (n == 0) {
        out[0] = -1;
        return true;
    }
    int X = n; /* synthetic entry above all exits */
    bool ok = false;
    const int **pl = (const int **)calloc((size_t)n + 1, sizeof(int *));
    int *pc = (int *)malloc(((size_t)n + 1) * sizeof(int));
    if (!pl || !pc) {
        free(pl);
        free(pc);
        return false;
    }
    for (int b = 0; b < n; b++) {
        int sc = cfg->blocks[b].succ_count;
        /* Reverse preds of b = original successors, plus X when b is an
         * exit block (the synthetic entry feeds it). */
        int *arr = (int *)malloc(((size_t)sc + 1) * sizeof(int));
        if (!arr) goto done;
        for (int k = 0; k < sc; k++) arr[k] = cfg->blocks[b].succ[k];
        pl[b] = arr;
        if (sc == 0) {
            arr[0] = X;
            pc[b] = 1;
        } else {
            pc[b] = sc;
        }
    }
    pl[n] = NULL; /* X itself has no predecessors (it is the entry) */
    pc[n] = 0;
    ZeroCCIdomGraph g = { n, X, pl, pc };
    ok = cfg_idom_pass(&g, out);
    if (ok) {
        /* A reverse idom of X means "no closer post-dominator exists"
         * (exit blocks, and blocks directly above a fork to several
         * exits): report the block itself. */
        for (int b = 0; b < n; b++) {
            if (out[b] == X) out[b] = b;
        }
    }
done:
    for (int b = 0; b < n; b++) free((void *)pl[b]);
    free((void *)pl);
    free(pc);
    return ok;
}

/* Lazily compute (once per cached snapshot) the idom / post_idom arrays.
 * Both live in the ZeroCCCFG and are freed with it. */
static bool cfg_ensure_idom(ZeroCCCompiler *c, bool postdom, int **slot) {
    if (!c || !cfg_build(c)) return false;
    ZeroCCCFG *cfg = &c->cfg_cache;
    if (*slot) return true;
    int *out = (int *)malloc(((size_t)cfg->block_count + 1) * sizeof(int));
    if (!out) return false;
    bool ok = postdom ? cfg_compute_post_idom(cfg, out)
                      : cfg_compute_idom(cfg, out);
    if (!ok) {
        free(out);
        return false;
    }
    *slot = out;
    return true;
}

const int *cfg_idom(ZeroCCCompiler *c) {
    if (!c) return NULL;
    return cfg_ensure_idom(c, false, &c->cfg_cache.idom)
               ? c->cfg_cache.idom : NULL;
}

const int *cfg_post_idom(ZeroCCCompiler *c) {
    if (!c) return NULL;
    return cfg_ensure_idom(c, true, &c->cfg_cache.post_idom)
               ? c->cfg_cache.post_idom : NULL;
}

/* Walk the idom chain from b up to the entry; true iff a is met.  The
 * chain terminates (idom[entry] == entry, and every other entry points to
 * a strictly earlier RPO block); the step cap is corruption insurance. */
static bool cfg_idom_chain_contains(const int *chain, int a, int b, int nblocks) {
    if (chain[b] < 0) return false;
    int x = b;
    for (int steps = 0; steps <= nblocks + 2; steps++) {
        if (x == a) return true;
        if (x == chain[x]) break; /* reached the root (entry / self) */
        x = chain[x];
        if (x < 0) return false;
    }
    return false;
}

bool cfg_block_dominates(const ZeroCCCFG *cfg, int a, int b) {
    if (!cfg || !cfg->idom) return false;
    if (a < 0 || a >= cfg->block_count || b < 0 || b >= cfg->block_count) {
        return false;
    }
    if (a == b) return true;
    return cfg_idom_chain_contains(cfg->idom, a, b, cfg->block_count);
}

bool cfg_block_post_dominates(const ZeroCCCFG *cfg, int a, int b) {
    if (!cfg || !cfg->post_idom) return false;
    if (a < 0 || a >= cfg->block_count || b < 0 || b >= cfg->block_count) {
        return false;
    }
    if (a == b) return true;
    return cfg_idom_chain_contains(cfg->post_idom, a, b, cfg->block_count);
}

void cfg_dump(ZeroCCCompiler *c, FILE *out) {
    if (!out) return;
    if (!c || !cfg_build(c)) return;
    ZeroCCCFG *cfg = &c->cfg_cache;
    const int *idom = cfg_idom(c);
    const int *pidom = cfg_post_idom(c);
    bool have_dom = (idom && pidom);
    const char *fn_name = "<anon>";
    if (c->function_count > 0 && c->lowering.current_func_idx < c->function_count) {
        fn_name = c->functions[c->lowering.current_func_idx].name;
    }
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
        fprintf(out, " loop_header=%d", (int)blk->is_loop_header);
        if (have_dom) {
            if (idom[b] < 0) {
                fprintf(out, " idom=-");
            } else {
                fprintf(out, " idom=bb%d", idom[b]);
            }
            if (pidom[b] < 0) {
                fprintf(out, " post_idom=-");
            } else {
                fprintf(out, " post_idom=bb%d", pidom[b]);
            }
        }
        fprintf(out, "\n");
    }
    fprintf(out, "loops:\n");
    for (int l = 0; l < cfg->loop_count; l++) {
        fprintf(out, "  bb%d->bb%d\n", cfg->loops[l].backedge_from, cfg->loops[l].header);
    }
    fprintf(out, "%d blocks, %d reachable, %d loops\n",
            cfg->block_count, reachable, cfg->loop_count);
}
