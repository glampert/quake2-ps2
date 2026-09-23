#!/usr/bin/env python3
"""Check openvcl's register allocation by reaching definitions.

openvcl allocates registers by source-order interval and gets loops wrong: a
value whose last write sits mid-loop looks dead from there to the bottom, even
though the loop's back edge reads it again, and its register goes to whatever
wants one next. textured_triangles lost its input pointer that way - to the
backface cull's verdict, the ADC word and the clipper's walk pointer - and the
program built cleanly with every other check passing. The earlier checks each
guard one shape of this (a loop counter, a value carried between sibling
loops); this one guards the property itself.

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
(check_vu_crossloop.py covers the sibling-loop case); and a write -c leaves
uncommented, which is a few percent of them, cannot be named, so a read it
reaches is checked against the named writes alone.

Usage: check_vu_regalloc.py <program-c.vsm> <program.vsm>
"""
import re, sys

BRANCHES = {'b', 'bal', 'ibeq', 'ibne', 'ibltz', 'ibgtz', 'iblez', 'ibgez', 'jr', 'jalr'}
UNCONDITIONAL = {'b', 'bal', 'jr', 'jalr'}
LANES = 'xyzw'

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

def main(cpath, realpath):
    prog = parse(cpath)
    name = realpath.split('/')[-1]
    if not same_instructions(prog, parse(realpath)):
        print(f"FAIL {name}: openvcl -c scheduled it differently beyond nops - cannot verify")
        return 1

    labels = {lab: i for i, p in enumerate(prog) for lab in p[0]}
    n = len(prog)

    # Successors, delay slots included: the instruction after a branch runs before it lands.
    succ = [[i + 1] if i + 1 < n else [] for i in range(n)]
    for i, (_, _, _, lower, ebit) in enumerate(prog):
        op, _, ops = split_op(lower)
        if ebit and i + 1 < n:
            succ[i + 1] = []
        if op in BRANCHES and i + 1 < n:
            target = labels.get(ops[-1]) if ops else None
            after = [target] if target is not None else []
            if op not in UNCONDITIONAL and i + 2 < n:
                after.append(i + 2)
            succ[i + 1] = after

    # Definitions: (pair, register, lane) -> name. A self-update with no source line
    # (iCount += 1 in a delay slot) carries its value on rather than starting one.
    gen = [dict() for _ in range(n)]
    uses = [[] for _ in range(n)]
    unnamed = 0
    for i, (_, comments, upper, lower, _) in enumerate(prog):
        for text in (upper, lower):
            reads, writes, _ = operands(text)
            uses[i] += reads
            op = split_op(text)[0]
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
    preds = [[] for _ in range(n)]
    for i in range(n):
        for s in succ[i]:
            preds[s].append(i)
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

    for i, reg_, names in bad[:20]:
        _, comments, upper, lower, _ = prog[i]
        print(f"FAIL {name}: instruction {i} ({lower.split()[0] if not lower.startswith('nop') else upper.split()[0]}) "
              f"reads {reg_}, which holds any of {', '.join(names)} depending on the path")
    if len(bad) > 20:
        print(f"FAIL {name}: ... and {len(bad) - 20} more")
    if not bad:
        print(f"ok   {name} (register allocation; {unnamed} writes without a source line)")
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
