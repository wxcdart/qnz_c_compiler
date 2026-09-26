#include "semantic.h"
#include "semantic/visitor.h"
#include <stdio.h>

bool semantic_analyze(ZeroCCASTArena *ast_arena,
                      ASTRef root,
                      Arena *perm_arena,
                      Arena *tmp_arena,
                      DiagBuf *diags) {
    if (!ast_arena || !perm_arena || !tmp_arena || !diags) {
        return false;
    }

    SemContext ctx;
    if (!sem_context_init(&ctx, perm_arena, tmp_arena, diags, ast_arena)) {
        return false;
    }

    /* Traverse the AST starting at root */
    if (root != AST_NULL) {
        (void)sem_analyze_ref(&ctx, root);
    }

    /* Clean up context */
    sem_context_destroy(&ctx);

    /* Return true if analysis completed without errors, false otherwise */
    return !diag_has_errors(diags);
}

void semantic_print_diagnostics(const DiagBuf *diags, FILE *out) {
    if (!diags || !out) return;
    for (size_t i = 0; i < diags->count; ++i) {
        const DiagEntry *e = &diags->entries[i];
        fprintf(out, "%u:%u: %s\n", e->line, e->col, e->msg ? e->msg : "");
    }
}
