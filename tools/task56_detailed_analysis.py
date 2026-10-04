#!/usr/bin/env python3
"""Task 56 Detailed Analysis - Understand Current Two-Pass State"""

import os
import re

def analyze_pass1_implementation():
    """Analyze Pass 1 (AST → IR) implementation"""
    print("=" * 70)
    print("PASS 1: AST → IR Implementation")
    print("=" * 70)
    
    # Check parser.c for lower_function
    parser_c = 'src/zero_cc/parser/parser.c'
    if os.path.exists(parser_c):
        with open(parser_c, 'r') as f:
            content = f.read()
        
        # Find function compilation entry point
        if 'compile_function' in content or 'lower_function' in content:
            print("✅ Function lowering entry point found")
        else:
            print("❌ Function lowering entry point not found")
        
        # Check for IR emission calls
        ir_calls = re.findall(r'ir_emit_\w+', content)
        if ir_calls:
            print(f"✅ IR emission calls found: {len(set(ir_calls))} unique functions")
        else:
            print("❌ No IR emission calls found")
    
    # Check ast_lowering.c
    ast_lowering_c = 'src/zero_cc/parser/ast_lowering.c'
    if os.path.exists(ast_lowering_c):
        with open(ast_lowering_c, 'r') as f:
            content = f.read()
        
        if 'lower_function' in content or 'compile_function' in content:
            print("✅ ast_lowering.c has function lowering")
        else:
            print("⚠️  ast_lowering.c may not have function lowering")
    
    print()

def analyze_pass2_implementation():
    """Analyze Pass 2 (IR → Machine Code) implementation"""
    print("=" * 70)
    print("PASS 2: IR → Machine Code Implementation")
    print("=" * 70)
    
    mc_c = 'src/zero_cc/parser/ast_lowering_mc.c'
    if not os.path.exists(mc_c):
        print(f"❌ File not found: {mc_c}")
        return
    
    with open(mc_c, 'r') as f:
        content = f.read()
        lines = content.split('\n')
    
    # Count case statements
    case_count = len(re.findall(r'case\s+OP_\w+:', content))
    print(f"✅ Found {case_count} opcode handlers in Pass 2")
    
    # Check for main Pass 2 function
    if 'emit_ir_to_machine_code' in content:
        print("✅ emit_ir_to_machine_code() found")
    else:
        print("❌ emit_ir_to_machine_code() not found")
    
    # Check for switch statement
    if 'switch (instr->op)' in content or 'switch(instr->op)' in content:
        print("✅ Switch statement for opcode dispatch found")
    else:
        print("⚠️  No switch statement found - may use if/else or other dispatch")
    
    print()

def analyze_missing_handlers():
    """Analyze missing opcode handlers in detail"""
    print("=" * 70)
    print("MISSING OPCODE HANDLERS - Detailed Analysis")
    print("=" * 70)
    
    ir_h = 'src/zero_cc/parser/ir.h'
    mc_c = 'src/zero_cc/parser/ast_lowering_mc.c'
    
    if not os.path.exists(ir_h) or not os.path.exists(mc_c):
        return
    
    with open(ir_h, 'r') as f:
        ir_content = f.read()
    with open(mc_c, 'r') as f:
        mc_content = f.read()
    
    # Get all opcodes
    opcodes = re.findall(r'OP_(\w+),', ir_content)
    
    # Check each opcode
    missing_handlers = []
    for opcode in opcodes:
        pattern = rf'case\s+OP_{opcode}:'
        if not re.search(pattern, mc_content):
            missing_handlers.append(opcode)
    
    # Categorize missing handlers
    categories = {
        'Stack Operations': ['PUSH_RAX', 'POP_RAX', 'PUSH_RBX', 'POP_RBX'],
        'Arithmetic': ['ADD_IMM', 'SUB_IMM', 'INC', 'DEC', 'SAR'],
        'Control Flow': ['RET', 'EPILOGUE', 'CALL_INDIRECT'],
        'Special': ['STACK_ALIGN'],
    }
    
    for category, ops in categories.items():
        missing = [op for op in ops if op in missing_handlers]
        if missing:
            print(f"\n{category}:")
            for op in missing:
                print(f"  - OP_{op}")
    
    if not any(missing_handlers):
        print("✅ All opcodes have handlers!")
    
    print(f"\nTotal missing: {len(missing_handlers)} opcodes")
    print()

