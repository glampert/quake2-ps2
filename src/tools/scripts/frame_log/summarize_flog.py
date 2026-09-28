#!/usr/bin/env python3
"""Extract the frame log from a PCSX2 emulog and summarise it.

A debug build prints one FLOG row per frame (ps2::debug::FrameLogCapture, see
src/ps2/renderer/profile.cpp): the profiler events in microseconds and the draw
counters, under a FLOG#hdr naming the columns. FLOG#map marks a level load,
FLOG#open names a file opened in that row and FLOG#end closes a complete run.
The perf run (ps2_perftest 1) plays the demos with the log on and quits, and
the rows end up in PCSX2's logs/emulog.txt (with EnableIOPConsole on).

The game's stdout interleaves without newlines, so rows can be embedded mid-line
(e.g. "[Q2] Map: base2FLOG#map,12,base2"); this pulls them out by position rather
than anchoring at the start of a line.

--rows writes the clean rows to a .flog file, which is what compare_flog.py
takes and what is worth keeping as a baseline: the emulog is overwritten by
the next PCSX2 run.

Usage: summarize_flog.py <emulog.txt> [--rows out.flog]
"""
import re, sys, statistics

def extract(path):
    text = open(path, errors='replace').read()
    hdr, rows, maps, opens, end = None, [], [], [], None
    for m in re.finditer(r'FLOG(#\w+)?,', text):
        start = m.start()
        line_end = text.find('\n', start)
        chunk = text[start:line_end if line_end != -1 else len(text)]
        # A row ends where the next embedded FLOG or a non-row escape begins.
        nxt = chunk.find('FLOG', 4)
        if nxt != -1:
            chunk = chunk[:nxt]
        chunk = chunk.split('\x1b')[0].rstrip()
        kind = m.group(1)
        if kind == '#hdr':   hdr = chunk
        elif kind == '#map': maps.append(chunk)
        elif kind == '#end': end = chunk
        elif kind == '#open': opens.append(chunk)
        elif kind is None:   rows.append(chunk)
    return hdr, rows, maps, opens, end

def main():
    path = sys.argv[1]
    out = None
    if '--rows' in sys.argv:
        out = sys.argv[sys.argv.index('--rows') + 1]

    hdr, rows, maps, opens, end = extract(path)
    if out:
        with open(out, 'w') as f:
            if hdr: f.write(hdr + '\n')
            for r in maps: f.write(r + '\n')
            for r in rows: f.write(r + '\n')
            for r in opens: f.write(r + '\n')
            if end: f.write(end + '\n')

    cols = hdr.split(',')[1:] if hdr else []
    data = {c: [] for c in cols}
    bad = 0
    for r in rows:
        f = r.split(',')[1:]
        if len(f) != len(cols):
            bad += 1
            continue
        for c, v in zip(cols, f):
            try: data[c].append(float(v))
            except ValueError: pass

    print(f"frames: {len(rows)}  malformed: {bad}  maps: {len(maps)}  opens: {len(opens)}  end: {end}")
    print(f"{'column':<16}{'mean':>12}{'p50':>10}{'p95':>10}{'max':>10}")
    def pct(v, q):
        v = sorted(v); return v[min(len(v) - 1, int(len(v) * q))]
    for c in cols:
        v = data[c]
        if not v or c == 'frame': continue
        print(f"{c:<16}{statistics.mean(v):>12.1f}{pct(v,0.50):>10.1f}"
              f"{pct(v,0.95):>10.1f}{max(v):>10.1f}")

if __name__ == '__main__':
    main()
