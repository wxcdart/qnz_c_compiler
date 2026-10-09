#!/usr/bin/env python3
"""
Vector Instruction Validator for ZeroCC
Validates SIMD instruction generation for x86-64 (SSE2), AArch64 (NEON), and RISC-V (RVV)

Usage:
    python3 vector_instruction_validator.py <assembly_file> --isa x86_64
    python3 vector_instruction_validator.py <assembly_file> --isa aarch64
    python3 vector_instruction_validator.py <assembly_file> --isa riscv64
    python3 vector_instruction_validator.py --check-ir <ir_dump>
"""

import sys
import re
import argparse
from typing import List, Dict, Tuple, Set
from enum import Enum


class ISA(Enum):
    X86_64 = "x86_64"
    AARCH64 = "aarch64"
    RISCV64 = "riscv64"


# x86-64 SSE2/AVX instruction patterns
X86_64_VEC_INSTRUCTIONS = {
    # Arithmetic - Scalar Single (SS)
    'addss': re.compile(r'\baddss\s+'),
    'subss': re.compile(r'\bsubss\s+'),
    'mulss': re.compile(r'\bmulss\s+'),
    'divss': re.compile(r'\bdivss\s+'),
    'sqrts': re.compile(r'\bsqrtss\s+'),
    'rsqrtss': re.compile(r'\brsqrtss\s+'),
    'rcpss': re.compile(r'\brcpss\s+'),
    
    # Arithmetic - Scalar Double (SD)
    'addsd': re.compile(r'\baddsd\s+'),
    'subsd': re.compile(r'\bsubsd\s+'),
    'mulsd': re.compile(r'\bmulsd\s+'),
    'divsd': re.compile(r'\bdivsd\s+'),
    'sqrtd': re.compile(r'\bsqrtsd\s+'),
    
    # Arithmetic - Packed Single (PS)
    'addps': re.compile(r'\baddps\s+'),
    'subps': re.compile(r'\bsubps\s+'),
    'mulps': re.compile(r'\bmulps\s+'),
    'divps': re.compile(r'\bdivps\s+'),
    
    # Arithmetic - Packed Double (PD)
    'addpd': re.compile(r'\baddpd\s+'),
    'subpd': re.compile(r'\bsubpd\s+'),
    'mulpd': re.compile(r'\bmulpd\s+'),
    'divpd': re.compile(r'\bdivpd\s+'),
    
    # Comparison
    'ucomiss': re.compile(r'\bucomiss\s+'),
    'ucomisd': re.compile(r'\bucomisd\s+'),
    'comiss': re.compile(r'\bcomiss\s+'),
    'comisd': re.compile(r'\bcomisd\s+'),
    'cmpeqss': re.compile(r'\bcmpeqss\s+'),
    'cmpltss': re.compile(r'\bcmpltss\s+'),
    'cmpless': re.compile(r'\bcmpless\s+'),
    'cmpeqsd': re.compile(r'\bcmpeqsd\s+'),
    'cmpltsd': re.compile(r'\bcmpltsd\s+'),
    'cmplesd': re.compile(r'\bcmplesd\s+'),
    'cmpps': re.compile(r'\bcmpps\s+'),
    'cmpsd': re.compile(r'\bcmpsd\s+'),
    
    # Logical/Bitwise
    'andps': re.compile(r'\bandps\s+'),
    'andpd': re.compile(r'\bandpd\s+'),
    'andnps': re.compile(r'\bandnps\s+'),
    'andnpd': re.compile(r'\bandnpd\s+'),
    'orps': re.compile(r'\borps\s+'),
    'orpd': re.compile(r'\borpd\s+'),
    'xorps': re.compile(r'\bxorps\s+'),
    'xorpd': re.compile(r'\bxorpd\s+'),
    
    # Data movement
    'movaps': re.compile(r'\bmovaps\s+'),
    'movups': re.compile(r'\bmovups\s+'),
    'movss': re.compile(r'\bmovss\s+'),
    'movsd': re.compile(r'\bmovsd\s+'),
    'movapd': re.compile(r'\bmovapd\s+'),
    'movupd': re.compile(r'\bmovupd\s+'),
    'movdqa': re.compile(r'\bmovdqa\s+'),
    'movdqu': re.compile(r'\bmovdqu\s+'),
    'movd': re.compile(r'\bmovd\s+'),
    'movq': re.compile(r'\bmovq\s+'),
    'movdq2q': re.compile(r'\bmovdq2q\s+'),
    'movq2dq': re.compile(r'\bmovq2dq\s+'),
    'movhlps': re.compile(r'\bmovhlps\s+'),
    'movlhps': re.compile(r'\bmovlhps\s+'),
    'movshdup': re.compile(r'\bmovshdup\s+'),
    'movsldup': re.compile(r'\bmovsldup\s+'),
    'movlpd': re.compile(r'\bmovlpd\s+'),
    'movhpd': re.compile(r'\bmovhpd\s+'),
    
    # Conversion
    'cvtsi2ss': re.compile(r'\bcvtsi2ss\s+'),
    'cvtsi2sd': re.compile(r'\bcvtsi2sd\s+'),
    'cvtss2si': re.compile(r'\bcvtss2si\s+'),
    'cvttsd2si': re.compile(r'\bcvttsd2si\s+'),
    'cvtps2pd': re.compile(r'\bcvtps2pd\s+'),
    'cvtpd2ps': re.compile(r'\bcvtpd2ps\s+'),
    'cvtdq2ps': re.compile(r'\bcvtdq2ps\s+'),
    
    # Shuffle/Pack/Unpack
    'shufps': re.compile(r'\bshufps\s+'),
    'shufpd': re.compile(r'\bshufpd\s+'),
    'unpcklps': re.compile(r'\bunpcklps\s+'),
    'unpckhps': re.compile(r'\bunpckhps\s+'),
    'unpcklpd': re.compile(r'\bunpcklpd\s+'),
    'unpckhp': re.compile(r'\bunpckhpd\s+'),
}

