#include "opt_pass.h"
#include "opt_constant_fold.h"
#include "opt_dce.h"
#include "opt_cse.h"
#include "opt_mem2reg.h"
#include "opt_loop.h"
#include "opt_vectorize.h"
#include "opt_gvn.h"
#include "opt_copy_prop.h"
#include "opt_profile.h"
#include "ir.h"

static OptLevel g_opt_level = OPT_LEVEL_NONE;

static void opt_pass_constant_fold(ZeroCCCompiler *c);
static void opt_pass_flag_dce(ZeroCCCompiler *c);
static void opt_pass_dce(ZeroCCCompiler *c);
static void opt_pass_mem2reg(ZeroCCCompiler *c);
static void opt_pass_copy_prop(ZeroCCCompiler *c);
static void opt_pass_cse(ZeroCCCompiler *c);
static void opt_pass_gvn(ZeroCCCompiler *c);
static void opt_pass_licm(ZeroCCCompiler *c);
static void opt_pass_strength_reduction(ZeroCCCompiler *c);
static void opt_pass_branch_simplify(ZeroCCCompiler *c);
static void opt_pass_auto_vectorize(ZeroCCCompiler *c);

// Enable passes: constant folding -> flag DCE -> DCE -> mem2reg -> CSE -> GVN -> LICM -> strength reduction -> branch simplify -> auto-vectorize
// Note: mem2reg disabled pending VLA handling improvements
static const OptPass g_opt_passes[] = {
    { "constant_fold", opt_pass_constant_fold, 1, 10 },
    { "flag_dce", opt_pass_flag_dce, 1, 20 },
    { "dce", opt_pass_dce, 1, 30 },
    { "copy_prop", opt_pass_copy_prop, 1, 35 },  /* Enabled: conservative implementation */
    { "mem2reg", opt_pass_mem2reg, 0, 40 },  /* Disabled: conflicts with VLA dynamic stack */
    { "cse", opt_pass_cse, 1, 45 },
    { "gvn", opt_pass_gvn, 1, 50 },  /* Enabled: basic block GVN */
    { "licm", opt_pass_licm, 1, 50 },
    { "strength_reduction", opt_pass_strength_reduction, 1, 60 },
    { "branch_simplify", opt_pass_branch_simplify, 1, 70 },
    { "auto_vectorize", opt_pass_auto_vectorize, 0, 80 },  /* Disabled: transformation produces incorrect results */
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

        for (int i = 0; i < OPT_PASS_COUNT; i++) {
            const OptPass *pass = &g_opt_passes[i];
            int enabled = pass->enabled_by_default;
            if (!enabled) continue;

            int before_count = (int)ir_get_count();
            pass->run(c);
            int after_count = (int)ir_get_count();

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
static void opt_pass_strength_reduction(ZeroCCCompiler *c) { opt_strength_reduction(c); }
static void opt_pass_branch_simplify(ZeroCCCompiler *c) { opt_branch_simplify(c); }
static void opt_pass_auto_vectorize(ZeroCCCompiler *c) { opt_auto_vectorize(c); }
