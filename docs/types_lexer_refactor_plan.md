# Types & Lexer Refactoring Plan ✅ COMPLETED

## Goal
Split the massive `src/zero_cc/types_lexer.h` file (631 lines) into smaller, logically-grouped sub-files while preserving the public API. The original `types_lexer.h` will be replaced with a wrapper that includes the new sub-files.

---

## ✅ Status: COMPLETED

All seven sub-files have been successfully created. Build passes and 39/40 tests pass (1 pre-existing failure unrelated to this work).

---

## 📊 Actual Results

| File | Planned | Actual |
|------|---------|--------|
| `types/token.h` | ~70 lines | 73 lines |
| `types/type_system.h` | ~105 lines | 105 lines |
| `types/symbols.h` | ~85 lines | 60 lines |
| `types/type_funcs.h` | ~100 lines | 102 lines |
| `types/registry.h` | ~105 lines | 107 lines |
| `types/emit_helpers.h` | ~105 lines | 111 lines |
| `types/symbol_mgmt.h` | ~100 lines | 105 lines |

**Total:** 631 lines → 663 lines (includes + headers overhead)

---

## 🧪 Test Results

```
Build: ✅ Clean (no errors)
Tests: 39/40 PASS
  ❌ rodata_demo_jit (pre-existing segfault, unrelated to types_lexer refactoring)
```

---

## 📋 Proposed Split

### 1️⃣ `src/zero_cc/types/token.h` (~70 lines)

| Line Range | Content |
|------------|---------|
| 1-70 | Token type definitions, token struct |

**Contents:**
- `ZeroCCTokType` enum (keywords, operators, punctuation)
- `ZeroCCToken` struct
- `ZeroCCTokenV2` typedef

**Guard:** `ZERO_CC_TYPES_TOKEN_H`

**Functions:** None (declarations only)

---

### 2️⃣ `src/zero_cc/types/type_system.h` (~105 lines)

| Line Range | Content |
|------------|---------|
| 71-170 | Type system definitions |

**Contents:**
- `ZeroCCTypeKind` enum
- `ZeroCCStructMember` struct
- `ZeroCCType` struct

**Guard:** `ZERO_CC_TYPES_TYPE_SYSTEM_H`

**Functions:** None (declarations only)

---

### 3️⃣ `src/zero_cc/types/symbols.h` (~85 lines)

| Line Range | Content |
|------------|---------|
| 171-225 | Symbol and compiler context definitions |

**Contents:**
- `ZeroCCStructDef` struct
- `ZeroCCTypedef` struct
- `ZeroCCEnumConst` struct
- `ZeroCCFunction` struct
- `ZeroCCReloc` struct
- `ZeroCCStringReloc` struct
- `ZeroCCCompiler` struct (complete definition)
- `decl_align_spec` global

**Guard:** `ZERO_CC_TYPES_SYMBOLS_H`

**Functions:** None (declarations only)

---

### 4️⃣ `src/zero_cc/types/type_funcs.h` (~100 lines)

| Line Range | Content |
|------------|---------|
| 226-322 | Type creation and manipulation functions |

**Functions:**
- `zero_cc_type_void()` - Create void type
- `zero_cc_type_bool()` - Create bool type
- `zero_cc_type_char()` - Create char type
- `zero_cc_type_uint8()` - Create uint8 type
- `zero_cc_type_int16()` - Create int16 type
- `zero_cc_type_uint16()` - Create uint16 type
- `zero_cc_type_int()` - Create int type
- `zero_cc_type_uint32()` - Create uint32 type
- `zero_cc_type_int64()` - Create int64 type
- `zero_cc_type_uint64()` - Create uint64 type
- `zero_cc_type_float()` - Create float type
- `zero_cc_type_double()` - Create double type
- `zero_cc_type_nullptr()` - Create nullptr type
- `zero_cc_type_user()` - Create user type
- `zero_cc_type_ptr()` - Create pointer type
- `zero_cc_type_array()` - Create array type
- `zero_cc_type_strip_qualifiers()` - Strip type qualifiers

**Guard:** `ZERO_CC_TYPES_TYPE_FUNCS_H`

---

### 5️⃣ `src/zero_cc/types/registry.h` (~105 lines)

| Line Range | Content |
|------------|---------|
| 323-424 | Symbol registry functions |

**Functions:**
- `zero_cc_register_struct()` - Register struct definition
- `zero_cc_find_struct()` - Find struct by name
- `zero_cc_register_typedef()` - Register typedef
- `zero_cc_find_typedef()` - Find typedef by name
- `zero_cc_register_enum_const()` - Register enum constant
- `zero_cc_find_enum_const()` - Find enum constant by name
- `zero_cc_init_standard_types()` - Initialize standard types

**Forward Declarations:**
- `void zero_cc_tokenize_c99(const char *p, ZeroCCCompiler *c);`
- `ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len);`

**Guard:** `ZERO_CC_TYPES_REGISTRY_H`

---

### 6️⃣ `src/zero_cc/types/emit_helpers.h` (~105 lines)

| Line Range | Content |
|------------|---------|
| 425-530 | Machine code emission helpers |

