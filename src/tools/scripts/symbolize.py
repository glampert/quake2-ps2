#!/usr/bin/env python3
# ================================================================================================
# File: symbolize.py
# Brief: Resolves a PS2 stack trace dump to function names, files and line numbers.
#
# The game prints raw addresses on its fatal paths (see src/ps2/debug/stack_trace.cpp)
# because the ELF that runs is stripped. This feeds those addresses through addr2line
# against the unstripped ELF built beside it, which is where the symbols stay.
#
#     build/tools/symbolize < dump.txt          # paste or pipe a whole console log
#     build/tools/symbolize dump.txt            # ...or name a file
#     build/tools/symbolize 0x001950fc 0x00133 # ...or just pass addresses
#
# Input is read loosely on purpose: paste the entire PCSX2/ps2client log if you like,
# banner lines, "[Q2]" prefixes, surrounding noise and all. Anything shaped like a
# "#N 0xADDR" frame line is picked out and everything else is ignored.
#
# This source code is released under the GNU GPL v2 license.
# Check the accompanying LICENSE file for details.
# ================================================================================================

import argparse
import os
import re
import shutil
import subprocess
import sys

DEFAULT_ADDR2LINE = "mips64r5900el-ps2-elf-addr2line"

# "#0  0x001950fc" as printed by PrintStackTrace, tolerant of leading log noise.
FRAME_RE = re.compile(r"#\s*(\d+)\s+(0x[0-9a-fA-F]+)")

# Fallback for input that has the addresses but not the frame numbering.
BARE_ADDR_RE = re.compile(r"\b(0x[0-9a-fA-F]{6,8})\b")

# A positional argument that is an address rather than a file to read.
ARG_ADDR_RE = re.compile(r"^(?:0x)?[0-9a-fA-F]{4,8}$")


def repo_root():
    """Repo root, whether this runs from src/tools/scripts/ or installed in build/tools/."""
    here = os.path.dirname(os.path.abspath(__file__))
    depth = 3 if os.path.basename(here) == "scripts" else 2
    return os.path.abspath(os.path.join(here, *[".."] * depth))


def find_default_elf():
    """The unstripped ELF to resolve against, plus any other candidates found.

    Prefers debug, because that is the config `make run` launches. Deliberately
    NOT the most recently built one: rebuilding the other config does not change
    which ELF a dump came from, and picking by timestamp silently resolves the
    addresses against the wrong layout - plausible names, all of them wrong.
    """
    candidates = []
    for root in (os.getcwd(), repo_root()):
        for config in ("debug", "release"):
            path = os.path.join(root, "build", config, "quake2_unstripped.elf")
            if os.path.isfile(path):
                candidates.append(os.path.realpath(path))

    # Same file reachable by two roots - keep one entry per real path, in order.
    unique = list(dict.fromkeys(candidates))
    return (unique[0], unique[1:]) if unique else (None, [])


def parse_addresses(text):
    """Pulls (frame number, address) pairs out of whatever was pasted in."""
    frames = [(int(n), int(a, 16)) for n, a in FRAME_RE.findall(text)]
    if frames:
        return frames

    # No "#N" numbering: take bare addresses in the order they appear, but skip
    # the "Resolve with:" hint line so its example text cannot leak in.
    addrs = []
    for line in text.splitlines():
        if "addr2line" in line:
            continue
        addrs += [int(a, 16) for a in BARE_ADDR_RE.findall(line)]
    return list(enumerate(addrs))


def resolve(addr2line, elf, address, want_inlines):
    """One address -> [(function, file:line), ...], innermost first.

    Run per address rather than in one batch: with -i a single address can expand
    to several lines and addr2line prints no separator between addresses, so a
    batch call cannot be split back up reliably.
    """
    cmd = [addr2line, "-f", "-C", "-e", elf]
    if want_inlines:
        cmd.append("-i")
    cmd.append("0x%08x" % address)

    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    except subprocess.CalledProcessError as err:
        return [("<addr2line failed: %s>" % err.stderr.strip(), "")]

    # Output alternates: function name, then file:line.
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    return [(lines[i], lines[i + 1] if i + 1 < len(lines) else "")
            for i in range(0, len(lines), 2)]


DISCRIMINATOR_RE = re.compile(r"\s*\(discriminator \d+\)\s*$")


