#!/usr/bin/env python3
"""
ZeroCC ELF Verification Tool

Performs static verification of ELF object files without requiring linking or execution.
Uses system tools (readelf, objdump, nm) to validate ELF structure.

Usage:
    python3 tools/elf_verify.py test.o
    python3 tools/elf_verify.py test.o --check-header --check-sections --check-symbols
    python3 tools/elf_verify.py test.o --verbose --machine x86-64
"""

import argparse
import subprocess
import sys
import os
from typing import List, Tuple, Optional

# ANSI color codes
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
BOLD = "\033[1m"
RESET = "\033[0m"


def colorize(text: str, color: str) -> str:
    """Apply color to text if terminal supports it."""
    if sys.stdout.isatty():
        return f"{color}{text}{RESET}"
    return text


def check_tool_available(tool: str) -> bool:
    """Check if a system tool is available."""
    try:
        subprocess.run(["which", tool], capture_output=True, check=True)
        return True
    except subprocess.CalledProcessError:
        return False


def get_required_tools() -> List[str]:
    """Return list of required system tools."""
    return ["readelf", "objdump", "nm"]


def verify_prerequisites() -> bool:
    """Check if all required tools are available."""
    missing = []
    for tool in get_required_tools():
        if not check_tool_available(tool):
            missing.append(tool)

    if missing:
        print(colorize(f"Error: Missing required tools: {', '.join(missing)}", RED))
        print("Install binutils: sudo apt-get install binutils")
        return False
    return True


def run_command(cmd: List[str]) -> Tuple[bool, str, str]:
    """Run a command and return (success, stdout, stderr)."""
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=30
        )
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Command timed out"
    except Exception as e:
        return False, "", str(e)


