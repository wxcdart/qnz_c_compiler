#!/usr/bin/env python3
"""pytrace - Lightweight Python-based ptrace debugger and crash diagnostics tool for ZeroCC.

Provides zero-dependency non-interactive crash triaging, JIT execution inspection,
system call tracing, and interactive instruction-level debugging without requiring GDB.

Supports x86-64, AArch64, ARM (AArch32), and RISC-V (64/32-bit) architectures.

Usage:
  python3 tools/analysis/pytrace.py [OPTIONS] <executable> [args...]

Examples:
  # Run and automatically capture detailed crash dump if a segfault/abort occurs:
  python3 tools/analysis/pytrace.py ./build/bin/zerocc -Iinclude -Isrc --run test/failing.c

  # Trace system calls (lightweight strace):
  python3 tools/analysis/pytrace.py --strace ./build/bin/zerocc -Iinclude -Isrc --run test/jit/basic.c

  # Interactive instruction-level debugging:
  python3 tools/analysis/pytrace.py -i ./build/bin/zerocc -Iinclude -Isrc --run test/jit/basic.c
"""

import argparse
import ctypes
import os
import platform
import re
import signal
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Repository root path resolution (docs/coding_style.md & AGENTS.md rules)
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root
from tools.analysis import elf_min

ROOT = find_repo_root()

# Load host libc
libc = ctypes.CDLL(None)

# Standard Linux Ptrace Request Constants
PTRACE_TRACEME = 0
PTRACE_PEEKTEXT = 1
PTRACE_PEEKDATA = 2
PTRACE_POKETEXT = 4
PTRACE_POKEDATA = 5
PTRACE_CONT = 7
PTRACE_KILL = 8
PTRACE_SINGLESTEP = 9
PTRACE_GETREGS = 12
PTRACE_SETREGS = 13
PTRACE_ATTACH = 16
PTRACE_DETACH = 17
PTRACE_SYSCALL = 24
PTRACE_SETOPTIONS = 0x4200
PTRACE_GETSIGINFO = 0x4202
PTRACE_SETSIGINFO = 0x4203
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205

# Ptrace options
PTRACE_O_TRACESYSGOOD = 1
PTRACE_O_TRACEFORK = 1 << 1
PTRACE_O_TRACEVFORK = 1 << 2
PTRACE_O_TRACECLONE = 1 << 3
PTRACE_O_TRACEEXEC = 1 << 4
PTRACE_O_EXITKILL = 1 << 20

# Configure libc.ptrace types cleanly on 64-bit systems
libc.ptrace.argtypes = [
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_void_p,
]
libc.ptrace.restype = ctypes.c_long

NT_PRSTATUS = 1


# struct iovec for PTRACE_GETREGSET / PTRACE_SETREGSET
class IOVec(ctypes.Structure):
    _fields_ = [
        ("iov_base", ctypes.c_void_p),
        ("iov_len", ctypes.c_size_t),
    ]


# Colors for terminal output
COLOR_BOLD = "\033[1m"
COLOR_RED = "\033[31m"
COLOR_GREEN = "\033[32m"
COLOR_YELLOW = "\033[33m"
COLOR_BLUE = "\033[34m"
COLOR_MAGENTA = "\033[35m"
COLOR_CYAN = "\033[36m"
COLOR_DIM = "\033[2m"
COLOR_RESET = "\033[0m"


def disable_colors():
    global COLOR_BOLD, COLOR_RED, COLOR_GREEN, COLOR_YELLOW
    global COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_DIM, COLOR_RESET
    COLOR_BOLD = COLOR_RED = COLOR_GREEN = COLOR_YELLOW = ""
    COLOR_BLUE = COLOR_MAGENTA = COLOR_CYAN = COLOR_DIM = COLOR_RESET = ""


# x86-64 user_regs_struct (216 bytes)
class UserRegsX86_64(ctypes.Structure):
    _fields_ = [
        ("r15", ctypes.c_ulonglong),
        ("r14", ctypes.c_ulonglong),
        ("r13", ctypes.c_ulonglong),
        ("r12", ctypes.c_ulonglong),
        ("rbp", ctypes.c_ulonglong),
        ("rbx", ctypes.c_ulonglong),
        ("r11", ctypes.c_ulonglong),
        ("r10", ctypes.c_ulonglong),
        ("r9", ctypes.c_ulonglong),
        ("r8", ctypes.c_ulonglong),
        ("rax", ctypes.c_ulonglong),
        ("rcx", ctypes.c_ulonglong),
        ("rdx", ctypes.c_ulonglong),
        ("rsi", ctypes.c_ulonglong),
        ("rdi", ctypes.c_ulonglong),
        ("orig_rax", ctypes.c_ulonglong),
        ("rip", ctypes.c_ulonglong),
        ("cs", ctypes.c_ulonglong),
        ("eflags", ctypes.c_ulonglong),
        ("rsp", ctypes.c_ulonglong),
        ("ss", ctypes.c_ulonglong),
        ("fs_base", ctypes.c_ulonglong),
        ("gs_base", ctypes.c_ulonglong),
        ("ds", ctypes.c_ulonglong),
        ("es", ctypes.c_ulonglong),
        ("fs", ctypes.c_ulonglong),
        ("gs", ctypes.c_ulonglong),
    ]


# AArch64 user_pt_regs (272 bytes: x0-x30, sp, pc, pstate)
class UserRegsAArch64(ctypes.Structure):
    _fields_ = [
        ("regs", ctypes.c_ulonglong * 31),
        ("sp", ctypes.c_ulonglong),
        ("pc", ctypes.c_ulonglong),
        ("pstate", ctypes.c_ulonglong),
    ]


# ARM (AArch32) pt_regs (72 bytes: r0-r15, cpsr, orig_r0)
class UserRegsARM(ctypes.Structure):
    _fields_ = [
        ("uregs", ctypes.c_uint32 * 18),
    ]


# RISC-V 64 user_regs_struct (256 bytes: pc, ra, sp, gp, tp, t0-t2, s0, s1, a0-a7, s2-s11, t3-t6)
class UserRegsRISCV64(ctypes.Structure):
    _fields_ = [
        ("pc", ctypes.c_ulonglong),
        ("ra", ctypes.c_ulonglong),
        ("sp", ctypes.c_ulonglong),
        ("gp", ctypes.c_ulonglong),
        ("tp", ctypes.c_ulonglong),
        ("t0", ctypes.c_ulonglong),
        ("t1", ctypes.c_ulonglong),
        ("t2", ctypes.c_ulonglong),
        ("s0", ctypes.c_ulonglong),
        ("s1", ctypes.c_ulonglong),
        ("a0", ctypes.c_ulonglong),
        ("a1", ctypes.c_ulonglong),
        ("a2", ctypes.c_ulonglong),
        ("a3", ctypes.c_ulonglong),
        ("a4", ctypes.c_ulonglong),
        ("a5", ctypes.c_ulonglong),
        ("a6", ctypes.c_ulonglong),
        ("a7", ctypes.c_ulonglong),
        ("s2", ctypes.c_ulonglong),
        ("s3", ctypes.c_ulonglong),
        ("s4", ctypes.c_ulonglong),
        ("s5", ctypes.c_ulonglong),
        ("s6", ctypes.c_ulonglong),
        ("s7", ctypes.c_ulonglong),
        ("s8", ctypes.c_ulonglong),
        ("s9", ctypes.c_ulonglong),
        ("s10", ctypes.c_ulonglong),
        ("s11", ctypes.c_ulonglong),
        ("t3", ctypes.c_ulonglong),
        ("t4", ctypes.c_ulonglong),
        ("t5", ctypes.c_ulonglong),
        ("t6", ctypes.c_ulonglong),
    ]


