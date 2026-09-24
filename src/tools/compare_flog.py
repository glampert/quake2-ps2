#!/usr/bin/env python3
"""Compare two frame-log captures column by column.

Takes either a raw PCSX2 emulog or a .flog written by summarize_flog.py --rows,
for each side. Prints mean and p95 for every column with the percentage change,
and exits non-zero if any column's mean moved past the threshold, so it can gate
a refactor. Columns are matched by name from each file's FLOG#hdr, so captures
from builds that log different columns still compare on the ones they share.

Both captures have to be the same run - the ps2_perftest demos - or the
differences are the content, not the code.

Usage: compare_flog.py <before> <after> [--threshold 3.0]
"""
import sys, statistics

# summarize_flog lives beside this; keep the import from leaving a __pycache__
# in src/tools.
sys.dont_write_bytecode = True
from summarize_flog import extract

# Counters that legitimately move with frame count rather than with cost: a time-based demo
# renders more frames when it is faster, so each frame holds proportionally less work.
RATE_COLUMNS = {'frame'}

def load(path):
    hdr, rows, _, _, _ = extract(path)
    if hdr is None:
        sys.exit(f"{path}: no FLOG#hdr - not a frame-log capture")
    cols = hdr.split(',')[1:]
    data = {c: [] for c in cols}
    for r in rows:
        f = r.split(',')[1:]
        if len(f) != len(cols):
            continue
        for c, v in zip(cols, f):
            try: data[c].append(float(v))
            except ValueError: pass
    return cols, data, len(rows)

def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(len(v) * q))]

def main():
    before, after = sys.argv[1], sys.argv[2]
    thresh = float(sys.argv[sys.argv.index('--threshold') + 1]) if '--threshold' in sys.argv else 3.0

    cols, b, bn = load(before)
    _,    a, an = load(after)

    print(f"frames: {bn} -> {an}  ({100.0 * (an - bn) / bn:+.2f}%)")
    print(f"threshold: +/-{thresh}% on the mean\n")
    print(f"{'column':<16}{'mean before':>13}{'mean after':>12}{'delta':>9}"
          f"{'p95 before':>12}{'p95 after':>11}{'delta':>9}")

    regressions = []
    for c in cols:
        if c in RATE_COLUMNS or not b.get(c) or not a.get(c):
            continue
        bm, am = statistics.mean(b[c]), statistics.mean(a[c])
        b95, a95 = pct(b[c], 0.95), pct(a[c], 0.95)
        dm  = 100.0 * (am - bm) / bm if bm else 0.0
        d95 = 100.0 * (a95 - b95) / b95 if b95 else 0.0
        flag = ''
        if abs(dm) > thresh and bm >= 1.0:
            flag = '  <-- CHECK'
            regressions.append((c, dm))
        print(f"{c:<16}{bm:>13.1f}{am:>12.1f}{dm:>8.1f}%{b95:>12.1f}{a95:>11.1f}{d95:>8.1f}%{flag}")

    print()
    if regressions:
        print("moved past threshold: " + ", ".join(f"{c} {d:+.1f}%" for c, d in regressions))
        sys.exit(1)
    print(f"all columns within +/-{thresh}%")

if __name__ == '__main__':
    main()
