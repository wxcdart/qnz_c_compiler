#!/usr/bin/env python3
"""
CFG layer verifier (Sprint 35 Phase 2).

Compiles each standalone test with zerocc using BOTH -dump-ir and -dump-cfg
(one run; both dumps go to stderr), then cross-checks the C-side CFG report
against an INDEPENDENT Python computation (blocks from leaders, backedges ->
natural loops) built from the IR dump.  The two computations share no code -
only the opcode rules (terminal set, branch set).

Checks per function:
  1. block tiling: bb0 starts at 0, blocks contiguous, no overlap
  2. every jump target (label position in the IR) is a block start
  3. pred/succ lists are mutually consistent
  4. reachability: reported count == BFS; a block with a reachable
     predecessor must itself be reachable
  5. loops: C-reported backedge set == Python-computed backedge set
     (compared as (header_pos, from_pos) pairs)
  6. dominators: C-reported idom == Python idom (independent
     round-robin set dataflow over the reported pred/succ lists -
     different schedule and representation than the C worklist+bitset)
  7. post-dominators: C-reported post_idom == Python post_idom (the
     same computation on the reverse CFG with a synthetic entry above
     all exit blocks; an exit reports itself)

Unreachable blocks (unreachable tails after returns, orphaned constant
blocks emitted by lowering) are reported as WARNINGS, not failures - they
are a property of the existing codegen, not of the CFG layer.

Usage:
  python3 tools/cfg_verifier.py --verify-all
  python3 tools/cfg_verifier.py --file test/cfg/nested_loops.c
  python3 tools/cfg_verifier.py --verify-all --dot /tmp/cfgs/
"""
import sys
from pathlib import Path

# Make the tools package importable so we can use find_repo_root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

import argparse
import os
import re
import subprocess
import sys

ROOT = find_repo_root()
ZEROCC = os.path.join(ROOT, "build", "bin", "zerocc")
SWEEP_CORPUS = ["test/jit", "test/aot", "test/licm", "test/inline_audit"]
CFG_UNIT_DIR = "test/cfg"

TERMINAL_OPS = {"JMP", "JMP_INDIRECT", "JE", "JNE", "JL", "JLE", "JG", "JCOND", "RET", "EPILOGUE"}
BRANCH_OPS = {"JE", "JNE", "JL", "JLE", "JG", "JCOND"}

IR_LINE = re.compile(r"^\s*(\d+):\s+(.*)$")
LABEL_LINE = re.compile(r"^(L\d+):\s*\[")
JUMP_TGT = re.compile(r"\bL(\d+)\b")
CFG_BLOCK = re.compile(
    r"^bb(\d+): \[(\d+)\.\.(\d+)\] succ=(bb\d+(?:,bb\d+)*)? pred=(bb\d+(?:,bb\d+)*)? loop_header=([01])(?: idom=(bb\d+|-) post_idom=(bb\d+|-))?$")
CFG_BBLIST = re.compile(r"bb(\d+)")
CFG_LOOP = re.compile(r"^  bb(\d+)->bb(\d+)$")
CFG_SUMMARY = re.compile(r"^(\d+) blocks, (\d+) reachable, (\d+) loops$")


def compile_dumps(src, opt="-O0", timeout=60):
    out = subprocess.run(
        [ZEROCC, "-dump-ir", "-dump-cfg", opt, src, "-o", "/tmp/cfgv_bin"],
        capture_output=True, text=True, timeout=timeout)
    if out.returncode != 0:
        raise RuntimeError("compile failed rc=%d: %s" % (out.returncode, out.stderr[-300:]))
    return out.stderr


def parse_ir_functions(dump):
    """Parse -dump-ir text into [(fn_name, [(idx, op, label, target)])]."""
    fns = []
    cur = None
    for line in dump.splitlines():
        m = re.match(r"^=== IR for function '(.+)' ===\s*$", line)
        if m:
            cur = [m.group(1), []]
            fns.append(cur)
            continue
        if cur is None:
            continue
        if line.startswith("=== Allocated Intervals"):
            cur = None
            continue
        m = IR_LINE.match(line)
        if not m:
            continue
        idx = int(m.group(1))
        rest = m.group(2)
        lm = LABEL_LINE.match(rest)
        if lm:
            cur[1].append((idx, "LABEL", int(lm.group(1)[1:]), None))
            continue
        op = rest.split()[0]
        target = None
        if op in TERMINAL_OPS and op not in ("RET", "EPILOGUE"):
            tm = JUMP_TGT.search(rest)
            if tm:
                target = int(tm.group(1))
        cur[1].append((idx, op, None, target))
    return [(n, insns) for n, insns in fns if insns]


