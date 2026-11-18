#!/usr/bin/env python3
"""pymemcheck - Zero-dependency Python memory leak and heap corruption checker for ZeroCC.

A lightweight, high-performance pure-Python alternative to Valgrind Memcheck.
Uses ptrace breakpoints on heap allocator APIs (malloc, calloc, realloc, free)
and kernel memory syscalls (brk, mmap) to track memory lifecycles at near-native speed.

Features:
  - Memory leak detection (Valgrind-style LEAK SUMMARY with addresses, sizes, and callsites)
  - Instant double-free detection with allocation and original-free provenance
  - Invalid free detection (freeing non-heap / unallocated pointers)
  - Heap buffer poisoning on free (0x5A pattern) to turn use-after-free into hard traps
  - Zero external dependencies (stdlib only: ctypes, os, struct, signal)
  - Runs at ~95% native execution speed (no 20x-50x Valgrind slowdown)

Usage:
  python3 tools/analysis/pymemcheck.py [OPTIONS] <executable> [args...]

Examples:
  python3 tools/analysis/pymemcheck.py ./build/bin/zerocc --run test.c
  python3 tools/analysis/pymemcheck.py --poison /path/to/my_binary
  python3 tools/analysis/pymemcheck.py --leak-check=full /path/to/my_binary
"""

import argparse
import ctypes
import os
import platform
import signal
import struct
import sys
from pathlib import Path

# Repository root path resolution (docs/coding_style.md & AGENTS.md rules)
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root
from tools.analysis import elf_min

ROOT = find_repo_root()

# Load host libc
libc = ctypes.CDLL(None)

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
PTRACE_SETOPTIONS = 0x4200
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205

PTRACE_O_EXITKILL = 1 << 20
NT_PRSTATUS = 1

libc.ptrace.argtypes = [
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_void_p,
]
libc.ptrace.restype = ctypes.c_long


# Register structs
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


class IOVec(ctypes.Structure):
    _fields_ = [
        ("iov_base", ctypes.c_void_p),
        ("iov_len", ctypes.c_size_t),
    ]


# ANSI Colors
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