# RISC-V 32 user_regs_struct (128 bytes)
class UserRegsRISCV32(ctypes.Structure):
    _fields_ = [
        ("pc", ctypes.c_uint32),
        ("ra", ctypes.c_uint32),
        ("sp", ctypes.c_uint32),
        ("gp", ctypes.c_uint32),
        ("tp", ctypes.c_uint32),
        ("t0", ctypes.c_uint32),
        ("t1", ctypes.c_uint32),
        ("t2", ctypes.c_uint32),
        ("s0", ctypes.c_uint32),
        ("s1", ctypes.c_uint32),
        ("a0", ctypes.c_uint32),
        ("a1", ctypes.c_uint32),
        ("a2", ctypes.c_uint32),
        ("a3", ctypes.c_uint32),
        ("a4", ctypes.c_uint32),
        ("a5", ctypes.c_uint32),
        ("a6", ctypes.c_uint32),
        ("a7", ctypes.c_uint32),
        ("s2", ctypes.c_uint32),
        ("s3", ctypes.c_uint32),
        ("s4", ctypes.c_uint32),
        ("s5", ctypes.c_uint32),
        ("s6", ctypes.c_uint32),
        ("s7", ctypes.c_uint32),
        ("s8", ctypes.c_uint32),
        ("s9", ctypes.c_uint32),
        ("s10", ctypes.c_uint32),
        ("s11", ctypes.c_uint32),
        ("t3", ctypes.c_uint32),
        ("t4", ctypes.c_uint32),
        ("t5", ctypes.c_uint32),
        ("t6", ctypes.c_uint32),
    ]


# Linux siginfo_t (128 bytes total)
class SigInfo(ctypes.Structure):
    _fields_ = [
        ("si_signo", ctypes.c_int),
        ("si_errno", ctypes.c_int),
        ("si_code", ctypes.c_int),
        ("__pad0", ctypes.c_int),
        ("si_addr", ctypes.c_void_p),
        ("__pad", ctypes.c_char * 104),
    ]


# Signal Code Lookups
SIG_CODES = {
    signal.SIGSEGV: {
        1: "SEGV_MAPERR (address not mapped to object)",
        2: "SEGV_ACCERR (invalid permissions for mapped object)",
        3: "SEGV_BNDERR (failed address bound checks)",
        4: "SEGV_PKUERR (access denied by memory protection keys)",
    },
    signal.SIGILL: {
        1: "ILL_ILLOPC (illegal opcode)",
        2: "ILL_ILLOPN (illegal operand)",
        3: "ILL_ILLADR (illegal addressing mode)",
        4: "ILL_ILLTRP (illegal trap)",
        5: "ILL_PRVOPC (privileged opcode)",
        6: "ILL_PRVREG (privileged register)",
        7: "ILL_COPROC (coprocessor error)",
        8: "ILL_BADSTK (internal stack error)",
    },
    signal.SIGBUS: {
        1: "BUS_ADRALN (invalid address alignment)",
        2: "BUS_ADRERR (nonexistent physical address)",
        3: "BUS_OBJERR (object-specific hardware error)",
    },
    signal.SIGFPE: {
        1: "FPE_INTDIV (integer divide by zero)",
        2: "FPE_INTOVF (integer overflow)",
        3: "FPE_FLTDIV (floating-point divide by zero)",
        4: "FPE_FLTOVF (floating-point overflow)",
        5: "FPE_FLTUND (floating-point underflow)",
        6: "FPE_FLTRES (floating-point inexact result)",
        7: "FPE_FLTINV (floating-point invalid operation)",
    },
    signal.SIGTRAP: {
        1: "TRAP_BRKPT (process breakpoint / trap)",
        2: "TRAP_TRACE (process trace trap / single-step)",
        3: "TRAP_BRANCH (process branch trap)",
        4: "TRAP_HWBKPT (hardware breakpoint/watchpoint)",
    },
}

# Common x86_64 Linux System Calls
SYSCALL_NAMES_X86_64 = {
    0: "read",
    1: "write",
    2: "open",
    3: "close",
    4: "stat",
    5: "fstat",
    6: "lstat",
    7: "poll",
    8: "lseek",
    9: "mmap",
    10: "mprotect",
    11: "munmap",
    12: "brk",
    13: "rt_sigaction",
    14: "rt_sigprocmask",
    16: "ioctl",
    20: "writev",
    21: "access",
    22: "pipe",
    24: "sched_yield",
    28: "madvise",
    39: "getpid",
    41: "socket",
    42: "connect",
    43: "accept",
    56: "clone",
    57: "fork",
    59: "execve",
    60: "exit",
    61: "wait4",
    62: "kill",
    72: "fcntl",
    79: "getcwd",
    80: "chdir",
    89: "readlink",
    158: "arch_prctl",
    202: "futex",
    218: "set_tid_address",
    228: "clock_gettime",
    231: "exit_group",
    257: "openat",
    262: "newfstatat",
    318: "getrandom",
}


def canonical_arch(raw_arch):
    raw = raw_arch.lower()
    if raw in ("x86_64", "amd64", "x64"):
        return "x86_64"
    if raw in ("aarch64", "arm64"):
        return "aarch64"
    if raw.startswith("arm") or raw in ("aarch32", "armhf", "armel"):
        return "arm"
    if raw in ("riscv64", "rv64", "riscv64gc"):
        return "riscv64"
    if raw in ("riscv32", "rv32", "riscv32gc"):
        return "riscv32"
    return raw


# Architecture specific breakpoint instruction bytes
ARCH_BREAKPOINTS = {
    "x86_64": bytes([0xCC]),  # int3 (1 byte)
    "aarch64": struct.pack("<I", 0xD4200000),  # brk #0 (4 bytes)
    "arm": struct.pack("<I", 0xE7F001F0),  # bkpt #0 (4 bytes ARM mode)
    "riscv64": struct.pack("<I", 0x00100073),  # ebreak (4 bytes)
    "riscv32": struct.pack("<I", 0x00100073),  # ebreak (4 bytes)
}

# Architecture specific objdump machine strings
ARCH_OBJDUMP_MACH = {
    "x86_64": "i386:x86-64",
    "aarch64": "aarch64",
    "arm": "arm",
    "riscv64": "riscv:rv64",
    "riscv32": "riscv:rv32",
}


