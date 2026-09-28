#!/usr/bin/env python3
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
see check_vu_regalloc.py) and, for every read of the clip flags or Q, finds the
shortest path back to a producer along any route. A waitq on the way satisfies
a Q read. Anything nearer than the latency fails.

Usage: check_vu_latency.py <program.vsm>
"""
import re, sys
from check_vu_regalloc import parse, split_op, successors

# (what produces it, what reads it, cycles until it lands)
RESOURCES = {
    'clip flags': (lambda op: op == 'clipw',
                   lambda op: op in ('fcand', 'fcor', 'fceq', 'fcget'),
                   4),
    'Q':          (lambda op: op in ('div', 'sqrt', 'rsqrt'),
                   lambda op: re.fullmatch(r'(add|sub|mul|madd|msub|adda|suba|mula|madda|msuba)q', op) is not None,
                   7),
}

def main(path):
    prog = parse(path)
    n = len(prog)
    succ = successors(prog)
    preds = [[] for _ in range(n)]
    for i in range(n):
        for s in succ[i]:
            preds[s].append(i)

    ops = [[split_op(upper)[0], split_op(lower)[0]] for _, _, upper, lower, _ in prog]
    name = path.split('/')[-1]
    bad = []

    for resource, (produces, reads, latency) in RESOURCES.items():
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

    for b in bad:
        print(f"FAIL {name}: {b}")
    if not bad:
        print(f"ok   {name} (clip flag and Q latency)")
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