# AArch64 NEON instruction patterns
AARCH64_VEC_INSTRUCTIONS = {
    # Arithmetic - Floating Point
    'fadd': re.compile(r'\bfadd\s+v'),
    'fsub': re.compile(r'\bfsub\s+v'),
    'fmul': re.compile(r'\bfmul\s+v'),
    'fdiv': re.compile(r'\bfdiv\s+v'),
    'fmax': re.compile(r'\bfmax\s+v'),
    'fmin': re.compile(r'\bfmin\s+v'),
    'fmaxnm': re.compile(r'\bfmaxnm\s+v'),
    'fminnm': re.compile(r'\bfminnm\s+v'),
    'fmadd': re.compile(r'\bfmadd\s+v'),
    'fmsub': re.compile(r'\bfmsub\s+v'),
    
    # Arithmetic - Integer
    'add': re.compile(r'\badd\s+v'),
    'sub': re.compile(r'\bsub\s+v'),
    'mul': re.compile(r'\bmul\s+v'),
    'sadd': re.compile(r'\bsadd\s+v'),
    'smul': re.compile(r'\bsmul\s+v'),
    'smax': re.compile(r'\bsmax\s+v'),
    'smin': re.compile(r'\bsmin\s+v'),
    'uadd': re.compile(r'\buadd\s+v'),
    'umul': re.compile(r'\bumul\s+v'),
    'umax': re.compile(r'\bumax\s+v'),
    'umin': re.compile(r'\bumin\s+v'),
    
    # Comparison
    'fcmp': re.compile(r'\bfcmp\s+v'),
    'cmge': re.compile(r'\bcmge\s+v'),
    'cmgt': re.compile(r'\bcmgt\s+v'),
    'cmle': re.compile(r'\bcmle\s+v'),
    'cmlt': re.compile(r'\bcmlt\s+v'),
    'cmeq': re.compile(r'\bcmeq\s+v'),
    'cmne': re.compile(r'\bcmne\s+v'),
    
    # Data movement
    'mov': re.compile(r'\bmov\s+v'),
    'dup': re.compile(r'\bdup\s+v'),
    'ld1': re.compile(r'\bld1\s+'),
    'st1': re.compile(r'\bst1\s+'),
    'ld2': re.compile(r'\bld2\s+'),
    'st2': re.compile(r'\bst2\s+'),
    'ld3': re.compile(r'\bld3\s+'),
    'st3': re.compile(r'\bst3\s+'),
    'ld4': re.compile(r'\bld4\s+'),
    'st4': re.compile(r'\bst4\s+'),
    'ld1r': re.compile(r'\bld1r\s+v'),
    'ld2r': re.compile(r'\bld2r\s+v'),
    'ld3r': re.compile(r'\bld3r\s+v'),
    'ld4r': re.compile(r'\bld4r\s+v'),
    
    # Conversion
    'fcvt': re.compile(r'\bfcvtx?\s+v'),
    'fcvtzs': re.compile(r'\bfcvtzs\s+v'),
    'fcvtzu': re.compile(r'\bfcvtku\s+v'),
    'scvtf': re.compile(r'\bscvtf\s+v'),
    'ucvtf': re.compile(r'\bucvtf\s+v'),
    
    # Shuffle/Extract/Insert
    'ext': re.compile(r'\bext\s+v'),
    'zip1': re.compile(r'\bzip1\s+v'),
    'zip2': re.compile(r'\bzip2\s+v'),
    'uzp1': re.compile(r'\buzp1\s+v'),
    'uzp2': re.compile(r'\buzp2\s+v'),
    'trn1': re.compile(r'\btrn1\s+v'),
    'trn2': re.compile(r'\btrn2\s+v'),
}

