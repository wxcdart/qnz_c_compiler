#ifndef ZERO_CC_PARSER_OPT_PASS_H
#define ZERO_CC_PARSER_OPT_PASS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/**
 * Optimization levels
 */
typedef enum {
    OPT_LEVEL_NONE = 0,    // -O0: No optimizations
    OPT_LEVEL_O1 = 1,      // -O1: Constant folding, DCE
    OPT_LEVEL_O2 = 2,      // -O2: + CSE, algebraic simplification
    OPT_LEVEL_O3 = 3,      // -O3: + LICM, strength reduction
} OptLevel;

/**
 * Optimization pass descriptor
 */
typedef struct OptPass {
    const char *name;
    void (*run)(ZeroCCCompiler *c);
    int enabled_by_default;
    int priority;  // Lower = run first
} OptPass;

/**
 * Initialize optimization system with given level
 */
void opt_init(OptLevel level);

/**
 * Run all enabled optimization passes
 */
void opt_run_passes(ZeroCCCompiler *c);

/**
 * Shutdown optimization system
 */
void opt_shutdown(void);

/**
 * Get current optimization level
 */
OptLevel opt_get_level(void);

/**
 * Check if optimization is enabled
 */
int opt_is_enabled(OptLevel required_level);

#endif /* ZERO_CC_PARSER_OPT_PASS_H */