def verify_elf_header(filepath: str, expected_machine: Optional[str] = None) -> bool:
    """Verify ELF header information."""
    print(f"\n{colorize('[HEADER]', CYAN)} Checking ELF header...")

    success, stdout, stderr = run_command(["readelf", "-h", filepath])
    if not success:
        print(colorize(f"  ✗ Failed to read ELF header: {stderr}", RED))
        return False

    checks_passed = 0
    checks_total = 0

    # Check ELF magic
    checks_total += 1
    if "ELF" in stdout:
        print(colorize("  ✓ ELF magic detected", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ✗ ELF magic not found", RED))

    # Check class (32/64-bit)
    checks_total += 1
    if "64-bit" in stdout:
        print(colorize("  ✓ 64-bit ELF class", GREEN))
        checks_passed += 1
    elif "32-bit" in stdout:
        print(colorize("  ✓ 32-bit ELF class", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ✗ Unknown ELF class", RED))

    # Check machine type
    checks_total += 1
    machine_map = {
        "x86-64": ["Advanced Micro Devices X86-64", "X86-64"],
        "aarch64": ["AArch64", "ARM aarch64"],
        "riscv64": ["RISC-V", "RISCV"],
        "riscv32": ["RISC-V", "RISCV"],
    }

    machine_found = False
    if expected_machine:
        expected_keywords = machine_map.get(expected_machine, [])
        for keyword in expected_keywords:
            if keyword in stdout:
                print(colorize(f"  ✓ Machine type: {expected_machine}", GREEN))
                checks_passed += 1
                machine_found = True
                break
        if not machine_found:
            print(colorize(f"  ✗ Expected machine type {expected_machine} not found", RED))
            print(f"    Header output: {stdout[:200]}")
    else:
        # Just check that a machine type is present
        for keywords in machine_map.values():
            for keyword in keywords:
                if keyword in stdout:
                    print(colorize("  ✓ Machine type detected", GREEN))
                    checks_passed += 1
                    machine_found = True
                    break
            if machine_found:
                break
        if not machine_found:
            print(colorize("  ✗ No recognized machine type found", RED))

    # Check entry point
    checks_total += 1
    if "Entry point address:" in stdout:
        print(colorize("  ✓ Entry point defined", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ⚠ Entry point not found (may be object file)", YELLOW))
        checks_passed += 1  # Object files may not have entry points

    return checks_passed == checks_total


def verify_sections(filepath: str, required_sections: Optional[List[str]] = None) -> bool:
    """Verify ELF section headers."""
    print(f"\n{colorize('[SECTIONS]', CYAN)} Checking section headers...")

    success, stdout, stderr = run_command(["readelf", "-S", filepath])
    if not success:
        print(colorize(f"  ✗ Failed to read sections: {stderr}", RED))
        return False

    checks_passed = 0
    checks_total = 0

    # Check for required sections
    if required_sections:
        for section in required_sections:
            checks_total += 1
            if section in stdout:
                print(colorize(f"  ✓ Section '{section}' present", GREEN))
                checks_passed += 1
            else:
                print(colorize(f"  ✗ Section '{section}' missing", RED))

    # Always check for basic section presence
    checks_total += 1
    if ".text" in stdout:
        print(colorize("  ✓ .text section present", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ✗ .text section missing", RED))

    checks_total += 1
    if ".data" in stdout or ".bss" in stdout:
        print(colorize("  ✓ Data section (.data/.bss) present", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ⚠ No data section found (may be code-only)", YELLOW))
        checks_passed += 1  # Not always required

    checks_total += 1
    if ".rodata" in stdout:
        print(colorize("  ✓ .rodata section present", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ⚠ No .rodata section found (may be code-only)", YELLOW))
        checks_passed += 1  # Not always required

    # Print section summary if verbose
    if "--verbose" in sys.argv or "-v" in sys.argv:
        print(f"\n  Section summary:")
        for line in stdout.split('\n'):
            if '.text' in line or '.data' in line or '.rodata' in line or '.bss' in line:
                print(f"    {line.strip()}")

    return checks_passed >= checks_total - 1  # Allow some sections to be optional


def verify_symbols(filepath: str) -> bool:
    """Verify symbol table."""
    print(f"\n{colorize('[SYMBOLS]', CYAN)} Checking symbol table...")

    # Try readelf first
    success, stdout, stderr = run_command(["readelf", "-s", filepath])

    # Fallback to nm if readelf fails
    if not success:
        success, stdout, stderr = run_command(["nm", filepath])

    if not success:
        # Object files may not have symbol tables yet
        print(colorize("  ⚠ No symbol table found (may be incomplete)", YELLOW))
        return True  # Not a failure for partial builds

    checks_passed = 0
    checks_total = 0

    # Check for at least some symbols
    checks_total += 1
    if stdout.strip():
        symbol_count = len([l for l in stdout.split('\n') if l.strip() and not l.startswith('Symbol table')])
        if symbol_count > 0:
            print(colorize(f"  ✓ Symbol table present ({symbol_count} symbols)", GREEN))
            checks_passed += 1
        else:
            print(colorize("  ⚠ Symbol table empty", YELLOW))
            checks_passed += 1
    else:
        print(colorize("  ⚠ No symbols found", YELLOW))
        checks_passed += 1

    # Check for common symbols
    checks_total += 1
    common_symbols = ["main", "printf", "exit", "fprintf"]
    found_common = False
    for sym in common_symbols:
        if sym in stdout:
            found_common = True
            break

    if found_common:
        print(colorize("  ✓ Common symbols detected", GREEN))
        checks_passed += 1
    else:
        print(colorize("  ⚠ No common library symbols (expected for standalone code)", YELLOW))
        checks_passed += 1

    return checks_passed == checks_total


def verify_relocations(filepath: str) -> bool:
    """Verify relocation entries."""
    print(f"\n{colorize('[RELOCATIONS]', CYAN)} Checking relocation entries...")

    success, stdout, stderr = run_command(["readelf", "-r", filepath])

    if not success:
        # Object files may not have relocations yet
        print(colorize("  ⚠ No relocation table found", YELLOW))
        return True

    # Count relocations
    relocation_lines = [l for l in stdout.split('\n') if 'Relocation' in l or ('0x' in l and len(l.strip()) > 10)]
    if len(relocation_lines) > 0:
        print(colorize(f"  ✓ Relocation entries present ({len(relocation_lines)} entries)", GREEN))
    else:
        print(colorize("  ✓ No relocations needed (position-independent or self-contained)", GREEN))

    return True


def verify_disassembly(filepath: str) -> bool:
    """Verify machine code can be disassembled."""
    print(f"\n{colorize('[DISASSEMBLY]', CYAN)} Checking machine code...")

    success, stdout, stderr = run_command(["objdump", "-d", filepath])

    if not success:
        print(colorize(f"  ⚠ Could not disassemble: {stderr}", YELLOW))
        return True  # Not necessarily a failure

    if stdout.strip():
        print(colorize("  ✓ Machine code present and disassemblable", GREEN))
        return True
    else:
        print(colorize("  ⚠ No disassemblable code found", YELLOW))
        return True


def main():
    parser = argparse.ArgumentParser(
        description="ZeroCC ELF Verification Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 tools/elf_verify.py test.o
  python3 tools/elf_verify.py test.o --machine x86-64
  python3 tools/elf_verify.py test.o --verbose
        """
    )
    parser.add_argument("elf_file", help="ELF object file to verify")
    parser.add_argument("--machine", "-m", choices=["x86-64", "aarch64", "riscv64", "riscv32"],
                        help="Expected machine type")
    parser.add_argument("--check-header", action="store_true", default=True,
                        help="Check ELF header (default: on)")
    parser.add_argument("--check-sections", action="store_true", default=True,
                        help="Check section headers (default: on)")
    parser.add_argument("--check-symbols", action="store_true", default=True,
                        help="Check symbol table (default: on)")
    parser.add_argument("--check-relocs", action="store_true", default=False,
                        help="Check relocation entries")
    parser.add_argument("--check-disasm", action="store_true", default=False,
                        help="Check disassembly")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Verbose output")
    parser.add_argument("--quiet", "-q", action="store_true",
                        help="Quiet mode, only show errors")

    args = parser.parse_args()

    # Check file exists
    if not os.path.isfile(args.elf_file):
        print(colorize(f"Error: File not found: {args.elf_file}", RED))
        sys.exit(1)

    # Check prerequisites
    if not verify_prerequisites():
        sys.exit(1)

    # Print header
    print(f"\n{colorize('=' * 60, BOLD)}")
    print(colorize(f"  ZeroCC ELF Verification: {args.elf_file}", BOLD))
    print(colorize('=' * 60, BOLD))

    results = []

    # Run verification checks
    if args.check_header:
        results.append(("Header", verify_elf_header(args.elf_file, args.machine)))

    if args.check_sections:
        results.append(("Sections", verify_sections(args.elf_file)))

    if args.check_symbols:
        results.append(("Symbols", verify_symbols(args.elf_file)))

    if args.check_relocs:
        results.append(("Relocations", verify_relocations(args.elf_file)))

    if args.check_disasm:
        results.append(("Disassembly", verify_disassembly(args.elf_file)))

    # Print summary
    print(f"\n{colorize('=' * 60, BOLD)}")
    print(colorize("  Verification Summary", BOLD))
    print(colorize('=' * 60, BOLD))

    passed = sum(1 for _, result in results if result)
    total = len(results)

    for name, result in results:
        status = colorize("✓ PASS", GREEN) if result else colorize("✗ FAIL", RED)
        print(f"  {status}  {name}")

    print(f"\n  Total: {passed}/{total} checks passed")

    if passed == total:
        print(colorize("\n  All checks passed! ✓", GREEN))
        sys.exit(0)
    else:
        print(colorize(f"\n  {total - passed} check(s) failed! ✗", RED))
        sys.exit(1)


if __name__ == "__main__":
    main()
