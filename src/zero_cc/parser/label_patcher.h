/* Label & Branch Patcher - Unified Patching API for Task 62
 *
 * This header provides a unified interface for label resolution and
 * branch patching, consolidating epilogue, break/continue, and goto
 * patching into a cohesive system.
 */

#ifndef ZERO_CC_PARSER_LABEL_PATCHER_H
#define ZERO_CC_PARSER_LABEL_PATCHER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Forward declarations */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Loop context - tracks break/continue targets for nested loops */
typedef struct {
    uint16_t break_label;     /* Label for break statements */
    uint16_t continue_label;  /* Label for continue statements */
} LoopContext;

/* User label table - maps user-defined labels to IR labels */
typedef struct {
    char name[64];
    uint16_t ir_label;
} UserLabel;

/* Label Patcher State - unified label/patching management */
typedef struct {
    /* Loop contexts for nested break/continue */
    LoopContext loop_stack[16];
    int loop_depth;
    
    /* User-defined labels (for goto) */
    UserLabel user_labels[256];
    int user_label_count;
} LabelPatcherState;

/* Initialize label patcher state */
static inline void label_patcher_init(LabelPatcherState *st) {
    memset(st, 0, sizeof(*st));
}

/* Enter a new loop scope */
static inline void label_patcher_enter_loop(LabelPatcherState *st, uint16_t break_label, uint16_t continue_label) {
    if (st->loop_depth < 16) {
        st->loop_stack[st->loop_depth].break_label = break_label;
        st->loop_stack[st->loop_depth].continue_label = continue_label;
        st->loop_depth++;
    }
}

/* Exit current loop scope */
static inline void label_patcher_exit_loop(LabelPatcherState *st) {
    if (st->loop_depth > 0) {
        st->loop_depth--;
    }
}

/* Get current loop's break label */
static inline uint16_t label_patcher_get_break_label(LabelPatcherState *st) {
    if (st->loop_depth > 0) {
        return st->loop_stack[st->loop_depth - 1].break_label;
    }
    return 0;
}

/* Get current loop's continue label */
static inline uint16_t label_patcher_get_continue_label(LabelPatcherState *st) {
    if (st->loop_depth > 0) {
        return st->loop_stack[st->loop_depth - 1].continue_label;
    }
    return 0;
}

/* Find or create a user label (for goto) */
static inline uint16_t label_patcher_find_or_create_user_label(LabelPatcherState *st, const char *name) {
    for (int i = 0; i < st->user_label_count; i++) {
        if (strcmp(st->user_labels[i].name, name) == 0) {
            return st->user_labels[i].ir_label;
        }
    }
    /* Create new label */
    if (st->user_label_count < 256) {
        int idx = st->user_label_count++;
        strncpy(st->user_labels[idx].name, name, 63);
        st->user_labels[idx].name[63] = '\0';
        /* Caller should assign ir_label */
        return st->user_labels[idx].ir_label;
    }
    return 0;
}

#endif /* ZERO_CC_PARSER_LABEL_PATCHER_H */
