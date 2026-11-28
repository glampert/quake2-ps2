#!/usr/bin/env python3
"""Read a GPU sampler capture: where GsWait goes, per measuring mode, and what VU1 work costs.

A build with PS2_QUAKE_GPU_SAMPLER (debug and profiling builds, see src/ps2/renderer/cmd_buffer.h)
splits each frame's GsWait into five columns by what held the chain up while the EE waited:

  gpuVu      a microprogram running
  gpuGs      the GS slow to take what VU1 or the VIF hands it (XGKICK stalls, PATH1, FLUSH waits)
  gpuDirect  the GIF on PATH2/PATH3: the 2D blocks, the clear, texture uploads
  gpuVif     VIF1 or the DMA still busy with nothing above
  gpuTail    all idle, the GS finishing its last primitives

and adds the triangle program's counts: vuSlow (triangles that took the clip/warp path), vuEmpty
(clipped to nothing), vuFanned (fan triangles sent instead) and vuCulled (back faces).

ps2_perftest 2 plays the demos three times, each pass opened by an FLOG#seg row: "pipelined"
(the normal frame: most GPU work overlaps the EE and never reaches a wait), "serial" (every
chain waited out whole: the split is the chain's whole cost) and "serial-nofill" (the 3D passes
scissored to one pixel: serial minus this is the GS's pixel work). This prints, per segment:

  - frames with a 3D view, mean GsWait, and the split's means and shares, with the check that
    the five add up to GsWait
  - each column per triangle handed to VU1 (tris)
  - for serial and serial-nofill together, the fill cost per frame and per visible pixel

--fit SEGMENT (default: serial) fits VU1 cycles a frame (gpuVu x 294.912) against the work that
drives them, by least squares: tris, batches, particles and the four VU counts, plus a constant.
That prices each kind of work in VU1 cycles. Only a console's capture means anything here: PCSX2
charges nearly everything to gpuVif (its VU1 finishes at once, and its GIF reports no path).

--pixels N gives the frame's pixel count for the per-pixel fill line (default 640x448).

Usage: gpu_split.py <emulog.txt|capture.flog> [--fit SEGMENT] [--pixels N]
"""
import argparse, sys, statistics

sys.dont_write_bytecode = True
from summarize_flog import extract, extract_segments

GPU = ['gpuVu', 'gpuGs', 'gpuDirect', 'gpuVif', 'gpuTail']
COUNTS = ['vuSlow', 'vuEmpty', 'vuFanned', 'vuCulled']
FIT_TERMS = ['tris', 'batches', 'particles'] + COUNTS
VU1_CYCLES_PER_US = 294.912


def segments_of(path):
    segs = []
    for line in extract_segments(path):
        _, f, name = line.split(',', 2)
        segs.append((int(f), name))
    return segs


def load(path):
    hdr, lines, _, _, _ = extract(path)
    if hdr is None:
        sys.exit(f"{path}: no FLOG#hdr - not a frame-log capture")
    cols = hdr.split(',')[1:]
    missing = [c for c in GPU + COUNTS if c not in cols]
    if missing:
        sys.exit(f"{path}: no {', '.join(missing)} - not a GPU sampler build's capture")
    rows = []
    for line in lines:
        v = line.split(',')[1:]
        if len(v) == len(cols):
            rows.append({c: int(x) for c, x in zip(cols, v)})
    segs = segments_of(path)
    for r in rows:
        # A marker is written in the row before its mode takes effect (the cvars are read at the
        # next BeginFrame), so a row belongs to the last marker strictly before it.
        name = 'all'
        for f, n in segs:
            if r['frame'] > f:
                name = n
        r['seg'] = name
    return rows, [n for _, n in segs] or ['all']


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))] if v else 0