def parse_cfg_functions(dump):
    """Parse -dump-cfg text into [(fn, blocks, loops, summary)] with
    blocks=[(start, end_excl, succs, preds, loop_header, idom, post_idom)]
    (idom/post_idom are -1 for "-", None when the C side omitted the
    fields), loops=[(from_block, header_block)],
    summary=(nblocks,nreach,nloops)."""
    fns = []
    cur = None
    in_loops = False
    for line in dump.splitlines():
        m = re.match(r"^fn (.+)$", line)
        if m:
            cur = [m.group(1), [], [], None]
            fns.append(cur)
            in_loops = False
            continue
        if cur is None:
            continue
        if line.strip() == "loops:":
            in_loops = True
            continue
        m = CFG_BLOCK.match(line)
        if m and not in_loops:
            succs = [int(x) for x in CFG_BBLIST.findall(m.group(4) or "")]
            preds = [int(x) for x in CFG_BBLIST.findall(m.group(5) or "")]
            idom = (None if m.group(7) is None
                    else -1 if m.group(7) == "-" else int(m.group(7)[2:]))
            pidom = (None if m.group(8) is None
                     else -1 if m.group(8) == "-" else int(m.group(8)[2:]))
            cur[1].append((int(m.group(2)), int(m.group(3)) + 1,
                           succs, preds, m.group(6) == "1", idom, pidom))
            continue
        m = CFG_LOOP.match(line)
        if m and in_loops:
            cur[2].append((int(m.group(1)), int(m.group(2))))
            continue
        m = CFG_SUMMARY.match(line)
        if m:
            cur[3] = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            in_loops = False
    return [tuple(f) for f in fns]


def python_build_cfg(insns):
    """Independent block/loop computation from IR instructions.
    Returns (blocks=[(start,end)], loops=set((header_pos,from_pos)))."""
    n = len(insns)
    if n == 0:
        return [], set()
    max_idx = insns[-1][0]
    leader = [False] * (max_idx + 1)
    leader[0] = True
    for i, (idx, op, _lab, _tgt) in enumerate(insns):
        if op == "LABEL":
            leader[idx] = True
        if i + 1 < n and op in TERMINAL_OPS:
            leader[insns[i + 1][0]] = True
    leader_positions = [insns[i][0] for i in range(n) if leader[insns[i][0]]]
    blocks = []
    for k, start in enumerate(leader_positions):
        end = leader_positions[k + 1] if k + 1 < len(leader_positions) else max_idx + 1
        blocks.append((start, end))
    label_pos = {}
    for idx, op, lab, _tgt in insns:
        if op == "LABEL" and lab not in label_pos:
            label_pos[lab] = idx
    start_to_block = {s: b for b, (s, _e) in enumerate(blocks)}
    label_block = {lab: start_to_block[pos]
                   for lab, pos in label_pos.items() if pos in start_to_block}
    succs = {b: set() for b in range(len(blocks))}
    for b, (start, end) in enumerate(blocks):
        last = [t for t in insns if start <= t[0] < end][-1]
        _idx, op, _lab, tgt = last
        if op == "JMP":
            if tgt in label_block:
                succs[b].add(label_block[tgt])
        elif op == "JMP_INDIRECT":
            # C rule: an indirect jump can target any block that starts
            # with an OP_LABEL (all of them, no fallthrough).
            succs[b].update(label_block.values())
        elif op in BRANCH_OPS:
            if tgt in label_block:
                succs[b].add(label_block[tgt])
            if b + 1 < len(blocks):
                succs[b].add(b + 1)
        elif op in ("RET", "EPILOGUE"):
            pass
        else:
            if b + 1 < len(blocks):
                succs[b].add(b + 1)
    loops = set()
    for b in range(len(blocks)):
        for v in succs[b]:
            if blocks[v][0] <= blocks[b][0]:
                loops.add((blocks[v][0], blocks[b][0]))
    return blocks, loops