# RISC-V RVV instruction patterns
RISCV_VEC_INSTRUCTIONS = {
    # Arithmetic - Floating Point
    'vfadd': re.compile(r'\bvfadd\.vv\s+v'),
    'vfsub': re.compile(r'\bvfsub\.vv\s+v'),
    'vfmul': re.compile(r'\bvfmul\.vv\s+v'),
    'vfdiv': re.compile(r'\bvfdiv\.vv\s+v'),
    'vfmin': re.compile(r'\bvfmin\.vv\s+v'),
    'vfmax': re.compile(r'\bvfmax\.vv\s+v'),
    'vfsqrt': re.compile(r'\bvfsqrt\.v\s+v'),
    'vfmadd': re.compile(r'\bvfmadd\.vvv\s+v'),
    'vfmsub': re.compile(r'\bvfmsub\.vvv\s+v'),
    
    # Arithmetic - Integer
    'vadd': re.compile(r'\bvadd\.vv\s+v'),
    'vsub': re.compile(r'\bvsub\.vv\s+v'),
    'vmul': re.compile(r'\bvmul\.vv\s+v'),
    'vdiv': re.compile(r'\bvdiv\.vv\s+v'),
    'vaddi': re.compile(r'\bvadd\.vi\s+v'),
    'vmuli': re.compile(r'\bvmul\.vi\s+v'),
    
    # Comparison
    'vfsgnj': re.compile(r'\bvfsgnj\.vv\s+v'),
    'vfsgnjn': re.compile(r'\bvfsgnjn\.vv\s+v'),
    'vfsgnjx': re.compile(r'\bvfsgnjx\.vv\s+v'),
    'vfclass': re.compile(r'\bvfclass\.v\s+v'),
    
    # Data movement
    'vmv': re.compile(r'\bvmv\.v\s+v'),
    'vfmv': re.compile(r'\bvfmv\.v\s+v'),
    
    # Load/Store
    'vle32': re.compile(r'\bvle32\.v\s+v'),
    'vle64': re.compile(r'\bvle64\.v\s+v'),
    'vle16': re.compile(r'\bvle16\.v\s+v'),
    'vlse32': re.compile(r'\bvlse32\.v\s+v'),
    'vlse64': re.compile(r'\bvlse64\.v\s+v'),
    'vse32': re.compile(r'\bvse32\.v\s+v'),
    'vse64': re.compile(r'\bvse64\.v\s+v'),
    'vse16': re.compile(r'\bvse16\.v\s+v'),
    
    # Configuration
    'vsetvli': re.compile(r'\bvsetvli\s+'),
    'vsetvl': re.compile(r'\bvsetvl\s+'),
}


class VectorValidationIssue:
    def __init__(self, severity: str, line_num: int, message: str, instruction: str = ""):
        self.severity = severity  # 'ERROR', 'WARNING', 'INFO'
        self.line_num = line_num
        self.message = message
        self.instruction = instruction
    
    def __str__(self):
        return f"[{self.severity}] Line {self.line_num}: {self.message}"