def analyze_ir_buffer_lifecycle():
    """Analyze IR buffer lifecycle management"""
    print("=" * 70)
    print("IR BUFFER LIFECYCLE")
    print("=" * 70)
    
    files = {
        'ir.h': 'src/zero_cc/parser/ir.h',
        'ir.c': 'src/zero_cc/parser/ir.c',
        'ast_lowering.c': 'src/zero_cc/parser/ast_lowering.c',
        'ast_lowering_mc.c': 'src/zero_cc/parser/ast_lowering_mc.c',
    }
    
    lifecycle_ops = {
        'ir_emit': 'Emit instruction',
        'ir_get_buffer': 'Get instruction buffer',
        'ir_get_count': 'Get instruction count',
        'ir_clear': 'Clear buffer (reset count)',
        'ir_init': 'Initialize buffer',
    }
    
    for name, path in files.items():
        if not os.path.exists(path):
            print(f"❌ {name}: Not found")
            continue
        
        with open(path, 'r') as f:
            content = f.read()
        
        found_ops = [op for op in lifecycle_ops if op in content]
        if found_ops:
            print(f"✅ {name}: {', '.join(found_ops)}")
        else:
            print(f"⚠️  {name}: No lifecycle ops found")
    
    print()

def analyze_two_pass_integration():
    """Analyze how Pass 1 and Pass 2 are integrated"""
    print("=" * 70)
    print("TWO-PASS INTEGRATION POINT")
    print("=" * 70)
    
    # Check parser.c
    parser_c = 'src/zero_cc/parser/parser.c'
    if os.path.exists(parser_c):
        with open(parser_c, 'r') as f:
            content = f.read()
        
        # Look for function compilation flow
        if 'ast_lowering' in content.lower() or 'emit_ir' in content:
            print("✅ parser.c includes ast_lowering")
        else:
            print("⚠️  parser.c may not include ast_lowering")
        
        # Check for function compilation entry
        if 'compile_function' in content:
            print("✅ compile_function() found in parser.c")
        elif 'lower_function' in content:
            print("✅ lower_function() found in parser.c")
        else:
            print("⚠️  Function compilation entry point unclear")
    
    # Check ast_lowering.c
    ast_lowering_c = 'src/zero_cc/parser/ast_lowering.c'
    if os.path.exists(ast_lowering_c):
        with open(ast_lowering_c, 'r') as f:
            content = f.read()
        
        if 'emit_ir_to_machine_code' in content:
            print("✅ emit_ir_to_machine_code() called from ast_lowering.c")
        else:
            print("⚠️  emit_ir_to_machine_code() not called from ast_lowering.c")
    
    print()

def main():
    print("=" * 70)
    print("Task 56 Detailed Analysis - Two-Pass IR Architecture")
    print("=" * 70)
    print()
    
    analyze_pass1_implementation()
    analyze_pass2_implementation()
    analyze_missing_handlers()
    analyze_ir_buffer_lifecycle()
    analyze_two_pass_integration()
    
    print("=" * 70)
    print("RECOMMENDATIONS FOR TASK 56")
    print("=" * 70)
    print("""
Task 56: Pass 2 Machine Code Emission

Priority 1: Missing Opcode Handlers
  - Add handlers for 15 missing opcodes in ast_lowering_mc.c
  - Focus on: RET, EPILOGUE, CALL_INDIRECT (critical for correctness)
  - Stack ops (PUSH/POP) may be ISA-specific backend calls

Priority 2: IR Buffer Lifecycle
  - Add ir_clear() function to reset buffer between functions
  - Ensure buffer is cleared before each function compilation

Priority 3: Two-Pass Integration
  - Verify parser.c → ast_lowering.c → ast_lowering_mc.c flow
  - Ensure Pass 1 (AST→IR) feeds into Pass 2 (IR→MC)
  - Add ir_clear() call between functions

Estimated Effort: 2-3 days
""")

if __name__ == '__main__':
    main()