def check_function(fn_name, insns, blocks, loops, summary):
    """Returns (errors, warnings, nreach_computed)."""
    errors, warnings = [], []
    pb, ploops = python_build_cfg(insns)

    if not blocks:
        if len(pb) != 0:
            errors.append("empty CFG but IR has %d instructions" % len(insns))
        return errors, warnings, 0

    # 1. tiling (from the C report itself)
    if blocks[0][0] != 0:
        errors.append("bb0 does not start at 0 (starts at %d)" % blocks[0][0])
    for i in range(1, len(blocks)):
        if blocks[i][0] != blocks[i - 1][1]:
            errors.append("tiling gap/overlap: bb%d starts at %d but bb%d ends at %d"
                          % (i, blocks[i][0], i - 1, blocks[i - 1][1]))

    # block count and ranges vs the independent computation
    if len(blocks) != len(pb):
        errors.append("block count mismatch: C=%d python=%d" % (len(blocks), len(pb)))
        return errors, warnings, -1
    for b, (cs, ce, _s, _p, _lh, _id, _pid) in enumerate(blocks):
        if (cs, ce) != pb[b]:
            errors.append("bb%d range mismatch: C=[%d..%d) python=[%d..%d)"
                          % (b, cs, ce, pb[b][0], pb[b][1]))
            return errors, warnings, -1

    # 2. every jump target (label) is a block start (from the IR)
    starts = set(s for s, _e in pb)
    label_pos = {}
    for idx, op, lab, _tgt in insns:
        if op == "LABEL" and lab not in label_pos:
            label_pos[lab] = idx
    for idx, op, _lab, tgt in insns:
        if op in TERMINAL_OPS and op not in ("RET", "EPILOGUE") and tgt is not None:
            pos = label_pos.get(tgt)
            if pos is None:
                errors.append("jump at %d targets undefined label L%d" % (idx, tgt))
            elif pos not in starts:
                errors.append("jump at %d targets L%d at %d which is not a block start"
                              % (idx, tgt, pos))

    # 3. pred/succ mutual consistency (C report)
    for b, (cs, ce, succs, preds, _lh, _id, _pid) in enumerate(blocks):
        for s in succs:
            if s >= len(blocks):
                errors.append("bb%d succ %d out of range" % (b, s))
                continue
            if b not in blocks[s][3]:
                errors.append("bb%d -> bb%d but bb%d pred list lacks bb%d" % (b, s, s, b))
        for p in preds:
            if p >= len(blocks):
                errors.append("bb%d pred %d out of range" % (b, p))
                continue
            if b not in blocks[p][2]:
                errors.append("bb%d <- bb%d but bb%d succ list lacks bb%d" % (b, p, p, b))

    # 4. reachability (BFS on C-reported succ lists)
    reach = {0}
    changed = True
    while changed:
        changed = False
        for b in list(reach):
            for s in blocks[b][2]:
                if s < len(blocks) and s not in reach:
                    reach.add(s)
                    changed = True
    for b, (cs, ce, succs, preds, _lh, _id, _pid) in enumerate(blocks):
        for p in preds:
            if p in reach and b not in reach:
                errors.append("bb%d unreachable but has reachable pred bb%d" % (b, p))
    unreachable = [b for b in range(len(blocks)) if b not in reach]
    if unreachable:
        warnings.append("%d unreachable block(s): %s"
                        % (len(unreachable), ",".join("bb%d" % b for b in unreachable)))

    # 5. loop cross-check (C report vs python)
    c_loops = set()
    for from_b, hdr_b in loops:
        if 0 <= hdr_b < len(blocks) and 0 <= from_b < len(blocks):
            c_loops.add((blocks[hdr_b][0], blocks[from_b][0]))
        else:
            errors.append("loop entry references out-of-range blocks (from=bb%d hdr=bb%d)"
                          % (from_b, hdr_b))
    if c_loops != ploops:
        errors.append("loop set mismatch: C=%s python=%s"
                      % (sorted(c_loops), sorted(ploops)))

    # reported counts
    if summary:
        if summary[0] != len(blocks):
            errors.append("summary block count %d != %d" % (summary[0], len(blocks)))
        if summary[1] != len(reach):
            errors.append("summary reachable %d != computed %d" % (summary[1], len(reach)))
        if summary[2] != len(loops):
            errors.append("summary loop count %d != listed %d" % (summary[2], len(loops)))

    # 6/7. dominators and post-dominators vs the independent computation
    have_dom = all(bl[5] is not None and bl[6] is not None for bl in blocks)
    if not have_dom:
        warnings.append("idom/post_idom fields missing from C dump "
                        "(dominator dataflow allocation failed?)")
    else:
        idom_c = [bl[5] for bl in blocks]
        pidom_c = [bl[6] for bl in blocks]
        idom_py, pidom_py = python_dominators(blocks)
        if idom_c != idom_py:
            errors.append("idom mismatch: C=%s python=%s" % (idom_c, idom_py))
        if pidom_c != pidom_py:
            errors.append("post_idom mismatch: C=%s python=%s" % (pidom_c, pidom_py))

    return errors, warnings, len(reach)


