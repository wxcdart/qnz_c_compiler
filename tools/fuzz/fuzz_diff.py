#!/usr/bin/env python3
"""
Differential Fuzzing Gate (Sprint 177, dev-plan Verification & Scale item 1)

Randomly generates self-checking C programs from a seeded AST (scoped to
the feature set ZeroCC supports), differential-compiles and
differential-executes them against a reference gcc, and ratchets failing
inputs into the seed corpus under test/fuzz/.

Architecture (why the expected values are exact):
  the generator builds a small AST per program. C text and the Python
  model are TWO PROJECTIONS OF THE SAME AST: `emit_*` renders the C,
  `eval_*`/`model_*` execute the same tree in Python (arbitrary-precision
  ints + explicit width masking per the C usual-arithmetic-conversion
  rules, LP64). Every random decision is recorded in the AST, so the
  model is a pure function of the AST (no RNG at emit/model time). The
  model's final values are embedded in the generated main() as
  compile-time self-checks (`if (v != EXPT) { printf(...); return 1; }`),
  so a pass = the program's observable behavior matches the model, and
  the gcc/zerocc comparison is then a pure (rc, stdout) equality.

  The model run doubles as a defined-behavior validator: division by
  zero, out-of-range shifts, signed overflow, and out-of-bounds index
  access all raise (the generator picked a program with UB in it) and
  the case is DISCARDED (deterministic per seed; the discard count is
  reported - a high rate means a generator bug, not a compiler bug).

Scope (honest by construction - the generator only emits defined
behavior, so gcc -O2 and zerocc -O0/-O2 must agree):
  - integers: char/short/int/long/long long (+unsigned), __int128,
    UAC per LP64 (equal width -> unsigned wins; else wider wins),
    integer promotions for <32-bit operands
  - structs (scalar/nested members, by-value <=16B, by-pointer,
    designated init, compound literals), enums skipped for v1,
    typedefs skipped for v1
  - arrays (const dim + VLA), pointers (array decay, &arr, *p),
    function pointers, pointer params
  - control flow: if/else, for (model-bounded), switch/break, ternary,
    ++/--, all binary integer ops, logical short-circuit, shifts
    (0<=s<width, signed left-shift representability-checked), div/mod
    (nonzero divisor, INT_MIN/-1 avoided by the validator)
  - recursion (fixed small patterns, caller passes a small constant),
    K&R definitions (low frequency), sizeof, bitfields skipped for v1
  - OUT of scope (extensions for later): FP/complex/nested functions/
    gotos/variadics/restrict/volatile/union/enum/typedef
  - char signedness: only explicit signed char / unsigned char
  - output: decimal integers only (no pointer values - ASLR would be
    nondeterministic across the two binaries)

Reference: gcc-15 preferred (FUZZ_GCC override, else gcc-15/14/gcc),
pinned -std=gnu11 (matches the -E gate; __int128 is a GNU extension).
zerocc is run at -O0 AOT + -O0 JIT by default; `--levels` adds AOT
levels.

Gate semantics (baseline ratchet, test/fuzz/baseline.json):
  * a case = one seed (seed = base_seed + index); the generated program
    is deterministic per seed, so the case set is stable across runs
  * GREEN: gcc and all zerocc configs compile and produce identical
    (rc, stdout)
  * a NEW failure (gcc-reject / zerocc-reject / run-mismatch / timeout)
    FAILS the gate unless ratcheted with --update-baseline; the source
    of a ratcheted failure is persisted as test/fuzz/seed_<seed>.c
  * a ratcheted failure that now passes is an improvement (non-failing;
    re-record to ratchet it out)
  * a baseline-PASS case that now FAILS is a hard regression

Usage:
  python3 tools/fuzz/fuzz_diff.py                 # 120 seeds, CI sweep
  python3 tools/fuzz/fuzz_diff.py --seeds 500 --verbose
  python3 tools/fuzz/fuzz_diff.py --levels 0 1 2 3
  python3 tools/fuzz/fuzz_diff.py --update-baseline
  python3 tools/fuzz/fuzz_diff.py --base-seed 777 --seeds 10   # probe
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = find_repo_root()
ZC = os.path.join(str(ROOT), "build", "bin", "zerocc")
FUZZ_DIR = os.path.join(str(ROOT), "test", "fuzz")
BASELINE = os.path.join(FUZZ_DIR, "baseline.json")
DEFAULT_SEEDS = 120
DEFAULT_BASE_SEED = 20261114
RUN_TIMEOUT = 15
MAX_DISCARD_RATE = 0.30

# model safety caps: exceeding them means the generator lost its bounds
# (a generator bug) - the case is discarded and the rate is reported.
MODEL_MAX_OPS = 20000
MODEL_MAX_RECURSION = 400


class GenUB(RuntimeError):
    """The generated program would exhibit undefined behavior."""


class GenFail(RuntimeError):
    """Case generation failed (discard)."""


class _Ret(Exception):
    """Model control flow: a return statement terminates the body."""

    def __init__(self, v):
        self.v = v


# ---------------------------------------------------------------------------
# C integer types (the generator's model)
# ---------------------------------------------------------------------------

class Ty:
    def __init__(self, name, width, signed, kind="int", elem=None,
                 n=None, pointee=None, struct=None):
        self.name = name        # C spelling
        self.width = width      # bits (0 for ptr/struct)
        self.signed = signed
        self.kind = kind        # int | ptr | array | struct
        self.elem = elem
        self.n = n              # int for const arrays
        self.pointee = pointee
        self.struct = struct    # struct tag name

    def is_int(self):
        return self.kind == "int"

    def is_ptr(self):
        return self.kind == "ptr"

    def is_array(self):
        return self.kind == "array"

    def is_struct(self):
        return self.kind == "struct"

    @property
    def c_name(self):
        """C spelling (structs need the `struct` keyword - no typedefs)."""
        return "struct %s" % self.name if self.kind == "struct" else self.name


TY_SCHAR = Ty("signed char", 8, True)
TY_UCHAR = Ty("unsigned char", 8, False)
TY_SHORT = Ty("short", 16, True)
TY_USHORT = Ty("unsigned short", 16, False)
TY_INT = Ty("int", 32, True)
TY_UINT = Ty("unsigned int", 32, False)
TY_LONG = Ty("long", 64, True)
TY_ULONG = Ty("unsigned long", 64, False)
TY_LLONG = Ty("long long", 64, True)
TY_ULLONG = Ty("unsigned long long", 64, False)
TY_I128 = Ty("__int128", 128, True)
TY_UI128 = Ty("unsigned __int128", 128, False)

INT_TYPES = [TY_INT, TY_UINT, TY_LONG, TY_ULONG, TY_LLONG, TY_ULLONG,
             TY_SHORT, TY_USHORT, TY_SCHAR, TY_UCHAR]
INT_TYPES_128 = INT_TYPES + [TY_I128]

STRUCT_SIZE = {}   # struct tag -> size (model)


def value_range(ty):
    w = ty.width
    if w <= 8:
        return 30
    if w <= 16:
        return 600
    if w <= 32:
        return 40000
    return 10 ** 9


def mask(v, ty):
    """Canonical two's-complement value for ty's width."""
    w = ty.width
    m = (1 << w) - 1
    v &= m
    if ty.signed and v >= (1 << (w - 1)):
        return v - (1 << w)
    return v


def fits(v, ty):
    if not ty.signed:
        return 0 <= v < (1 << ty.width)
    return -(1 << (ty.width - 1)) <= v < (1 << (ty.width - 1))


def c_trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def c_mod(a, b):
    return a - c_trunc_div(a, b) * b


def uac(t1, t2):
    """Usual arithmetic conversions (LP64): equal width -> unsigned wins,
    else the wider type wins."""
    if t1.width == t2.width:
        return t1 if t1.signed else t2
    return t1 if t1.width > t2.width else t2


def ptr_ty(pointee):
    return Ty(pointee.name + " *", 0, False, "ptr", pointee=pointee)


def arr_ty(elem, n):
    return Ty(elem.name + ("[%d]" % n if n else "[]"), 0, False,
              "array", elem=elem, n=n)


def ty_align(ty):
    if ty.kind == "int":
        return {8: 1, 16: 2, 32: 4, 64: 8, 128: 16}[ty.width]
    return 8


def ty_size(ty):
    if ty.kind == "int":
        return ty.width // 8
    if ty.kind == "ptr":
        return 8
    if ty.kind == "array":
        return ty_size(ty.elem) * (ty.n or 0)
    if ty.kind == "struct":
        return STRUCT_SIZE.get(ty.struct, 8)
    return 8


def struct_layout(members):
    """SysV-ish aligned placement for our scalar members."""
    off = 0
    maxalign = 1
    for _, mty in members:
        al = ty_align(mty)
        maxalign = max(maxalign, al)
        off = (off + al - 1) // al * al + ty_size(mty)
    return (off + maxalign - 1) // maxalign * maxalign


def conv_for(t):
    """printf conversion for an integer type (shorts/chars are cast at
    the emit site)."""
    if t.width == 64:
        if t.name in ("long", "unsigned long"):
            return "%ld" if t.signed else "%lu"
        return "%lld" if t.signed else "%llu"
    if t.width == 32:
        return "%d" if t.signed else "%u"
    return "%d"  # 8/16-bit go through an (int)/(unsigned) cast


# ---------------------------------------------------------------------------
# AST node shapes
#   expr: ('lit', ty, v) | ('id', name, ty) | ('un', op, e) |
#         ('bin', op, l, r) | ('tern', c, a, b) | ('cast', ty, e) |
#         ('sub', arr, idx) | ('deref', p) | ('addr', e) |
#         ('mem', e, field, mty) | ('call', fname, [args]) |
#         ('sz', 'type', ty) | ('sz', 'expr', e) |
#         ('clit', ty, {member: v})
#   stmt: ('decl', name, ty, init|None) | ('assign', target, e) |
#         ('incdec', pre, dir, target) | ('if', c, then, els) |
#         ('for', init, cond, step, body) |
#         ('switch', e, [(v, body)], dflt) | ('ret', e)
#   target: ('id', name, ty) | ('sub', arr, idx)
# ---------------------------------------------------------------------------


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.seed = seed
        self.file_lines = []
        self.structs = {}
        self.fns = {}
        self.n128 = False
        self.ops = 0
        self.recur = 0
        self.return_value = None
        self.main_text = None
        self.expected_stdout = None
        self._used = set()

    # -- rng helpers (generation-time only) ------------------------------
    def ri(self, lo, hi):
        return self.rng.randint(lo, hi)

    def pick(self, seq):
        return self.rng.choice(seq)

    def fresh(self, prefix, env):
        for i in range(1, 500):
            n = "%s%d" % (prefix, i)
            if n not in env and n not in self._used:
                self._used.add(n)
                return n
        n = prefix + str(self.rng.randint(10000, 99999))
        self._used.add(n)
        return n

    # -- types / values ----------------------------------------------------
    def int_type(self, allow128=False):
        pool = INT_TYPES_128 if (allow128 and self.n128) else INT_TYPES
        return self.pick(pool)

    def int_value(self, ty, small=False):
        big = min(value_range(ty), 12) if small else value_range(ty)
        if ty.signed:
            return self.rng.randint(-big, big)
        return self.rng.randint(0, big)

    # -- expression generation: returns (node, ty) --------------------------
    def gen_expr(self, env, depth=0, allow_call=True):
        if depth >= 3:
            return self.gen_atom(env)
        r = self.rng.random()
        if r < 0.50:
            return self.gen_bin(env, depth)
        if r < 0.66:
            return self.gen_atom(env)
        if r < 0.78:
            n1, t1 = self.gen_atom(env)
            n2, t2 = self.gen_atom(env)
            if t1.is_int() and t2.is_int():
                ct = uac(t1, t2)
                op = self.pick(["==", "!=", "<", ">", "<=", ">="])
                return ("tern", ("bin", op, n1, n2), n1, n2, ct), ct
            return self.gen_atom(env)
        if r < 0.90 and allow_call and self.fns:
            return self.gen_call(env)
        return self.gen_atom(env)

    def gen_atom(self, env):
        if env:
            r = self.rng.random()
            if r < 0.60:
                name, ty = self.pick(list(env.items()))
                if ty.is_int():
                    return ("id", name, ty), ty
                if ty.is_array():
                    n = ty.n if ty.n else 4
                    i = self.ri(0, max(0, n - 1))
                    return ("sub", ("id", name, ty), ("lit", TY_INT, i)), ty.elem
                if ty.is_ptr():
                    return ("deref", ("id", name, ty)), TY_INT
                if ty.is_struct():
                    m, mty = self.pick([(m, t) for m, t in self.structs[ty.struct]
                                        if t.is_int()])
                    return ("mem", ("id", name, ty), m, mty), mty
        ty = self.int_type(allow128=True)
        v = self.int_value(ty)
        return ("lit", ty, v), ty

    def gen_bin(self, env, depth):
        r = self.rng.random()
        if r < 0.60:
            op = self.pick(["+", "-", "*"])
            if op == "*":
                # keep products in range: use small constants on both sides
                n1, t1 = self.gen_const(self.int_type(allow128=False), small=True)
                n2, t2 = self.gen_const(self.int_type(allow128=False), small=True)
            else:
                n1, t1 = self.gen_atom(env)
                n2, t2 = self.gen_atom(env)
                if not (t1.is_int() and t2.is_int()):
                    return self.gen_atom(env)
        elif r < 0.70:
            op = self.pick(["/", "%"])
            n1, t1 = self.gen_atom(env)
            n2, t2 = self.gen_atom(env)
            if not (t1.is_int() and t2.is_int()):
                return self.gen_atom(env)
            # force a nonzero divisor (a constant) - the validator still
            # double-checks at model time for variable divisors
            if t2.width <= 64:
                dv = self.int_value(t2, small=True)
                if dv == 0:
                    dv = 1
                n2 = ("lit", t2, dv)
        elif r < 0.80:
            op = self.pick(["<<", ">>"])
            n1, t1 = self.gen_atom(env)
            if not t1.is_int():
                return self.gen_atom(env)
            n2 = ("lit", TY_INT, self.ri(0, 6))
            t2 = TY_INT
        elif r < 0.90:
            op = self.pick(["&", "|", "^"])
            n1, t1 = self.gen_atom(env)
            n2, t2 = self.gen_atom(env)
            if not (t1.is_int() and t2.is_int()):
                return self.gen_atom(env)
        else:
            op = self.pick(["<", "<=", ">", ">=", "==", "!=", "&&", "||"])
            n1, t1 = self.gen_atom(env)
            n2, t2 = self.gen_atom(env)
            if not (t1.is_int() and t2.is_int()):
                return self.gen_atom(env)
        ct = uac(t1, t2)
        return ("bin", op, n1, n2), ct

    def gen_const(self, ty, small=False):
        return ("lit", ty, self.int_value(ty, small)), ty

    def gen_call(self, env):
        fname = self.pick(list(self.fns.keys()))
        args = self.gen_call_args(fname, env)
        ret = self.fns[fname][2]
        return ("call", fname, args), ret

    def gen_call_args(self, fname, env):
        params, _, _, _ = self.fns[fname]
        args = []
        arr_name = None
        for n, t in env.items():
            if t.is_array():
                arr_name = n
                break
        for _, pty in params:
            if fname == "h_rec" and pty.is_int():
                # recursion depth must stay small
                a = ("lit", pty, self.ri(3, 8))
            elif pty.is_int():
                a, _ = self.gen_const(pty)
            elif pty.is_struct():
                inits = {m: self.int_value(t, small=True)
                         for m, t in self.structs[pty.struct] if t.is_int()}
                a = ("clit", pty, inits)
            elif pty.is_ptr():
                if arr_name is None:
                    a, _ = self.gen_const(TY_INT)
                else:
                    # the array decays to int * (no & needed)
                    a = ("id", arr_name, env[arr_name])
            else:
                a, _ = self.gen_const(TY_INT)
            args.append(a)
        return args

    # -- statement generation ------------------------------------------------
    def gen_stmts(self, env, count):
        out = []
        for _ in range(count):
            s = self.gen_stmt(env)
            if s:
                out.append(s)
        return out

    def gen_stmt(self, env):
        r = self.rng.random()
        if r < 0.44:
            return self.gen_assign(env)
        if r < 0.54:
            return self.gen_for(env)
        if r < 0.64:
            return self.gen_if(env)
        if r < 0.70 and self.fns:
            return self.gen_call_stmt(env)
        if r < 0.78:
            return self.gen_incdec(env)
        if r < 0.84:
            return self.gen_switch(env)
        return self.gen_assign(env)

    def gen_assign(self, env):
        ty = self.int_type(allow128=True)
        if ty.width < 32:
            # keep narrow inits in range (avoid implementation-defined
            # conversions in the generated C)
            e, _ = self.gen_const(ty)
        else:
            e, _ = self.gen_expr(env)
        n = self.fresh("v", env)
        env[n] = ty
        return ("decl", n, ty, e)

    def gen_incdec(self, env):
        ints = [(n, t) for n, t in env.items() if t.is_int() and t.width <= 64]
        if not ints:
            return None
        name, ty = self.pick(ints)
        return ("incdec", self.rng.random() < 0.5, self.rng.random() < 0.5,
                ("id", name, ty))

    def gen_if(self, env):
        n1, t1 = self.gen_atom(env)
        n2, t2 = self.gen_atom(env)
        if not (t1.is_int() and t2.is_int()):
            return None
        op = self.pick(["==", "!=", "<", ">", "<=", ">="])
        cond = ("bin", op, n1, n2)
        # child envs: variables declared inside the branches are C-scoped
        # to the branch - they must not leak to the statements that follow
        then = self.gen_stmts(dict(env), self.ri(1, 3))
        els = self.gen_stmts(dict(env), self.ri(0, 2)) if self.rng.random() < 0.6 else []
        return ("if", cond, then, els)

    def gen_for(self, env):
        body_env = dict(env)  # the counter is C-scoped to the for
        i = self.fresh("i", body_env)
        body_env[i] = TY_INT
        a = self.ri(0, 3)
        b = self.ri(4, 9)
        s = self.ri(1, 2)
        body = self.gen_stmts(body_env, self.ri(1, 3))
        return ("for", ("decl", i, TY_INT, ("lit", TY_INT, a)),
                ("bin", "<", ("id", i, TY_INT), ("lit", TY_INT, b)),
                ("bin", "+", ("id", i, TY_INT), ("lit", TY_INT, s)), body)

    def gen_switch(self, env):
        ints = [(n, t) for n, t in env.items() if t.is_int() and t.width <= 64]
        if not ints:
            return None
        name, ty = self.pick(ints)
        base = self.ri(0, 4)
        cases = []
        for k in range(self.ri(2, 4)):
            cases.append((base + k, self.gen_stmts(dict(env), self.ri(1, 2))))
        dflt = self.gen_stmts(dict(env), self.ri(0, 2))
        return ("switch", ("id", name, ty), cases, dflt)

    def gen_call_stmt(self, env):
        e, ret = self.gen_call(env)
        n = self.fresh("f", env)
        env[n] = ret
        return ("decl", n, ret, e)

    # -- program assembly ------------------------------------------------------
    def gen_file(self):
        self.n128 = self.rng.random() < 0.5
        self.gen_struct_defs()
        self.gen_helpers()
        self.gen_main()

    def gen_struct_defs(self):
        for i in range(self.ri(1, 2)):
            nm = "S%d" % i
            members = [("m%d" % j, self.pick([TY_INT, TY_UINT, TY_LONG]))
                       for j in range(self.ri(2, 3))]
            if i > 0 and self.rng.random() < 0.5:
                members.append(("inner",
                                Ty("S0", 0, False, "struct", struct="S0")))
            self.structs[nm] = members
            STRUCT_SIZE[nm] = struct_layout(members)
            self.file_lines.append("struct %s {" % nm)
            for m, t in members:
                self.file_lines.append("    %s %s;" % (t.c_name, m))
            self.file_lines.append("};")

    def struct_ty(self, nm):
        return Ty(nm, 0, False, "struct", struct=nm)

    def gen_helpers(self):
        # 1. plain binary helper (K&R with low frequency)
        kr = self.rng.random() < 0.35
        params = [("a", TY_INT), ("b", TY_UINT)]
        body = [
            ("decl", "xr", uac(TY_INT, TY_UINT),
             ("bin", "+", ("id", "a", TY_INT), ("id", "b", TY_UINT))),
            ("ret", ("id", "xr", uac(TY_INT, TY_UINT))),
        ]
        self.register_fn("h_add", params, body, uac(TY_INT, TY_UINT), kr)

        # 2. struct helper
        nm = "S0"
        if nm in self.structs:
            members = self.structs[nm]
            sty = self.struct_ty(nm)
            e = ("bin", "+",
                 ("mem", ("id", "p", sty), members[0][0], members[0][1]),
                 ("mem", ("id", "p", sty), members[1][0], members[1][1]))
            body = [
                ("decl", "r", TY_INT, e),
                ("ret", ("id", "r", TY_INT)),
            ]
            self.register_fn("h_struct", [("p", sty)], body, TY_INT, False)

        # 3. pointer helper
        body = [
            ("decl", "r", TY_INT,
             ("bin", "+", ("deref", ("id", "q", ptr_ty(TY_INT))),
                          ("lit", TY_INT, 1))),
            ("ret", ("id", "r", TY_INT)),
        ]
        self.register_fn("h_ptr", [("q", ptr_ty(TY_INT))], body, TY_INT, False)

        # 4. recursive helper
        pat = self.pick(["sum", "fact", "trib"])
        n = ("id", "n", TY_INT)
        dec = ("bin", "-", n, ("lit", TY_INT, 1))
        if pat == "sum":
            body = [
                ("if", ("bin", "<=", n, ("lit", TY_INT, 1)),
                 [("ret", n)], []),
                ("ret", ("bin", "+", n, ("call", "h_rec", [dec]))),
            ]
        elif pat == "fact":
            body = [
                ("if", ("bin", "<=", n, ("lit", TY_INT, 1)),
                 [("ret", ("lit", TY_INT, 1))], []),
                ("ret", ("bin", "*", n, ("call", "h_rec", [dec]))),
            ]
        else:
            body = [
                ("if", ("bin", "<=", n, ("lit", TY_INT, 1)),
                 [("ret", ("lit", TY_INT, 1))], []),
                ("ret", ("bin", "+",
                         ("call", "h_rec", [dec]),
                         ("call", "h_rec",
                          [("bin", "-", dec, ("lit", TY_INT, 1))]))),
            ]
        self.register_fn("h_rec", [("n", TY_INT)], body, TY_INT, False)

    def register_fn(self, name, params, body, ret, kr):
        self.fns[name] = (params, body, ret, kr)

    def gen_main(self):
        env = {}
        stmts = []
        # scalar locals (init expr from env BEFORE the new name is added)
        for _ in range(self.ri(3, 5)):
            ty = self.int_type(allow128=True)
            if ty.width < 32:
                e, _ = self.gen_const(ty)
            else:
                e, _ = self.gen_expr(env, allow_call=False)
            n = self.fresh("g", env)
            env[n] = ty
            stmts.append(("decl", n, ty, e))
        # VLA: int nv = k; int va[nv];   (dim >= 4: gen_atom indexes < 4)
        nv = self.fresh("nv", env)
        vdim = self.ri(4, 6)
        env[nv] = TY_INT
        stmts.append(("decl", nv, TY_INT, ("lit", TY_INT, vdim)))
        vn = self.fresh("va", env)
        vaty = arr_ty(TY_INT, None)
        env[vn] = vaty
        stmts.append(("decl", vn, vaty, ("id", nv, TY_INT)))
        for i in range(vdim):
            e, _ = self.gen_expr(env, allow_call=False)
            stmts.append(("assign", ("sub", ("id", vn, vaty),
                                     ("lit", TY_INT, i)), e))
        # struct local + member read
        if self.structs and self.rng.random() < 0.7:
            nm = "S0"
            sty = self.struct_ty(nm)
            sn = self.fresh("s", env)
            inits = {m: self.int_value(t, small=True)
                     for m, t in self.structs[nm] if t.is_int()}
            env[sn] = sty
            stmts.append(("decl", sn, sty, ("clit", sty, inits)))
            members = [(m, t) for m, t in self.structs[nm] if t.is_int()]
            if len(members) >= 2:
                sm = self.fresh("sm", env)
                env[sm] = TY_INT
                stmts.append(("decl", sm, TY_INT,
                              ("bin", "+",
                               ("mem", ("id", sn, sty), members[0][0], members[0][1]),
                               ("mem", ("id", sn, sty), members[1][0], members[1][1]))))
        # sizeof
        if self.rng.random() < 0.5:
            szn = self.fresh("sz", env)
            env[szn] = TY_INT
            if self.structs:
                e = ("sz", "type", self.struct_ty("S0"))
            else:
                e = ("sz", "type", TY_INT)
            stmts.append(("decl", szn, TY_INT, e))
        # helper calls
        for fn in list(self.fns.keys()):
            if self.rng.random() < 0.55:
                e, ret = self.gen_call(env)
                n = self.fresh("f", env)
                env[n] = ret
                stmts.append(("decl", n, ret, e))
        # pointer to the VLA (the array decays), then *pv + e
        pn = self.fresh("pv", env)
        env[pn] = ptr_ty(TY_INT)
        stmts.append(("decl", pn, ptr_ty(TY_INT),
                      ("id", vn, vaty)))
        e, _ = self.gen_expr(env, allow_call=False)
        x = self.fresh("x", env)
        env[x] = TY_INT
        stmts.append(("decl", x, TY_INT,
                      ("bin", "+", ("deref", ("id", pn, ptr_ty(TY_INT))), e)))
        # more statements
        stmts.extend(self.gen_stmts(env, self.ri(4, 8)))
        self.finish_main(env, stmts)

    def finish_main(self, env, stmts):
        # run the model (validates defined behavior + computes values)
        menv = {}
        self.model_run_body(stmts, menv)
        # checkpoints: up to 3 int-typed locals. 128-bit locals get a bare
        # `!=` check only (no printf form); 8-64-bit ones are also printed.
        checks = []
        for n, (t, v) in menv.items():
            if t.is_int() and n != "nv":
                checks.append((n, t, v))
            if len(checks) >= 3:
                break
        if not checks:
            raise GenFail("no checkpoints")
        out = ["int main(void) {"]
        for s in stmts:
            out.extend(self.emit_stmt(s, 1))
        for n, t, v in checks:
            if t.width == 64 and t.signed:
                lit = "%dLL" % v
            elif t.width == 64 and not t.signed:
                lit = "%dULL" % v
            else:
                lit = str(v)
            out.append('    if (%s != %s) { printf("FAIL %s\\n"); return 1; }'
                       % (n, lit, n))
        fmts = []
        pargs = []
        pvals = []
        for n, t, v in checks:
            if t.width > 64:
                continue
            fmts.append(conv_for(t))
            if t.width <= 16:
                pargs.append("((int)%s)" % n if t.signed else "((unsigned)%s)" % n)
            else:
                pargs.append(n)
            pvals.append(str(v))
        if fmts:
            out.append('    printf("ok %s\\n", %s);'
                       % (" ".join(fmts), ", ".join(pargs)))
        else:
            out.append('    printf("ok\\n");')
        out.append("    return 0;")
        out.append("}")
        self.main_text = "\n".join(out)
        self.expected_stdout = ("ok " + " ".join(pvals) if pvals else "ok") + "\n"

    # -- C emission (pure function of the AST) ---------------------------------
    def emit(self):
        out = ["#include <stdio.h>", ""]
        out.extend(self.file_lines)
        for name, (params, body, ret, kr) in self.fns.items():
            ptext = ", ".join("%s %s" % (t.c_name, pn) for pn, t in params)
            if kr:
                out.append("%s %s(%s)" % (ret.c_name, name,
                                          ", ".join(pn for pn, _ in params)))
                for pn, t in params:
                    out.append("%s %s;" % (t.c_name, pn))
                out.append("{")
            else:
                out.append("%s %s(%s) {" % (ret.c_name, name, ptext))
            for s in body:
                out.extend(self.emit_stmt(s, 1))
            out.append("}")
        out.append(self.main_text)
        return "\n".join(out) + "\n"

    def emit_expr(self, e):
        tag = e[0]
        if tag == "lit":
            # C literals carry no type: a 64/128-bit model literal must be
            # cast, else C picks int/unsigned-int and the UAC (and shift
            # representability) diverges from the model.
            v = e[2]
            if e[1].width >= 64:
                return "((%s)(%d))" % (e[1].c_name, v)
            return str(v)
        if tag == "id":
            return e[1]
        if tag == "un":
            x = self.emit_expr(e[2])
            return {"-": "(-%s)", "!": "(!%s)", "~": "(~%s)"}[e[1]] % x
        if tag == "bin":
            return "(%s %s %s)" % (self.emit_expr(e[2]), e[1],
                                   self.emit_expr(e[3]))
        if tag == "tern":
            return "((%s) ? (%s) : (%s))" % (
                self.emit_expr(e[1]), self.emit_expr(e[2]), self.emit_expr(e[3]))
        if tag == "cast":
            return "((%s)(%s))" % (e[1].name, self.emit_expr(e[2]))
        if tag == "sub":
            return "((%s)[%s])" % (self.emit_expr(e[1]), self.emit_expr(e[2]))
        if tag == "deref":
            return "(*(%s))" % self.emit_expr(e[1])
        if tag == "addr":
            return "(&(%s))" % self.emit_expr(e[1])
        if tag == "mem":
            return "((%s).%s)" % (self.emit_expr(e[1]), e[2])
        if tag == "call":
            return "%s(%s)" % (e[1], ", ".join(self.emit_expr(a) for a in e[2]))
        if tag == "sz":
            if e[1] == "type":
                return "sizeof(%s)" % e[2].c_name
            return "sizeof((%s))" % self.emit_expr(e[2])
        if tag == "clit":
            ty, init = e[1], e[2]
            parts = [".%s = %s" % (m, v) for m, v in init.items()]
            return "((%s){%s})" % (ty.c_name, ", ".join(parts))
        raise ValueError("emit_expr %s" % tag)

    def emit_target(self, t):
        if t[0] == "id":
            return t[1]
        if t[0] == "sub":
            return "((%s)[%s])" % (self.emit_expr(t[1]), self.emit_expr(t[2]))
        raise ValueError("emit_target")

    def emit_stmt(self, s, ind):
        pad = "    " * ind
        tag = s[0]
        if tag == "decl":
            _, name, ty, init = s
            if ty.is_array():
                if ty.n:
                    return ["%s%s %s[%d];" % (pad, ty.elem.c_name, name, ty.n)]
                dim = self.emit_expr(init) if init is not None else "4"
                return ["%s%s %s[%s];" % (pad, ty.elem.c_name, name, dim)]
            if init is None:
                return ["%s%s %s;" % (pad, ty.c_name, name)]
            return ["%s%s %s = %s;" % (pad, ty.c_name, name, self.emit_expr(init))]
        if tag == "assign":
            return ["%s%s = %s;" % (pad, self.emit_target(s[1]),
                                    self.emit_expr(s[2]))]
        if tag == "incdec":
            _, pre, direction, t = s
            op = "++" if direction else "--"
            return ["%s%s%s%s;" % (pad, op if pre else "", self.emit_target(t),
                                   op if not pre else "")]
        if tag == "if":
            out = ["%sif (%s) {" % (pad, self.emit_expr(s[1]))]
            for ss in s[2]:
                out.extend(self.emit_stmt(ss, ind + 1))
            if s[3]:
                out.append(pad + "} else {")
                for ss in s[3]:
                    out.extend(self.emit_stmt(ss, ind + 1))
                out.append(pad + "}")
            else:
                out.append(pad + "}")
            return out
        if tag == "for":
            init, cond, step, body = s[1], s[2], s[3], s[4]
            itext = self.emit_stmt(init, 0)[0].strip().rstrip(";")
            out = ["%sfor (%s; %s; %s = %s) {" % (
                pad, itext, self.emit_expr(cond),
                self.emit_target(("id", init[1], init[2])), self.emit_expr(step))]
            for ss in body:
                out.extend(self.emit_stmt(ss, ind + 1))
            out.append(pad + "}")
            return out
        if tag == "switch":
            out = ["%sswitch (%s) {" % (pad, self.emit_expr(s[1]))]
            for cv, body in s[2]:
                out.append("%scase %d: {" % (pad + "    ", cv))
                for ss in body:
                    out.extend(self.emit_stmt(ss, ind + 2))
                out.append(pad + "        break;")
                out.append(pad + "    }")
            if s[3]:
                out.append(pad + "    default: {")
                for ss in s[3]:
                    out.extend(self.emit_stmt(ss, ind + 2))
                out.append(pad + "    }")
            out.append(pad + "}")
            return out
        if tag == "ret":
            return ["%sreturn %s;" % (pad, self.emit_expr(s[1]))]
        raise ValueError("emit_stmt %s" % tag)

    # -- model execution (pure function of the AST) ------------------------------
    def model_run_body(self, stmts, env):
        """_Ret propagates: only the function-body call (call_model) catches
        it, so a return inside an if/for body terminates the whole function
        (C semantics)."""
        for s in stmts:
            self.model_stmt(s, env)

    def model_stmt(self, s, env):
        self.ops += 1
        if self.ops > MODEL_MAX_OPS:
            raise GenFail("model op cap")
        tag = s[0]
        if tag == "decl":
            _, name, ty, init = s
            if ty.is_array():
                n = self.eval_expr(init, env)[0] if init is not None else ty.n
                env[name] = (ty, [0] * n)
            elif ty.is_struct():
                env[name] = (ty, self.eval_expr(init, env)[0])
            else:
                v = self.eval_expr(init, env)
                env[name] = (ty, mask(v[0], ty) if ty.is_int() else v[0])
        elif tag == "assign":
            v = self.eval_expr(s[2], env)[0]
            self.write_target(s[1], v, env)
        elif tag == "incdec":
            _, pre, direction, t = s
            cur = self.read_target(t, env)
            nv = cur + (1 if direction else -1)
            self.write_target(t, nv, env)
        elif tag == "if":
            c = self.eval_expr(s[1], env)[0]
            self.model_run_body(s[2] if c else s[3], env)
        elif tag == "for":
            init, cond, step, body = s[1], s[2], s[3], s[4]
            self.model_stmt(init, env)
            while self.eval_expr(cond, env)[0]:
                self.model_run_body(body, env)
                self.model_stmt(("assign", ("id", init[1], init[2]), step), env)
        elif tag == "switch":
            v = self.eval_expr(s[1], env)[0]
            hit = False
            for cv, body in s[2]:
                if v == cv:
                    self.model_run_body(body, env)
                    hit = True
                    break
            if not hit and s[3]:
                self.model_run_body(s[3], env)
        elif tag == "ret":
            raise _Ret(self.eval_expr(s[1], env)[0])

    def read_target(self, t, env):
        if t[0] == "id":
            return env[t[1]][1]
        if t[0] == "sub":
            a = self.eval_expr(t[1], env)[0]
            i = self.eval_expr(t[2], env)[0]
            if not (0 <= i < len(a)):
                raise GenUB("array oob %d" % i)
            return a[i]

    def write_target(self, t, v, env):
        if t[0] == "id":
            ty, _ = env[t[1]]
            env[t[1]] = (ty, mask(v, ty) if ty.is_int() else v)
        elif t[0] == "sub":
            av, at = self.eval_expr(t[1], env)
            i = self.eval_expr(t[2], env)[0]
            if not (0 <= i < len(av)):
                raise GenUB("array oob %d" % i)
            av[i] = mask(v, at.elem) if at.elem.is_int() else v

    def struct_zero(self, ty, depth=0):
        d = {}
        for m, mty in self.structs[ty.struct]:
            if mty.is_struct():
                d[m] = self.struct_zero(mty, depth + 1)
            else:
                d[m] = 0
        return d

    def eval_expr(self, e, env):
        """Returns (value, result_ty)."""
        self.ops += 1
        if self.ops > MODEL_MAX_OPS:
            raise GenFail("model op cap")
        tag = e[0]
        if tag == "lit":
            return e[2], e[1]
        if tag == "id":
            t, v = env[e[1]]
            return v, t
        if tag == "un":
            v, t = self.eval_expr(e[2], env)
            op = e[1]
            if t.is_int() and t.width < 32:
                t = TY_INT
                v = mask(v, t)  # integer promotion (sign/zero-extended)
            if op == "-":
                return -v, t
            if op == "!":
                return (0 if v == 0 else 1), TY_INT
            if op == "~":
                return mask(~v, t), t
        if tag == "bin":
            op = e[1]
            if op == "&&":
                a = self.eval_expr(e[2], env)[0]
                b = self.eval_expr(e[3], env)[0] if a else 0
                return (1 if (a and b) else 0), TY_INT
            if op == "||":
                a = self.eval_expr(e[2], env)[0]
                b = self.eval_expr(e[3], env)[0] if not a else 0
                return (1 if (a or b) else 0), TY_INT
            a, at = self.eval_expr(e[2], env)
            b, bt = self.eval_expr(e[3], env)
            if at.is_int() and at.width < 32:
                at = TY_INT
                a = mask(a, at)
            if bt.is_int() and bt.width < 32:
                bt = TY_INT
                b = mask(b, bt)
            if op in ("<", "<=", ">", ">=", "==", "!="):
                r = {"<": a < b, "<=": a <= b, ">": a > b,
                     ">=": a >= b, "==": a == b, "!=": a != b}[op]
                return (1 if r else 0), TY_INT
            return self.apply_bin(op, a, at, b, bt)
        if tag == "tern":
            c = self.eval_expr(e[1], env)[0]
            v, _ = self.eval_expr(e[2] if c else e[3], env)
            ct = e[4]
            if ct.is_int():
                v = mask(v, ct)
            return v, ct
        if tag == "cast":
            v, _ = self.eval_expr(e[2], env)
            return mask(v, e[1]), e[1]
        if tag == "sub":
            a, at = self.eval_expr(e[1], env)
            i, _ = self.eval_expr(e[2], env)
            if not (0 <= i < len(a)):
                raise GenUB("array oob %d" % i)
            return a[i], at.elem
        if tag == "deref":
            p, _ = self.eval_expr(e[1], env)
            if not isinstance(p, list) or len(p) == 0:
                raise GenUB("bad deref")
            return p[0], TY_INT
        if tag == "addr":
            v, t = self.eval_expr(e[1], env)
            return v, ptr_ty(TY_INT)
        if tag == "mem":
            st, _ = self.eval_expr(e[1], env)
            return st[e[2]], e[3]
        if tag == "call":
            args = [self.eval_expr(a, env)[0] for a in e[2]]
            return self.call_model(e[1], args)
        if tag == "sz":
            if e[1] == "type":
                return ty_size(e[2]), TY_INT
            _, t = self.eval_expr(e[2], env)
            return ty_size(t), TY_INT
        if tag == "clit":
            ty, init = e[1], e[2]
            d = self.struct_zero(ty)
            d.update(init)
            return d, ty
        raise ValueError("eval_expr %s" % tag)

    def call_model(self, fname, args):
        if self.recur > MODEL_MAX_RECURSION:
            raise GenFail("recursion cap")
        self.recur += 1
        try:
            params, body, ret, kr = self.fns[fname]
            env = {}
            for (pn, pt), a in zip(params, args):
                if pt.is_int():
                    env[pn] = (pt, mask(a, pt))
                else:
                    env[pn] = (pt, a)
            self.return_value = None
            try:
                self.model_run_body(body, env)
            except _Ret as r:
                self.return_value = r.v
            return self.return_value, ret
        finally:
            self.recur -= 1

    def apply_bin(self, op, a, at, b, bt):
        ct = uac(at, bt)
        if op in ("+", "-", "*"):
            r = {"+": a + b, "-": a - b, "*": a * b}[op]
            if ct.signed and not fits(r, ct):
                raise GenUB("signed overflow %s (%d %s %d)" % (ct.name, a, op, b))
            return mask(r, ct), ct
        if op == "/":
            if b == 0:
                raise GenUB("div by zero")
            r = c_trunc_div(a, b)
            if ct.signed and not fits(r, ct):
                raise GenUB("div overflow %d/%d" % (a, b))
            return r, ct
        if op == "%":
            if b == 0:
                raise GenUB("mod by zero")
            return c_mod(a, b), ct
        if op in ("<<", ">>"):
            if not (0 <= b < ct.width):
                raise GenUB("shift by %d" % b)
            if op == "<<":
                r = a << b
                if ct.signed and (a < 0 or not fits(r, ct)):
                    raise GenUB("left shift UB")
                # unsigned results wrap mod 2^width (C 6.2.5); the pre-fix
                # unmasked python bignum leaked into 128-bit decls (fuzz
                # seed 20261137: (2^64-17)<<4)
                return mask(r, ct), ct
            if ct.signed and a < 0:
                return c_trunc_div(a, 1 << b), ct  # implementation-defined; gcc: arithmetic
            return a >> b, ct
        if op in ("&", "|", "^"):
            m = (1 << ct.width) - 1
            r = {"&": a & b, "|": a | b, "^": a ^ b}[op] & m
            return (r if not ct.signed else mask(r, ct)), ct
        raise ValueError("apply_bin %s" % op)


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

def find_gcc():
    env = os.environ.get("FUZZ_GCC")
    if env:
        return env
    for c in ("gcc-15", "gcc-14", "gcc"):
        if shutil.which(c):
            return c
    return None


def gen_program(seed):
    g = Gen(seed)
    g.gen_file()
    src = g.emit()
    return src, g.expected_stdout


def run_cmd(cmd, timeout=RUN_TIMEOUT):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return None, "", "timeout"


def run_case(src, path, expected, gcc, args):
    """Returns (status, category, detail)."""
    # gcc reference (compile + run)
    gbin = path + ".g"
    rc, out, err = run_cmd([gcc, "-std=gnu11", "-O2", "-w", "-o", gbin, path])
    if rc != 0:
        # the generator must only emit gcc-clean C: a gcc reject is a
        # generator defect, not an accept/reject agreement to be green.
        return "error", "gcc-reject", "gcc rejects generated C: " + err.strip()[:300]
    grc, gout, _ = run_cmd([gbin])
    if grc is None:
        return "diff", "gcc-timeout", "gcc binary timed out"
    if grc != 0 or gout != expected:
        return "diff", "model-mismatch", \
            "gcc rc=%s stdout=%r expected=%r" % (grc, gout[:200], expected[:200])
    # zerocc configs
    for mode in (["aot"] + (["jit"] if not args.no_jit else [])):
        for lvl in args.levels:
            if mode == "jit" and lvl != 0:
                continue
            tag = "%s-O%d" % (mode, lvl)
            if mode == "aot":
                zbin = path + ".z%d" % lvl
                rcz, outz, errz = run_cmd([ZC, "-O%d" % lvl, "-o", zbin, path])
                if rcz != 0:
                    return "diff", "zerocc-reject", \
                        "%s compile: %s" % (tag, errz.strip()[:200])
                zrc, zout, _ = run_cmd([zbin])
            else:
                zrc, zout, errz = run_cmd([ZC, "--run", path])
            if zrc is None:
                return "diff", "zerocc-timeout", "%s run timed out" % tag
            if zrc != grc or zout != gout:
                return "diff", "run-mismatch", \
                    "%s: rc %s vs %s; out %r vs %r" % (
                        tag, zrc, grc, zout[:200], gout[:200])
    return "pass", None, "all configs match gcc"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, default=DEFAULT_SEEDS)
    ap.add_argument("--base-seed", type=int, default=DEFAULT_BASE_SEED)
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--levels", type=int, nargs="+", default=[0])
    ap.add_argument("--no-jit", action="store_true")
    ap.add_argument("--only", default=None, help="comma-separated case indices")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(ZC):
        print("error: %s missing (make build/bin/zerocc)" % ZC, file=sys.stderr)
        return 2
    gcc = args.gcc or find_gcc()
    if not gcc:
        print("error: no reference gcc found (FUZZ_GCC=gcc-15?)", file=sys.stderr)
        return 2

    baseline = {}
    if os.path.exists(BASELINE):
        with open(BASELINE) as f:
            baseline = json.load(f).get("cases", {})

    only = None
    if args.only:
        only = set(int(x) for x in args.only.split(","))

    results = {}
    fails = 0
    errors = 0
    discards = 0
    tmpdir = tempfile.mkdtemp(prefix="zerocc_fuzz_")
    try:
        for i in range(args.seeds):
            if only is not None and i not in only:
                continue
            seed = args.base_seed + i
            case = "case_%04d" % i
            try:
                src, expected = gen_program(seed)
            except Exception as ex:
                discards += 1
                results[case] = {"status": "discarded",
                                 "detail": "%s: %s" % (type(ex).__name__, ex),
                                 "seed": seed}
                if args.verbose:
                    print("[skip] %s (seed %d) discarded: %s" %
                          (case, seed, ex))
                continue
            path = os.path.join(tmpdir, "p.c")
            with open(path, "w") as f:
                f.write(src)
            status, cat, detail = run_case(src, path, expected, gcc, args)
            results[case] = {"status": status, "category": cat,
                             "detail": detail, "seed": seed}
            if status == "error":
                errors += 1
            if status != "pass":
                fails += 1
                if args.verbose:
                    print("[FAIL] %s (seed %d): %s - %s" %
                          (case, seed, cat, detail))
            elif args.verbose:
                print("[ok]   %s (seed %d)" % (case, seed))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    ncases = len(results)
    if errors:
        for case, r in results.items():
            if r["status"] == "error":
                print("GATE ERROR %s (seed %d): %s - %s" %
                      (case, r["seed"], r["category"], r["detail"]))
        print("GATE FAILED: %d generator defect(s) (gcc rejects the "
              "generated C) - fix the generator, not the compiler" % errors)
        return 2
    if ncases and discards / ncases > MAX_DISCARD_RATE:
        print("GATE ERROR: discard rate %.0f%% exceeds %.0f%% - "
              "generator bounds bug, not a compiler result" %
              (100.0 * discards / ncases, 100 * MAX_DISCARD_RATE))
        return 2

    if args.update_baseline:
        os.makedirs(FUZZ_DIR, exist_ok=True)
        newbase = {}
        for case, r in results.items():
            if r["status"] == "discarded":
                continue
            if r["status"] == "pass":
                newbase[case] = {"status": "pass"}
            else:
                src, _ = gen_program(r["seed"])
                sf = os.path.join(FUZZ_DIR, "seed_%d.c" % r["seed"])
                with open(sf, "w") as f:
                    f.write(src)
                newbase[case] = {"status": "diff", "category": r["category"],
                                 "detail": r["detail"], "seed": r["seed"],
                                 "file": os.path.basename(sf)}
        with open(BASELINE, "w") as f:
            json.dump({"generated_by": "tools/fuzz/fuzz_diff.py --update-baseline",
                       "note": "ratchet: case = base_seed + index; a diff entry records a known divergence (source persisted as seed_<seed>.c). The gate fails on a new or changed divergence. Re-record after an intentional fix.",
                       "reference": gcc, "base_seed": args.base_seed,
                       "cases": newbase}, f, indent=2)
        print("baseline updated: %d cases (%d diff, %d discarded)" %
              (len(newbase),
               sum(1 for v in newbase.values() if v["status"] == "diff"),
               discards))
        return 0

    # ratchet check
    regressions = []
    new_diffs = []
    fixed = []
    for case, r in results.items():
        if r["status"] == "discarded":
            continue
        b = baseline.get(case)
        if b is None:
            if r["status"] != "pass":
                new_diffs.append((case, r))
            continue
        if b.get("status") == "pass" and r["status"] != "pass":
            regressions.append((case, b, r))
        elif b.get("status") == "diff" and r["status"] == "pass":
            fixed.append(case)
        elif b.get("status") == "diff" and r["status"] != "pass":
            if (b.get("category"), b.get("detail")) != \
               (r.get("category"), r.get("detail")):
                regressions.append((case, b, r))

    total = ncases - discards
    npass = total - fails
    print("%d cases: %d pass, %d fail, %d discarded (reference: %s, "
          "levels %s%s)" %
          (ncases, npass, fails, discards, gcc, args.levels,
           ", +JIT" if not args.no_jit else ""))
    for case, b, r in regressions:
        print("REGRESSION %s (seed %d): was %s/%s, now %s/%s - %s" %
              (case, r["seed"], b.get("status"), b.get("category"),
               r["status"], r.get("category"), r.get("detail")))
    for case, r in new_diffs:
        print("NEW-DIFF %s (seed %d): %s - %s" %
              (case, r["seed"], r["category"], r["detail"]))
    for case in fixed:
        print("fixed  %s (now passes; --update-baseline to ratchet out)" % case)
    if regressions or new_diffs:
        print("GATE FAILED (%d regression(s), %d new diff(s))" %
              (len(regressions), len(new_diffs)))
        return 1
    print("gate OK: no regressions vs baseline (%d fixed pending ratchet)"
          % len(fixed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
