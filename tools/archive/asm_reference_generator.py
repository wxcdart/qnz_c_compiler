#!/usr/bin/env python3
"""
Assembly Reference Generator
Generates reference assembly patterns for IR opcodes.
Helps implement the -S assembly emitter by showing expected output.
"""

import sys

# x86-64 assembly reference for IR opcodes
ASSEMBLY_REFERENCE = {
    # Prologue/Epilogue
    'PROLOGUE': '''
    # Function prologue
    push rbp
    mov rbp, rsp
    sub rsp, <stack_size>
    # Save callee-saved registers if needed
''',
    
    'EPILOGUE': '''
    # Function epilogue
    leave
    ret
''',
    
    # Integer operations
    'MOV_IMM': '''
    # MOV_IMM dst, imm
    mov <dst_reg>, <imm_value>
''',
    
    'MOV_REG': '''
    # MOV_REG dst, src
    mov <dst_reg>, <src_reg>
''',
    
    'ADD': '''
    # ADD dst, src1, src2
    add <dst_reg>, <src1_reg>
    # or: lea <dst_reg>, [<src1_reg> + <src2_reg>]
''',
    
    'SUB': '''
    # SUB dst, src1, src2
    sub <dst_reg>, <src1_reg>
''',
    
    'MUL': '''
    # MUL dst, src1, src2 (signed)
    mov rax, <src1_reg>
    imul <src2_reg>
    mov <dst_reg>, rax
''',
    
    'DIV': '''
    # DIV dst, src1, src2 (signed)
    mov rax, <src1_reg>
    cqo
    idiv <src2_reg>
    mov <dst_reg>, rax
''',
    
    'AND': '''
    # AND dst, src1, src2
    mov <dst_reg>, <src1_reg>
    and <dst_reg>, <src2_reg>
''',
    
    'OR': '''
    # OR dst, src1, src2
    mov <dst_reg>, <src1_reg>
    or <dst_reg>, <src2_reg>
''',
    
    'XOR': '''
    # XOR dst, src1, src2
    mov <dst_reg>, <src1_reg>
    xor <dst_reg>, <src2_reg>
''',
    
    'SHL': '''
    # SHL dst, src, shift
    mov <dst_reg>, <src_reg>
    mov cl, <shift_reg>
    shl <dst_reg>, cl
''',
    
    'SHR': '''
    # SHR dst, src1, src2 (logical right shift)
    mov <dst_reg>, <src1_reg>
    mov cl, <src2_reg>
    shr <dst_reg>, cl
''',
    
    'SAR': '''
    # SAR dst, src1, src2 (arithmetic right shift)
    mov <dst_reg>, <src1_reg>
    mov cl, <src2_reg>
    sar <dst_reg>, cl
''',
    
    'CMP': '''
    # CMP src1, src2
    cmp <src1_reg>, <src2_reg>
''',
    
    'CMP_ZERO': '''
    # CMP_ZERO src
    test <src_reg>, <src_reg>
''',
    
    # Load/Store
    'LOAD': '''
    # LOAD dst, base, offset
    mov <dst_reg>, [<base_reg> + <offset>]
''',
    
    'STORE': '''
    # STORE src, base, offset
    mov [<base_reg> + <offset>], <src_reg>
''',
    
    'DEREF': '''
    # DEREF dst, src (load from pointer)
    mov <dst_reg>, [<src_reg>]
''',
    
    'LEA': '''
    # LEA dst, base, offset (load effective address)
    lea <dst_reg>, [<base_reg> + <offset>]
''',
    
    # Control flow
    'JMP': '''
    # JMP label
    jmp <label>
''',
    
    'JEQ': '''
    # JEQ label (jump if equal)
    je <label>
''',
    
    'JNE': '''
    # JNE label (jump if not equal)
    jne <label>
''',
    
    'JLT': '''
    # JLT label (jump if less than)
    jl <label>
''',
    
    'JLE': '''
    # JLE label (jump if less or equal)
    jle <label>
''',
    
    'JGT': '''
    # JGT label (jump if greater than)
    jg <label>
''',
    
    'JGE': '''
    # JGE label (jump if greater or equal)
    jge <label>
''',
    
    'CALL': '''
    # CALL func_name
    call <func_name>
''',
    
    'RET': '''
    # RET
    ret
''',
    
    # Floating point
    'FLOAD': '''
    # FLOAD xmm_dst, base, offset
    movsd <xmm_dst>, [<base_reg> + <offset>]
''',
    
    'FSTORE': '''
    # FSTORE src, base, offset
    movsd [<base_reg> + <offset>], <xmm_src>
''',
    
    'FADD': '''
    # FADD dst, src1, src2
    addsd <xmm_dst>, <xmm_src1>
''',
    
    'FSUB': '''
    # FSUB dst, src1, src2
    subsd <xmm_dst>, <xmm_src1>
''',
    
    'FMUL': '''
    # FMUL dst, src1, src2
    mulsd <xmm_dst>, <xmm_src1>
''',
    
    'FDIV': '''
    # FDIV dst, src1, src2
    divsd <xmm_dst>, <xmm_src1>
''',
    
    'FNEG': '''
    # FNEG dst, src
    xorps <xmm_dst>, <xmm_src>  # XOR with sign bit
    # or: neg <xmm_dst> (if supported)
''',
    
    'FCMP': '''
    # FCMP src1, src2
    cmpsd <xmm_src1>, <xmm_src2>, <comparison_predicate>
''',
    
    'FCVT_SI': '''
    # FCVT_SI int_dst, fp_src (float to int)
    cvttsd2si <int_dst>, <xmm_fp_src>
''',
    
    'FCVT_SF': '''
    # FCVT_SF fp_dst, int_src (int to float)
    cvtsi2sd <xmm_fp_dst>, <int_src>
''',
    
    'FMOV': '''
    # FMOV dst, src (float register move)
    movsd <xmm_dst>, <xmm_src>
''',
    
    # SIMD (SSE2) - Task 71 target
    'VADDSS': '''
    # VADDSS xmm_dst, xmm_src1, xmm_src2 (scalar single-precision add)
    addss <xmm_dst>, <xmm_src1>
''',
    
    'VSUBSS': '''
    # VSUBSS xmm_dst, xmm_src1, xmm_src2 (scalar single-precision sub)
    subss <xmm_dst>, <xmm_src1>
''',
    
    'VMULSS': '''
    # VMULSS xmm_dst, xmm_src1, xmm_src2 (scalar single-precision mul)
    mulss <xmm_dst>, <xmm_src1>
''',
    
    'VDIVSS': '''
    # VDIVSS xmm_dst, xmm_src1, xmm_src2 (scalar single-precision div)
    divss <xmm_dst>, <xmm_src1>
''',
    
    'VADDSD': '''
    # VADDSD xmm_dst, xmm_src1, xmm_src2 (scalar double-precision add)
    addsd <xmm_dst>, <xmm_src1>
''',
    
    'VSUBSD': '''
    # VSUBSD xmm_dst, xmm_src1, xmm_src2 (scalar double-precision sub)
    subsd <xmm_dst>, <xmm_src1>
''',
    
    'VMULSD': '''
    # VMULSD xmm_dst, xmm_src1, xmm_src2 (scalar double-precision mul)
    mulsd <xmm_dst>, <xmm_src1>
''',
    
    'VDIVSD': '''
    # VDIVSD xmm_dst, xmm_src1, xmm_src2 (scalar double-precision div)
    divsd <xmm_dst>, <xmm_src1>
''',
    
    'VMOV': '''
    # VMOV xmm_dst, xmm_src (128-bit vector move)
    movaps <xmm_dst>, <xmm_src>
''',
    
    'VMOVSS': '''
    # VMOVSS xmm_dst, xmm_src (scalar single-precision move)
    movss <xmm_dst>, <xmm_src>
''',
    
    'VMOVSD': '''
    # VMOVSD xmm_dst, xmm_src (scalar double-precision move)
    movsd <xmm_dst>, <xmm_src>
''',
    
    'VLOAD': '''
    # VLOAD xmm_dst, addr_reg (vector load 128-bit)
    movaps <xmm_dst>, [<addr_reg>]
''',
    
    'VSTORE': '''
    # VSTORE addr_reg, xmm_src (vector store 128-bit)
    movaps [<addr_reg>], <xmm_src>
''',
    
    'VLOADSS': '''
    # VLOADSS xmm_dst, addr_reg (load scalar single)
    movss <xmm_dst>, [<addr_reg>]
''',
    
    'VLOADSD': '''
    # VLOADSD xmm_dst, addr_reg (load scalar double)
    movsd <xmm_dst>, [<addr_reg>]
''',
    
    'VSTORESS': '''
    # VSTORESS addr_reg, xmm_src (store scalar single)
    movss [<addr_reg>], <xmm_src>
''',
    
    'VSTORESD': '''
    # VSTORESD addr_reg, xmm_src (store scalar double)
    movsd [<addr_reg>], <xmm_src>
''',
    
    'VCVT_SI2SS': '''
    # VCVT_SI2SS xmm_dst, int_src (int to scalar single)
    cvtsi2ss <xmm_dst>, <int_src>
''',
    
    'VCVT_SS2SI': '''
    # VCVT_SS2SI int_dst, xmm_src (scalar single to int)
    cvttss2si <int_dst>, <xmm_src>
''',
    
    'VCVT_SI2SD': '''
    # VCVT_SI2SD xmm_dst, int_src (int to scalar double)
    cvtsi2sd <xmm_dst>, <int_src>
''',
    
    'VCVT_SD2SI': '''
    # VCVT_SD2SI int_dst, xmm_src (scalar double to int)
    cvttsd2si <int_dst>, <xmm_src>
''',
    
    'VCMPSS': '''
    # VCMPSS xmm_dst, xmm_src1, xmm_src2 (scalar single compare)
    cmpss <xmm_dst>, <xmm_src1>, <xmm_src2>, <predicate>
''',
    
    'VCMPSD': '''
    # VCMPSD xmm_dst, xmm_src1, xmm_src2 (scalar double compare)
    cmpsd <xmm_dst>, <xmm_src1>, <xmm_src2>, <predicate>
''',
    
    'VAND': '''
    # VAND xmm_dst, xmm_src1, xmm_src2 (vector AND)
    andps <xmm_dst>, <xmm_src1>
''',
    
    'VOR': '''
    # VOR xmm_dst, xmm_src1, xmm_src2 (vector OR)
    orps <xmm_dst>, <xmm_src1>
''',
    
    'VXOR': '''
    # VXOR xmm_dst, xmm_src1, xmm_src2 (vector XOR)
    xorps <xmm_dst>, <xmm_src1>
''',
    
    'VANDN': '''
    # VANDN xmm_dst, xmm_src1, xmm_src2 (vector AND NOT)
    andnps <xmm_dst>, <xmm_src1>
''',
    
    'VSHUF': '''
    # VSHUF xmm_dst, xmm_src1, xmm_src2, imm (shuffle)
    shufps <xmm_dst>, <xmm_src1>, <imm>
''',
    
    'VORPS': '''
    # VORPS xmm_dst, xmm_src1, xmm_src2 (packed OR single)
    orps <xmm_dst>, <xmm_src1>
''',
    
    'VSUBPS': '''
    # VSUBPS xmm_dst, xmm_src1, xmm_src2 (packed sub single)
    subps <xmm_dst>, <xmm_src1>
''',
    
    'VMULPS': '''
    # VMULPS xmm_dst, xmm_src1, xmm_src2 (packed mul single)
    mulps <xmm_dst>, <xmm_src1>
''',
    
    'VADDPD': '''
    # VADDPD xmm_dst, xmm_src1, xmm_src2 (packed add double)
    addpd <xmm_dst>, <xmm_src1>
''',
    
    # Special operations
    'VLA_ALLOC': '''
    # VLA_ALLOC dst, size (variable length array)
    # size is in src_reg
    sub rsp, <size>
    and rsp, -16  # Align to 16 bytes
    mov <dst_reg>, rsp
''',
    
    'LABEL': '''
    # LABEL name
    <label_name>:
''',
    
    'CONST': '''
    # CONST value
    # Usually emitted as immediate in instruction
    # For large constants, stored in .rodata
''',
}