class VectorInstructionValidator:
    def __init__(self, isa: ISA = ISA.X86_64):
        self.isa = isa
        self.issues: List[VectorValidationIssue] = []
        self.vec_registers: Set[int] = set()
        self.vec_instructions_found: Dict[str, int] = {}
        
        # Set register pattern and instruction patterns based on ISA
        if isa == ISA.X86_64:
            self.reg_pattern = re.compile(r'xmm(\d+)')
            self.vec_instructions = X86_64_VEC_INSTRUCTIONS
            self.num_vec_regs = 16
        elif isa == ISA.AARCH64:
            self.reg_pattern = re.compile(r'v(\d+)')
            self.vec_instructions = AARCH64_VEC_INSTRUCTIONS
            self.num_vec_regs = 32
        else:  # RISCV64
            self.reg_pattern = re.compile(r'v(\d+)')
            self.vec_instructions = RISCV_VEC_INSTRUCTIONS
            self.num_vec_regs = 32
    
    def validate_vec_register(self, reg_str: str) -> bool:
        """Validate vector register format and range"""
        match = self.reg_pattern.search(reg_str)
        if not match:
            return False
        
        reg_num = int(match.group(1))
        if reg_num >= self.num_vec_regs:
            self.issues.append(VectorValidationIssue(
                'ERROR', 0, f"Invalid vector register: {reg_str} (must be {self.reg_pattern.pattern[:3]}0-{self.reg_pattern.pattern[:3]}{self.num_vec_regs-1})"
            ))
            return False
        
        self.vec_registers.add(reg_num)
        return True
    
    def validate_instruction(self, line: str, line_num: int) -> bool:
        """Validate a single assembly instruction"""
        line = line.strip()
        if not line or line.startswith('#') or line.startswith('.'):
            return True
        
        # Check for vector instructions
        for instr_name, pattern in self.vec_instructions.items():
            if pattern.search(line):
                self.vec_instructions_found[instr_name] = \
                    self.vec_instructions_found.get(instr_name, 0) + 1
                
                # Validate vector registers in instruction
                reg_matches = self.reg_pattern.findall(line)
                for reg in reg_matches:
                    self.validate_vec_register(f"{self.reg_pattern.pattern[:3]}{reg}")
                
                return True
        
        return True
    
    def validate_file(self, filepath: str) -> bool:
        """Validate vector instructions in assembly file"""
        try:
            with open(filepath, 'r') as f:
                lines = f.readlines()
        except IOError as e:
            self.issues.append(VectorValidationIssue(
                'ERROR', 0, f"Cannot read file: {filepath} - {e}"
            ))
            return False
        
        for line_num, line in enumerate(lines, 1):
            self.validate_instruction(line, line_num)
        
        return len([i for i in self.issues if i.severity == 'ERROR']) == 0
    
    def validate_ir(self, ir_content: str) -> bool:
        """Validate SIMD-related IR instructions"""
        lines = ir_content.split('\n')
        
        for line_num, line in enumerate(lines, 1):
            # Check for SIMD IR opcodes
            if any(op in line for op in ['OP_VADD', 'OP_VMUL', 'OP_VLOAD', 'OP_VSTORE', 
                                         'OP_VSUB', 'OP_VDIV', 'OP_VAND', 'OP_VOR', 
                                         'OP_VSHUF', 'OP_VCVT', 'OP_VCMP']):
                self.issues.append(VectorValidationIssue(
                    'INFO', line_num, f"Found vector IR instruction: {line.strip()}"
                ))
        
        return True
    
    def generate_report(self) -> str:
        """Generate validation report"""
        isa_name = self.isa.value.upper()
        report = []
        report.append("=" * 60)
        report.append(f"Vector Instruction Validation Report ({isa_name})")
        report.append("=" * 60)
        
        if self.issues:
            report.append(f"\nIssues Found: {len(self.issues)}")
            report.append("-" * 60)
            for issue in self.issues:
                report.append(str(issue))
        else:
            report.append("\nNo issues found!")
        
        if self.vec_instructions_found:
            report.append(f"\nVector Instructions Found: {len(self.vec_instructions_found)}")
            report.append("-" * 60)
            for instr, count in sorted(self.vec_instructions_found.items()):
                report.append(f"  {instr}: {count}")
        
        if self.vec_registers:
            report.append(f"\nVector Registers Used: {sorted(self.vec_registers)}")
            report.append(f"Register Range: {self.reg_pattern.pattern[:3]}0-{self.reg_pattern.pattern[:3]}{self.num_vec_regs-1}")
        
        report.append("=" * 60)
        return '\n'.join(report)


def main():
    parser = argparse.ArgumentParser(
        description='Validate vector instruction generation for ZeroCC (x86-64 SSE2, AArch64 NEON, RISC-V RVV)'
    )
    parser.add_argument('file', nargs='?', help='Assembly file to validate')
    parser.add_argument('--isa', choices=['x86_64', 'aarch64', 'riscv64'], default='x86_64',
                        help='Target ISA (default: x86_64)')
    parser.add_argument('--check-ir', metavar='IR_FILE', help='Validate IR dump')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    isa = ISA(args.isa)
    validator = VectorInstructionValidator(isa=isa)
    
    if args.check_ir:
        try:
            with open(args.check_ir, 'r') as f:
                ir_content = f.read()
            success = validator.validate_ir(ir_content)
        except IOError as e:
            print(f"Error reading IR file: {e}")
            sys.exit(1)
    elif args.file:
        success = validator.validate_file(args.file)
    else:
        parser.print_help()
        sys.exit(1)
    
    print(validator.generate_report())
    
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