def shorten(location, root, full_paths):
    """Build-time absolute paths are long; show them relative to the repo."""
    location = DISCRIMINATOR_RE.sub("", location)
    if full_paths or not location:
        return location

    path, _, line = location.rpartition(":")
    if not path:
        return location

    # Paths come out of the ELF as the compiler saw them, which for anything
    # outside the repo (the SDK's own crt0) means a long unnormalized chain of
    # "..". Collapse it either way: repo-relative when it is ours, plain
    # absolute when it is not.
    real = os.path.realpath(path)
    try:
        rel = os.path.relpath(real, root)
    except ValueError:  # different drive on Windows
        return "%s:%s" % (real, line)

    return "%s:%s" % (rel if not rel.startswith("..") else real, line)


def main():
    parser = argparse.ArgumentParser(
        description="Resolve a PS2 stack trace dump to function names via addr2line.",
        epilog="Addresses are already the call instruction itself (the runtime "
               "subtracts the branch delay slot), so do not adjust them further.")
    parser.add_argument("input", nargs="*",
                        help="dump file(s) to read, or addresses directly; stdin if omitted")
    parser.add_argument("-e", "--elf",
                        help="unstripped ELF to resolve against "
                             "(default: newest build/<config>/quake2_unstripped.elf)")
    parser.add_argument("--addr2line", default=os.environ.get("ADDR2LINE", DEFAULT_ADDR2LINE),
                        help="addr2line to use (default: %s)" % DEFAULT_ADDR2LINE)
    parser.add_argument("--no-inlines", action="store_true",
                        help="do not expand inlined frames")
    parser.add_argument("--full-paths", action="store_true",
                        help="print absolute source paths instead of repo-relative ones")
    args = parser.parse_args()

    addr2line = shutil.which(args.addr2line)
    if addr2line is None:
        sys.exit("error: '%s' not found on PATH. Is the ps2dev toolchain set up?" % args.addr2line)

    elf, others = (args.elf, []) if args.elf else find_default_elf()
    if elf is None:
        sys.exit("error: no quake2_unstripped.elf found. Build first, or pass -e <elf>.")
    if not os.path.isfile(elf):
        sys.exit("error: no such ELF: %s" % elf)

    # Positional args are either addresses to resolve or files to read.
    literal = [a for a in args.input if ARG_ADDR_RE.match(a)]
    files = [a for a in args.input if not ARG_ADDR_RE.match(a)]

    frames = []
    if literal:
        frames += list(enumerate(int(a, 16) for a in literal))
    for path in files:
        try:
            with open(path, "r", errors="replace") as handle:
                frames += parse_addresses(handle.read())
        except OSError as err:
            sys.exit("error: %s" % err)
    if not args.input:
        frames += parse_addresses(sys.stdin.read())

    if not frames:
        sys.exit("error: no stack trace addresses found in the input.")

    def display(path):
        shown = os.path.relpath(path, os.getcwd())
        return path if shown.startswith("..") else shown

    print("ELF: %s" % display(elf))
    if others:
        # A dump resolved against the other config yields wrong names, not errors,
        # so make the choice visible rather than quietly picking one.
        print("     (also built: %s - pass -e to use it)"
              % ", ".join(display(o) for o in others))
    print()

    root = repo_root()
    unresolved = 0

    for number, address in frames:
        entries = resolve(addr2line, elf, address, not args.no_inlines)
        function, location = entries[0]
        if function == "??":
            unresolved += 1

        print("#%-2d 0x%08x  %-34s %s"
              % (number, address, function, shorten(location, root, args.full_paths)))

        # With -i the rest of the list is the chain of callers it was inlined into.
        # Indent by the width of the "#N 0xADDR  " gutter so the columns line up.
        for function, location in entries[1:]:
            print("%s%-34s %s"
                  % (" " * 16, "^- inlined into " + function,
                     shorten(location, root, args.full_paths)))

    if unresolved == len(frames):
        print()
        sys.stdout.flush()  # so the hint below lands after the table, not before
        print("None of the addresses resolved. Two usual causes:", file=sys.stderr)
        print("  - that ELF is the stripped one; use quake2_unstripped.elf", file=sys.stderr)
        print("  - the dump came from a different build than this ELF", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
