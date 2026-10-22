# ZeroCC Coding Style

Style reference for contributors and agents. This records the conventions the
codebase actually follows (verified against the tree, Sprint 42); where the
existing code is mixed, the guide prescribes the form for **new and touched
code** — do not churn untouched files to reflow them.

Build/verification rules live in [`AGENTS.md`](../AGENTS.md) §2; architectural
invariants live in the OKF bundle ([`docs/okf/index.md`](okf/index.md)).

## 1. Directory layout

```
include/              # public API surface (flat by design — see §2.3)
  zero_cc.h           #   umbrella header: the only one external embedders need
  zero_cc_fwd.h       #   forward typedefs for embedders that can't take the umbrella
  threads.h           #   virtual C11 <threads.h> (system-header emulation — must stay root-level)
src/
  main.c              # CLI entry point
  zero_cc/            # all implementation; private headers co-located with sources
    core/             #   preprocessor, compiler state, pragma state
    lexer/  parser/   #   tokenizing + AST
    semantic/         #   type checking, scopes, diagnostics
    ir/               #   IR, lowering, machine emitter, regalloc
    opt/              #   optimization passes (one file per pass)
    isa/              #   per-ISA backends (x86_64, aarch64, riscv) + _asm.c text emitters
    jit/              #   in-memory JIT, ELF writers, type bridge
    common/  types/   #   shared utilities, token/symbol tables
test/<category>/      # regression tests; every test/*.c is auto-built by `make all`
                      # (wildcard — no Makefile edit needed for new tests)
tools/                # Python verifiers/harnesses (gate runners live here)
docs/                 # plans, changelog, this file
docs/okf/             # OKF v0.2 knowledge bundle (the architecture knowledge graph)
```

Module rule: one directory per concern under `src/zero_cc/`; a new concern
gets its own directory, not a sibling pile in an existing one.

## 2. Includes

### 2.1 The convention (enforced since Sprint 42)

All project headers are included **relative to the include roots**
(`-Isrc -Iinclude` in the Makefile):

- `src/zero_cc/core/core.h` → `#include "zero_cc/core/core.h"`
- `include/zero_cc.h` → `#include "zero_cc.h"`

Rules:
- **No `../` or `./` navigation** in project includes (fragile under file moves).
- **No bare flat names for subfolder files** — `#include "ir.h"` from
  `zero_cc/opt/` is banned; write `#include "zero_cc/ir/ir.h"`. Bare names are
  only for files that actually sit at an include root.
- Project headers use **quoted** form; system/external headers use **angled**
  form (`<stdio.h>`, `<elf.h>`). Never angle-bracket a project header.
- One known exemption class: dormant freestanding placeholders for headers
  not yet created (`"zero_elf.h"`, `"zero_posix_baremetal.h"`) — they stay
  bare until the zero_rt phase creates the headers, then get root paths.

### 2.2 Include order (prescribed for new/touched files)

1. The file's own sibling header (if any — e.g. `foo.c` includes `foo.h`).
2. System/external headers (`<...>`).
3. Project headers (`"zero_cc/..."`), umbrella/dependency-first.

The existing tree is mixed on order (≈half the files lead with project
headers); do not reflow untouched files, but new files and files you are
already editing should follow the order above.

### 2.3 Why `include/` is flat (design decision, Sprint 42)

The common "put public headers in `include/<project>/`" pattern is
**deliberately not used**:

- `threads.h` emulates the *system* `<threads.h>`; it must resolve as a bare
  root name for test binaries compiled with host gcc + `-Iinclude`.
- `zero_cc.h` is a single-file umbrella API; the collision risk the
  subdirectory pattern guards against only materializes when multiple
  projects share an include path, which this tree does not do.

**Trigger to revisit:** when a second public header family appears (e.g. the
`zero_rt` runtime API), migrate to `include/zero_cc/` + `include/zero_rt/`
in one step, keeping a `zero_cc.h` shim for compatibility.

### 2.4 `.c`-file includes

Three ISA backends `#include` their text-emitter `.c` file
(`"zero_cc/isa/riscv_asm.c"`) to pull static emission helpers into the raw
emitter's translation unit. This is an established pattern; keep it
root-relative like everything else.

## 3. Naming

| Thing | Convention | Examples |
|---|---|---|
| Top-level public functions | `<module>_<action>` — per-module prefixes, never bare words | core library `zero_cc_preprocess_ext2`; `opt_licm`, `diag_report`, `type_is_void`, `stmt_handle_block`, `ir_get`, `lower_expr`, `emit_ir_to_asm`, `elf_writer_write`, `vectorization_analyze_full`, `riscv64_emit_call` / `aarch64_*` / `x86_*` for ISA backends, `pthread_*` for the `<threads.h>` emulation (deliberately mirroring the system API) |
| Module-private functions | `static`, module-prefixed | `static void riscv64_emit_call(...)`, `static int opt_licm(...)` |
| Structs / typedefs | `ZeroCC<PascalCase>` | `ZeroCCPreprocessor`, `ZeroCCIncludeDirs` |
| Macros / constants | `ZERO_CC_*` / module `SCREAMING_SNAKE` | `ZERO_CC_MAX_RELOCS`, `VREG_0`, `OP_MUL` |
| Enum tags | module `SCREAMING_SNAKE` or `ZeroCC<Pascal>` | `TYPE_INT32`, `ZeroCCISA` |
| Files | `snake_case.c` / `snake_case.h` | `ast_lowering_expr.c` |
| Header guards | `#pragma once` or `ZERO_CC_<PATH>_H` (either accepted; every `.h` must have one) | `ZERO_CC_CORE_PREPROCESSOR_H` |

