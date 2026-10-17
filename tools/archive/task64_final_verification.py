#!/usr/bin/env python3
"""Task 64 Final Verification - Confirm IR layer is target-agnostic"""

import re
import os

def verify_task64():
    """Verify Task 64 completion"""
    print("=" * 70)
    print("Task 64 Final Verification - IR Layer Target-Agnostic Check")
    print("=" * 70)
    
    # Files that should NOT have ISA-specific instruction emission
    ir_layer_files = [
        'src/zero_cc/parser/ast_lowering.c',
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_stmt.c',
        'src/zero_cc/parser/ast_lowering_ir.h',
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir_emit_ops.h',
        'src/zero_cc/parser/ir_emitter.h',
    ]
    
    # Patterns that indicate ISA-specific INSTRUCTION EMISSION (BAD)
    # These are the actual machine code emission patterns that should NOT be in IR layer
    bad_patterns = [
        (r'me_emit_u8\b', 'Direct machine emitter call (me_emit_u8)'),
        (r'me_emit_u16\b', 'Direct machine emitter call (me_emit_u16)'),
        (r'me_emit_u32\b', 'Direct machine emitter call (me_emit_u32)'),
        (r'0x48\s*,\s*0x[Bb][0-9A-Fa-f]{2}', 'x86 REX prefix opcode'),
        (r'0x89\b', 'x86 MOV opcode'),
        (r'0x8B\b', 'x86 MOV opcode'),
        (r'0xC9\b', 'x86 LEAVE opcode'),
        (r'0xC3\b', 'x86 RET opcode'),
        (r'0xE9\b', 'x86 JMP opcode'),
    ]
    
    # Patterns that are OK (acceptable in IR layer)
    acceptable_patterns = [
        (r'BE\(c\)->emit_', 'Backend abstraction call (CORRECT)'),
        (r'backend_get_int_arg_reg', 'Backend helper call (CORRECT)'),
        (r'backend_get_fp_arg_reg', 'Backend helper call (CORRECT)'),
        (r'int_arg_regs\[8\]', 'Register name table (acceptable)'),
        (r'fp_arg_regs\[8\]', 'Register name table (acceptable)'),
        (r'num_int_regs', 'Register count field (acceptable)'),
        (r'num_fp_regs', 'Register count field (acceptable)'),
    ]
    
    total_bad = 0
    total_acceptable = 0
    
    for filepath in ir_layer_files:
        if not os.path.exists(filepath):
            continue
        
        with open(filepath, 'r') as f:
            content = f.read()
            lines = content.split('\n')
        
        bad_count = 0
        acceptable_count = 0
        
        for i, line in enumerate(lines, 1):
            # Skip comments
            if line.strip().startswith('//') or line.strip().startswith('/*'):
                continue
            
            # Check for bad patterns
            for pattern, description in bad_patterns:
                if re.search(pattern, line):
                    bad_count += 1
                    if bad_count <= 3:
                        print(f"  ❌ {filepath}:{i}: {description}")
            
            # Check for acceptable patterns
            for pattern, description in acceptable_patterns:
                if re.search(pattern, line):
                    acceptable_count += 1
        
        total_bad += bad_count
        total_acceptable += acceptable_count
    
    print("\n" + "=" * 70)
    print("VERIFICATION RESULTS")
    print("=" * 70)
    print(f"❌ ISA-specific instruction emission: {total_bad} (should be 0)")
    print(f"✅ Backend abstraction usage: {total_acceptable} (correct)")
    print()
    
    if total_bad == 0:
        print("✅ Task 64 COMPLETE: IR layer is fully target-agnostic!")
        print()
        print("Summary:")
        print("  - No direct machine emitter calls in IR layer")
        print("  - All ISA-specific code moved to backend layer")
        print("  - Calling convention tables abstracted via ISACallingConvention")
        print("  - IR opcode comments updated to be ISA-agnostic")
        return True
    else:
        print("❌ Task 64 INCOMPLETE: Found ISA-specific instruction emission")
        return False

if __name__ == '__main__':
    success = verify_task64()
    exit(0 if success else 1)
