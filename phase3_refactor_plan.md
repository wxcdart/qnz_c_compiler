# Phase 3: Root Directory Cleanup & Reorganization Plan

## Goal
Reduce root-level file clutter in `src/zero_cc/` by organizing utility files, data structures, architecture-specific code, and platform abstractions into dedicated subdirectories.

---

## ✅ Current State

**Root level files:** 20 files (.c/.h)

| File | Purpose | Recommended Location |
|------|---------|---------------------|
| `arena.c/.h` | Memory arena utilities | `utils/` |
| `hash_table.h` | Hash table interface | `data_structures/` |
| `hash_table_tpht.c` | Double hashing implementation | `data_structures/` |
| `zero_arm64_emitter.h` | ARM64 code emitter | `arch/` |
| `zero_mmap_adapter.h` | Mmap platform adapter | `platform/` |

**Files to keep at root (wrappers/interfaces):**
- `jit_linker.h`, `lexer.c/.h`, `parser.c/.h`, `semantic.c/.h`, `types_lexer.h`, `zero_cc.h`, `zero_cc_backends.h`

---

## 📋 Proposed Restructure

### New Directory Structure

```
src/zero_cc/
├── arena.c/.h                    # → utils/arena.c/.h
├── hash_table.h                  # → data_structures/hash_table.h
├── hash_table_tpht.c             # → data_structures/hash_table_tpht.c
├── jit_linker.h                  # ✅ Keep (wrapper)
├── lexer.c                       # ✅ Keep (wrapper)
├── lexer.h                       # ✅ Keep (interface)
├── parser.c                      # ✅ Keep (wrapper)
├── parser.h                      # ✅ Keep (interface)
├── semantic.c/.h                 # ✅ Keep (entry points)
├── types_lexer.h                 # ✅ Keep (wrapper)
├── zero_cc.h                     # ✅ Keep (main header)
├── zero_cc_backends.h            # ✅ Keep (backend definitions)
│
├── core/                         # ✅ Already organized
├── expressions/                  # ✅ Already organized
├── statements/                   # ✅ Already organized
├── parser/                       # ✅ Already organized
├── types/                        # ✅ Already organized
├── jit/                          # ✅ Already organized
├── lexer/                        # ✅ Already organized
├── semantic/                     # ✅ Already organized
├── structures/                   # ✅ Already organized
├── backends/                     # ✅ Already organized
│
├── utils/                        # ← NEW: arena.c/.h
├── data_structures/              # ← NEW: hash_table.*
├── arch/                         # ← NEW: arm64_emitter.h
└── platform/                     # ← NEW: mmap_adapter.h
```

---

## 🚀 Implementation Steps

### Step 1: Create New Directories

```bash
mkdir -p src/zero_cc/utils
mkdir -p src/zero_cc/data_structures
mkdir -p src/zero_cc/arch
mkdir -p src/zero_cc/platform
```

---

### Step 2: Move Arena Files to `utils/`

**Files:**
- `arena.c` → `utils/arena.c`
- `arena.h` → `utils/arena.h`

**Include Changes Required:**
| File | Old Include | New Include |
|------|-------------|-------------|
| All files using `arena.h` | `"arena.h"` | `"utils/arena.h"` |

**Affected Files:**
- `src/zero_cc/*.c`
- `src/zero_cc/semantic/*.c`
- `src/zero_cc/parser.c`
- `src/zero_cc/jit/*.h`
- `src/zero_cc/types/*.h`

---

### Step 3: Move Hash Table Files to `data_structures/`

**Files:**
- `hash_table.h` → `data_structures/hash_table.h`
- `hash_table_tpht.c` → `data_structures/hash_table_tpht.c`

**Include Changes Required:**
| File | Old Include | New Include |
|------|-------------|-------------|
| All files using `hash_table.h` | `"hash_table.h"` | `"data_structures/hash_table.h"` |

**Affected Files:**
- `src/zero_cc/*.c`
- `src/zero_cc/structures/zero_tpht.h`
- `src/zero_cc/structures/zero_chained_tpht.h`