Prefix discipline: a top-level non-`static` function is *module API* and
must carry its module prefix (`<module>_<action>`); everything else in a
module is `static` (or module-prefixed when it must be shared within a
`.c`-include pair). A new module gets its own prefix — do not invent
bare-word public functions. `tools/style_check.py` (rule NAM001) enforces
the prefix presence.

## 4. Code style

- **4-space indent, K&R braces** (`if (x) {`), one statement per line.
- **`/* */` comments** are the house style (~90% of the tree); `//` is
  tolerated in small doses but prefer `/* */` for anything multi-line.
- **Section banners** for long files: `/* ---- subtopic ---- */` or
  `/* ===== SECTION ===== */`.
- **Header-only implementation** via `static inline` functions is the norm
  for `core/`, `jit/`, and table-driven helpers (e.g. `preprocessor.h` is
  ~1600 lines of `static inline`). Keep a header includable standalone: it
  must not depend on include order (forward typedefs live in
  `include/zero_cc_fwd.h` / module headers).
- **`bool`** from `<stdbool.h>`, not 0/1 ints, for flags/returns.
- **Error handling**: functions return `NULL`/`false` on failure and the
  caller decides the diagnostic; user-facing CLI errors go to `stderr` in
  the `Compile error: ...` / `<file>:<line>: <kind>: <msg>` shape (match
  gcc's `<file>:<line>: fatal error:` wording for fatal front-end errors).
- **Fixed-capacity arrays** carry an explicit `ZERO_CC_MAX_*` limit and the
  bounded-write discipline (`snprintf` with the cap, never unchecked
  `sprintf`); dynamic growth uses the project's TPHT/arena patterns rather
  than ad-hoc `realloc` loops.
- **No compiler warnings** on a clean `make rebuild` (the tree is warning-free
  under `-Wall -Wextra`; keep it that way).

## 5. Style checker (`tools/style_check.py`)

Ruff-style, stdlib-only, fast — codifies the rules in this file so drift is
catchable in seconds:

```bash
python3 tools/style_check.py            # check src/ include/ (errors gate, warnings report)
python3 tools/style_check.py --fix      # repair trailing whitespace + missing EOF newline
python3 tools/style_check.py --quiet    # failures only
```

Rules: include conventions (no `../`/`./`, no bare subfolder names, no
angled project headers, order as warning), tab-free indentation,
trailing whitespace, EOF newline, header-guard presence, the
`<module>_<action>` public-function prefix (NAM001), and unbounded
string calls (STR010, warning). Exit code reflects errors only; the
warning set (include order, `strcpy`/`strcat`/`sprintf`) tracks accepted
legacy instances so the tree is not churned.

## 6. IR / optimization / codegen invariants

Do not duplicate invariant knowledge here — the OKF bundle is the source of
truth:
- IR opcodes, `IRCallArgs`, reserved physical registers → `docs/okf/ir-and-regalloc.md`
- Active passes and safety rules (LICM multi-def guard, DCE fixpoint
  liveness, mem2reg slot audit) → `docs/okf/optimization-passes.md`
- ABI / sret rules → `docs/okf/abi-sysv-x86_64.md`
- Per-ISA raw-emitter state → `docs/okf/isa-backends.md`

Any change to a pass, lowering, or emitter must pass the gates in
`AGENTS.md` §2 (`make test`, `make test-opt` for optimizer changes, the
QEMU gate for raw-emitter changes).

## 7. Commits & documentation discipline

- **Commit message**: `type(scope): imperative summary` —
  `fix(riscv):`, `feat(c23):`, `style(build):`, `test:`, `tools:`. Body
  explains root cause + verification results (the repo convention; see
  recent `git log`).
- **Sprint numbering**: ongoing work groups under a sprint number; each
  landed unit gets a `## [Sprint N] - <date> - <title>` entry in
  `docs/CHANGELOG.md` (newest first) with a verification-results block.
- **OKF upkeep** (mandatory for any architectural change — see
  `AGENTS.md` §3): update the relevant concept page(s), then
  `python3 tools/okf.py update <page.md> --log "..."` and
  `python3 tools/okf.py lint && python3 tools/okf.py index`.
- **New regression tests**: drop them in `test/<category>/` (auto-picked by
  the Makefile wildcard); for preprocessor/frontend behavior prefer a
  library-API test (host-gcc-built, runs under `make test`) plus a direct
  `./build/bin/zerocc` JIT+AOT check of the CLI behavior (per
  `AGENTS.md` §2).