class MemoryMap:
    def __init__(self, start, end, perms, offset, dev, inode, pathname):
        self.start = start
        self.end = end
        self.perms = perms
        self.offset = offset
        self.dev = dev
        self.inode = inode
        self.pathname = pathname

    def contains(self, addr):
        return self.start <= addr < self.end

    def is_executable(self):
        return "x" in self.perms

    def is_writable(self):
        return "w" in self.perms

    def describe(self):
        tag = self.pathname if self.pathname else "[anon]"
        if not self.pathname and "x" in self.perms:
            tag = "[anon: JIT code RX]"
        return f"0x{self.start:012x}-0x{self.end:012x} {self.perms:4s} {tag}"


class PyTracer:
    def __init__(self, args, cmd, is_interactive=False, do_strace=False):
        self.args = args
        self.cmd = cmd
        self.is_interactive = is_interactive
        self.do_strace = do_strace
        self.do_leak_check = getattr(args, "leak_check", False) or getattr(
            args, "heap_check", False
        )
        self.pid = None
        self.arch = canonical_arch(platform.machine())
        self.breakpoints = {}  # addr -> orig_bytes
        self.maps = []
        self.insn_count = 0
        self.in_syscall = False
        self.last_syscall_num = None

        # Heap & Leak tracking structures
        self.active_allocs = {}  # ptr -> {'size': sz, 'callsite': pc, 'id': n}
        self.freed_allocs = {}  # ptr -> (info, free_callsite)
        self.pending_alloc_ret = {}  # ret_addr -> alloc_metadata
        self.heap_errors = []
        self.total_alloc_count = 0
        self.total_free_count = 0
        self.total_allocated_bytes = 0
        self.sym_malloc = None
        self.sym_calloc = None
        self.sym_realloc = None
        self.sym_free = None
        self.symbols_by_addr = {}

    def resolve_symbols(self):
        """Extract allocator and function symbols from ELF binary if available."""
        bin_path = self.cmd[0]
        if not os.path.exists(bin_path):
            for p in os.environ.get("PATH", "").split(os.pathsep):
                cand = os.path.join(p, bin_path)
                if os.path.isfile(cand) and os.access(cand, os.X_OK):
                    bin_path = cand
                    break

        try:
            elf = elf_min.load(bin_path)
            for s in elf.symbols:
                if s.name and s.value > 0:
                    self.symbols_by_addr[s.value] = s.name
                    if s.name in ("malloc", "rt_malloc"):
                        self.sym_malloc = s.value
                    elif s.name in ("calloc", "rt_calloc"):
                        self.sym_calloc = s.value
                    elif s.name in ("realloc", "rt_realloc"):
                        self.sym_realloc = s.value
                    elif s.name in ("free", "rt_free"):
                        self.sym_free = s.value
        except Exception:
            pass

    def lookup_symbol(self, addr):
        """Format an address as symbol+offset if available."""
        if addr in self.symbols_by_addr:
            return self.symbols_by_addr[addr]
        best_sym = None
        best_delta = 0xFFFFFFFF
        for sym_addr, name in self.symbols_by_addr.items():
            if sym_addr <= addr and (addr - sym_addr) < best_delta:
                best_delta = addr - sym_addr
                best_sym = name
        if best_sym and best_delta < 0x2000:
            return f"{best_sym}+0x{best_delta:x}"
        return f"0x{addr:x}"

    def print_leak_summary(self):
        print("\n" + "=" * 60)
        print(f"{COLOR_BOLD}=== pytrace: HEAP & LEAK SUMMARY ==={COLOR_RESET}")
        print("=" * 60)
        print(
            f"Total heap allocations: {self.total_alloc_count} ({self.total_allocated_bytes} bytes)"
        )
        print(f"Total heap frees:       {self.total_free_count}")

        if self.heap_errors:
            print("\n" + "-" * 60)
            print(
                f"{COLOR_BOLD}{COLOR_RED}*** HEAP CORRUPTION DETECTED ({len(self.heap_errors)} error(s)) ***{COLOR_RESET}"
            )
            print("-" * 60)
            for err in self.heap_errors:
                print(f"  {COLOR_RED}[!] {err}{COLOR_RESET}")

        print("\n" + "-" * 60)
        print(f"{COLOR_BOLD}LEAK SUMMARY:{COLOR_RESET}")
        print("-" * 60)
        if not self.active_allocs:
            print(
                f"{COLOR_GREEN}All heap blocks were freed -- no leaks are possible!{COLOR_RESET}"
            )
        else:
            total_lost = sum(i["size"] for i in self.active_allocs.values())
            print(
                f"{COLOR_RED}definitely lost: {total_lost} bytes in {len(self.active_allocs)} block(s){COLOR_RESET}"
            )
            for ptr, info in sorted(
                self.active_allocs.items(), key=lambda x: x[1]["id"]
            ):
                callsite_str = self.lookup_symbol(info["callsite"])
                print(
                    f"  {COLOR_YELLOW}block 0x{ptr:012x}{COLOR_RESET}: {info['size']:>5d} bytes allocated at {COLOR_CYAN}{callsite_str}{COLOR_RESET}"
                )
        print("=" * 60 + "\n")

    def read_maps(self):
        self.maps = []
        if not self.pid:
            return
        try:
            with open(f"/proc/{self.pid}/maps", "r") as f:
                for line in f:
                    parts = line.strip().split(maxsplit=5)
                    if len(parts) >= 5:
                        addrs = parts[0].split("-")
                        start = int(addrs[0], 16)
                        end = int(addrs[1], 16)
                        perms = parts[1]
                        offset = int(parts[2], 16)
                        dev = parts[3]
                        inode = int(parts[4])
                        pathname = parts[5] if len(parts) > 5 else ""
                        self.maps.append(
                            MemoryMap(
                                start, end, perms, offset, dev, inode, pathname
                            )
                        )
        except Exception:
            pass

    def locate_addr(self, addr):
        if addr is None:
            return "NULL"
        if addr == 0:
            return "0x0 (NULL pointer)"
        if addr < 0x1000:
            return f"0x{addr:x} (low page access / NULL offset)"
        for m in self.maps:
            if m.contains(addr):
                tag = m.pathname if m.pathname else "[anon]"
                if not m.pathname and "x" in m.perms:
                    tag = "[anon: JIT code RX]"
                return f"0x{addr:x} in {tag} ({m.perms})"
        return f"0x{addr:x} [unmapped address]"

    def read_mem(self, addr, length):
        if not self.pid:
            return None
        # Try reading through /proc/{pid}/mem first (fastest)
        try:
            with open(f"/proc/{self.pid}/mem", "rb", 0) as f:
                f.seek(addr)
                data = f.read(length)
                if data:
                    return data
        except Exception:
            pass

        # Fallback to PTRACE_PEEKDATA word-by-word
        res = bytearray()
        for offset in range(0, length, 8):
            cur = addr + offset
            val = libc.ptrace(
                PTRACE_PEEKDATA, self.pid, ctypes.c_void_p(cur), None
            )
            if val == -1:
                break
            chunk = struct.pack("<q", val)
            res.extend(chunk[: min(8, length - offset)])
        return bytes(res) if res else None

    def write_mem(self, addr, data_bytes):
        """Write bytes to tracee memory word-by-word."""
        cur = addr
        offset = 0
        length = len(data_bytes)
        while offset < length:
            aligned_addr = cur & ~7
            in_word_offset = cur & 7
            val = libc.ptrace(
                PTRACE_PEEKTEXT, self.pid, ctypes.c_void_p(aligned_addr), None
            )
            if val == -1:
                return False
            raw = bytearray(struct.pack("<q", val))
            chunk_len = min(8 - in_word_offset, length - offset)
            raw[in_word_offset : in_word_offset + chunk_len] = data_bytes[
                offset : offset + chunk_len
            ]
            new_val = struct.unpack("<q", raw)[0]
            res = libc.ptrace(
                PTRACE_POKETEXT,
                self.pid,
                ctypes.c_void_p(aligned_addr),
                ctypes.c_void_p(new_val),
            )
            if res == -1:
                return False
            cur += chunk_len
            offset += chunk_len
        return True

    def set_breakpoint(self, addr, verbose=False):
        bp_bytes = ARCH_BREAKPOINTS.get(self.arch, ARCH_BREAKPOINTS["x86_64"])
        length = len(bp_bytes)
        orig_bytes = self.read_mem(addr, length)
        if not orig_bytes:
            if verbose or self.args.verbose:
                print(
                    f"{COLOR_RED}Error: Cannot read memory at 0x{addr:x}{COLOR_RESET}"
                )
            return False
        if self.write_mem(addr, bp_bytes):
            self.breakpoints[addr] = orig_bytes
            if verbose or self.args.verbose:
                print(
                    f"{COLOR_GREEN}Breakpoint set at 0x{addr:x} (orig: {orig_bytes.hex()}){COLOR_RESET}"
                )
            return True
        else:
            if verbose or self.args.verbose:
                print(
                    f"{COLOR_RED}Failed to set breakpoint at 0x{addr:x}{COLOR_RESET}"
                )
            return False

    def remove_breakpoint(self, addr, verbose=False):
        if addr in self.breakpoints:
            orig_bytes = self.breakpoints[addr]
            self.write_mem(addr, orig_bytes)
            del self.breakpoints[addr]
            if verbose or self.args.verbose:
                print(f"{COLOR_YELLOW}Breakpoint removed at 0x{addr:x}{COLOR_RESET}")
            return True
        return False

    def get_regs(self):
        if self.arch == "x86_64":
            regs = UserRegsX86_64()
            iov = IOVec(
                ctypes.cast(ctypes.byref(regs), ctypes.c_void_p),
                ctypes.sizeof(regs),
            )
            res = libc.ptrace(
                PTRACE_GETREGSET,
                self.pid,
                ctypes.c_void_p(NT_PRSTATUS),
                ctypes.byref(iov),
            )
            if res == 0:
                return regs
        elif self.arch == "aarch64":
            regs = UserRegsAArch64()
            iov = IOVec(
                ctypes.cast(ctypes.byref(regs), ctypes.c_void_p),
                ctypes.sizeof(regs),
            )
            if (
                libc.ptrace(
                    PTRACE_GETREGSET,
                    self.pid,
                    ctypes.c_void_p(NT_PRSTATUS),
                    ctypes.byref(iov),
                )
                == 0
            ):
                return regs
        elif self.arch == "arm":
            regs = UserRegsARM()
            iov = IOVec(
                ctypes.cast(ctypes.byref(regs), ctypes.c_void_p),
                ctypes.sizeof(regs),
            )
            if (
                libc.ptrace(
                    PTRACE_GETREGSET,
                    self.pid,
                    ctypes.c_void_p(NT_PRSTATUS),
                    ctypes.byref(iov),
                )
                == 0
            ):
                return regs
        elif self.arch == "riscv64":
            regs = UserRegsRISCV64()
            iov = IOVec(
                ctypes.cast(ctypes.byref(regs), ctypes.c_void_p),
                ctypes.sizeof(regs),
            )
            if (
                libc.ptrace(
                    PTRACE_GETREGSET,
                    self.pid,
                    ctypes.c_void_p(NT_PRSTATUS),
                    ctypes.byref(iov),
                )
                == 0
            ):
                return regs
        elif self.arch == "riscv32":
            regs = UserRegsRISCV32()
            iov = IOVec(
                ctypes.cast(ctypes.byref(regs), ctypes.c_void_p),
                ctypes.sizeof(regs),
            )
            if (
                libc.ptrace(
                    PTRACE_GETREGSET,
                    self.pid,
                    ctypes.c_void_p(NT_PRSTATUS),
                    ctypes.byref(iov),
                )
                == 0
            ):
                return regs
        return None

    def set_regs(self, regs):
        iov = IOVec(
            ctypes.cast(ctypes.byref(regs), ctypes.c_void_p),
            ctypes.sizeof(regs),
        )
        return (
            libc.ptrace(
                PTRACE_SETREGSET,
                self.pid,
                ctypes.c_void_p(NT_PRSTATUS),
                ctypes.byref(iov),
            )
            == 0
        )

    def get_pc(self, regs):
        if not regs:
            return 0
        if self.arch == "x86_64":
            return regs.rip
        if self.arch == "aarch64":
            return regs.pc
        if self.arch == "arm":
            return regs.uregs[15]  # r15 = pc
        if self.arch in ("riscv64", "riscv32"):
            return regs.pc
        return 0

    def set_pc(self, regs, val):
        if not regs:
            return
        if self.arch == "x86_64":
            regs.rip = val
        elif self.arch == "aarch64":
            regs.pc = val
        elif self.arch == "arm":
            regs.uregs[15] = val
        elif self.arch in ("riscv64", "riscv32"):
            regs.pc = val

    def get_sp(self, regs):
        if not regs:
            return 0
        if self.arch == "x86_64":
            return regs.rsp
        if self.arch == "aarch64":
            return regs.sp
        if self.arch == "arm":
            return regs.uregs[13]  # r13 = sp
        if self.arch in ("riscv64", "riscv32"):
            return regs.sp
        return 0

    def get_siginfo(self):
        info = SigInfo()
        res = libc.ptrace(
            PTRACE_GETSIGINFO, self.pid, None, ctypes.byref(info)
        )
        if res == 0:
            return info
        return None

    def format_eflags(self, eflags):
        flags = []
        if eflags & 0x0001:
            flags.append("CF")
        if eflags & 0x0004:
            flags.append("PF")
        if eflags & 0x0010:
            flags.append("AF")
        if eflags & 0x0040:
            flags.append("ZF")
        if eflags & 0x0080:
            flags.append("SF")
        if eflags & 0x0100:
            flags.append("TF")
        if eflags & 0x0200:
            flags.append("IF")
        if eflags & 0x0400:
            flags.append("DF")
        if eflags & 0x0800:
            flags.append("OF")
        return " ".join(flags)

    def print_regs(self, regs, fault_addr=None):
        if not regs:
            print("Registers unavailable.")
            return

        def fmt_reg(name, val, is_32=False):
            is_fault = (
                fault_addr is not None and abs(val - fault_addr) < 4096
            )
            color = (
                f"{COLOR_BOLD}{COLOR_RED}"
                if is_fault
                else f"{COLOR_BOLD}{COLOR_CYAN}"
            )
            val_color = COLOR_YELLOW if is_fault else COLOR_RESET
            if is_32:
                return f"{color}{name:>4s}{COLOR_RESET}: {val_color}0x{val:08x}{COLOR_RESET}"
            return f"{color}{name:>4s}{COLOR_RESET}: {val_color}0x{val:016x}{COLOR_RESET}"

        if self.arch == "x86_64":
            print(
                f"  {fmt_reg('RAX', regs.rax)}  {fmt_reg('RBX', regs.rbx)}  {fmt_reg('RCX', regs.rcx)}"
            )
            print(
                f"  {fmt_reg('RDX', regs.rdx)}  {fmt_reg('RSI', regs.rsi)}  {fmt_reg('RDI', regs.rdi)}"
            )
            print(
                f"  {fmt_reg('RBP', regs.rbp)}  {fmt_reg('RSP', regs.rsp)}  {fmt_reg('R8', regs.r8)}"
            )
            print(
                f"  {fmt_reg('R9', regs.r9)}  {fmt_reg('R10', regs.r10)}  {fmt_reg('R11', regs.r11)}"
            )
            print(
                f"  {fmt_reg('R12', regs.r12)}  {fmt_reg('R13', regs.r13)}  {fmt_reg('R14', regs.r14)}"
            )
            print(
                f"  {fmt_reg('R15', regs.r15)}  {fmt_reg('RIP', regs.rip)}  {COLOR_BOLD}{COLOR_CYAN}EFL{COLOR_RESET}: 0x{regs.eflags:08x} [{self.format_eflags(regs.eflags)}]"
            )
        elif self.arch == "aarch64":
            for i in range(0, 30, 3):
                print(
                    f"  {fmt_reg(f'X{i}', regs.regs[i])}  {fmt_reg(f'X{i+1}', regs.regs[i+1])}  {fmt_reg(f'X{i+2}', regs.regs[i+2])}"
                )
            print(
                f"  {fmt_reg('X30', regs.regs[30])}  {fmt_reg('SP', regs.sp)}  {fmt_reg('PC', regs.pc)}"
            )
            print(
                f"  {COLOR_BOLD}{COLOR_CYAN}PSTATE{COLOR_RESET}: 0x{regs.pstate:016x}"
            )
        elif self.arch == "arm":
            names = [
                "R0",
                "R1",
                "R2",
                "R3",
                "R4",
                "R5",
                "R6",
                "R7",
                "R8",
                "R9",
                "R10",
                "FP",
                "IP",
                "SP",
                "LR",
                "PC",
            ]
            for i in range(0, 15, 4):
                print(
                    "  "
                    + "  ".join(
                        fmt_reg(names[i + j], regs.uregs[i + j], is_32=True)
                        for j in range(4)
                        if i + j < 16
                    )
                )
            print(
                f"  {fmt_reg('CPSR', regs.uregs[16], is_32=True)}  {fmt_reg('ORIG', regs.uregs[17], is_32=True)}"
            )
        elif self.arch in ("riscv64", "riscv32"):
            is_32 = self.arch == "riscv32"
            rv_regs = [
                ("PC", regs.pc),
                ("RA", regs.ra),
                ("SP", regs.sp),
                ("GP", regs.gp),
                ("TP", regs.tp),
                ("T0", regs.t0),
                ("T1", regs.t1),
                ("T2", regs.t2),
                ("S0", regs.s0),
                ("S1", regs.s1),
                ("A0", regs.a0),
                ("A1", regs.a1),
                ("A2", regs.a2),
                ("A3", regs.a3),
                ("A4", regs.a4),
                ("A5", regs.a5),
                ("A6", regs.a6),
                ("A7", regs.a7),
                ("S2", regs.s2),
                ("S3", regs.s3),
                ("S4", regs.s4),
                ("S5", regs.s5),
                ("S6", regs.s6),
                ("S7", regs.s7),
                ("S8", regs.s8),
                ("S9", regs.s9),
                ("S10", regs.s10),
                ("S11", regs.s11),
                ("T3", regs.t3),
                ("T4", regs.t4),
                ("T5", regs.t5),
                ("T6", regs.t6),
            ]
            for i in range(0, len(rv_regs), 4):
                print(
                    "  "
                    + "  ".join(
                        fmt_reg(
                            rv_regs[i + j][0], rv_regs[i + j][1], is_32=is_32
                        )
                        for j in range(4)
                        if i + j < len(rv_regs)
                    )
                )

    def disassemble_around(self, addr, count=6):
        """Disassemble instruction window around target address using objdump."""
        start = max(0, addr - 16)
        length = 48
        data = self.read_mem(start, length)
        if not data:
            print(
                f"  {COLOR_DIM}[Cannot read memory at 0x{addr:x} for disassembly]{COLOR_RESET}"
            )
            return

        mach = ARCH_OBJDUMP_MACH.get(self.arch, "i386:x86-64")

        try:
            with tempfile.NamedTemporaryFile() as tf:
                tf.write(data)
                tf.flush()
                res = subprocess.run(
                    [
                        "objdump",
                        "-D",
                        "-b",
                        "binary",
                        "-m",
                        mach,
                        f"--adjust-vma={start}",
                        tf.name,
                    ],
                    capture_output=True,
                    text=True,
                )
                if res.returncode == 0:
                    lines = res.stdout.splitlines()
                    found_section = False
                    for line in lines:
                        if "<.data>:" in line:
                            found_section = True
                            continue
                        if not found_section or not line.strip():
                            continue
                        m = re.match(r"^\s*([0-9a-f]+):\s+(.+)$", line)
                        if m:
                            line_addr = int(m.group(1), 16)
                            content = m.group(2)
                            if line_addr == addr:
                                print(
                                    f"  {COLOR_BOLD}{COLOR_RED}=>{COLOR_RESET} {COLOR_BOLD}0x{line_addr:012x}:{COLOR_RESET}  {COLOR_GREEN}{content}{COLOR_RESET}"
                                )
                            elif abs(line_addr - addr) <= 32:
                                print(
                                    f"     {COLOR_DIM}0x{line_addr:012x}:{COLOR_RESET}  {content}"
                                )
                    return
        except Exception:
            pass

        # Fallback raw hex display
        print(f"  Raw bytes at 0x{addr:x}: {data[:16].hex(' ')}")

    def dump_stack(self, sp, words=8):
        """Dump words on stack and annotate code pointers."""
        is_32 = self.arch in ("arm", "riscv32")
        word_size = 4 if is_32 else 8
        fmt = "<I" if is_32 else "<Q"
        print(
            f"{COLOR_BOLD}Stack Dump (SP: 0x{sp:012x}):{COLOR_RESET}"
        )
        for i in range(words):
            cur = sp + i * word_size
            data = self.read_mem(cur, word_size)
            if not data or len(data) < word_size:
                break
            val = struct.unpack(fmt, data)[0]
            loc = self.locate_addr(val)
            annot = ""
            for m in self.maps:
                if m.contains(val) and m.is_executable():
                    annot = f" {COLOR_YELLOW}<- [code pointer / return addr?]{COLOR_RESET}"
                    break
            print(
                f"  [SP+0x{i*word_size:02x}] 0x{cur:012x}: 0x{val:016x}  ({loc}){annot}"
            )

    def print_crash_dump(self, signo, siginfo):
        self.read_maps()
        regs = self.get_regs()
        sig_name = (
            signal.Signals(signo).name
            if signo in signal.Signals._value2member_map_
            else f"Signal {signo}"
        )
        code_str = ""
        if siginfo and signo in SIG_CODES:
            code_str = SIG_CODES[signo].get(
                siginfo.si_code, f"code={siginfo.si_code}"
            )

        fault_addr = siginfo.si_addr if siginfo else None
        pc = self.get_pc(regs)
        sp = self.get_sp(regs)

        print("\n" + "=" * 78)
        print(
            f"{COLOR_BOLD}{COLOR_RED}*** ZeroCC pytrace: CRASH / EXCEPTION DETECTED ({self.arch}) ***{COLOR_RESET}"
        )
        print("=" * 78)
        print(
            f"{COLOR_BOLD}Process:{COLOR_RESET} PID {self.pid} [{os.path.basename(self.cmd[0])}] (Arch: {self.arch})"
        )
        print(
            f"{COLOR_BOLD}Signal: {COLOR_RESET}{COLOR_RED}{sig_name} ({signo}){COLOR_RESET} - {code_str}"
        )
        if fault_addr is not None:
            loc = self.locate_addr(fault_addr)
            print(
                f"{COLOR_BOLD}Fault Address:{COLOR_RESET} {COLOR_RED}0x{fault_addr:012x}{COLOR_RESET} -> {loc}"
            )
        if regs:
            rip_loc = self.locate_addr(pc)
            print(
                f"{COLOR_BOLD}Faulting Instruction Pointer:{COLOR_RESET} 0x{pc:012x} -> {rip_loc}"
            )
            print("-" * 78)
            print(f"{COLOR_BOLD}CPU Registers:{COLOR_RESET}")
            self.print_regs(regs, fault_addr)
            print("-" * 78)
            print(f"{COLOR_BOLD}Disassembly Window ({self.arch}):{COLOR_RESET}")
            self.disassemble_around(pc)
            print("-" * 78)
            self.dump_stack(sp, words=8)
        print("=" * 78 + "\n")

    def interactive_prompt(self, signo=None):
        self.read_maps()
        regs = self.get_regs()
        pc = self.get_pc(regs)
        sp = self.get_sp(regs)
        prompt_sig = f" [stopped by {signo}]" if signo else ""
        print(
            f"{COLOR_CYAN}ZeroCC pytrace interactive mode ({self.arch}){prompt_sig}. Type 'h' for help.{COLOR_RESET}"
        )
        if regs:
            print(f"PC: 0x{pc:012x} ({self.locate_addr(pc)})")

        while True:
            try:
                line = input(
                    f"{COLOR_BOLD}(pytrace){COLOR_RESET} "
                ).strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

            if not line:
                continue

            parts = line.split()
            cmd = parts[0].lower()

            if cmd in ("h", "help", "?"):
                print("Commands:")
                print(
                    "  c, cont, continue       - Resume execution until signal or exit"
                )
                print("  s, si, step, stepi [N]  - Single-step N instructions")
                print("  r, regs                 - Print CPU register state")
                print(
                    "  dis, disasm [addr]      - Disassemble around address (default PC)"
                )
                print(
                    "  st, stack [words]       - Dump words from current stack pointer"
                )
                print(
                    "  m, mem <addr> [bytes]   - Hex dump memory at specified address"
                )
                print(
                    "  b, break <addr>         - Set software breakpoint (hex address)"
                )
                print("  d, del <addr>           - Delete breakpoint")
                print("  bl, breakpoints         - List active breakpoints")
                print("  maps                    - Show memory mappings")
                print("  q, quit, exit           - Terminate child process and exit")

            elif cmd in ("c", "cont", "continue"):
                return "continue"

            elif cmd in ("s", "si", "step", "stepi"):
                steps = int(parts[1]) if len(parts) > 1 else 1
                for _ in range(steps):
                    libc.ptrace(PTRACE_SINGLESTEP, self.pid, None, None)
                    _, status = os.waitpid(self.pid, 0)
                    if os.WIFEXITED(status):
                        print(
                            f"Child process exited with code {os.WEXITSTATUS(status)}"
                        )
                        return "exited"
                    if os.WIFSIGNALED(status):
                        print(
                            f"Child terminated by signal {os.WTERMSIG(status)}"
                        )
                        return "exited"
                    self.insn_count += 1
                regs = self.get_regs()
                if regs:
                    cur_pc = self.get_pc(regs)
                    print(
                        f"Step -> PC: 0x{cur_pc:012x} ({self.locate_addr(cur_pc)})"
                    )
                    self.disassemble_around(cur_pc, count=2)

            elif cmd in ("r", "regs"):
                regs = self.get_regs()
                self.print_regs(regs)

            elif cmd in ("dis", "disasm"):
                regs = self.get_regs()
                addr = (
                    int(parts[1], 16)
                    if len(parts) > 1
                    else self.get_pc(regs)
                )
                self.disassemble_around(addr)

            elif cmd in ("st", "stack"):
                regs = self.get_regs()
                words = int(parts[1]) if len(parts) > 1 else 8
                if regs:
                    self.dump_stack(self.get_sp(regs), words=words)

            elif cmd in ("m", "mem"):
                if len(parts) < 2:
                    print("Usage: mem <hex_addr> [bytes]")
                    continue
                addr = int(parts[1], 16)
                length = int(parts[2]) if len(parts) > 2 else 32
                data = self.read_mem(addr, length)
                if data:
                    print(f"Memory at 0x{addr:x}: {data.hex(' ')}")
                else:
                    print(f"Cannot read memory at 0x{addr:x}")

            elif cmd in ("b", "break"):
                if len(parts) < 2:
                    print("Usage: break <hex_addr>")
                    continue
                addr = int(parts[1], 16)
                self.set_breakpoint(addr)

            elif cmd in ("d", "del"):
                if len(parts) < 2:
                    print("Usage: del <hex_addr>")
                    continue
                addr = int(parts[1], 16)
                self.remove_breakpoint(addr)

            elif cmd in ("bl", "breakpoints"):
                if not self.breakpoints:
                    print("No breakpoints set.")
                for b, orig in self.breakpoints.items():
                    print(
                        f"  0x{b:012x} (orig: {orig.hex()}) -> {self.locate_addr(b)}"
                    )

            elif cmd == "maps":
                self.read_maps()
                for m in self.maps:
                    print(f"  {m.describe()}")

            elif cmd in ("q", "quit", "exit"):
                try:
                    os.kill(self.pid, signal.SIGKILL)
                    os.waitpid(self.pid, 0)
                except Exception:
                    pass
                sys.exit(0)

        return "exit"

    def run(self):
        # Fork child tracee
        pid = os.fork()
        if pid == 0:
            # Child process: enable tracing and stop self
            res = libc.ptrace(PTRACE_TRACEME, 0, None, None)
            if res != 0:
                sys.stderr.write("pytrace: PTRACE_TRACEME failed\n")
                sys.exit(1)
            os.kill(os.getpid(), signal.SIGSTOP)
            # Exec target binary
            os.execvp(self.cmd[0], self.cmd)
            sys.exit(127)

        self.pid = pid

        # Wait for child's initial SIGSTOP before execvp
        _, status = os.waitpid(pid, 0)
        if not os.WIFSTOPPED(status):
            sys.stderr.write("pytrace: Failed to stop child on launch\n")
            return 1

        # Configure ptrace options
        options = PTRACE_O_EXITKILL
        if self.do_strace:
            options |= PTRACE_O_TRACESYSGOOD
        libc.ptrace(PTRACE_SETOPTIONS, pid, None, options)

        # Resume child to let execvp execute
        libc.ptrace(PTRACE_CONT, pid, None, None)
        _, status = os.waitpid(pid, 0)

        # Child should now be stopped at execve entry point via SIGTRAP
        if not os.WIFSTOPPED(status):
            sys.stderr.write("pytrace: Target process failed to execute\n")
            return 1

        # Populate initial memory maps
        self.read_maps()

        # Arm heap/leak tracking breakpoints if enabled
        if self.do_leak_check:
            self.resolve_symbols()
            if self.sym_malloc:
                self.set_breakpoint(self.sym_malloc)
            if self.sym_calloc:
                self.set_breakpoint(self.sym_calloc)
            if self.sym_realloc:
                self.set_breakpoint(self.sym_realloc)
            if self.sym_free:
                self.set_breakpoint(self.sym_free)

        # Set initial breakpoints from CLI
        if self.args.initial_break:
            for brk in self.args.initial_break:
                addr = int(brk, 16)
                self.set_breakpoint(addr)

        # If interactive requested on launch, enter prompt
        if self.is_interactive:
            action = self.interactive_prompt()
            if action in ("exit", "exited"):
                return 0

        # Main supervision loop
        bp_step = 1 if self.arch == "x86_64" else 0

        while True:
            # Resume child
            if self.do_strace:
                libc.ptrace(PTRACE_SYSCALL, pid, None, None)
            else:
                libc.ptrace(PTRACE_CONT, pid, None, None)

            # Wait for event
            try:
                _, status = os.waitpid(pid, 0)
            except KeyboardInterrupt:
                print(f"\n{COLOR_YELLOW}Interrupted by user.{COLOR_RESET}")
                if self.is_interactive:
                    self.interactive_prompt()
                    continue
                else:
                    try:
                        os.kill(pid, signal.SIGKILL)
                        os.waitpid(pid, 0)
                    except Exception:
                        pass
                    return 130

            # Check if child terminated normally
            if os.WIFEXITED(status):
                exitcode = os.WEXITSTATUS(status)
                if self.do_leak_check:
                    self.print_leak_summary()
                if self.args.verbose or self.args.count_insns:
                    print(
                        f"{COLOR_DIM}[pytrace: child exited normally with code {exitcode}]{COLOR_RESET}"
                    )
                if self.args.count_insns:
                    print(f"Total instructions executed: {self.insn_count}")
                return exitcode

            # Check if child was terminated by unhandled signal
            if os.WIFSIGNALED(status):
                term_sig = os.WTERMSIG(status)
                if self.do_leak_check:
                    self.print_leak_summary()
                print(
                    f"{COLOR_RED}[pytrace: child terminated by signal {term_sig} ({signal.Signals(term_sig).name})]{COLOR_RESET}"
                )
                return 128 + term_sig

            if os.WIFSTOPPED(status):
                stopsig = os.WSTOPSIG(status)

                # Syscall event under PTRACE_O_TRACESYSGOOD (stopsig == SIGTRAP | 0x80 == 133)
                if self.do_strace and stopsig == (signal.SIGTRAP | 0x80):
                    regs = self.get_regs()
                    if regs and self.arch == "x86_64":
                        if not self.in_syscall:
                            # Syscall entry
                            self.in_syscall = True
                            self.last_syscall_num = regs.orig_rax
                            sc_name = SYSCALL_NAMES_X86_64.get(
                                regs.orig_rax, f"sys_{regs.orig_rax}"
                            )
                            print(
                                f"{COLOR_DIM}[strace] {sc_name}(0x{regs.rdi:x}, 0x{regs.rsi:x}, 0x{regs.rdx:x}, 0x{regs.r10:x}){COLOR_RESET}",
                                end="",
                                flush=True,
                            )
                        else:
                            # Syscall exit
                            self.in_syscall = False
                            print(
                                f"{COLOR_DIM} = 0x{regs.rax:x} ({ctypes.c_long(regs.rax).value}){COLOR_RESET}"
                            )
                    continue

                # Check software breakpoint hit
                if stopsig == signal.SIGTRAP:
                    regs = self.get_regs()
                    cur_pc = self.get_pc(regs)
                    hit_addr = cur_pc - bp_step

                    # Heap & Leak tracking breakpoint dispatch
                    if self.do_leak_check and regs and self.arch == "x86_64":
                        if hit_addr in self.pending_alloc_ret:
                            meta = self.pending_alloc_ret.pop(hit_addr)
                            self.remove_breakpoint(hit_addr)
                            if bp_step > 0:
                                self.set_pc(regs, hit_addr)
                                self.set_regs(regs)
                            ret_ptr = regs.rax
                            if ret_ptr != 0:
                                self.total_alloc_count += 1
                                self.total_allocated_bytes += meta["size"]
                                self.active_allocs[ret_ptr] = {
                                    "size": meta["size"],
                                    "callsite": meta["callsite"],
                                    "id": self.total_alloc_count,
                                }
                                if self.args.verbose:
                                    print(
                                        f"{COLOR_DIM}[pytrace:heap] {meta['fn']}({meta['size']}) -> 0x{ret_ptr:x} at {self.lookup_symbol(meta['callsite'])}{COLOR_RESET}"
                                    )
                            libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                            os.waitpid(pid, 0)
                            continue

                        if self.sym_malloc and hit_addr == self.sym_malloc:
                            self.remove_breakpoint(hit_addr)
                            if bp_step > 0:
                                self.set_pc(regs, hit_addr)
                                self.set_regs(regs)
                            size = regs.rdi
                            rsp_val = libc.ptrace(
                                PTRACE_PEEKDATA, pid, ctypes.c_void_p(regs.rsp), None
                            )
                            ret_addr = rsp_val & 0xFFFFFFFFFFFFFFFF
                            self.pending_alloc_ret[ret_addr] = {
                                "fn": "malloc",
                                "size": size,
                                "callsite": ret_addr,
                            }
                            self.set_breakpoint(ret_addr)
                            libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                            os.waitpid(pid, 0)
                            self.set_breakpoint(self.sym_malloc)
                            continue

                        if self.sym_calloc and hit_addr == self.sym_calloc:
                            self.remove_breakpoint(hit_addr)
                            if bp_step > 0:
                                self.set_pc(regs, hit_addr)
                                self.set_regs(regs)
                            total_size = regs.rdi * regs.rsi
                            rsp_val = libc.ptrace(
                                PTRACE_PEEKDATA, pid, ctypes.c_void_p(regs.rsp), None
                            )
                            ret_addr = rsp_val & 0xFFFFFFFFFFFFFFFF
                            self.pending_alloc_ret[ret_addr] = {
                                "fn": "calloc",
                                "size": total_size,
                                "callsite": ret_addr,
                            }
                            self.set_breakpoint(ret_addr)
                            libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                            os.waitpid(pid, 0)
                            self.set_breakpoint(self.sym_calloc)
                            continue

                        if self.sym_free and hit_addr == self.sym_free:
                            self.remove_breakpoint(hit_addr)
                            if bp_step > 0:
                                self.set_pc(regs, hit_addr)
                                self.set_regs(regs)
                            ptr = regs.rdi
                            rsp_val = libc.ptrace(
                                PTRACE_PEEKDATA, pid, ctypes.c_void_p(regs.rsp), None
                            )
                            callsite = rsp_val & 0xFFFFFFFFFFFFFFFF
                            if ptr == 0:
                                pass
                            elif ptr in self.freed_allocs:
                                prev_info, prev_site = self.freed_allocs[ptr]
                                msg = f"DOUBLE FREE of 0x{ptr:x} at {self.lookup_symbol(callsite)} (originally allocated at {self.lookup_symbol(prev_info['callsite'])}, first freed at {self.lookup_symbol(prev_site)})"
                                self.heap_errors.append(msg)
                                print(f"{COLOR_BOLD}{COLOR_RED}[!] {msg}{COLOR_RESET}")
                            elif ptr in self.active_allocs:
                                info = self.active_allocs.pop(ptr)
                                self.freed_allocs[ptr] = (info, callsite)
                                self.total_free_count += 1
                                if self.args.verbose:
                                    print(
                                        f"{COLOR_DIM}[pytrace:heap] free(0x{ptr:x}, {info['size']}B) at {self.lookup_symbol(callsite)}{COLOR_RESET}"
                                    )
                            else:
                                msg = f"INVALID FREE of unallocated pointer 0x{ptr:x} at {self.lookup_symbol(callsite)}"
                                self.heap_errors.append(msg)
                                print(f"{COLOR_BOLD}{COLOR_RED}[!] {msg}{COLOR_RESET}")
                            libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                            os.waitpid(pid, 0)
                            self.set_breakpoint(self.sym_free)
                            continue

                    if regs and hit_addr in self.breakpoints:
                        bp_addr = hit_addr
                        # Restore original instruction and rewind PC if x86
                        orig_bytes = self.breakpoints[bp_addr]
                        self.write_mem(bp_addr, orig_bytes)
                        if bp_step > 0:
                            self.set_pc(regs, bp_addr)
                            self.set_regs(regs)
                        print(
                            f"\n{COLOR_GREEN}Hit breakpoint at 0x{bp_addr:012x}!{COLOR_RESET}"
                        )
                        action = self.interactive_prompt(signo=stopsig)
                        if action in ("exit", "exited"):
                            return 0
                        # Re-arm breakpoint by single-stepping past instruction then re-inserting
                        libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                        os.waitpid(pid, 0)
                        bp_bytes = ARCH_BREAKPOINTS.get(
                            self.arch, ARCH_BREAKPOINTS["x86_64"]
                        )
                        self.write_mem(bp_addr, bp_bytes)
                        continue

                # Crash or fault signal: SIGSEGV, SIGILL, SIGBUS, SIGFPE, SIGABRT
                if stopsig in (
                    signal.SIGSEGV,
                    signal.SIGILL,
                    signal.SIGBUS,
                    signal.SIGFPE,
                    signal.SIGABRT,
                ):
                    siginfo = self.get_siginfo()
                    self.print_crash_dump(stopsig, siginfo)
                    if self.is_interactive:
                        self.interactive_prompt(signo=stopsig)
                    # Terminate child
                    try:
                        os.kill(pid, signal.SIGKILL)
                        os.waitpid(pid, 0)
                    except Exception:
                        pass
                    return 128 + stopsig

                # Other signal (e.g. SIGSTOP, SIGWINCH)
                if self.args.verbose:
                    print(
                        f"{COLOR_DIM}[pytrace: child received signal {stopsig}]{COLOR_RESET}"
                    )
                libc.ptrace(PTRACE_CONT, pid, None, stopsig)


