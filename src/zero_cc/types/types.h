/* ==========================================================================
 *  types_lexer.h
 *  Type System, Token Definitions, and Lexer Declarations
 *  ==========================================================================
 *
 *  INCLUDE ORDER DEPENDENCIES:
 *  ===========================
 *  This header must be included in the following order (enforced below):
 *
 *  1. types/token.h      - Token types (ZeroCCToken, ZeroCCTokType)
 *  2. types/type_system.h - Type enums and ZeroCCType struct
 *  3. types/symbols.h    - ZeroCCCompiler struct (depends on token.h, type_system.h)
 *  4. types/type_funcs.h - Type helper functions (depends on type_system.h)
 *  5. types/registry.h   - Symbol registry functions (depends on symbols.h)
 *  6. types/emit_helpers.h - Code emission helpers (depends on buffers.h)
 *  7. types/symbol_mgmt.h - Symbol management (depends on symbols.h)
 *
 *  WARNING: Do not change the include order. Several headers depend on
 *  types defined in earlier includes.
 *  ==========================================================================*/

#ifndef ZERO_CC_TYPES_H
#define ZERO_CC_TYPES_H

#include "zero_cc/types/token.h"
#include "zero_cc/types/type_system.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/types/type_funcs.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/emit_helpers.h"
#include "zero_cc/types/symbol_mgmt.h"

#endif /* ZERO_CC_TYPES_H */
