#!/usr/bin/env python3
"""Differential for the c-torture REGRESSION execute/ files only (fast:
compiles via zerocc, runs, compares rc to the gcc reference). Usage:
  python3 tools/regress_exec_diff.py [file-list]
Defaults to /tmp/reg_execute.txt (the regression list from last_run.json)."""
import subprocess, sys, os, hashlib, json
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(REPO, 'third_party/vendor/c-torture')
ZEROC = os.path.join(REPO, 'build/bin/zerocc')
GCC = os.environ.get('TORTURE_GCC', 'gcc')
REFCACHE = os.path.join(REPO, 'test', 'torture', 'work', 'refcache')
os.makedirs(REFCACHE, exist_ok=True)

def run(cmd, timeout):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', '', ''

def main():
    listfile = sys.argv[1] if len(sys.argv) > 1 else '/tmp/reg_execute.txt'
    files = [l.strip() for l in open(listfile) if l.strip()]
    ok, crash, rej = [], [], []
    for f in files:
        path = os.path.join(CORPUS, f)
        # zerocc compile (AOT)
        binp = '/tmp/regexec_bin'
        rc, _o, err = run([ZEROC, '-O2', path, '-o', binp], 120)
        if rc != 0:
            rej.append((f, 'zc-compile', (err.strip().splitlines() or ['?'])[-1][:90]))
            continue
        # gcc reference run
        key = hashlib.sha1((GCC + path + 'run').encode()).hexdigest()[:16]
        ref = os.path.join(REFCACHE, key + '.bin')
        if not os.path.exists(ref):
            rc, _o, err = run([GCC, '-O1', path, '-o', ref, '-lm'], 120)
            if rc != 0:
                rej.append((f, 'gcc-ref', 'gcc-refbuild')); continue
        rc_ref, out_ref, _e = run([ref], 30)
        rc_zc, out_zc, _e = run([binp], 30)
        if rc_zc in ('TIMEOUT', -11, -6, -8, -4) or rc_zc != rc_ref:
            crash.append((f, f'rc_ref={rc_ref} rc_zc={rc_zc}'))
        else:
            ok.append(f)
    print(f'execute regressions: {len(files)} total, {len(ok)} now-OK, {len(crash)} crash/mismatch, {len(rej)} compile-reject')
    print('=== crash/mismatch ===')
    for f, d in crash: print(f'  {f}: {d}')
    print('=== compile-reject ===')
    for f, why, e in rej: print(f'  {f}: [{why}] {e}')

if __name__ == '__main__':
    main()
