#!/usr/bin/env python3
"""Catch values openvcl will lose between two sibling loops.

openvcl's liveness does not carry an integer register from one loop into a
later, disjoint loop. It decides the value is dead at the end of the first
loop and hands the register to a temporary inside the second - silently, with
no diagnostic. The program then reads garbage.

So: flag any symbolic integer register written inside one loop body and read
inside a *different* one without being written there first. Such a value must
be parked in VU memory across the boundary instead (see kClipSpill).

Runs on the vclpp output, where registers still have their source names.
"""
import re, sys, glob

WRITE = re.compile(r'^\s*(?:iaddiu|iaddi|iadd|isub|isubiu|iand|ior|ilw|ilw\.\w|mtir|xtop)\s+(i\w+)\s*,')
NAME  = re.compile(r'\b(i[A-Z]\w*)\b')
LABEL = re.compile(r'^\s*(\w+):\s*$')
BACK  = re.compile(r'^\s*(?:b|ib\w+)\s+.*?\b(\w+)\s*$')

def loops(lines):
    """(start, end) for each label with a backward branch to it."""
    labels = {}
    for i, l in enumerate(lines):
        m = LABEL.match(l)
        if m: labels[m.group(1)] = i
    out = []
    for i, l in enumerate(lines):
        m = BACK.match(l)
        if m and m.group(1) in labels and labels[m.group(1)] < i:
            out.append((labels[m.group(1)], i))
    return out

def check(path):
    lines = open(path).read().split('\n')
    bodies = loops(lines)
    bad = []
    for a, (sa, ea) in enumerate(bodies):
        for b, (sb, eb) in enumerate(bodies):
            if a == b or not (eb < sa or ea < sb):
                continue                         # same loop, or nested
            if sb < sa:
                continue                         # only look forwards
            wrote_a = {m.group(1) for l in lines[sa:ea+1] if (m := WRITE.match(l))}
            wrote_b = {m.group(1) for l in lines[sb:eb+1] if (m := WRITE.match(l))}
            read_b  = set()
            for l in lines[sb:eb+1]:
                m = WRITE.match(l)
                names = NAME.findall(l)
                read_b |= set(names[1:]) if m else set(names)
            for reg in sorted((wrote_a & read_b) - wrote_b):
                bad.append((reg, lines[sa].strip().rstrip(':'), lines[sb].strip().rstrip(':')))
    return bad

rc = 0
for path in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else 'build/vu/*.pp.vcl')):
    bad = check(path)
    name = path.split('/')[-1].replace('.pp.vcl', '')
    for reg, la, lb in bad:
        rc = 1
        print(f"FAIL {name}: '{reg}' is set in loop '{la}' and read in loop '{lb}' "
              f"without being set there - openvcl will not keep it; park it in VU memory")
    if not bad:
        print(f"ok   {name}")
sys.exit(rc)
