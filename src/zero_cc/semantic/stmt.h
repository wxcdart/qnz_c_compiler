#pragma once

#include <stdbool.h>
#include "visitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Statement handlers */
bool stmt_handle_block(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_if(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_while(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_do_while(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_for(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_switch(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_case(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_default(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_break(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_continue(SemContext *ctx, ZeroCCASTNode *node);
bool stmt_handle_return(SemContext *ctx, ZeroCCASTNode *node);

#ifdef __cplusplus
}
#endif
