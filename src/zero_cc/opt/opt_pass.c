#include "zero_cc/opt/opt_pass.h"
#include "zero_cc/opt/opt_constant_fold.h"
#include "zero_cc/opt/opt_dce.h"
#include "zero_cc/opt/opt_cse.h"
#include "zero_cc/opt/opt_mem2reg.h"
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_vectorize.h"
#include "zero_cc/opt/opt_gvn.h"
#include "zero_cc/opt/opt_branchless.h"
#include "zero_cc/opt/opt_copy_prop.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/opt/cfg.h"
#include <time.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/symbols.h"

static OptLevel g_opt_level = OPT_LEVEL_NONE;

static void opt_pass_constant_fold(ZeroCCCompiler *c);
static void opt_pass_flag_dce(ZeroCCCompiler *c);
static void opt_pass_dce(ZeroCCCompiler *c);
static void opt_pass_mem2reg(ZeroCCCompiler *c);
static void opt_pass_copy_prop(ZeroCCCompiler *c);
static void opt_pass_cse(ZeroCCCompiler *c);
static void opt_pass_gvn(ZeroCCCompiler *c);
static void opt_pass_licm(ZeroCCCompiler *c);
static void opt_pass_branchless(ZeroCCCompiler *c);
static void opt_pass_strength_reduction(ZeroCCCompiler *c);
static void opt_pass_branch_simplify(ZeroCCCompiler *c);
static void opt_pass_auto_vectorize(ZeroCCCompiler *c);

// Enable passes: constant folding -> flag DCE -> DCE -> mem2reg -> CSE -> GVN -> LICM -> strength reduction -> branch simplify -> auto-vectorize
// Note: mem2reg now enabled with safe VLA handling
static const OptPass g_opt_passes[] = {
    { "constant_fold", opt_pass_constant_fold, 1, 10, OPT_LEVEL_O1 },
    { "flag_dce", opt_pass_flag_dce, 1, 20, OPT_LEVEL_O1 },
    { "dce", opt_pass_dce, 1, 30, OPT_LEVEL_O1 },
    { "copy_prop", opt_pass_copy_prop, 1, 35, OPT_LEVEL_O1 },
    { "mem2reg", opt_pass_mem2reg, 1, 40, OPT_LEVEL_O1 },
    { "cse", opt_pass_cse, 1, 45, OPT_LEVEL_O2 },
    { "gvn", opt_pass_gvn, 1, 50, OPT_LEVEL_O2 },
    { "licm", opt_pass_licm, 1, 55, OPT_LEVEL_O2 },
    { "branchless", opt_pass_branchless, 1, 58, OPT_LEVEL_O3 },
    { "strength_reduction", opt_pass_strength_reduction, 1, 60, OPT_LEVEL_O3 },
    { "branch_simplify", opt_pass_branch_simplify, 1, 65, OPT_LEVEL_O3 },
    { "auto_vectorize", opt_pass_auto_vectorize, 1, 70, OPT_LEVEL_O3 },
};

#define OPT_PASS_COUNT (sizeof(g_opt_passes) / sizeof(g_opt_passes[0]))

void opt_init(OptLevel level) { g_opt_level = level; }
void opt_run_passes(ZeroCCCompiler *c) {
    int changed = 1;
    int iteration = 0;
    const int max_iterations = 10;

    /* Profile: record initial instruction count */
    opt_profile_set_instructions_before((size_t)ir_get_count());

    while (changed && iteration < max_iterations) {
        changed = 0;
        iteration++;

        /* Profile: count optimization iterations */
        opt_profile_count_iteration();

        for (size_t i = 0; i < OPT_PASS_COUNT; i++) {
            const OptPass *pass = &g_opt_passes[i];
            int enabled = pass->enabled_by_default;
            if (!enabled) continue;

            /* Check if this pass is enabled at the current optimization level */
            if (!opt_is_enabled(pass->min_level)) continue;

            int before_count = (int)ir_get_count();
            /* Per-pass wall time (Sprint 35 Phase 4): only when profiling
             * is enabled (--profile-opts). */
            struct timespec t0, t1;
            if (opt_profile_is_enabled()) clock_gettime(CLOCK_MONOTONIC, &t0);
            pass->run(c);
            if (opt_profile_is_enabled()) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                opt_profile_time_pass(i, pass->name,
                    (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                    (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6);
            }
            int after_count = (int)ir_get_count();

            /* The pass may have rewritten the IR in place (DCE compaction,
             * mem2reg forwarding, LICM rebuild): invalidate the cached CFG
             * (Sprint 35 Phase 2). */
            c->cfg_version++;

            if (before_count != after_count) {
                changed = 1;
            }
        }
    }

    /* Profile: record final instruction count */
    opt_profile_set_instructions_after((size_t)ir_get_count());
}
void opt_shutdown(void) { g_opt_level = OPT_LEVEL_NONE; }
OptLevel opt_get_level(void) { return g_opt_level; }
int opt_is_enabled(OptLevel required_level) { return g_opt_level >= required_level; }

static void opt_pass_constant_fold(ZeroCCCompiler *c) { opt_fold_constants(c); }
static void opt_pass_flag_dce(ZeroCCCompiler *c) { opt_flag_dce(c); }
static void opt_pass_dce(ZeroCCCompiler *c) { opt_eliminate_dead_code(c); }
static void opt_pass_copy_prop(ZeroCCCompiler *c) { opt_copy_propagate(c); }
static void opt_pass_mem2reg(ZeroCCCompiler *c) { opt_promote_mem2reg(c); }
static void opt_pass_cse(ZeroCCCompiler *c) { opt_eliminate_cse(c); }
static void opt_pass_gvn(ZeroCCCompiler *c) { opt_eliminate_gvn(c); }
static void opt_pass_licm(ZeroCCCompiler *c) { opt_licm(c); }
static void opt_pass_branchless(ZeroCCCompiler *c) { opt_branchless_select(c); }
static void opt_pass_strength_reduction(ZeroCCCompiler *c) { opt_strength_reduction(c); }
static void opt_pass_branch_simplify(ZeroCCCompiler *c) { opt_branch_simplify(c); }
static void opt_pass_auto_vectorize(ZeroCCCompiler *c) { opt_auto_vectorize(c); }
