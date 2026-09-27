# 5‑to‑600‑Line Refactor Plan

## Goal
Break the four massive header files in **src/zero_cc** into smaller, logically‑grouped sub‑headers (≈ 400‑600 lines each) while preserving the public API. The original wrapper headers (`core.h`, `expressions.h`, `zero_posix.h`, `statements.h`) must remain unchanged except for replacing their bodies with a series of `#include` directives that reference the new files in the exact original order.

---

## 1️⃣ `src/zero_cc/core.h` (≈ 1200 lines)
| New Sub‑header | Line range (original) | Guard | Content |
|---|---|---|---|
| `src/zero_cc/core/utils.h` | 1‑200 | `ZERO_CC_CORE_UTILS_H` | Inline utilities (`zero_strlcpy`), architecture macros, `ZeroPtrStack`. |
| `src/zero_cc/core/buffers.h` | 201‑400 | `ZERO_CC_CORE_BUFFERS_H` | `ZeroCCBuffer` definition + helper functions. |
| `src/zero_cc/core/preprocessor.h` | 401‑800 | `ZERO_CC_CORE_PREPROCESSOR_H` | Macro structs, hash tables, include‑dir handling, preprocessing state. |
| `src/zero_cc/core/compiler_state.h` | 801‑1195 | `ZERO_CC_CORE_COMPILER_STATE_H` | `ZeroCCCompiler` struct and related helper functions (`zero_cc_preprocess_ext`, …). |

**Wrapper (`core.h`)
```c
#ifndef ZERO_CC_CORE_H
#define ZERO_CC_CORE_H

#include "core/utils.h"
#include "core/buffers.h"
#include "core/preprocessor.h"
#include "core/compiler_state.h"

#endif /* ZERO_CC_CORE_H */
```

---

## 2️⃣ `src/zero_cc/expressions.h` (≈ 1670 lines)
| New Sub‑header | Line range | Guard | Content |
|---|---|---|---|
| `src/zero_cc/expressions/primary.h` | 1‑350 | `ZERO_CC_EXPRESSIONS_PRIMARY_H` | Primary‑expression parser \& type‑only parsers. |
| `src/zero_cc/expressions/unary.h` | 351‑800 | `ZERO_CC_EXPRESSIONS_UNARY_H` | Unary‑operator parsing. |
| `src/zero_cc/expressions/binary.h` | 801‑1200 | `ZERO_CC_EXPRESSIONS_BINARY_H` | Binary arithmetic/relational/logical parsing (`/* Binary Arithmetic … */`). |
| `src/zero_cc/expressions/ternary.h` | 1201‑1400 | `ZERO_CC_EXPRESSIONS_TERNARY_H` | Ternary conditional operator handling (`/* Ternary Conditional Operator … */`). |
| `src/zero_cc/expressions/visitor.h` | 1401‑1670 | `ZERO_CC_EXPRESSIONS_VISITOR_H` | Visitor / emitter sections that walk the AST and emit bytecode. |

**Wrapper (`expressions.h`)
```c
#ifndef ZERO_CC_EXPRESSIONS_H
#define ZERO_CC_EXPRESSIONS_H

#include "expressions/primary.h"
#include "expressions/unary.h"
#include "expressions/binary.h"
#include "expressions/ternary.h"
#include "expressions/visitor.h"

#endif /* ZERO_CC_EXPRESSIONS_H */
```

---

## 3️⃣ `src/zero_cc/structures/zero_posix.h` (≈ 1425 lines)
| New Sub‑header | Line range | Guard | Content |
|---|---|---|---|
| `src/zero_cc/structures/posix_error.h` | 1‑330 | `ZERO_CC_POSIX_ERROR_H` | Error constants, `zero_posix_errno`. |
| `src/zero_cc/structures/posix_mmap.h` | 331‑800 | `ZERO_CC_POSIX_MMAP_H` | Memory‑mapping, timers, nanosleep, terminal sizing. |
| `src/zero_cc/structures/posix_fileio.h` | 801‑1200 | `ZERO_CC_POSIX_FILEIO_H` | File I/O, directory handling, stream abstractions. |
| `src/zero_cc/structures/posix_system.h` | 1201‑1300 | `ZERO_CC_POSIX_SYSTEM_H` | System utilities (process exec, high‑resolution clock, `gethostname`, CPU count). |
| `src/zero_cc/structures/posix_libc.h` | 1301‑1425 | `ZERO_CC_POSIX_LIBC_H` | Misc libc wrappers (`abort`, `memcpy`, `memset`, `strcmp`, …). |

**Wrapper (`zero_posix.h`)
```c
#ifndef ZERO_POSIX_H
#define ZERO_POSIX_H

#include "structures/posix_error.h"
#include "structures/posix_mmap.h"
#include "structures/posix_fileio.h"
#include "structures/posix_system.h"
#include "structures/posix_libc.h"

#endif /* ZERO_POSIX_H */
```

---

## 4️⃣ `src/zero_cc/statements.h` (≈ 1317 lines)
| New Sub‑header | Line range | Guard | Content |
|---|---|---|---|
| `src/zero_cc/statements/definitions.h` | 1‑800 | `ZERO_CC_STATEMENTS_DEFINITIONS_H` | Parsing of typedef/struct/enum definitions, inline‑asm, compound‑block handling. |
| `src/zero_cc/statements/variables.h` | 801‑1200 | `ZERO_CC_STATEMENTS_VARIABLES_H` | Variable declarations, array handling, post‑increment/decrement of locals and struct members. |
| `src/zero_cc/statements/control.h` | 1201‑1315 | `ZERO_CC_STATEMENTS_CONTROL_H` | Control‑flow statements (`if/else`, `switch`, loops, `for`, `while`, `do‑while`, `return`, `static_assert`). |

**Wrapper (`statements.h`)
```c
#ifndef ZERO_CC_STATEMENTS_H
#define ZERO_CC_STATEMENTS_H

#include "statements/definitions.h"
#include "statements/variables.h"
#include "statements/control.h"

#endif /* ZERO_CC_STATEMENTS_H */
```

---

## ✅ Guarantees
- **Include‑guard integrity** – each new file gets its own unique guard; the original wrapper guards stay untouched.
- **No mid‑definition splits** – all split points lie between complete comment blocks or after a closing `}`/`;` so structs, enums, and macro definitions are never broken.
- **Preserved order** – wrapper headers include the new sub‑headers **exactly in the original source order**, guaranteeing the same compilation semantics.
- **Forward‑declaration safety** – if a sub‑header requires a type defined later, a minimal forward declaration will be added locally.
- **Public API unchanged** – external code can still `#include "core.h"`, `"expressions.h"`, etc., with no modifications.

---

## Next Implementation Steps
1. **Create the sub‑header files** using the line ranges above.
2. **Replace the bodies of the four wrapper headers** with the shown include blocks.
3. **Run a clean build** (`make clean && make`) to confirm there are no missing symbols.
4. If the compiler reports missing forward declarations, add them to the appropriate sub‑header.

Feel free to let me know if any part of the plan needs further tweaking!
