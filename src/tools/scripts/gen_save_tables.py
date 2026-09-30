#!/usr/bin/env python3
# ================================================================================================
# File: gen_save_tables.py
# Brief: Generates the name tables the game saves its function and mmove_t pointers through.
#
# A save game has to refer to the code an entity runs next (think, touch, pain, the monster
# callbacks...) and to the animation it is playing (monsterinfo.currentmove). id's code stored
# those as offsets from InitGame and from a dummy mmove_t, which moved with any change to the
# executable, so no save survived a rebuild. g_save.c now stores a 32-bit FNV-1a hash of the
# pointee's NAME instead, and maps between the two with the tables this script writes:
#
#     g_saveFuncs[]  - every global function defined by the game (from nm over its objects)
#     g_saveMmoves[] - every mmove_t defined by the game (from its sources)
#
# Both are sorted by hash, for a binary search on load; g_save.c sorts a copy of each by
# address at startup, into arrays sized here, for the search the other way when saving.
# Listing every function rather than only the ones assigned to callbacks means no assignment
# can be missed; a callback that is static still can't be reached from here, and g_save.c
# refuses to save a pointer it can't name.
#
# The generated file includes no game headers. It declares each symbol with a placeholder
# type instead (void f(void) / const char m[]): only the addresses are used, and without
# the real prototypes in sight nothing conflicts. That relies on the build not using LTO.
#
#     gen_save_tables.py --nm <ee nm> --src src/game -o <out.c> <game objects...>
#
# Fails on a hash collision within a table (rename one of the two), on a zero hash (0 means
# NULL on disk), and on a mmove_t in the sources that no object exports.
#
# This source code is released under the GNU GPL v2 license.
# Check the accompanying LICENSE file for details.
# ================================================================================================

import argparse
import glob
import os
import re
import subprocess
import sys

# "mmove_t soldier_move_stand1 = {...}" at the start of a line. Every definition in the game
# looks like this; declarations ("extern mmove_t x;") and pointers don't match.
MMOVE_DEF_RE = re.compile(r"^mmove_t\s+(\w+)\s*=", re.MULTILINE)

# Game sources whose functions never become callbacks, and whose names shadow libc/GCC
# builtins when redeclared with the placeholder type (q_shared.c is shared with the engine).
EXCLUDED_OBJECTS = {"q_shared.o"}


def Fnv1a32(name):
    h = 0x811C9DC5
    for byte in name.encode("ascii"):
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def DefinedGlobals(nm, objects):
    """Maps symbol name -> nm type letter, for the global symbols defined by the objects."""
    symbols = {}
    for obj in objects:
        if os.path.basename(obj) in EXCLUDED_OBJECTS:
            continue
        out = subprocess.run([nm, "-g", "--defined-only", obj],
                             check=True, capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3:
                symbols[parts[2]] = parts[1]
    return symbols


def StripIfZeroBlocks(text):
    """Drops the '#if 0 ... #endif' regions (nesting-aware), which id's code has a few of."""
    kept = []
    depth = 0 # #if nesting inside a dropped region; 0 = keeping lines
    for line in text.splitlines():
        directive = line.strip()
        if depth == 0:
            if re.match(r"#\s*if\s+0\b", directive):
                depth = 1
            else:
                kept.append(line)
        elif re.match(r"#\s*if", directive):
            depth += 1
        elif re.match(r"#\s*endif\b", directive):
            depth -= 1
    return "\n".join(kept)


def MmoveNames(srcDir):
    names = []
    for path in sorted(glob.glob(os.path.join(srcDir, "*.c"))):
        with open(path, "r", encoding="latin-1") as f:
            names.extend(MMOVE_DEF_RE.findall(StripIfZeroBlocks(f.read())))
    return names


def BuildTable(kind, names):
    """[(hash, name)] sorted by hash; exits on a collision or a zero hash."""
    byHash = {}
    for name in names:
        h = Fnv1a32(name)
        if h == 0:
            sys.exit(f"gen_save_tables: the {kind} '{name}' hashes to 0, which means NULL on disk - rename it")
        if h in byHash and byHash[h] != name:
            sys.exit(f"gen_save_tables: {kind} hash collision between '{byHash[h]}' and '{name}' - rename one")
        byHash[h] = name
    return sorted(byHash.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True, help="the EE toolchain's nm")
    parser.add_argument("--src", required=True, help="the game source directory (src/game)")
    parser.add_argument("-o", "--output", required=True, help="the .c file to write")
    parser.add_argument("objects", nargs="+", help="the compiled game objects")
    args = parser.parse_args()

    symbols = DefinedGlobals(args.nm, args.objects)
    funcs = [name for name, kind in symbols.items() if kind == "T"]
    mmoves = MmoveNames(args.src)

    for name in mmoves:
        if symbols.get(name) not in ("D", "d", "B", "b", "C"):
            sys.exit(f"gen_save_tables: mmove_t '{name}' is in the sources but no game object exports it "
                     f"(static, or compiled out?)")

    funcTable = BuildTable("function", funcs)
    mmoveTable = BuildTable("mmove_t", mmoves)

    lines = [
        "/* Generated by src/tools/scripts/gen_save_tables.py - do not edit. See that script. */",
        "",
        "typedef struct { unsigned int hash; const void * ptr; } g_save_ptr_t;",
        "",
        "/* Placeholder types: only the addresses are used (see the script). */",
    ]
    lines += [f"extern void {name}(void);" for _, name in funcTable]
    lines += [f"extern const char {name}[];" for _, name in mmoveTable]
    lines += ["", "const g_save_ptr_t g_saveFuncs[] = {"]
    lines += [f"    {{ 0x{h:08X}u, (const void *){name} }}," for h, name in funcTable]
    lines += ["};", f"const int g_saveNumFuncs = {len(funcTable)};", ""]
    lines += ["const g_save_ptr_t g_saveMmoves[] = {"]
    lines += [f"    {{ 0x{h:08X}u, (const void *){name} }}," for h, name in mmoveTable]
    lines += ["};", f"const int g_saveNumMmoves = {len(mmoveTable)};", ""]
    lines += ["/* Room for the address-sorted copies g_save.c builds at startup (addresses aren't known here). */",
              f"g_save_ptr_t g_saveFuncsByPtr[{len(funcTable)}];",
              f"g_save_ptr_t g_saveMmovesByPtr[{len(mmoveTable)}];", ""]

    text = "\n".join(lines)

    # Unchanged (an edit inside a game function): only bring the timestamp forward, so make
    # sees the tables as up to date with the objects instead of running this every time.
    if os.path.exists(args.output):
        with open(args.output, "r") as f:
            if f.read() == text:
                os.utime(args.output, None)
                return

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w") as f:
        f.write(text)
    print(f"gen_save_tables: {len(funcTable)} functions, {len(mmoveTable)} mmove_t -> {args.output}")


if __name__ == "__main__":
    main()
