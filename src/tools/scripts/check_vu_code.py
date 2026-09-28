#!/usr/bin/env python3
"""Check VU1 microprograms for the toolchain's silent failures.

vclpp substitutes text, openvcl allocates registers and schedules, dvp-as
encodes - and none of them owns the whole picture, so each can turn out a
program that builds cleanly and is wrong. Every check here exists because one
of them did, and the symptom reached the screen rather than the build log:

  crossloop   a value openvcl loses between two sibling loops
  regalloc    openvcl's register allocation, proven by reaching definitions
  loopvar     a loop counter openvcl hands to a temporary
  immediates  an immediate dvp-as truncates to its field width
  latency     a clip flag or Q read openvcl did not pad across a branch
  branches    a branch offset dvp-as wrapped, decoded from the object

A program is named by its object, and the checks read the build products the
Makefile's VU rule leaves beside it:

  <name>.pp.vcl   vclpp output, registers still under their source names
  <name>.vsm      openvcl output, the program dvp-as assembles
  <name>.c.vsm    openvcl -c output, the same with source lines as comments
  <name>.o        dvp-as output

Every check runs on every program, so one build reports every problem.

Usage: check_vu_code.py [program.o ...]    (default: every build/vu/*.o)
"""
import glob, os, re, struct, sys

# ---------------------------------------------------------------------------
#  The scheduled program: parsing and control flow (regalloc, latency)
# ---------------------------------------------------------------------------

BRANCHES = {'b', 'bal', 'ibeq', 'ibne', 'ibltz', 'ibgtz', 'iblez', 'ibgez', 'jr', 'jalr'}
UNCONDITIONAL = {'b', 'bal', 'jr', 'jalr'}

SOURCE = re.compile(r'^\s*; Line \d+:\s*(.*)$')
LABEL  = re.compile(r'^([A-Za-z_][\w.]*):\s*$')
REG    = re.compile(r'\b(VI\d\d|VF\d\d)([xyzw]?)\b')

def split_op(text):
    """'maddw.xyzw VF09, VF04, VF00w' -> ('maddw', 'xyzw', ['VF09', 'VF04', 'VF00w'])."""
    head, _, rest = text.partition(' ')
    op, _, mask = head.partition('.')
    ops = [t.strip() for t in rest.split(',')] if rest.strip() else []
    return op.lower().replace('[e]', ''), mask.lower().replace('[e]', ''), ops

def parse(path):
    """Instruction pairs, in order: (labels, source comments, upper, lower, has E bit)."""
    pairs, labels, comments = [], [], []
    for line in open(path):
        if m := SOURCE.match(line):
            comments.append(m.group(1).strip())
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith(('.', ';')):
            continue
        if m := LABEL.match(stripped):
            labels.append(m.group(1))
            continue
        halves = re.split(r'\s{2,}', stripped)
        if len(halves) < 2:
            continue
        ebit = '[E]' in stripped
        pairs.append((labels, comments, halves[0].replace('[E]', ''), halves[1].replace('[E]', ''), ebit))
        labels, comments = [], []
    return pairs

def successors(pairs):
    """Each pair's successors, delay slots included: the pair after a branch runs before it
    lands, so it is the delay slot whose successors are the branch's targets."""
    labels = {lab: i for i, p in enumerate(pairs) for lab in p[0]}
    n = len(pairs)
    succ = [[i + 1] if i + 1 < n else [] for i in range(n)]
    for i, (_, _, _, lower, ebit) in enumerate(pairs):
        op, _, ops = split_op(lower)
        if ebit and i + 1 < n:
            succ[i + 1] = []
        if op in BRANCHES and i + 1 < n:
            target = labels.get(ops[-1]) if ops else None
            after = [target] if target is not None else []
            if op not in UNCONDITIONAL and i + 2 < n:
                after.append(i + 2)
            succ[i + 1] = after
    return succ

