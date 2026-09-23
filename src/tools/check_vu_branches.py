#!/usr/bin/env python3
"""Prove every VU branch in an assembled microprogram lands on its label.

A VU branch carries an 11-bit signed offset, -1024 .. 1023 instructions, and a
microprogram past about a thousand instructions has branches that reach further
- textured_triangles' main loop is over 1600 long, so its closing branch is one.
dvp-as warns 'operand out of range' and keeps the low 11 bits. That happens to
be right: VU1 micro memory is exactly 2048 instructions and the program counter
wraps, so an offset taken modulo 2048 reaches the same instruction from
anywhere. It is right only as long as both of those hold, though, and a warning
that is expected is a warning nobody reads.

So this decodes the object dvp-as produced, resolves each branch's target the
way the VU does - (pc + 1 + imm11) mod 2048 - and checks it against where the
.vsm puts the label. A toolchain that clamped rather than truncated, or a branch
the offset cannot express, fails the build here instead of on screen.

Usage: check_vu_branches.py <program.vsm> <program.o>
"""
import re, struct, sys

MICRO_MEM_INSTRUCTIONS = 2048

# Lower-instruction opcodes (bits 31..25) that take an imm11 branch offset.
BRANCH_OPS = {0x20: 'b', 0x21: 'bal', 0x28: 'ibeq', 0x29: 'ibne',
              0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez'}

LABEL  = re.compile(r'^([A-Za-z_][\w.]*):\s*$')
INSTR  = re.compile(r'^\s+[a-z]')
BRANCH = re.compile(r'\b(b|bal|ibeq|ibne|ibltz|ibgtz|iblez|ibgez)\s+(?:VI\d+\s*,\s*){0,2}(\w+)\s*$')

def vutext(path):
    """The raw .vutext section of a dvp-as ELF object (dvp-objcopy will not read them)."""
    data = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    sections = [struct.unpack_from('<IIIIII', data, shoff + k * shentsize) for k in range(shnum)]
    strtab = sections[shstrndx][4]
    for name, _type, _flags, _addr, offset, size in sections:
        if data[strtab + name:data.index(b'\0', strtab + name)] == b'.vutext':
            return data[offset:offset + size]
    sys.exit(f"FAIL {path}: no .vutext section")

def main(vsm, obj):
    labels, expected, count = {}, {}, 0
    for line in open(vsm):
        stripped = line.strip()
        if m := LABEL.match(stripped):
            labels[m.group(1)] = count
        elif INSTR.match(line):
            if m := BRANCH.search(line):
                expected[count] = m.group(2)
            count += 1

    code = vutext(obj)
    name = vsm.split('/')[-1]
    bad, far, seen = [], 0, 0
    for pc in range(len(code) // 8):
        lower, = struct.unpack_from('<I', code, pc * 8)
        if (lower >> 25) not in BRANCH_OPS:
            continue
        seen += 1
        imm = lower & 0x7FF
        imm = imm - 0x800 if imm & 0x400 else imm
        lands = (pc + 1 + imm) % MICRO_MEM_INSTRUCTIONS
        label = expected.get(pc)
        want = labels.get(label)
        if want is None or lands != want:
            bad.append(f"instruction {pc}: {BRANCH_OPS[lower >> 25]} {label} lands on {lands}, label is at {want}")
        elif not -1024 <= want - (pc + 1) <= 1023:
            far += 1

    if seen != len(expected):
        bad.append(f"{seen} branches in the object, {len(expected)} in the .vsm - the decoder is out of step")
    for b in bad:
        print(f"FAIL {name}: {b}")
    if not bad:
        print(f"ok   {name} ({seen} branches, {far} past +/-1024 and landing by wraparound)")
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
