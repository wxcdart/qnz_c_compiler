#!/usr/bin/env python3
"""
Task 63: Modular Header Separation Analysis.
Identifies header-only includes that should become .c files.
Usage: python3 tools/task63_analyzer.py
"""

import re
import glob

def analyze_header_includes(filepath):
    """Analyze file for header-only includes."""
    try:
        with open(filepath, 'r') as f:
            content = f.read()
        
        # Find all #include "..." patterns
        includes = re.findall(r'#include\s+"([^"]+)"', content)
        
        # Check if file is header-only (included by others but no .c counterpart)
        basename = filepath.replace('src/', '').replace('.h', '')
        
        return {
            'includes': includes,
            'is_header_only': True,  # We'll check this
            'lines': len(content.split('\n')),
            'functions': len(re.findall(r'^\s*\w+\s+\w+\s*\(', content, re.MULTILINE))
        }
    except:
        return None

def main():
    print("=" * 70)
    print("Task 63: Modular Header Separation Analysis")
    print("=" * 70)
    
    header_files = glob.glob('src/**/*.h', recursive=True)
    
    print("\nHeader-only files (candidates for .c conversion):")
    for filepath in sorted(header_files):
        # Check if there's a corresponding .c file
        c_file = filepath.replace('.h', '.c')
        has_c = glob.glob(c_file)
        
        info = analyze_header_includes(filepath)
        if info:
            status = "✅ has .c" if has_c else "❌ header-only"
            print(f"\n{status}: {filepath}")
            print(f"  Lines: {info['lines']}, Functions: {info['functions']}")
            print(f"  Includes: {info['includes'][:5]}")
    
    print("\n" + "=" * 70)
    print("Recommended for Task 63:")
    print("  - ast_lowering.h → ast_lowering.c + ast_lowering_internal.h")
    print("  - ast_lowering_expr.h → ast_lowering_expr.c")
    print("  - ast_lowering_stmt.h → ast_lowering_stmt.c")
    print("  - ast_lowering_mc.h → ast_lowering_mc.c")
    print("  - ir_emitter.h → ir_emitter.c (keep as header for inline functions)")
    print("=" * 70)

if __name__ == "__main__":
    main()