def predecessors(succ):
    preds = [[] for _ in range(len(succ))]
    for i, targets in enumerate(succ):
        for s in targets:
            preds[s].append(i)
    return preds

# ---------------------------------------------------------------------------
#  crossloop
# ---------------------------------------------------------------------------

CROSSLOOP_WRITE = re.compile(r'^\s*(?:iaddiu|iaddi|iadd|isub|isubiu|iand|ior|ilw|ilw\.\w|mtir|xtop)\s+(i\w+)\s*,')
CROSSLOOP_NAME  = re.compile(r'\b(i[A-Z]\w*)\b')
CROSSLOOP_LABEL = re.compile(r'^\s*(\w+):\s*$')
CROSSLOOP_BACK  = re.compile(r'^\s*(?:b|ib\w+)\s+.*?\b(\w+)\s*$')

def source_loops(lines):
    """(start, end) for each label with a backward branch to it."""
    labels = {}
    for i, l in enumerate(lines):
        m = CROSSLOOP_LABEL.match(l)
        if m: labels[m.group(1)] = i
    out = []
    for i, l in enumerate(lines):
        m = CROSSLOOP_BACK.match(l)
        if m and m.group(1) in labels and labels[m.group(1)] < i:
            out.append((labels[m.group(1)], i))
    return out

def check_crossloop(prog):
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
    lines = open(prog.pp_vcl).read().split('\n')
    bodies = source_loops(lines)
    bad = []
    for a, (sa, ea) in enumerate(bodies):
        for b, (sb, eb) in enumerate(bodies):
            if a == b or not (eb < sa or ea < sb):
                continue                         # same loop, or nested
            if sb < sa:
                continue                         # only look forwards
            wrote_a = {m.group(1) for l in lines[sa:ea+1] if (m := CROSSLOOP_WRITE.match(l))}
            wrote_b = {m.group(1) for l in lines[sb:eb+1] if (m := CROSSLOOP_WRITE.match(l))}
            read_b  = set()
            for l in lines[sb:eb+1]:
                m = CROSSLOOP_WRITE.match(l)
                names = CROSSLOOP_NAME.findall(l)
                read_b |= set(names[1:]) if m else set(names)
            for reg in sorted((wrote_a & read_b) - wrote_b):
                bad.append(f"'{reg}' is set in loop '{lines[sa].strip().rstrip(':')}' and read in loop "
                           f"'{lines[sb].strip().rstrip(':')}' without being set there - openvcl will not "
                           f"keep it; park it in VU memory")
    return bad, None

# ---------------------------------------------------------------------------
#  regalloc
# ---------------------------------------------------------------------------

def same_instructions(a, b):
    """The two programs are one instruction stream once whole-nop pairs are dropped."""
    def stream(pairs):
        return [(u, l) for _, _, u, l, _ in pairs if not (u.startswith('nop') and l.startswith('nop'))]
    return stream(a) == stream(b)

def operands(text):
    """(reads, writes) of one instruction: lists of (register, lanes string); plus the write mask."""
    op, mask, ops = split_op(text)
    if op == 'nop' or not op:
        return [], [], ''
    mask = mask or 'xyzw'

    def reg(tok, default_lanes):
        m = REG.search(tok)
        if not m:
            return None
        name, bc = m.group(1), m.group(2)
        lanes = bc if bc else (default_lanes if name.startswith('VF') else 'x')
        return (name, lanes)

    reads, writes = [], []
    if op in BRANCHES or op in ('xgkick', 'isw', 'iswr', 'sq', 'sqd'):
        for t in ops:
            r = reg(t, mask)
            if r and r[0] not in ('VI00', 'VF00'):
                reads.append(r)
    elif op in ('sqi', 'lqi'):
        # Post-increment: the pointer is read, and stepped - which carries its value on
        # rather than writing a new one, so it is not a write for this check's purposes.
        for t in ops:
            r = reg(t, mask)
            if r and r[0].startswith('VI'):
                reads.append(r)
            elif r and r[0] != 'VF00':
                (reads if op == 'sqi' else writes).append(r)
    elif op == 'fcand' or op in ('fcor', 'fcget', 'fmand', 'fmor', 'fsand', 'fsor'):
        writes.append(('VI01', 'x'))
    elif op in ('div', 'sqrt', 'rsqrt', 'clipw') or ops[:1] == ['ACC']:
        for t in ops:
            r = reg(t, mask)
            if r and r[0] not in ('VI00', 'VF00'):
                reads.append(r)
    else:
        # First operand written, the rest read. A memory operand's base is a read.
        if ops:
            d = reg(ops[0], mask)
            if d and d[0] not in ('VI00', 'VF00'):
                writes.append((d[0], mask if d[0].startswith('VF') else 'x'))
            for t in ops[1:]:
                r = reg(t, mask)
                if r and r[0] not in ('VI00', 'VF00'):
                    reads.append(r)
    return reads, writes, mask

