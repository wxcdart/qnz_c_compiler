#!/usr/bin/env python3
"""
Query dependency graph for C source code.
Usage: python3 tools/query_deps.py <query_type> <args...>

Query types:
  globals <name>              - Show all usage of a global variable
  files <name>                - Show all dependencies of a file
  functions <name>            - Show all callers/callees of a function
  breaks <file>               - Show what breaks if you change a file
  task61                      - Show Task 61 dependencies
  task62                      - Show Task 62 dependencies
  globals_used <file>         - Show globals used in a file
  functions_defined <file>    - Show functions defined in a file
"""

import json
import sys
import os

def load_deps():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    deps_file = os.path.join(script_dir, '..', 'deps_graph.json')
    with open(deps_file) as f:
        return json.load(f)

def query_globals(name):
    deps = load_deps()
    if name not in deps['globals']:
        print(f"Global '{name}' not found")
        return
    info = deps['globals'][name]
    print(f"\n{name}:")
    print(f"  Defined in: {info.get('defined_in', 'N/A')}")
    print(f"  Used in: {info.get('used_in', [])}")
    files_using = info.get('used_in', [])
    for f in files_using:
        if f in deps['files']:
            funcs = deps['files'][f].get('functions_defined', [])
            if funcs:
                print(f"  Functions in {f}: {funcs}")

def query_files(name):
    deps = load_deps()
    if name not in deps['files']:
        print(f"File '{name}' not found")
        return
    info = deps['files'][name]
    print(f"\n{name}:")
    print(f"  Includes: {info.get('includes', [])}")
    print(f"  Globals used: {info.get('globals_used', [])}")
    print(f"  Functions defined: {info.get('functions_defined', [])}")
    print(f"  Functions called: {info.get('functions_called', [])}")

def query_functions(name):
    deps = load_deps()
    defined_in = None
    for f, info in deps['files'].items():
        if name in info.get('functions_defined', []):
            defined_in = f
            break
    callers = []
    for f, info in deps['files'].items():
        if name in info.get('functions_called', []):
            callers.append(f)
    print(f"\n{name}:")
    print(f"  Defined in: {defined_in or 'N/A'}")
    print(f"  Called in: {callers}")

def query_breaks(file):
    deps = load_deps()
    dependents = []
    for f, info in deps['files'].items():
        if file in info.get('includes', []):
            dependents.append(f)
    globals_defined = []
    for g, info in deps['globals'].items():
        if info.get('defined_in') == file:
            globals_defined.append(g)
    print(f"\nIf you change {file}:")
    print(f"  Direct dependents: {dependents}")
    print(f"  Globals defined: {globals_defined}")
    for g in globals_defined:
        if g in deps['globals']:
            print(f"  Files using {g}: {deps['globals'][g].get('used_in', [])}")

def query_task61():
    deps = load_deps()
    globals_to_track = ['g_ir_emitter_ctx', 'g_ir_epilogue_label', 'g_loop_label_stack', 'g_user_labels']
    print("\n" + "=" * 60)
    print("Task 61: Global State Encapsulation Dependencies")
    print("=" * 60)
    files_to_change = set()
    for gname in globals_to_track:
        if gname in deps['globals']:
            info = deps['globals'][gname]
            print(f"\n{gname}:")
            print(f"  Defined in: {info.get('defined_in', 'N/A')}")
            print(f"  Used in: {info.get('used_in', [])}")
            files_to_change.update(info.get('used_in', []))
            if info.get('defined_in'):
                files_to_change.add(info['defined_in'])
    print("\n" + "=" * 60)
    print("Files to modify:")
    print("=" * 60)
    for f in sorted(files_to_change):
        print(f"  - {f}")

def query_task62():
    deps = load_deps()
    print("\n" + "=" * 60)
    print("Task 62: Unified Label & Branch Patching Dependencies")
    print("=" * 60)
    print("\nLabel-related state:")
    print("  - c->labels (backend ISA labels)")
    print("  - g_ir_emitter_ctx.epilogue_patches (IR epilogue patches)")
    print("  - c->ir_next_label (IR label counter)")
    print("\nFiles with label/patching logic:")
    for f, info in deps['files'].items():
        funcs = info.get('functions_defined', [])
        if any('label' in fn.lower() or 'patch' in fn.lower() or 'resolve' in fn.lower() for fn in funcs):
            print(f"  - {f}: {funcs}")

def query_globals_used(file):
    deps = load_deps()
    if file not in deps['files']:
        print(f"File '{file}' not found")
        return
    globals_used = deps['files'][file].get('globals_used', [])
    print(f"\nGlobals used in {file}:")
    for g in globals_used:
        if g in deps['globals']:
            info = deps['globals'][g]
            print(f"  - {g} (defined in {info.get('defined_in', 'N/A')})")

def query_functions_defined(file):
    deps = load_deps()
    if file not in deps['files']:
        print(f"File '{file}' not found")
        return
    funcs = deps['files'][file].get('functions_defined', [])
    print(f"\nFunctions defined in {file}:")
    for f in funcs:
        print(f"  - {f}")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    query_type = sys.argv[1]
    if query_type == 'globals':
        if len(sys.argv) < 3:
            print("Usage: query_deps.py globals <name>")
            return
        query_globals(sys.argv[2])
    elif query_type == 'files':
        if len(sys.argv) < 3:
            print("Usage: query_deps.py files <name>")
            return
        query_files(sys.argv[2])
    elif query_type == 'functions':
        if len(sys.argv) < 3:
            print("Usage: query_deps.py functions <name>")
            return
        query_functions(sys.argv[2])
    elif query_type == 'breaks':
        if len(sys.argv) < 3:
            print("Usage: query_deps.py breaks <file>")
            return
        query_breaks(sys.argv[2])
    elif query_type == 'task61':
        query_task61()
    elif query_type == 'task62':
        query_task62()
    elif query_type == 'globals_used':
        if len(sys.argv) < 3:
            print("Usage: query_deps.py globals_used <file>")
            return
        query_globals_used(sys.argv[2])
    elif query_type == 'functions_defined':
        if len(sys.argv) < 3:
            print("Usage: query_deps.py functions_defined <file>")
            return
        query_functions_defined(sys.argv[2])
    else:
        print(f"Unknown query type: {query_type}")
        print(__doc__)

if __name__ == "__main__":
    main()