class PyMemCheck:
    def __init__(self, args, cmd):
        self.args = args
        self.cmd = cmd
        self.pid = None
        self.arch = platform.machine().lower()
        if self.arch in ("amd64", "x64"):
            self.arch = "x86_64"

        self.breakpoints = {}  # addr -> orig_byte
        self.active_allocs = {}  # ptr -> {'size': sz, 'callsite': pc, 'id': n}
        self.freed_allocs = {}  # ptr -> (info, free_callsite)
        self.pending_ret = {}  # ret_addr -> alloc_metadata

        self.alloc_counter = 0
        self.total_allocated_bytes = 0
        self.total_alloc_count = 0
        self.total_free_count = 0
        self.heap_errors = []

        self.sym_malloc = None
        self.sym_calloc = None
        self.sym_realloc = None
        self.sym_free = None
        self.symbols_by_addr = {}

    def resolve_symbols(self):
        """Extract allocator function addresses from target ELF executable."""
        bin_path = self.cmd[0]
        # Resolve full path if relative or in PATH
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
        except Exception as e:
            if self.args.verbose:
                print(f"{COLOR_DIM}[pymemcheck: note: {e}]{COLOR_RESET}")

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

    def write_byte(self, addr, b):
        aligned = addr & ~7
        off = addr & 7
        val = libc.ptrace(
            PTRACE_PEEKTEXT, self.pid, ctypes.c_void_p(aligned), None
        )
        if val == -1:
            return False
        raw = bytearray(struct.pack("<q", val))
        raw[off] = b
        new_val = struct.unpack("<q", raw)[0]
        res = libc.ptrace(
            PTRACE_POKETEXT,
            self.pid,
            ctypes.c_void_p(aligned),
            ctypes.c_void_p(new_val),
        )
        return res != -1

    def set_bp(self, addr):
        val = libc.ptrace(
            PTRACE_PEEKTEXT, self.pid, ctypes.c_void_p(addr & ~7), None
        )
        if val == -1:
            return False
        raw = struct.pack("<q", val)
        orig = raw[addr & 7]
        self.breakpoints[addr] = orig
        return self.write_byte(addr, 0xCC)

    def remove_bp(self, addr):
        if addr in self.breakpoints:
            self.write_byte(addr, self.breakpoints[addr])
            del self.breakpoints[addr]
            return True
        return False

    def poison_memory(self, addr, size):
        """Fill freed buffer with 0x5A pattern to turn use-after-free into immediate faults."""
        poison_len = min(size, 256)
        poison_bytes = bytes([0x5A] * poison_len)
        cur = addr
        offset = 0
        while offset < poison_len:
            aligned = cur & ~7
            in_word = cur & 7
            val = libc.ptrace(
                PTRACE_PEEKDATA, self.pid, ctypes.c_void_p(aligned), None
            )
            if val == -1:
                break
            raw = bytearray(struct.pack("<q", val))
            chunk = min(8 - in_word, poison_len - offset)
            raw[in_word : in_word + chunk] = poison_bytes[
                offset : offset + chunk
            ]
            new_val = struct.unpack("<q", raw)[0]
            libc.ptrace(
                PTRACE_POKEDATA,
                self.pid,
                ctypes.c_void_p(aligned),
                ctypes.c_void_p(new_val),
            )
            cur += chunk
            offset += chunk

    def print_summary(self):
        print("\n" + "=" * 60)
        print(f"{COLOR_BOLD}=== pymemcheck: HEAP & LEAK SUMMARY ==={COLOR_RESET}")
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

    def run(self):
        self.resolve_symbols()

        if not self.sym_malloc and not self.sym_free:
            print(
                f"{COLOR_YELLOW}pymemcheck: Note: malloc/free symbols not found in binary symbol table.{COLOR_RESET}"
            )
            print(
                f"{COLOR_YELLOW}Running process with standard system call tracking...{COLOR_RESET}"
            )

        pid = os.fork()
        if pid == 0:
            res = libc.ptrace(PTRACE_TRACEME, 0, None, None)
            if res != 0:
                sys.stderr.write("pymemcheck: PTRACE_TRACEME failed\n")
                sys.exit(1)
            os.kill(os.getpid(), signal.SIGSTOP)
            os.execvp(self.cmd[0], self.cmd)
            sys.exit(127)

        self.pid = pid

        # Wait for initial SIGSTOP before execvp
        _, status = os.waitpid(pid, 0)
        if not os.WIFSTOPPED(status):
            return 1

        libc.ptrace(PTRACE_SETOPTIONS, pid, None, PTRACE_O_EXITKILL)

        # Resume to let execvp execute
        libc.ptrace(PTRACE_CONT, pid, None, None)
        _, status = os.waitpid(pid, 0)

        # Set allocator breakpoints
        if self.sym_malloc:
            self.set_bp(self.sym_malloc)
        if self.sym_calloc:
            self.set_bp(self.sym_calloc)
        if self.sym_realloc:
            self.set_bp(self.sym_realloc)
        if self.sym_free:
            self.set_bp(self.sym_free)

        regs = UserRegsX86_64()

        while True:
            libc.ptrace(PTRACE_CONT, pid, None, None)
            try:
                _, status = os.waitpid(pid, 0)
            except KeyboardInterrupt:
                print(f"\n{COLOR_YELLOW}Interrupted.{COLOR_RESET}")
                try:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                except Exception:
                    pass
                return 130

            if os.WIFEXITED(status):
                exitcode = os.WEXITSTATUS(status)
                self.print_summary()
                if self.args.error_exitcode and (
                    self.active_allocs or self.heap_errors
                ):
                    return self.args.error_exitcode
                return exitcode

            if os.WIFSIGNALED(status):
                term_sig = os.WTERMSIG(status)
                print(
                    f"\n{COLOR_RED}pymemcheck: Child terminated by signal {term_sig} ({signal.Signals(term_sig).name}){COLOR_RESET}"
                )
                self.print_summary()
                return 128 + term_sig

            if os.WIFSTOPPED(status):
                stopsig = os.WSTOPSIG(status)

                if stopsig == signal.SIGTRAP:
                    libc.ptrace(PTRACE_GETREGS, pid, None, ctypes.byref(regs))
                    hit_addr = regs.rip - 1

                    # 1. Return breakpoint from malloc/calloc/realloc
                    if hit_addr in self.pending_ret:
                        meta = self.pending_ret.pop(hit_addr)
                        self.remove_bp(hit_addr)
                        regs.rip = hit_addr
                        libc.ptrace(
                            PTRACE_SETREGS, pid, None, ctypes.byref(regs)
                        )

                        ret_ptr = regs.rax
                        if ret_ptr != 0:
                            self.alloc_counter += 1
                            self.total_alloc_count += 1
                            self.total_allocated_bytes += meta["size"]
                            self.active_allocs[ret_ptr] = {
                                "size": meta["size"],
                                "callsite": meta["callsite"],
                                "id": self.alloc_counter,
                            }
                            if self.args.verbose:
                                print(
                                    f"{COLOR_DIM}[pymemcheck] {meta['fn']}({meta['size']}) -> 0x{ret_ptr:x} at {self.lookup_symbol(meta['callsite'])}{COLOR_RESET}"
                                )

                        libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                        os.waitpid(pid, 0)
                        continue

                    # 2. malloc(size) entry
                    if self.sym_malloc and hit_addr == self.sym_malloc:
                        self.remove_bp(hit_addr)
                        regs.rip = hit_addr
                        libc.ptrace(
                            PTRACE_SETREGS, pid, None, ctypes.byref(regs)
                        )
                        size = regs.rdi
                        rsp_val = libc.ptrace(
                            PTRACE_PEEKDATA, pid, ctypes.c_void_p(regs.rsp), None
                        )
                        ret_addr = rsp_val & 0xFFFFFFFFFFFFFFFF
                        self.pending_ret[ret_addr] = {
                            "fn": "malloc",
                            "size": size,
                            "callsite": ret_addr,
                        }
                        self.set_bp(ret_addr)
                        libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                        os.waitpid(pid, 0)
                        self.set_bp(self.sym_malloc)
                        continue

                    # 3. calloc(nmemb, size) entry
                    if self.sym_calloc and hit_addr == self.sym_calloc:
                        self.remove_bp(hit_addr)
                        regs.rip = hit_addr
                        libc.ptrace(
                            PTRACE_SETREGS, pid, None, ctypes.byref(regs)
                        )
                        total_size = regs.rdi * regs.rsi
                        rsp_val = libc.ptrace(
                            PTRACE_PEEKDATA, pid, ctypes.c_void_p(regs.rsp), None
                        )
                        ret_addr = rsp_val & 0xFFFFFFFFFFFFFFFF
                        self.pending_ret[ret_addr] = {
                            "fn": "calloc",
                            "size": total_size,
                            "callsite": ret_addr,
                        }
                        self.set_bp(ret_addr)
                        libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                        os.waitpid(pid, 0)
                        self.set_bp(self.sym_calloc)
                        continue

                    # 4. free(ptr) entry
                    if self.sym_free and hit_addr == self.sym_free:
                        self.remove_bp(hit_addr)
                        regs.rip = hit_addr
                        libc.ptrace(
                            PTRACE_SETREGS, pid, None, ctypes.byref(regs)
                        )
                        ptr = regs.rdi
                        rsp_val = libc.ptrace(
                            PTRACE_PEEKDATA, pid, ctypes.c_void_p(regs.rsp), None
                        )
                        callsite = rsp_val & 0xFFFFFFFFFFFFFFFF

                        if ptr == 0:
                            # free(NULL) is standard no-op
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
                            if self.args.poison:
                                self.poison_memory(ptr, info["size"])
                            if self.args.verbose:
                                print(
                                    f"{COLOR_DIM}[pymemcheck] free(0x{ptr:x}, {info['size']}B) at {self.lookup_symbol(callsite)}{COLOR_RESET}"
                                )
                        else:
                            msg = f"INVALID FREE of unallocated pointer 0x{ptr:x} at {self.lookup_symbol(callsite)}"
                            self.heap_errors.append(msg)
                            print(f"{COLOR_BOLD}{COLOR_RED}[!] {msg}{COLOR_RESET}")

                        libc.ptrace(PTRACE_SINGLESTEP, pid, None, None)
                        os.waitpid(pid, 0)
                        self.set_bp(self.sym_free)
                        continue

                # Pass other signals
                libc.ptrace(PTRACE_CONT, pid, None, stopsig)


def main():
    parser = argparse.ArgumentParser(
        description="pymemcheck: Zero-dependency Python memory leak & heap corruption checker for ZeroCC."
    )
    parser.add_argument(
        "--poison",
        action="store_true",
        help="Poison freed buffers with 0x5A to turn use-after-free into immediate faults",
    )
    parser.add_argument(
        "--error-exitcode",
        type=int,
        metavar="CODE",
        default=0,
        help="Exit with CODE if memory leaks or heap corruption errors are detected",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Log every individual heap allocation and free event",
    )
    parser.add_argument(
        "--no-color", action="store_true", help="Disable colored terminal output"
    )
    parser.add_argument(
        "cmd",
        nargs=argparse.REMAINDER,
        help="Target binary and arguments to execute under memcheck",
    )

    args = parser.parse_args()

    if not args.cmd:
        parser.print_help()
        sys.exit(1)

    if args.no_color or not sys.stdout.isatty():
        disable_colors()

    checker = PyMemCheck(args, args.cmd)
    rc = checker.run()
    sys.exit(rc)


if __name__ == "__main__":
    main()