def main():
    parser = argparse.ArgumentParser(
        description="pytrace: Multi-ISA Python ptrace debugger & crash inspector for ZeroCC."
    )
    parser.add_argument(
        "-i",
        "--interactive",
        action="store_true",
        help="Start in interactive debugger prompt before execution",
    )
    parser.add_argument(
        "--strace", action="store_true", help="Trace Linux system calls"
    )
    parser.add_argument(
        "--leak-check",
        action="store_true",
        help="Track heap memory allocations and report memory leaks on exit (Valgrind alternative)",
    )
    parser.add_argument(
        "--heap-check",
        action="store_true",
        help="Detect double-frees, invalid frees, and heap corruptions",
    )
    parser.add_argument(
        "--count-insns",
        action="store_true",
        help="Count total instructions executed (single-step mode)",
    )
    parser.add_argument(
        "-b",
        "--break",
        dest="initial_break",
        action="append",
        metavar="HEX_ADDR",
        help="Set initial software breakpoint at hex address (can repeat)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Print verbose ptrace status messages",
    )
    parser.add_argument(
        "--no-color", action="store_true", help="Disable colored terminal output"
    )
    parser.add_argument(
        "cmd",
        nargs=argparse.REMAINDER,
        help="Target executable and arguments to run under trace",
    )

    args = parser.parse_args()

    if not args.cmd:
        parser.print_help()
        sys.exit(1)

    if args.no_color or not sys.stdout.isatty():
        disable_colors()

    tracer = PyTracer(
        args,
        args.cmd,
        is_interactive=args.interactive,
        do_strace=args.strace,
    )
    rc = tracer.run()
    sys.exit(rc)


if __name__ == "__main__":
    main()