def source_name(comments, text):
    """The destination name the source line for 'text' gives, if one of the comments is it."""
    op, _, ops = split_op(text)
    # openvcl moves a 'move' to the upper pipe as a max of a register with itself.
    candidates = [op]
    if op in ('max', 'mini') and len(ops) == 3 and ops[1] == ops[2]:
        candidates.append('move')
    for c in comments:
        sop, _, sops = split_op(c)
        for cand in candidates:
            if sop == cand or (cand.startswith(sop) and cand[len(sop):] in ('x', 'y', 'z', 'w', 'i', 'q')):
                if sops:
                    return sops[0].split('[')[0].strip()
    return None

def check_regalloc(prog):
    """Check openvcl's register allocation by reaching definitions.

    openvcl allocates registers by source-order interval and gets loops wrong: a
    value whose last write sits mid-loop looks dead from there to the bottom, even
    though the loop's back edge reads it again, and its register goes to whatever
    wants one next. textured_triangles lost its input pointer that way - to the
    backface cull's verdict, the ADC word and the clipper's walk pointer - and the
    program built cleanly with every other check passing. The crossloop and
    loopvar checks each guard one shape of this (a value carried between sibling
    loops, a loop counter); this one guards the property itself.

    In a correct allocation every read of a register is reached only by writes of
    one source variable. If writes of two different variables reach the same read,
    then on some path one of them overwrote the other while it was still live. So:

      1. openvcl -c emits the scheduled program with each instruction's source line
         as a comment, which names what every write writes;
      2. the program's control flow is rebuilt from the scheduled code, branch delay
         slots included;
      3. reaching definitions run per physical register - per lane, for VF, since a
         masked write only replaces the lanes it names;
      4. any read reached by writes of two different names fails.

    -c has to be a separate openvcl run, since it schedules a few nops differently.
    So the -c program is first compared with the real one, instruction for
    instruction with whole-nop pairs dropped; only if they match does a clean result
    here say anything about the code that ships.

    Blind spots, deliberately accepted: two values that share a *name* - the same
    macro expanded twice, or a loop's own counter - are one variable to this check
    (the crossloop check covers the sibling-loop case); and a write -c leaves
    uncommented, which is a few percent of them, cannot be named, so a read it
    reaches is checked against the named writes alone.
    """
    pairs = parse(prog.c_vsm)
    if not same_instructions(pairs, parse(prog.vsm)):
        return ["openvcl -c scheduled it differently beyond nops - cannot verify"], None

    n = len(pairs)
    succ = successors(pairs)
    preds = predecessors(succ)

    # Definitions: (pair, register, lane) -> name. A self-update with no source line
    # (iCount += 1 in a delay slot) carries its value on rather than starting one.
    gen = [dict() for _ in range(n)]
    uses = [[] for _ in range(n)]
    unnamed = 0
    for i, (_, comments, upper, lower, _) in enumerate(pairs):
        for text in (upper, lower):
            reads, writes, _ = operands(text)
            uses[i] += reads
            for reg_, lanes in writes:
                label = 'vi01' if reg_ == 'VI01' else source_name(comments, text)
                if label is None:
                    if any(r == reg_ for r, _ in reads):
                        continue
                    unnamed += 1
                    label = '?'
                for lane in lanes:
                    gen[i][(reg_, lane)] = (i, label)

    # Reaching definitions: per (register, lane), the set of (pair, name) that can arrive.
    reach_in = [dict() for _ in range(n)]
    work = list(range(n))
    out = [dict() for _ in range(n)]
    while work:
        i = work.pop()
        merged = {}
        for p in preds[i]:
            for k, defs in out[p].items():
                merged.setdefault(k, set()).update(defs)
        reach_in[i] = merged
        new_out = {k: set(v) for k, v in merged.items()}
        for k, d in gen[i].items():
            new_out[k] = {d}
        if new_out != out[i]:
            out[i] = new_out
            work.extend(succ[i])

    bad = []
    for i in range(n):
        for reg_, lanes in uses[i]:
            names = set()
            for lane in lanes:
                names |= {nm for _, nm in reach_in[i].get((reg_, lane), ())}
            names.discard('?')
            if len(names) > 1:
                bad.append((i, reg_, sorted(names)))

    fails = []
    for i, reg_, names in bad[:20]:
        _, _, upper, lower, _ = pairs[i]
        fails.append(f"instruction {i} ({lower.split()[0] if not lower.startswith('nop') else upper.split()[0]}) "
                     f"reads {reg_}, which holds any of {', '.join(names)} depending on the path")
    if len(bad) > 20:
        fails.append(f"... and {len(bad) - 20} more")
    return fails, f"{unnamed} writes without a source line"