---

### Step 4: Move ARM64 Emitter to `arch/`

**Files:**
- `zero_arm64_emitter.h` → `arch/arm64_emitter.h`

**Include Changes Required:**
| File | Old Include | New Include |
|------|-------------|-------------|
| Files using ARM64 emitter | `"zero_arm64_emitter.h"` | `"arch/arm64_emitter.h"` |

**Affected Files:**
- Check `src/zero_cc/*.c` for includes
- Check `src/zero_cc/backends/*.c` for includes

---

### Step 5: Move Mmap Adapter to `platform/`

**Files:**
- `zero_mmap_adapter.h` → `platform/mmap_adapter.h`

**Include Changes Required:**
| File | Old Include | New Include |
|------|-------------|-------------|
| Files using mmap adapter | `"zero_mmap_adapter.h"` | `"platform/mmap_adapter.h"` |

**Affected Files:**
- `src/zero_cc/jit/compile_unit.h`
- `src/zero_cc/jit/execution.h`
- `src/zero_cc/jit/elf_emitter.h`

---

### Step 6: Update Makefile

**Changes:**
- Update object file paths for moved files
- Update dependency rules

**Current:**
```makefile
$(ARENA_OBJ): src/zero_cc/arena.c src/zero_cc/arena.h
$(HASH_TABLE_OBJ): src/zero_cc/hash_table_tpht.c src/zero_cc/hash_table.h
```

**New:**
```makefile
$(ARENA_OBJ): src/zero_cc/utils/arena.c src/zero_cc/utils/arena.h
$(HASH_TABLE_OBJ): src/zero_cc/data_structures/hash_table_tpht.c src/zero_cc/data_structures/hash_table.h
```

---

## 🔧 Detailed Implementation

### 2.1 Move Arena Files

```bash
# Create directory
mkdir -p src/zero_cc/utils

# Move files
mv src/zero_cc/arena.c src/zero_cc/utils/
mv src/zero_cc/arena.h src/zero_cc/utils/

# Update includes in arena.h (if it includes other moved files)
# Update includes in arena.c
```

**Update all references to `arena.h`:**
```bash
# Find all files that include arena.h
grep -rl '#include "arena.h"' src/zero_cc/

# Replace includes
find src/zero_cc -name "*.c" -o -name "*.h" | xargs sed -i 's|#include "arena.h"|#include "utils/arena.h"|g'
```

---

### 2.2 Move Hash Table Files

```bash
# Create directory
mkdir -p src/zero_cc/data_structures

# Move files
mv src/zero_cc/hash_table.h src/zero_cc/data_structures/
mv src/zero_cc/hash_table_tpht.c src/zero_cc/data_structures/

# Update includes in hash_table_tpht.c (if it includes hash_table.h)
```

**Update all references to `hash_table.h`:**
```bash
# Find all files that include hash_table.h
grep -rl '#include "hash_table.h"' src/zero_cc/

# Replace includes
find src/zero_cc -name "*.c" -o -name "*.h" | xargs sed -i 's|#include "hash_table.h"|#include "data_structures/hash_table.h"|g'
```

---

### 2.3 Move ARM64 Emitter

```bash
# Create directory
mkdir -p src/zero_cc/arch

# Move file
mv src/zero_cc/zero_arm64_emitter.h src/zero_cc/arch/arm64_emitter.h

# Rename file (optional, removes zero_ prefix)
# mv src/zero_cc/arch/zero_arm64_emitter.h src/zero_cc/arch/arm64_emitter.h
```

**Update all references to `zero_arm64_emitter.h`:**
```bash
# Find all files that include zero_arm64_emitter.h
grep -rl '#include "zero_arm64_emitter.h"' src/zero_cc/

# Replace includes
find src/zero_cc -name "*.c" -o -name "*.h" | xargs sed -i 's|#include "zero_arm64_emitter.h"|#include "arch/arm64_emitter.h"|g'
```

---

### 2.4 Move Mmap Adapter

