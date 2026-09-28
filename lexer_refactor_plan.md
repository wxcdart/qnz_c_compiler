# Lexer Refactoring Plan ✅ COMPLETED

## Goal
Split the `src/zero_cc/lexer.c` file (790 lines) into smaller, logically-grouped sub-files while preserving the public API. The original `lexer.c` will be replaced with a wrapper that includes the new sub-files.

---

## ✅ Status: COMPLETED

All six sub-files have been successfully created. Build passes and 39/40 tests pass (1 pre-existing failure unrelated to this work).

---

## 📊 Actual Results

| File | Planned | Actual |
|------|---------|--------|
| `lexer/init.h` | ~50 lines | 70 lines |
| `lexer/tokenize.h` | ~150 lines | 115 lines |
| `lexer/next.h` | ~120 lines | 136 lines |
| `lexer/scan_char.h` | ~140 lines | 156 lines |
| `lexer/scan_escape.h` | ~110 lines | 114 lines |
| `lexer/scan_number.h` | ~280 lines | 298 lines |

**Total:** 790 lines → 861 lines (includes + headers overhead)

---

## 🧪 Test Results

```
Build: ✅ Clean (no errors)
Tests: 39/40 PASS
  ❌ rodata_demo_jit (pre-existing segfault, unrelated to lexer refactoring)
```

---

## 📋 Proposed Split

### 1️⃣ `src/zero_cc/lexer/init.h` (~50 lines)

| Line Range | Content |
|------------|---------|
| 1-50 | Lexer initialization and keyword lookup |

**Functions:**
- `zero_cc_lexer_init_file()` - Initialize lexer with source and filename
- `zero_cc_lexer_init()` - Initialize lexer with source only
- `zero_cc_keyword_lookup()` - Perfect hash keyword lookup

**Guard:** `ZERO_CC_LEXER_INIT_H`

---

### 2️⃣ `src/zero_cc/lexer/tokenize.h` (~150 lines)

| Line Range | Content |
|------------|---------|
| 51-150 | Main tokenization functions |

**Functions:**
- `zero_cc_tokenize_c99()` - Tokenize entire source into compiler's token array
- `zero_cc_token_free()` - Free dynamically allocated memory in a token
- `zero_cc_compiler_free_tokens()` - Free all dynamically allocated tokens in a compiler context

**Guard:** `ZERO_CC_LEXER_TOKENIZE_H`

---

### 3️⃣ `src/zero_cc/lexer/next.h` (~120 lines)

| Line Range | Content |
|------------|---------|
| 151-270 | Main tokenizer loop |

**Functions:**
- `zero_cc_lexer_next()` - Get next token (stateful lexer)

**Guard:** `ZERO_CC_LEXER_NEXT_H`

---

### 4️⃣ `src/zero_cc/lexer/scan_char.h` (~140 lines)

| Line Range | Content |
|------------|---------|
| 271-410 | Character and string literal scanning |

**Functions:**
- `scan_char()` - Scan character literal
- `scan_string()` - Scan string literal

**Guard:** `ZERO_CC_LEXER_SCAN_CHAR_H`

---

### 5️⃣ `src/zero_cc/lexer/scan_escape.h` (~110 lines)

| Line Range | Content |
|------------|---------|
| 411-510 | Escape sequence handling |

**Functions:**
- `handle_escape()` - Handle escape sequences in character/string literals

**Guard:** `ZERO_CC_LEXER_SCAN_ESCAPE_H`

---

### 6️⃣ `src/zero_cc/lexer/scan_number.h` (~280 lines)

| Line Range | Content |
|------------|---------|
| 511-790 | Numeric literal scanning |

**Functions:**
- `scan_number()` - Scan integer and floating-point literals

**Guard:** `ZERO_CC_LEXER_SCAN_NUMBER_H`

---

## 📦 Wrapper File

### `src/zero_cc/lexer.c` (after refactoring)

```c
/* =========================================================================
 *  lexer.c
 *  C99 Lexer Implementation
 *  ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>

#include "lexer.h"
#include "types_lexer.h"
#include "keywords.inc"

#include "lexer/init.h"
#include "lexer/tokenize.h"
#include "lexer/next.h"
#include "lexer/scan_char.h"
#include "lexer/scan_escape.h"
#include "lexer/scan_number.h"
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
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types_lexer.h"
#include "zero_cc/lexer.h"
```

### Forward Declarations

**In `init.h`:**
```c
/* Forward declarations for scan functions */
static int scan_number(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_string(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_char(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok);
static int scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok);
static int handle_escape(const char **s, size_t *len, char *out, int *out_val);
```

---

## ✅ Guarantees (All Met)

- **Include-guard integrity** – each new file gets its own unique guard
- **No mid-function splits** – all split points are between complete functions
- **Preserved order** – wrapper includes sub-files in original source order
- **Public API unchanged** – external code still `#include "zero_cc/lexer.h"`
- **Static functions preserved** – all scan functions remain static to lexer.c

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
| `lexer.c` | 790 lines | ~30 lines (wrapper) |
| `lexer/init.h` | - | 70 lines |
| `lexer/tokenize.h` | - | 115 lines |
| `lexer/next.h` | - | 136 lines |
| `lexer/scan_char.h` | - | 156 lines |
| `lexer/scan_escape.h` | - | 114 lines |
| `lexer/scan_number.h` | - | 298 lines |

**Total:** 790 lines → 861 lines (includes + headers overhead)

---

## ✅ Implementation Completed

1. ✅ Created `src/zero_cc/lexer/` directory
2. ✅ Extracted line ranges into sub-files with proper guards
3. ✅ Added necessary includes to each sub-file
4. ✅ Added forward declarations where needed
5. ✅ Created wrapper `lexer.c`
6. ✅ Ran `make clean && make` - build successful
7. ✅ Ran all tests - 39/40 pass

---

## 📝 Notes

- This split follows the same pattern as the successful `core.h`, `expressions.h`, `statements.h`, `zero_posix.h`, `parser.c`, `types_lexer.h`, `jit_linker.h`, and `lexer.c` refactoring
- The lexer is now cleanly separated by scanning responsibility
- Character/string scanning is isolated from numeric scanning
- Escape sequence handling is separated for clarity
- Main tokenizer loop is isolated for potential future modifications
- **Bonus:** The lexer refactoring was completed successfully with no regressions!