# ---------------------------------------------------------------------------
#  loopvar
# ---------------------------------------------------------------------------

LOOPVAR_WRITE  = re.compile(r'\b(?:iaddiu|iaddi|iadd|isub|isubiu|iand|ior|ilw|ilw\.\w|mtir)\s+(VI\d+)\s*,')
LOOPVAR_BRANCH = re.compile(r'\bibne\s+(VI\d+),\s*VI00,\s*(\w+)')

def check_loopvar(prog):
    """Catch openvcl clobbering a microprogram's loop counter.

    openvcl allocates VI registers by liveness, and on control flow more involved
    than a single counted loop it gets that analysis wrong: it will hand the outer
    loop's counter to a temporary inside a nested loop, silently. The program then
    never terminates and the GIF waits forever on a packet that never completes.

    The counter register is whatever the loop's closing branch tests. It should be
    written exactly twice - once where it is initialised, once where it steps - so
    anything else writing it is a miscompile.
    """
    lines = open(prog.vsm).read().split('\n')
    labels = {l.strip().rstrip(':'): i for i, l in enumerate(lines)
              if re.match(r'^\w+:\s*$', l.strip())}
    bad = []
    for i, line in enumerate(lines):
        m = LOOPVAR_BRANCH.search(line)
        if not m:
            continue
        reg, label = m.group(1), m.group(2)
        if label not in labels or labels[label] > i:
            continue                       # forward branch: not a loop
        body = range(labels[label], i + 1)
        writes = [j for j, l in enumerate(lines) if (w := LOOPVAR_WRITE.search(l)) and w.group(1) == reg]
        inside = [j + 1 for j in writes if j in body]
        if len(inside) != 1:
            bad.append(f"loop '{label}' counter {reg} written {len(inside)} times inside the loop "
                       f"(lines {inside}) - expected exactly 1")
    return bad, None

# ---------------------------------------------------------------------------
#  immediates
# ---------------------------------------------------------------------------