def print_reference(opcode=None):
    """Print assembly reference for opcode(s)."""
    if opcode:
        if opcode in ASSEMBLY_REFERENCE:
            print(f"\n{'='*70}")
            print(f"Assembly Reference: {opcode}")
            print(f"{'='*70}")
            print(ASSEMBLY_REFERENCE[opcode])
        else:
            print(f"Unknown opcode: {opcode}")
            print(f"\nAvailable opcodes: {', '.join(sorted(ASSEMBLY_REFERENCE.keys()))}")
    else:
        print(f"\n{'='*70}")
        print("Assembly Reference Guide for ZeroCC -S Emitter")
        print(f"{'='*70}")
        print("\nUsage: python3 tools/asm_reference_generator.py [OPCODE]")
        print("\nAvailable opcodes:")
        for opcode in sorted(ASSEMBLY_REFERENCE.keys()):
            print(f"  {opcode}")
        print("\nExamples:")
        print("  python3 tools/asm_reference_generator.py VADDSS")
        print("  python3 tools/asm_reference_generator.py FADD")
        print("  python3 tools/asm_reference_generator.py")
        print(f"\n{'='*70}")

def main():
    opcode = sys.argv[1] if len(sys.argv) > 1 else None
    print_reference(opcode)

if __name__ == '__main__':
    main()
