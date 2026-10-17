#!/usr/bin/env python3
"""
Generate dependency graph from C source code.
Usage: python3 tools/generate_deps.py src/ > deps_graph.json
"""

import os
import re
import json
import sys
from collections import defaultdict

def extract_includes(filepath):
    """Extract #include directives from a file."""
    includes = []
    try:
        with open(filepath, 'r') as f:
            for line in f:
                match = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
                if match:
                    includes.append(match.group(1))
    except:
        pass
    return sorted(set(includes))

def extract_globals_defined(filepath):
    """Extract static/global variable definitions."""
    globals_defined = {}
    try:
        with open(filepath, 'r') as f:
            content = f.read()
            # Match: static TYPE name[SIZE] = ... or static TYPE name = ...
            pattern = r'static\s+(\w+(?:\s+\w+)*\s+)(\w+)\s*(?:\[[^\]]*\])?\s*=?'
            for match in re.finditer(pattern, content):
                name = match.group(2)
                if name.startswith('g_'):
                    globals_defined[name] = match.group(1).strip()
    except:
        pass
    return globals_defined

def extract_globals_used(filepath):
    """Extract global variable usage (g_* pattern)."""
    globals_used = set()
    try:
        with open(filepath, 'r') as f:
            content = f.read()
            # Match g_* followed by . or [ or space or ; or )
            pattern = r'\bg_(\w+)(?:\.|\[|\s|;|\)|,)'
            for match in re.finditer(pattern, content):
                globals_used.add('g_' + match.group(1))
    except:
        pass
    return sorted(globals_used)

def extract_functions_defined(filepath):
    """Extract function definitions."""
    functions = set()
    try:
        with open(filepath, 'r') as f:
            content = f.read()
            # Match: static TYPE func_name(...) { or TYPE func_name(...) {
            pattern = r'(?:static\s+)?(?:inline\s+)?\w+\s+\**\s*(\w+)\s*\([^)]*\)\s*{'
            for match in re.finditer(pattern, content):
                functions.add(match.group(1))
    except:
        pass
    return sorted(functions)

def extract_function_calls(filepath):
    """Extract function calls (simple pattern)."""
    calls = set()
    try:
        with open(filepath, 'r') as f:
            content = f.read()
            # Match: func_name( pattern (not including definitions)
            pattern = r'(?<!\w)(\w+)\s*\([^)]*\)'
            for match in re.finditer(pattern, content):
                name = match.group(1)
                # Skip keywords and common patterns
                if name not in ['if', 'while', 'for', 'switch', 'return', 'sizeof', 'memset', 'free', 'malloc', 'calloc', 'realloc', 'fprintf', 'printf', 'snprintf', 'strlen', 'strcmp', 'strncmp', 'strcpy', 'strncpy', 'strcat', 'sprintf', 'scanf', 'fopen', 'fclose', 'fgets', 'fputs', 'exit', 'assert', 'qsort', 'bsearch', 'getenv', 'system', 'time', 'clock', 'srand', 'rand', 'abs', 'labs', 'llabs', 'div', 'ldiv', 'lldiv', 'atoi', 'atol', 'atoll', 'strtol', 'strtoll', 'strtoul', 'strtoull', 'strtod', 'strtof', 'strtold']:
                    calls.add(name)
    except:
        pass
    return sorted(calls)

def analyze_source_dir(src_dir):
    """Analyze all .h and .c files in source directory."""
    deps = {
        "files": {},
        "globals": defaultdict(lambda: {"defined_in": None, "used_in": [], "fields_used": defaultdict(list)}),
        "functions": defaultdict(lambda: {"defined_in": None, "called_in": [], "depends_on": []})
    }
    
    for root, dirs, files in os.walk(src_dir):
        dirs.sort()  # deterministic traversal (stable JSON key order, defined_in)
        for filename in sorted(files):
            if filename.endswith(('.h', '.c')):
                filepath = os.path.join(root, filename)
                rel_path = os.path.relpath(filepath, os.path.dirname(src_dir))
                
                deps["files"][rel_path] = {
                    "includes": extract_includes(filepath),
                    "globals_used": extract_globals_used(filepath),
                    "functions_defined": extract_functions_defined(filepath),
                    "functions_called": extract_function_calls(filepath)
                }
                
                # Track global definitions
                for gname, gtype in extract_globals_defined(filepath).items():
                    deps["globals"][gname]["defined_in"] = rel_path
                    deps["globals"][gname]["type"] = gtype
                
                # Track global usage
                for gname in extract_globals_used(filepath):
                    if gname in deps["globals"]:
                        deps["globals"][gname]["used_in"].append(rel_path)
                    else:
                        deps["globals"][gname] = {
                            "defined_in": None,
                            "used_in": [rel_path],
                            "fields_used": {}
                        }
                
                # Track function definitions and calls
                for func in extract_functions_defined(filepath):
                    deps["functions"][func]["defined_in"] = rel_path
                
                for func in extract_function_calls(filepath):
                    deps["functions"][func]["called_in"].append(rel_path)
    
    # Convert defaultdicts to regular dicts for JSON serialization
    deps["globals"] = dict(deps["globals"])
    deps["functions"] = dict(deps["functions"])
    # Deterministic order for the accumulation lists
    for g in deps["globals"].values():
        g["used_in"] = sorted(g["used_in"])
    for f in deps["functions"].values():
        f["called_in"] = sorted(f["called_in"])
    
    return deps

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_deps.py <source_dir>")
        sys.exit(1)
    
    src_dir = sys.argv[1]
    deps = analyze_source_dir(src_dir)
    
    print(json.dumps(deps, indent=2, sort_keys=True, default=str))

if __name__ == "__main__":
    main()