IADDI  = re.compile(r'\bi(addi)\s+VI\d+\s*,\s*VI\d+\s*,\s*(-?\d+)')
IADDIU = re.compile(r'\bi(addiu|subiu)\s+VI\d+\s*,\s*VI\d+\s*,\s*(-?\d+)')
MEMOFF = re.compile(r'\b(lq|sq|ilw|isw)(?:\.\w+)?(?:i)?\s+V[FI]\d+\s*,\s*(-?\d+)\s*\(')

IMMEDIATE_LIMITS = {'addi': (-16, 15), 'addiu': (0, 32767), 'subiu': (0, 32767),
                    'lq': (-1024, 1023), 'sq': (-1024, 1023),
                    'ilw': (-1024, 1023), 'isw': (-1024, 1023)}

def check_immediates(prog):
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
    bad = []
    for i, line in enumerate(open(prog.vsm).read().split('\n'), 1):
        for rx in (IADDI, IADDIU, MEMOFF):
            m = rx.search(line)
            if not m:
                continue
            op, imm = m.group(1), int(m.group(2))
            lo, hi = IMMEDIATE_LIMITS[op]
            if not (lo <= imm <= hi):
                bad.append(f"{prog.vsm}:{i}: {op} immediate {imm} is outside {lo}..{hi} "
                           f"and will be silently truncated - {line.strip()}")
    return bad, None

# ---------------------------------------------------------------------------
#  latency
# ---------------------------------------------------------------------------

# (what produces it, what reads it, cycles until it lands)
LATENCIES = {
    'clip flags': (lambda op: op == 'clipw',
                   lambda op: op in ('fcand', 'fcor', 'fceq', 'fcget'),
                   4),
    'Q':          (lambda op: op in ('div', 'sqrt', 'rsqrt'),
                   lambda op: re.fullmatch(r'(add|sub|mul|madd|msub|adda|suba|mula|madda|msuba)q', op) is not None,
                   7),
}

def check_latency(prog):
    """Check that no VU read of a pipelined result can land before the result does.

    Two VU results arrive late and are read without an interlock: the clip flags,
    four cycles after a clipw, and the Q register, seven after a div (sqrt, rsqrt).
    Read early, they give the *previous* value, silently. openvcl pads for both -
    but only within a basic block. Across a branch or a label it does not look, so
    a read that is safe in one block becomes unsafe the moment control flow moves
    between it and its producer.

    That is how the lerp merge broke world clipping. The fcand judging a triangle
    moved behind the join of the two vertex-format blocks, two cycles after the last
    corner's clipw, and read flags without that corner in them. A triangle whose
    third corner was behind the eye was judged inside and drawn unclipped, straight
    across the screen - with every other check passing.

    So this rebuilds the scheduled program's control flow (delay slots included,
    see successors()) and, for every read of the clip flags or Q, finds the
    shortest path back to a producer along any route. A waitq on the way satisfies
    a Q read. Anything nearer than the latency fails.
    """
    pairs = parse(prog.vsm)
    n = len(pairs)
    preds = predecessors(successors(pairs))

    ops = [[split_op(upper)[0], split_op(lower)[0]] for _, _, upper, lower, _ in pairs]
    bad = []

    for resource, (produces, reads, latency) in LATENCIES.items():
        for u in range(n):
            if not any(reads(op) for op in ops[u]) or 'waitq' in ops[u]:
                continue
            # Walk back from the reader. A producer in the reader's own pair does not count:
            # the pair reads before it writes.
            nearest, seen, stack = None, {}, [(p, 1) for p in preds[u]]
            while stack:
                i, d = stack.pop()
                if d >= latency or seen.get(i, latency) <= d:
                    continue
                seen[i] = d
                if resource == 'Q' and 'waitq' in ops[i]:
                    continue
                if any(produces(op) for op in ops[i]):
                    nearest = d if nearest is None else min(nearest, d)
                    continue
                stack += [(p, d + 1) for p in preds[i]]
            if nearest is not None:
                bad.append(f"instruction {u} reads the {resource} {nearest} cycle(s) after it is "
                           f"produced on some path; it lands after {latency}")
    return bad, None