```bash
# Create directory
mkdir -p src/zero_cc/platform

# Move file
mv src/zero_cc/zero_mmap_adapter.h src/zero_cc/platform/mmap_adapter.h

# Rename file (optional, removes zero_ prefix)
# mv src/zero_cc/platform/zero_mmap_adapter.h src/zero_cc/platform/mmap_adapter.h
```

**Update all references to `zero_mmap_adapter.h`:**
```bash
# Find all files that include zero_mmap_adapter.h
grep -rl '#include "zero_mmap_adapter.h"' src/zero_cc/

# Replace includes
find src/zero_cc -name "*.c" -o -name "*.h" | xargs sed -i 's|#include "zero_mmap_adapter.h"|#include "platform/mmap_adapter.h"|g'
```

---

## 🧪 Testing Strategy

### 1. Build Test
```bash
make clean && make -j4
```

Expected: Clean build with no errors

### 2. Unit Tests
```bash
make test
```

Expected: 39/40 tests pass (same as before)

### 3. Regression Check
Verify all existing functionality works:
- Compilation tests
- JIT execution tests
- ELF emission tests

---

## 📊 Expected Results

### Before
```
src/zero_cc/
├── arena.c
├── arena.h
├── hash_table.h
├── hash_table_tpht.c
├── zero_arm64_emitter.h
├── zero_mmap_adapter.h
└── ... (14 other files)
```
**Root level: 20 files**

### After
```
src/zero_cc/
├── utils/
│   ├── arena.c
│   └── arena.h
├── data_structures/
│   ├── hash_table.h
│   └── hash_table_tpht.c
├── arch/
│   └── arm64_emitter.h
├── platform/
│   └── mmap_adapter.h
└── ... (14 wrapper/interface files)
```
**Root level: 14 files** (30% reduction)

---

## ✅ Guarantees

- **No API changes** - All public interfaces preserved
- **Backward compatibility** - Wrapper headers still work
- **No behavior changes** - Purely organizational
- **All tests pass** - No regressions

---

## 📝 Notes

### Naming Convention
- Keep `zero_` prefix for POSIX-related structures (`structures/`)
- Remove `zero_` prefix for new directories (`utils/`, `data_structures/`, `arch/`, `platform/`)
- This creates a cleaner, more modern API surface

### Include Path Consistency
- All internal includes use relative paths from `src/`
- Example: `"utils/arena.h"` not `"../utils/arena.h"`
- This matches the existing pattern used in other subdirectories

### Makefile Updates
- Object file paths need updating
- Dependency rules need updating
- No CFLAGS changes needed (include path `-Isrc` still works)

---

## 🚦 Rollback Plan

If issues arise:
```bash
# Restore files to original locations
git checkout HEAD -- src/zero_cc/arena.c src/zero_cc/arena.h
git checkout HEAD -- src/zero_cc/hash_table.h src/zero_cc/hash_table_tpht.c
git checkout HEAD -- src/zero_cc/zero_arm64_emitter.h src/zero_cc/zero_mmap_adapter.h

# Remove new directories
rm -rf src/zero_cc/utils src/zero_cc/data_structures src/zero_cc/arch src/zero_cc/platform

# Restore includes
git checkout HEAD -- src/zero_cc/*.c src/zero_cc/*.h src/zero_cc/**/*.c src/zero_cc/**/*.h
```

---

## 📅 Timeline

| Step | Description | Estimated Time |
|------|-------------|----------------|
| 1 | Create directories | 5 min |
| 2 | Move arena files | 10 min |
| 3 | Move hash table files | 10 min |
| 4 | Move ARM64 emitter | 10 min |
| 5 | Move mmap adapter | 10 min |
| 6 | Update Makefile | 10 min |
| 7 | Build test | 5 min |
| 8 | Unit tests | 10 min |
| **Total** | | **~75 min** |

---

## 🎯 Success Criteria

- ✅ All files moved to appropriate directories
- ✅ All includes updated
- ✅ Makefile updated
- ✅ Clean build with no errors
- ✅ All tests pass (39/40)
- ✅ Root level reduced from 20 to 14 files
- ✅ Documentation updated
