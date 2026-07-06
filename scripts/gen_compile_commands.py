#!/usr/bin/env python3
"""Generate compile_commands.json for IntelliSense / clangd from the Makefile.

Usage (see the `compiledb` target in the Makefile):
    make -Bnk | python3 scripts/gen_compile_commands.py

Reads a `make --dry-run` transcript on stdin, extracts every EE compiler
invocation that compiles a single source (`... -c <src> -o <obj>`), and writes a
compile database to compile_commands.json in the current directory. This keeps
editor tooling in exact sync with the real per-file C/C++ flags (standards,
-isystem, warning set) without needing `bear` or `compiledb` installed.
"""

import json
import os
import shlex
import sys


def main() -> int:
    root = os.getcwd()
    entries = []
    seen = set()

    for line in sys.stdin:
        line = line.strip()
        if " -c " not in line or "mips64r5900el-ps2-elf-g" not in line:
            continue
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue

        src = None
        for i, tok in enumerate(tokens):
            if tok == "-c" and i + 1 < len(tokens):
                src = tokens[i + 1]
                break
        if not src or not src.endswith((".c", ".cpp", ".cc")):
            continue
        if src in seen:
            continue
        seen.add(src)

        entries.append({
            "directory": root,
            "command": line,
            "file": os.path.join(root, src),
        })

    with open("compile_commands.json", "w", encoding="utf-8") as out:
        json.dump(entries, out, indent=2)
        out.write("\n")

    print(f"gen_compile_commands: wrote {len(entries)} entries", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