# ---------------------------------------------------------------------------
#  branches
# ---------------------------------------------------------------------------

MICRO_MEM_INSTRUCTIONS = 2048

# Lower-instruction opcodes (bits 31..25) that take an imm11 branch offset.
BRANCH_OPCODES = {0x20: 'b', 0x21: 'bal', 0x28: 'ibeq', 0x29: 'ibne',
                  0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez'}

ASM_INSTR  = re.compile(r'^\s+[a-z]')
ASM_BRANCH = re.compile(r'\b(b|bal|ibeq|ibne|ibltz|ibgtz|iblez|ibgez)\s+(?:VI\d+\s*,\s*){0,2}(\w+)\s*$')

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
    return None

def check_branches(prog):
    """Prove every VU branch in the assembled microprogram lands on its label.

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
    """
    labels, expected, count = {}, {}, 0
    for line in open(prog.vsm):
        stripped = line.strip()
        if m := LABEL.match(stripped):
            labels[m.group(1)] = count
        elif ASM_INSTR.match(line):
            if m := ASM_BRANCH.search(line):
                expected[count] = m.group(2)
            count += 1

    code = vutext(prog.obj)
    if code is None:
        return [f"no .vutext section in {prog.obj}"], None
    bad, far, seen = [], 0, 0
    for pc in range(len(code) // 8):
        lower, = struct.unpack_from('<I', code, pc * 8)
        if (lower >> 25) not in BRANCH_OPCODES:
            continue
        seen += 1
        imm = lower & 0x7FF
        imm = imm - 0x800 if imm & 0x400 else imm
        lands = (pc + 1 + imm) % MICRO_MEM_INSTRUCTIONS
        label = expected.get(pc)
        want = labels.get(label)
        if want is None or lands != want:
            bad.append(f"instruction {pc}: {BRANCH_OPCODES[lower >> 25]} {label} lands on {lands}, label is at {want}")
        elif not -1024 <= want - (pc + 1) <= 1023:
            far += 1

    if seen != len(expected):
        bad.append(f"{seen} branches in the object, {len(expected)} in the .vsm - the decoder is out of step")
    return bad, f"{seen} branches, {far} past +/-1024 and landing by wraparound"

# ---------------------------------------------------------------------------
#  Driver
# ---------------------------------------------------------------------------

CHECKS = [
    ('crossloop',  check_crossloop),
    ('regalloc',   check_regalloc),
    ('loopvar',    check_loopvar),
    ('immediates', check_immediates),
    ('latency',    check_latency),
    ('branches',   check_branches),
]

class Program:
    """One microprogram's build products, found beside its object."""
    def __init__(self, obj):
        stem = obj[:-len('.o')] if obj.endswith('.o') else obj
        self.name   = os.path.basename(stem)
        self.pp_vcl = stem + '.pp.vcl'
        self.vsm    = stem + '.vsm'
        self.c_vsm  = stem + '.c.vsm'
        self.obj    = stem + '.o'

def check(prog):
    missing = [p for p in (prog.pp_vcl, prog.vsm, prog.c_vsm, prog.obj) if not os.path.exists(p)]
    if missing:
        print(f"FAIL {prog.name}: missing {', '.join(missing)}")
        return 1
    failed, notes = False, []
    for check_name, run in CHECKS:
        fails, note = run(prog)
        for f in fails:
            print(f"FAIL {prog.name} ({check_name}): {f}")
        failed = failed or bool(fails)
        if note:
            notes.append(note)
    if failed:
        return 1
    print(f"ok   {prog.name}: all {len(CHECKS)} VU checks ({'; '.join(notes)})")
    return 0

def main(objects):
    if not objects:
        print("FAIL no VU objects to check (build/vu/*.o) - run make first")
        return 1
    rc = 0
    for obj in objects:
        rc |= check(Program(obj))
    return rc

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:] or sorted(glob.glob('build/vu/*.o'))))
