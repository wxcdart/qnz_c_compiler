#include "opt_pass.h"
#include "opt_constant_fold.h"
#include "opt_dce.h"
#include "opt_cse.h"
#include "ir.h"
#include "regalloc.h"

static OptLevel g_opt_level = OPT_LEVEL_NONE;

static void opt_pass_constant_fold(ZeroCCCompiler *c);
static void opt_pass_dce(ZeroCCCompiler *c);
static void opt_pass_cse(ZeroCCCompiler *c);

// Enable all passes: constant folding + DCE + CSE
static const OptPass g_opt_passes[] = {
    { "constant_fold", opt_pass_constant_fold, 1, 10 },
    { "dce", opt_pass_dce, 1, 30 },
    { "cse", opt_pass_cse, 1, 40 },
};

#define OPT_PASS_COUNT (sizeof(g_opt_passes) / sizeof(g_opt_passes[0]))

void opt_init(OptLevel level) { g_opt_level = level; }
void opt_run_passes(ZeroCCCompiler *c) {
    int changed = 1;
    int iteration = 0;
    const int max_iterations = 10;

    while (changed && iteration < max_iterations) {
        changed = 0;
        iteration++;

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
}
void opt_shutdown(void) { g_opt_level = OPT_LEVEL_NONE; }
OptLevel opt_get_level(void) { return g_opt_level; }
int opt_is_enabled(OptLevel required_level) { return g_opt_level >= required_level; }

static void opt_pass_constant_fold(ZeroCCCompiler *c) { opt_fold_constants(c); }
static void opt_pass_dce(ZeroCCCompiler *c) { opt_eliminate_dead_code(c); }
static void opt_pass_cse(ZeroCCCompiler *c) { opt_eliminate_cse(c); }
