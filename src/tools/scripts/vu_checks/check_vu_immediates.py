#!/usr/bin/env python3
"""Catch a VU immediate that does not fit its instruction's field.

dvp-as truncates an out-of-range immediate to the field width and says nothing:
'iaddi VI03, VI10, -18' assembles to exactly the same word as '+14', because
IADDI's immediate is five bits signed. Nothing upstream complains either -
vclpp substitutes text and openvcl schedules registers, and neither owns the
encoding - so the first sign of it is on screen.

That one cost a window room check. 'iRoom = iVertsLeft - 18' became
'iVertsLeft + 14', which is never negative, so the output window never closed
for room, the vertex count ran past its end, and the GIF was handed an NLOOP
that walked it out of the packet.

Field widths, from the VU instruction set:

  iaddi                 5 bits signed      -16 .. 15
  iaddiu, isubiu       15 bits unsigned      0 .. 32767
  lq/sq/ilw/isw/...    11 bits signed    -1024 .. 1023  (and VU1 memory is
                                                         1024 qwords, so the
                                                         top address is 1023)
"""
import re, sys, glob

IADDI  = re.compile(r'\bi(addi)\s+VI\d+\s*,\s*VI\d+\s*,\s*(-?\d+)')
IADDIU = re.compile(r'\bi(addiu|subiu)\s+VI\d+\s*,\s*VI\d+\s*,\s*(-?\d+)')
MEMOFF = re.compile(r'\b(lq|sq|ilw|isw)(?:\.\w+)?(?:i)?\s+V[FI]\d+\s*,\s*(-?\d+)\s*\(')

LIMITS = {'addi': (-16, 15), 'addiu': (0, 32767), 'subiu': (0, 32767),
          'lq': (-1024, 1023), 'sq': (-1024, 1023),
          'ilw': (-1024, 1023), 'isw': (-1024, 1023)}

def check(path):
    bad = []
    for i, line in enumerate(open(path).read().split('\n'), 1):
        for rx in (IADDI, IADDIU, MEMOFF):
            m = rx.search(line)
            if not m:
                continue
            op, imm = m.group(1), int(m.group(2))
            lo, hi = LIMITS[op]
            if not (lo <= imm <= hi):
                bad.append((i, line.strip(), op, imm, lo, hi))
    return bad

rc = 0
for path in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else 'build/vu/*.vsm')):
    bad = check(path)
    name = path.split('/')[-1]
    if bad:
        rc = 1
        for i, text, op, imm, lo, hi in bad:
            print(f"FAIL {name}:{i}: {op} immediate {imm} is outside {lo}..{hi} "
                  f"and will be silently truncated - {text}")
    else:
        print(f"ok   {name}")
sys.exit(rc)