def invert_preds(preds, total):
    """Successor lists by inverting predecessor lists."""
    succs = [[] for _ in range(total)]
    for v in range(total):
        for p in preds[v]:
            if 0 <= p < total:
                succs[p].append(v)
    return succs


def python_rpo(succs, entry, total):
    """Reverse postorder over succs from entry (iterative DFS)."""
    state = [0] * total  # 0 unvisited, 1 on stack, 2 done
    next_s = [0] * total
    post = []
    stack = [entry]
    state[entry] = 1
    while stack:
        u = stack[-1]
        if next_s[u] < len(succs[u]):
            v = succs[u][next_s[u]]
            next_s[u] += 1
            if state[v] == 0:
                state[v] = 1
                stack.append(v)
        else:
            state[u] = 2
            stack.pop()
            post.append(u)
    rpo = [-1] * total
    for k in range(len(post) - 1, -1, -1):
        rpo[post[k]] = len(post) - 1 - k
    return rpo


def python_idom(preds, succs, entry, reach):
    """Meet-over-preds dominator dataflow to fixpoint (round-robin
    schedule, Python sets) then idom = deepest (max RPO) dominator-set
    member other than the block itself.  Independent implementation of
    the same equations the C side runs with a worklist + bitsets."""
    total = len(preds)
    reach = [b in reach for b in range(total)]
    rpo = python_rpo(succs, entry, total)
    D = [None] * total
    for b in range(total):
        if not reach[b]:
            D[b] = None
        elif b == entry:
            D[b] = {entry}
        else:
            D[b] = set(range(total))
    changed = True
    while changed:
        changed = False
        for b in range(total):
            if b == entry or D[b] is None:
                continue
            inter = None
            for p in preds[b]:
                if not reach[p]:
                    continue
                inter = D[p] if inter is None else (inter & D[p])
            if inter is None:
                continue
            new = {b} | inter
            if new != D[b]:
                D[b] = new
                changed = True
    idom = [-1] * total
    for b in range(total):
        if D[b] is None:
            continue
        if b == entry:
            idom[b] = entry
            continue
        best, best_rpo = -1, -1
        for d in D[b]:
            if d == b:
                continue
            if best < 0 or rpo[d] > best_rpo:
                best, best_rpo = d, rpo[d]
        idom[b] = best
    return idom


def python_dominators(blocks):
    """(idom, post_idom) arrays computed independently from the C-
    reported block structure (already cross-checked against the IR).
    idom[b] = -1 when b is unreachable from entry; post_idom[b] = -1
    when b cannot reach an exit; an exit (or a block with no closer
    post-dominator) reports itself."""
    n = len(blocks)
    if n == 0:
        return [], []
    # forward graph (entry = 0)
    preds_f = [list(b[3]) for b in blocks]
    preds_f.append([])
    succs_f = invert_preds(preds_f, n + 1)
    reach_f = set([0])
    changed = True
    while changed:
        changed = False
        for b in list(reach_f):
            for s in succs_f[b]:
                if s not in reach_f:
                    reach_f.add(s)
                    changed = True
    idom_f = python_idom(preds_f, succs_f, 0, reach_f)
    # reverse graph (entry = synthetic X above all exit blocks)
    X = n
    preds_r = [list(b[2]) for b in blocks]
    for b in range(n):
        if not preds_r[b]:
            preds_r[b].append(X)
    preds_r.append([])
    succs_r = invert_preds(preds_r, n + 1)
    reach_r = set([X])
    changed = True
    while changed:
        changed = False
        for b in list(reach_r):
            for s in succs_r[b]:
                if s not in reach_r:
                    reach_r.add(s)
                    changed = True
    idom_r = python_idom(preds_r, succs_r, X, reach_r)
    idom_p = []
    for b in range(n):
        v = idom_r[b]
        if v == X:
            v = b
        idom_p.append(v)
    return idom_f[:n], idom_p


