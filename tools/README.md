# Dependency Analysis Tools

This directory contains tools for analyzing code dependencies in the ZeroCC compiler.

## Quick Start

### Generate Dependency Graph
```bash
python3 tools/generate_deps.py src/ > deps_graph.json
```

### Query Dependencies
```bash
# Task 61: Global State Encapsulation
python3 tools/query_deps.py task61

# Task 62: Unified Label Patching  
python3 tools/query_deps.py task62

# Show all usage of a global
python3 tools/query_deps.py globals g_ir_emitter_ctx

# Show what breaks if you change a file
python3 tools/query_deps.py breaks zero_cc/parser/ast_lowering.h

# Show globals used in a file
python3 tools/query_deps.py globals_used zero_cc/parser/ast_lowering.h
```

## Output Format

`deps_graph.json` contains:
- `files`: Per-file dependencies (includes, globals used, functions defined/called)
- `globals`: Global variable definitions and usage
- `functions`: Function definitions and call sites

## Example Queries

```bash
# Find all files that need changes for Task 61
python3 tools/query_deps.py task61 | grep "Files to modify" -A10

# Find what breaks if you change ir_emit()
python3 tools/query_deps.py functions ir_emit

# Find all globals used in ast_lowering.h
python3 tools/query_deps.py globals_used zero_cc/parser/ast_lowering.h
```

## Adding New Queries

Edit `tools/query_deps.py` and add a new `query_taskXX()` function, then add it to the `main()` dispatcher.