**Functions:**
- `zero_cc_emit_u8()` - Emit single byte
- `zero_cc_emit_u32()` - Emit 32-bit value
- `zero_cc_emit_u64()` - Emit 64-bit value
- `zero_cc_emit_prologue()` - Emit function prologue
- `zero_cc_emit_epilogue()` - Emit function epilogue
- `zero_cc_emit_mov_imm64()` - Emit mov immediate 64-bit
- `zero_cc_emit_movss_rodata()` - Emit movss from rodata
- `zero_cc_emit_movsd_rodata()` - Emit movsd from rodata
- `zero_cc_emit_lea_rax_rodata()` - Emit lea rax from rodata

**Guard:** `ZERO_CC_TYPES_EMIT_HELPERS_H`

---

### 7️⃣ `src/zero_cc/types/symbol_mgmt.h` (~100 lines)

| Line Range | Content |
|------------|---------|
| 531-631 | Symbol management and type compatibility |

**Functions:**
- `zero_cc_find_local()` - Find local variable by name
- `zero_cc_find_tls()` - Find thread-local variable by name
- `zero_cc_add_local()` - Add local variable
- `zero_cc_find_function()` - Find function by name
- `zero_cc_resolve_symbol()` - Dynamic symbol lookup for JIT
- `zero_cc_types_compatible()` - Check type compatibility

**Guard:** `ZERO_CC_TYPES_SYMBOL_MGMT_H`

---

## 📦 Wrapper File

### `src/zero_cc/types_lexer.h` (after refactoring)

```c
/* =========================================================================
 *  types_lexer.h
 *  Type System, Token Definitions, and Lexer Declarations
 *  ========================================================================= */

#ifndef ZERO_CC_TYPES_LEXER_H
#define ZERO_CC_TYPES_LEXER_H

#include "types/token.h"
#include "types/type_system.h"
#include "types/symbols.h"
#include "types/type_funcs.h"
#include "types/registry.h"
#include "types/emit_helpers.h"
#include "types/symbol_mgmt.h"

#endif /* ZERO_CC_TYPES_LEXER_H */
```

**Status:** ✅ Implemented

---

## 🔧 Implementation Details

### Required Includes

Each sub-file will need:
```c
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"  // For cross-references
```

### Forward Declarations

**In `registry.h`:**
```c
/* Lexer declarations (defined in lexer.c) */
void zero_cc_tokenize_c99(const char *p, ZeroCCCompiler *c);
ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len);
```

### Cross-References Between Files

| File Depends On | Reason |
|----------------|--------|
| `type_funcs.h` → `symbols.h` | Uses `ZeroCCType`, `ZeroCCCompiler` |
| `registry.h` → `symbols.h` | Uses `ZeroCCCompiler`, `ZeroCCType` |
| `emit_helpers.h` → `symbols.h` | Uses `ZeroCCBuffer`, `ZeroCCCompiler` |
| `symbol_mgmt.h` → `symbols.h` | Uses `ZeroCCSym`, `ZeroCCFunction`, `ZeroCCCompiler` |

Since all files include the main `types_lexer.h`, which includes all sub-files in order, forward declarations should resolve correctly.

---

## ✅ Guarantees (All Met)

- **Include-guard integrity** – each new file gets its own unique guard
- **No mid-function splits** – all split points are between complete functions/structs
- **Preserved order** – wrapper includes sub-files in original source order
- **Forward-declaration safety** – lexer declarations added where needed
- **Public API unchanged** – external code still `#include "types_lexer.h"`

---

## 🧪 Testing Strategy

1. **Build test:** `make clean && make` ✅
2. **Unit tests:** Run all existing tests ✅
3. **Regression check:** Verify no behavior changes ✅

Expected result: All tests pass (same as before refactoring) ✅

---

## 📊 File Size Comparison

| File | Before | After |
|------|--------|-------|
| `types_lexer.h` | 631 lines | ~30 lines (wrapper) |
| `types/token.h` | - | 73 lines |
| `types/type_system.h` | - | 105 lines |
| `types/symbols.h` | - | 60 lines |
| `types/type_funcs.h` | - | 102 lines |
| `types/registry.h` | - | 107 lines |
| `types/emit_helpers.h` | - | 111 lines |
| `types/symbol_mgmt.h` | - | 105 lines |

**Total:** 631 lines → 663 lines (includes + headers overhead)

---

## ✅ Implementation Completed

1. ✅ Created `src/zero_cc/types/` directory
2. ✅ Extracted line ranges into sub-files with proper guards
3. ✅ Added necessary includes to each sub-file
4. ✅ Added forward declarations for lexer functions
5. ✅ Created wrapper `types_lexer.h`
6. ✅ Ran `make clean && make` - build successful
7. ✅ Ran all tests - 39/40 pass

---

## 📝 Notes

- This split follows the same pattern as the successful `core.h`, `expressions.h`, `statements.h`, `zero_posix.h`, `parser.c`, and `types_lexer.h` refactoring
- The type system is now cleanly separated from token definitions and symbol management
- Machine code emission helpers are isolated for potential future refactoring
- The symbol management functions are separated from the registry for better organization
- All type creation functions are kept together for easy reference
- **Bonus:** The types_lexer refactoring was completed successfully with no regressions!
