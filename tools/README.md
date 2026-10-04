# ZeroCC Compiler Tooling

Automated tooling for Sprint 7 tasks (60-64) and general development.

## Quick Start

```bash
# Generate/update dependency graph
python3 tools/generate_deps.py src/ > deps_graph.json

# Run all Sprint 7 analysis
python3 tools/query_deps.py task61
python3 tools/task62_analyzer.py
python3 tools/task63_analyzer.py
python3 tools/task64_analyzer.py
```

## Task-Specific Tools

### Task 61: Global State Encapsulation

| Tool | Command | Purpose |
|------|---------|---------|
| Dependency Analysis | `python3 tools/query_deps.py task61` | Find files to modify |
| Global Replacement | `python3 tools/replace_globals.py --dry-run` | Preview changes |
| Global Replacement | `python3 tools/replace_globals.py --apply` | Apply changes |
| Verification | `python3 tools/verify_task61.py` | Check completion |

**Workflow:**
```bash
# 1. See what needs to change
python3 tools/replace_globals.py --dry-run

# 2. Apply changes
python3 tools/replace_globals.py --apply

# 3. Verify
python3 tools/verify_task61.py

# 4. Test
make test
```

### Task 62: Unified Label & Branch Patching

| Tool | Command | Purpose |
|------|---------|---------|
| Analysis | `python3 tools/task62_analyzer.py` | Find all label/patching code |

**Key Patterns to Unify:**
- `epilogue_patches` → `c->lowering.epilogue_patches`
- `break_patch_pos` → `c->lowering.break_patches`
- `continue_patch_pos` → `c->lowering.continue_patches`
- `goto_patch_list` → `c->lowering.goto_patches`

### Task 63: Modular Header Separation

| Tool | Command | Purpose |
|------|---------|---------|
| Analysis | `python3 tools/task63_analyzer.py` | Find header-only files |

**Recommended Refactoring:**
```
ast_lowering.h       → ast_lowering.c + ast_lowering_internal.h
ast_lowering_expr.h  → ast_lowering_expr.c
ast_lowering_stmt.h  → ast_lowering_stmt.c
ast_lowering_mc.h    → ast_lowering_mc.c
ir_emitter.h         → ir_emitter.c (keep inline functions)
```

### Task 64: Target-Independent IR Canonicalization

| Tool | Command | Purpose |
|------|---------|---------|
| Analysis | `python3 tools/task64_analyzer.py` | Find ISA-specific patterns |

**Key Patterns to Remove from IR Layer:**
- `BE(c)->emit_*` calls → Move to backend
- `x86_*` register names → Use VREG_*
- `machine_emitter.h` functions → Move to ISA backend

## General Tools

| Tool | Command | Purpose |
|------|---------|---------|
| Query Globals | `python3 tools/query_deps.py globals <name>` | Find global usage |
| Query Files | `python3 tools/query_deps.py files <name>` | File dependencies |
| Query Functions | `python3 tools/query_deps.py functions <name>` | Call graph |
| Breakage Analysis | `python3 tools/query_deps.py breaks <file>` | Impact analysis |

## Example: Complete Task 61 Workflow

```bash
# Step 1: Analyze dependencies
python3 tools/query_deps.py task61

# Step 2: Preview changes
python3 tools/replace_globals.py --dry-run

# Step 3: Apply changes
python3 tools/replace_globals.py --apply

# Step 4: Verify
python3 tools/verify_task61.py

# Step 5: Test
make test

# Step 6: Commit
git add src/
git commit -m "Task 61: Encapsulate global state in LoweringState"
```

## Output Formats

### deps_graph.json Structure

```json
{
  "files": {
    "path/to/file.h": {
      "includes": ["other.h"],
      "globals_used": ["g_var"],
      "functions_defined": ["func1", "func2"],
      "functions_called": ["other_func"]
    }
  },
  "globals": {
    "g_var": {
      "defined_in": "path/to/file.h",
      "used_in": ["path/to/other.h"]
    }
  },
  "functions": {
    "func1": {
      "defined_in": "path/to/file.h",
      "called_in": ["other.h"]
    }
  }
}
```

## Adding New Tools

1. Create `tools/<task>_<function>.py`
2. Add to `tools/README.md`
3. Make executable: `chmod +x tools/<task>_<function>.py`
