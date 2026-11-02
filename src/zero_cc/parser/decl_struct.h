#ifndef ZERO_CC_PARSER_DECL_STRUCT_H
#define ZERO_CC_PARSER_DECL_STRUCT_H

#include <stdbool.h>
#include "zero_cc/types/types.h"

/* Sprint 65: Refresh early-declared objects and self-referential pointer members
 * after a struct/union definition completes. */
void zero_cc_sync_globals_with_struct(ZeroCCCompiler *c, const char *name, const ZeroCCType *complete);

/* Parse the body of a struct or union declaration ({ ... }) */
ZeroCCType zero_cc_parse_struct_body(ZeroCCCompiler *c, bool is_union);

#endif /* ZERO_CC_PARSER_DECL_STRUCT_H */