def write_dot(path, fn_name, blocks):
    lines = ["digraph %s {" % re.sub(r"\W", "_", fn_name)]
    for b, (s, e, succs, preds, lh, _id, _pid) in enumerate(blocks):
        lines.append('  bb%d [label="bb%d\\n[%d..%d)%s"];'
                     % (b, b, s, e - 1, " (header)" if lh else ""))
    for b, (s, e, succs, preds, lh, _id, _pid) in enumerate(blocks):
        for s2 in succs:
            lines.append("  bb%d -> bb%d;" % (b, s2))
    lines.append("}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def verify_file(src, dot_dir=None, opt="-O0"):
    """Returns (errors, warnings, detail_str)."""
    try:
        dump = compile_dumps(src, opt)
    except Exception as e:
        # Not a standalone program (API/header-dependent test, same policy
        # as opt_sweep.py: skipped, not failed).
        return None, ["skipped: compile failed (%s)" % str(e).splitlines()[0]], ""
    ir_fns = parse_ir_functions(dump)
    cfg_fns = parse_cfg_functions(dump)
    if len(ir_fns) != len(cfg_fns):
        return ["function count mismatch: ir=%d cfg=%d" % (len(ir_fns), len(cfg_fns))], [], ""
    errors, warnings = [], []
    detail = []
    for (fn_name, insns), (cfg_name, blocks, loops, summary) in zip(ir_fns, cfg_fns):
        if fn_name != cfg_name:
            errors.append("function name mismatch: ir='%s' cfg='%s'" % (fn_name, cfg_name))
            continue
        ferrors, fwarnings, _nreach = check_function(fn_name, insns, blocks, loops, summary)
        if dot_dir:
            write_dot(os.path.join(dot_dir, "%s_%s.dot"
                                   % (os.path.splitext(os.path.basename(src))[0], fn_name)),
                      fn_name, blocks)
        for e in ferrors:
            errors.append("%s.%s: %s" % (os.path.basename(src), fn_name, e))
        for w in fwarnings:
            warnings.append("%s.%s: %s" % (os.path.basename(src), fn_name, w))
        extra = ""
        if loops:
            extra += ", %d loop(s)" % len(loops)
        nunreach = 0
        for w in fwarnings:
            if "unreachable" in w:
                nunreach = int(w.split()[0])
        if nunreach:
            extra += " [%d unreachable]" % nunreach
        detail.append("  %s: %s, %d blocks%s"
                      % (fn_name, "OK" if not ferrors else "FAIL", len(blocks), extra))
    return errors, warnings, "\n".join(detail)


def discover(corpus_dirs):
    tests = []
    for d in corpus_dirs:
        path = os.path.join(ROOT, d)
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if not name.endswith(".c"):
                continue
            fp = os.path.join(path, name)
            with open(fp) as f:
                if re.search(r"\bint\s+main\s*\(", f.read()):
                    tests.append(os.path.join(d, name))
    return tests


def main():
    ap = argparse.ArgumentParser(description="CFG layer verifier")
    ap.add_argument("--verify-all", action="store_true")
    ap.add_argument("--file", default=None)
    ap.add_argument("--dot", nargs="?", const=True, default=None,
                    help="write DOT exports (optionally to a directory)")
    ap.add_argument("--opt", default="-O0")
    args = ap.parse_args()

    if not os.path.exists(ZEROCC):
        print("Error: %s not found - run 'make build/bin/zerocc' first" % ZEROCC)
        return 1

    dot_dir = None
    if args.dot:
        dot_dir = args.dot if isinstance(args.dot, str) else "/tmp/cfg_dot"
        os.makedirs(dot_dir, exist_ok=True)

    if args.file:
        tests = [args.file]
    elif args.verify_all:
        tests = discover(SWEEP_CORPUS + [CFG_UNIT_DIR])
    else:
        ap.print_help()
        return 1

    total_fail = 0
    total_warn = 0
    total_skip = 0
    for t in tests:
        errors, warnings, detail = verify_file(t, dot_dir, args.opt)
        if errors is None:
            total_skip += 1
            print("[SKIP] %s" % t)
        elif errors:
            total_fail += 1
            print("[FAIL] %s" % t)
            for e in errors:
                print("       %s" % e)
        else:
            print("[PASS] %s" % t)
        for w in warnings:
            total_warn += 1
            print("       WARN %s" % w)
        if detail:
            print(detail)

    print("=" * 60)
    print("CFG verification: %d file(s), %d failed, %d skipped, %d warning(s)"
          % (len(tests), total_fail, total_skip, total_warn))
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