def report_segment(name, rows):
    view = [r for r in rows if r['View'] > 0]
    if not view:
        print(f"\n[{name}] no frames with a 3D view")
        return None
    n = len(view)
    wait = statistics.mean(r['GsWait'] for r in view)
    split = {c: statistics.mean(r[c] for r in view) for c in GPU}
    total = sum(split.values())
    tris = statistics.mean(r['tris'] for r in view)
    print(f"\n[{name}] {n} frames with a 3D view")
    print(f"  GsWait mean {wait:8.0f} us  p50 {pct([r['GsWait'] for r in view], .5):6d}"
          f"  p95 {pct([r['GsWait'] for r in view], .95):6d}  max {max(r['GsWait'] for r in view):6d}"
          f"   waits/frame {statistics.mean(r['gpuWaits'] for r in view):.2f}")
    print(f"  split sum {total:8.0f} us ({100 * total / max(1, wait):5.1f}% of GsWait)")
    print(f"  {'column':<10}{'mean us':>10}{'share':>8}{'p95':>8}{'ns/tri':>9}")
    for c in GPU:
        share = 100 * split[c] / max(1, total)
        per_tri = 1000 * split[c] / max(1, tris)
        print(f"  {c:<10}{split[c]:>10.0f}{share:>7.1f}%{pct([r[c] for r in view], .95):>8d}{per_tri:>9.1f}")
    print(f"  tris {tris:.0f}  batches {statistics.mean(r['batches'] for r in view):.0f}"
          f"  particles {statistics.mean(r['particles'] for r in view):.0f}"
          f"  vuSlow {statistics.mean(r['vuSlow'] for r in view):.0f}"
          f"  vuEmpty {statistics.mean(r['vuEmpty'] for r in view):.0f}"
          f"  vuFanned {statistics.mean(r['vuFanned'] for r in view):.0f}"
          f"  vuCulled {statistics.mean(r['vuCulled'] for r in view):.0f}")
    return {'frames': n, 'wait': wait, 'total': total, 'split': split, 'tris': tris}


def solve(a, b):
    # Gaussian elimination with partial pivoting; a column that is all zeros gets 0.
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[piv][col]) < 1e-9:
            continue
        m[col], m[piv] = m[piv], m[col]
        for r in range(n):
            if r != col and m[r][col] != 0:
                f = m[r][col] / m[col][col]
                for k in range(col, n + 1):
                    m[r][k] -= f * m[col][k]
    return [m[i][n] / m[i][i] if abs(m[i][i]) >= 1e-9 else 0.0 for i in range(n)]


def fit(rows, name):
    view = [r for r in rows if r['View'] > 0 and r['seg'] == name]
    if len(view) < 2 * len(FIT_TERMS):
        print(f"\n--fit {name}: {len(view)} frames, too few to fit {len(FIT_TERMS) + 1} terms")
        return
    terms = FIT_TERMS + ['const']
    xs = [[float(r[t]) for t in FIT_TERMS] + [1.0] for r in view]
    ys = [r['gpuVu'] * VU1_CYCLES_PER_US for r in view]
    k = len(terms)
    ata = [[sum(x[i] * x[j] for x in xs) for j in range(k)] for i in range(k)]
    aty = [sum(x[i] * y for x, y in zip(xs, ys)) for i in range(k)]
    coef = solve(ata, aty)
    pred = [sum(c * xv for c, xv in zip(coef, x)) for x in xs]
    mean_y = statistics.mean(ys)
    ss_res = sum((y - p) ** 2 for y, p in zip(ys, pred))
    ss_tot = sum((y - mean_y) ** 2 for y in ys) or 1.0
    print(f"\n--fit {name}: VU1 cycles a frame over {len(view)} frames, R^2 {1 - ss_res / ss_tot:.3f}")
    print(f"  {'term':<10}{'cycles each':>12}{'mean count':>12}{'us a frame':>12}")
    for t, c in zip(terms, coef):
        mean_x = 1.0 if t == 'const' else statistics.mean(r[t] for r in view)
        print(f"  {t:<10}{c:>12.1f}{mean_x:>12.1f}{c * mean_x / VU1_CYCLES_PER_US:>12.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--fit', nargs='?', const='serial', default=None, metavar='SEGMENT')
    ap.add_argument('--pixels', type=int, default=640 * 448)
    opts = ap.parse_args()
    fit_seg, pixels = opts.fit, opts.pixels
    rows, seg_names = load(opts.capture)

    results = {}
    for name in dict.fromkeys(seg_names):
        results[name] = report_segment(name, [r for r in rows if r['seg'] == name])

    serial, nofill = results.get('serial'), results.get('serial-nofill')
    if serial and nofill:
        fill = serial['total'] - nofill['total']
        print(f"\nfill (serial - serial-nofill): {fill:.0f} us a frame, "
              f"{1000 * fill / pixels:.2f} ns a screen pixel at {pixels} pixels")
        for c in GPU:
            print(f"  {c:<10}{serial['split'][c] - nofill['split'][c]:>10.0f} us")

    if fit_seg:
        fit(rows, fit_seg)


if __name__ == '__main__':
    main()
