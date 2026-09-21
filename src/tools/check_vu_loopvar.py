#!/usr/bin/env python3
"""Catch openvcl clobbering a microprogram's loop counter.

openvcl allocates VI registers by liveness, and on control flow more involved
than a single counted loop it gets that analysis wrong: it will hand the outer
loop's counter to a temporary inside a nested loop, silently. The program then
never terminates and the GIF waits forever on a packet that never completes.

The counter register is whatever the loop's closing branch tests. It should be
written exactly twice - once where it is initialised, once where it steps - so
anything else writing it is a miscompile.
"""
import re, sys, glob

WRITE = re.compile(r'\b(?:iaddiu|iaddi|iadd|isub|isubiu|iand|ior|ilw|ilw\.\w|mtir)\s+(VI\d+)\s*,')
BRANCH = re.compile(r'\bibne\s+(VI\d+),\s*VI00,\s*(\w+)')

def check(path):
    lines = open(path).read().split('\n')
    labels = {l.strip().rstrip(':'): i for i, l in enumerate(lines)
              if re.match(r'^\w+:\s*$', l.strip())}
    bad = []
    for i, line in enumerate(lines):
        m = BRANCH.search(line)
        if not m:
            continue
        reg, label = m.group(1), m.group(2)
        if label not in labels or labels[label] > i:
            continue                       # forward branch: not a loop
        body = range(labels[label], i + 1)
        writes = [j for j, l in enumerate(lines) if (w := WRITE.search(l)) and w.group(1) == reg]
        inside = [j for j in writes if j in body]
        if len(inside) != 1:
            bad.append((label, reg, [j + 1 for j in inside]))
    return bad

rc = 0
for path in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else 'build/vu/*.vsm')):
    bad = check(path)
    name = path.split('/')[-1]
    if bad:
        rc = 1
        for label, reg, at in bad:
            print(f"FAIL {name}: loop '{label}' counter {reg} written "
                  f"{len(at)} times inside the loop (lines {at}) - expected exactly 1")
    else:
        print(f"ok   {name}")
sys.exit(rc)
