#!/usr/bin/env python3
"""Break one frame-log capture down against the 60 fps budget.

Where compare_flog.py tells two captures apart, this reads one and answers which
frames miss vsync and what they were doing. Takes a raw PCSX2 emulog or a .flog
written by summarize_flog.py --rows.

A frame's EE work is Frame - VSync. The vsync spin sits in the middle of the Frame
scope, so Frame alone jitters by however much the post-flip tail (sound, dlight and
lightstyle ticks) moves between frames; EE work is the stable measure. A frame whose
work passes one field (16683 us) waits for the next one, and Frame lands near 33.4 ms:
that is a dropped frame.

SV_Frame and CL_ReadPackets run before PS2_BeginFrame rolls the profiler over, so the
Server and ClParse columns land one row early (see src/ps2/debug/engine_profile.h).
They are shifted back here before anything is added up.

Prints, in order:
  - dropped frames, frames over budget and frames with no margin left, for steady frames
    and for the first 30 frames of each map (loading work) apart
  - EE work percentiles and a 1 ms histogram
  - the same per map
  - every column's mean over the over-budget frames against all steady frames
  - runs of consecutive over-budget frames, and the 15 worst frames broken down
  - every FLOG#open note: the file, the ClParse and FsIo of the row it was charged to,
    and the EE work of that row and the next, which is what a mid-level load costs

Usage: frame_budget.py <emulog.txt|capture.flog>
"""
import sys, statistics, collections

# summarize_flog lives beside this; keep the import from leaving a __pycache__
# in src/tools.
sys.dont_write_bytecode = True
from summarize_flog import extract

FIELD = 16683

def load(path):
    hdr, lines, map_lines, open_lines, _ = extract(path)
    if hdr is None:
        sys.exit(f"{path}: no FLOG#hdr - not a frame-log capture")
    cols = hdr.split(',')[1:]
    rows = []
    for line in lines:
        v = line.split(',')[1:]
        if len(v) == len(cols):
            rows.append({c: int(x) for c, x in zip(cols, v)})
    maps = []
    for line in map_lines:
        _, f, name = line.split(',', 2)
        maps.append((int(f), name))
    opens = []
    for line in open_lines:
        _, f, name = line.split(',', 2)
        opens.append((int(f), name))
    return rows, maps, opens

def map_of(frame, maps):
    name = '?'
    for f, n in maps:
        if frame >= f:
            name = n
    return name

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))] if v else 0

