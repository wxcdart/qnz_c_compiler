#!/usr/bin/env python3
"""
Debug tool for struct type compatibility issues.

This tool helps diagnose why struct types are considered incompatible
by analyzing the type compatibility check logic.
"""

import sys
import re
import subprocess
import tempfile
import os
from pathlib import Path

def analyze_type_compatibility(test_code):
    """Analyze type compatibility issues in test code."""
    print("=" * 60)
    print("Struct Type Compatibility Analysis")
    print("=" * 60)
    
    # Check for anonymous typedef struct pattern
    anonymous_typedef = re.findall(r'typedef\s+struct\s*\{[^}]+\}\s*(\w+);', test_code, re.DOTALL)
    named_typedef = re.findall(r'typedef\s+struct\s+(\w+)\s*\{[^}]+\}\s*\1;', test_code, re.DOTALL)
    
    print("\n1. Struct Definition Patterns:")
    if anonymous_typedef:
        print(f"   ⚠️  Anonymous typedef structs found: {anonymous_typedef}")
        print("      These structs have no tag name, which can cause type compatibility issues.")
    if named_typedef:
        print(f"   ✓ Named typedef structs found: {named_typedef}")
    
    # Check for struct usage patterns
    struct_params = re.findall(r'\b(\w+)\s+(\w+)\s*\(', test_code)
    struct_vars = re.findall(r'(\w+)\s+(\w+)\s*[;=]', test_code)
    
    print("\n2. Struct Usage:")
    for ret_type, func_name in struct_params:
        if ret_type in anonymous_typedef or ret_type in named_typedef:
            print(f"   - Function parameter: {func_name}({ret_type} ...)")
    
    for var_type, var_name in struct_vars:
        if var_type in anonymous_typedef or var_type in named_typedef:
            print(f"   - Variable: {var_type} {var_name}")
    
    print("\n3. Compatibility Check Logic:")
    print("   The type_is_compatible function checks:")
    print("   - If both types have tags: compare tag names")
    print("   - If neither has tags: compare size and alignment")
    print("   - Otherwise: return false")
    print()
    print("   For anonymous typedef structs, tag is NULL, so it falls back to")
    print("   size/alignment comparison. This can fail if TypeInfo objects are")
    print("   created separately in different contexts.")
    
    print("\n4. Recommended Fix:")
    print("   Change anonymous typedef to named typedef:")
    print("   BEFORE: typedef struct { int x; int y; } Point;")
    print("   AFTER:  typedef struct Point { int x; int y; } Point;")
    
    print("\n5. Alternative Fix (in compiler):")
    print("   - Ensure anonymous typedef structs get a unique tag name")
    print("   - Or compare struct fields for anonymous types")
    print("   - Or reuse TypeInfo objects for the same typedef")

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <test_file.c>")
        print("\nThis tool analyzes struct type compatibility issues.")
        sys.exit(1)
    
    test_file = sys.argv[1]
    
    if not os.path.exists(test_file):
        print(f"Error: file not found: {test_file}")
        sys.exit(1)
    
    with open(test_file, 'r') as f:
        code = f.read()
    
    analyze_type_compatibility(code)

if __name__ == '__main__':
    main()