def report_budget(rows, maps):
    # Shift Server and ClParse back to the row whose Frame holds their time, so a row's
    # columns add up to its own Frame.
    prev = {}
    for r in rows:
        for c in ('Server', 'ClParse'):
            if c in r:
                r[c], prev[c] = prev.get(c, 0), r[c]
    for r in rows:
        r['ee'] = r['Frame'] - r['VSync']
        r['map'] = map_of(r['frame'], maps)
        # Sound nests inside SndMix when the engine probes exist; older logs lack them.
        eng = sum(r.get(c, 0) for c in ('Server', 'ClParse', 'ClScene'))
        snd = r['SndMix'] if 'SndMix' in r else r['Sound']
        r['rest'] = r['ee'] - r['View'] - r['Ui'] - r['Overlay'] - snd - eng

    view = [r for r in rows if r['View'] > 0]
    noview = len(rows) - len(view)
    # The first frames of a map are loading work (precache, first-touch VRAM uploads,
    # lightmap builds), not steady-state rendering - reported apart.
    settle_ids = set()
    for f, _ in maps:
        first = [r['frame'] for r in view if r['frame'] >= f][:30]
        settle_ids.update(first)
    steady = [r for r in view if r['frame'] not in settle_ids]
    settle = [r for r in view if r['frame'] in settle_ids]

    def classify(rs):
        dropped = [r for r in rs if r['Frame'] > FIELD * 1.5]
        over = [r for r in rs if r['ee'] > FIELD and r['Frame'] <= FIELD * 1.5]
        tight = [r for r in rs if 15000 < r['ee'] <= FIELD and r['Frame'] <= FIELD * 1.5]
        return dropped, over, tight

    print(f"rows {len(rows)}  with a 3D view {len(view)}  without (console/loading) {noview}")
    print(f"  settling (first 30 frames of a map) {len(settle)}  steady {len(steady)}")
    for label, rs in (('steady', steady), ('settling', settle)):
        d, o, t = classify(rs)
        print(f"\n[{label}] {len(rs)} frames")
        print(f"  dropped (Frame > 25 ms):            {len(d):5d}  {100*len(d)/max(1,len(rs)):5.2f}%")
        print(f"  EE work > 16.68 ms, not dropped:    {len(o):5d}  {100*len(o)/max(1,len(rs)):5.2f}%")
        print(f"  EE work 15.0-16.68 ms (no margin):  {len(t):5d}  {100*len(t)/max(1,len(rs)):5.2f}%")
        ee = [r['ee'] for r in rs]
        if ee:
            print(f"  EE work  mean {statistics.mean(ee):7.0f}  p50 {pct(ee,.5):6d}  p90 {pct(ee,.9):6d}"
                  f"  p95 {pct(ee,.95):6d}  p99 {pct(ee,.99):6d}  max {max(ee):6d}")

    if not steady:
        return

    # Histogram of EE work, steady frames.
    print("\nEE work histogram, steady frames (1 ms buckets):")
    h = collections.Counter(min(r['ee'] // 1000, 40) for r in steady)
    for k in sorted(h):
        bar = '#' * max(1, h[k] * 60 // max(h.values()))
        print(f"  {k:2d}{'+' if k == 40 else ' '}ms {h[k]:5d} {bar}")

    # Per map.
    print("\nPer map (steady frames):")
    print(f"  {'map':<10}{'frames':>7}{'dropped':>9}{'ee>16.7':>9}{'ee>15':>7}{'ee p50':>8}{'ee p95':>8}{'ee max':>8}")
    for _, name in maps:
        rs = [r for r in steady if r['map'] == name]
        if not rs:
            continue
        d, o, t = classify(rs)
        ee = [r['ee'] for r in rs]
        print(f"  {name:<10}{len(rs):7d}{len(d):9d}{len(o):9d}{len(t):7d}{pct(ee,.5):8d}{pct(ee,.95):8d}{max(ee):8d}")

    # Where the time goes: over-budget steady frames vs the average steady frame.
    d, o, t = classify(steady)
    bad = d + o
    cols = ['ee', 'View', 'World', 'TexChains', 'LmChains', 'LmChain', 'BspWalk', 'MarkLeaves',
            'Entities', 'EntGeom', 'EntShadow', 'EntBrush', 'EntShade', 'Particles', 'AlphaSurfs',
            'TurbSurfs', 'Sky', 'Ui', 'Overlay', 'Sound', 'SndMix', 'Server', 'ClParse', 'ClScene',
            'GsWait', 'DmaSend', 'DmaFlush', 'rest',
            'tris', 'batches', 'entities', 'particles', 'dlights', 'lmDynamic', 'lmStyle',
            'vramUploads', 'vramOomSyncs', 'chainKB', 'chainDrains', 'surfs', 'nodes']
    print(f"\nOver-budget steady frames ({len(bad)}) vs all steady frames, means:")
    print(f"  {'column':<13}{'all':>9}{'over':>9}{'delta':>9}")
    for c in [c for c in cols if c in steady[0]]:
        a = statistics.mean(r[c] for r in steady)
        b = statistics.mean(r[c] for r in bad) if bad else 0
        print(f"  {c:<13}{a:9.0f}{b:9.0f}{b-a:+9.0f}")

    # Streaks: consecutive over-budget frames, so one-off spikes and sustained heavy scenes
    # can be told apart.
    badset = {r['frame'] for r in bad}
    streaks, cur = [], []
    for r in steady:
        if r['frame'] in badset and (not cur or r['frame'] == cur[-1]['frame'] + 1):
            cur.append(r)
        else:
            if cur: streaks.append(cur)
            cur = [r] if r['frame'] in badset else []
    if cur: streaks.append(cur)
    lens = collections.Counter(min(len(s), 10) for s in streaks)
    print(f"\nStreaks of over-budget frames: {len(streaks)}")
    for k in sorted(lens):
        print(f"  length {k}{'+' if k == 10 else ' '}: {lens[k]}")
    print("\nLongest streaks:")
    for s in sorted(streaks, key=len, reverse=True)[:12]:
        ee = [r['ee'] for r in s]
        print(f"  {s[0]['map']:<8} frames {s[0]['frame']:5d}-{s[-1]['frame']:5d} ({len(s):3d})"
              f"  ee mean {statistics.mean(ee):6.0f} max {max(ee):6d}"
              f"  View {statistics.mean(r['View'] for r in s):6.0f}"
              f"  Ent {statistics.mean(r['Entities'] for r in s):5.0f}"
              f"  World {statistics.mean(r['World'] for r in s):5.0f}"
              f"  Mix {statistics.mean(r.get('SndMix', 0) for r in s):5.0f}"
              f"  Sv {statistics.mean(r.get('Server', 0) for r in s):5.0f}"
              f"  Parse {statistics.mean(r.get('ClParse', 0) for r in s):5.0f}"
              f"  Scene {statistics.mean(r.get('ClScene', 0) for r in s):5.0f}"
              f"  rest {statistics.mean(r['rest'] for r in s):5.0f}"
              f"  tris {statistics.mean(r['tris'] for r in s):5.0f}")

    print("\nWorst 15 steady frames by EE work:")
    for r in sorted(steady, key=lambda r: r['ee'], reverse=True)[:15]:
        print(f"  {r['map']:<8} #{r['frame']:5d} Frame {r['Frame']:6d} ee {r['ee']:6d} View {r['View']:5d}"
              f" World {r['World']:5d} Ent {r['Entities']:5d} Part {r['Particles']:4d} Ui {r['Ui']:4d}"
              f" Mix {r.get('SndMix', 0):5d} Sv {r.get('Server', 0):5d} Parse {r.get('ClParse', 0):5d}"
              f" Scene {r.get('ClScene', 0):5d} rest {r['rest']:5d} vramUp {r['vramUploads']} oom {r['vramOomSyncs']}"
              f" lmDyn {r['lmDynamic']} dl {r['dlights']}")

def report_opens(rows, opens):
    # Takes the rows as logged, not shifted: FLOG#open names the row charged with the read,
    # and a load in CL_ReadPackets/SV_Frame is charged to it with the ClParse and FsIo it
    # caused, while the time itself stretches the Frame of the row after. So show both.
    if not opens:
        return
    by = {r['frame']: r for r in rows}
    print(f"\nFiles opened ({len(opens)} notes): the charged row's ClParse and FsIo, and EE work of it and the row after:")
    for f, name in opens:
        a, b = by.get(f), by.get(f + 1)
        ee = lambda r: f"{r['Frame'] - r['VSync']:6d}" if r else "     -"
        col = lambda r, c: f"{r.get(c, 0):6d}" if r else "     -"
        print(f"  row {f:5d}  parse {col(a, 'ClParse')}  fs {col(a, 'FsIo')}  ee {ee(a)} / {ee(b)} us  {name}")

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.rstrip())
    rows, maps, opens = load(sys.argv[1])
    logged = [dict(r) for r in rows] # report_budget shifts columns in place
    report_budget(rows, maps)
    report_opens(logged, opens)

if __name__ == '__main__':
    main()
