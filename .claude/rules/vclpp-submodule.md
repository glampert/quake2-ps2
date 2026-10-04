---
paths:
  - "src/tools/vclpp/**"
---

# vclpp and parse-utils (submodules)

vclpp (`src/tools/vclpp`, github.com/glampert/vclpp) preprocesses the VU programs. vclpp 2
is token-based, built on the lexer of parse-utils (nested submodule
`src/tools/vclpp/external/parse-utils`). The engine lives in vclpp, not in parse-utils'
`preprocessor`, which minifies and expands into text. vclpp only borrows its `eval()` for
`#if` and `-x`. The README is the syntax reference, including "MASP Mode" and "Upgrading
from VCLPP 1".

## Conventions (different from `src/ps2`)

- parse-utils style in both repos: **snake_case** types and functions, `m_` private members.
  vclpp code lives in `namespace vclpp`. Don't apply the backend's PascalCase rule here.
- **C++17, and it must build with GCC 9**, because tyra builds vclpp with GCC 9 on Ubuntu
  20.04. That rules out C++20 library features (`std::span`, `starts_with`/`ends_with`,
  `contains`, `std::bit_cast`, `string_view` lookups in unordered maps; use `std::map` with
  `std::less<>`) and the C++17 pieces GCC 9's libstdc++ lacks (floating-point
  `from_chars`/`to_chars`).
- Check with `-std=c++17 -pedantic-errors` under clang and the EE GCC. Compile with
  `-c -o /dev/null`, not `-fsyntax-only`, which skips GCC's `-O2`-only warnings.
- **Only Linux CI catches LP64 format mismatches.** `std::int64_t` is `long` on 64-bit Linux
  but `long long` on macOS and the EE. Use `PRId64` and friends.
- GCC 9 can't be installed on this Mac (arm64; Homebrew disabled `gcc@9`/`gcc@10`). Rely on
  CI, or use an Ubuntu 20.04 container.

## Publishing

- Push order: **parse-utils → vclpp → quake2-ps2**, each bumping the next one's gitlink.
  Before assuming any of them is published, run `git fetch` + `git status -sb` in each.
- CI is `.github/workflows/ci.yml` in vclpp, with three jobs (GCC 9 in an `ubuntu:20.04`
  container, GCC on ubuntu-latest, Apple clang on macos-latest). Each builds with `-Werror`
  and runs both test suites. Run and job status need no auth:
  `api.github.com/repos/glampert/vclpp/actions/runs`, then `.../runs/<id>/jobs`. Poll at most
  every 60 s (the unauthenticated limit is 60 requests/hour). Job *logs* need repo admin
  rights.

## Verifying a change

- Tests: `make test` in `src/tools/vclpp` (golden cases under `tests/`: features, errors,
  warnings, compat, masp) and in parse-utils (its tests use `-Werror`).
- **A change that could affect VU output** must leave quake2-ps2's VU build byte-identical:
  - build vclpp 1 from `git -C src/tools/vclpp show 00e44ec:vclpp_main.cpp` (`-std=c++14`) and
    save `build/vu/*` from it;
  - `diff -w -B` the `.pp.vcl` files, `cmp` the `.vsm`/`.o`, and `cmp` the `.c.vsm` with its
    `^ ; Line N:` lines dropped.
  - The Makefile runs vclpp with `-Wundef -Werror`, so a new warning fails the VU build.
- **tyra compat check** (github.com/h4570/tyra, local clone `~/Repos/GitHub/tyra`): 15
  `engine/src/**/*.vclpp` programs, run with no flags *from `engine/`* (includes are relative
  to it). They exercise CRLF programs with LF macro files, a 1430-line VCL SML copy with
  `QSL1@`-style labels, and C headers with `//` comments and `\` continuations. Run vclpp 1
  and 2 on each, drop CRs, trailing whitespace and blank lines, and compare. Also compare
  with whitespace runs collapsed, since `diff -w` hides two tokens gluing together. Last run:
  all 15 identical.
- **Never modify tyra's files or propose tyra PRs or Dockerfile changes.** Whether tyra adopts
  vclpp 2 is up to tyra. Its Dockerfile's plain `git clone` without submodules can't build
  vclpp 2, and the user chose to leave that.

## MASP mode (`-m`/`--masp`, or run as `masp`/`gasp`; `-f` flattens subscripts)

For ps2gl-style GASP/masp VU code. Implemented in `src/masp.{hpp,cpp}` (`masp_reader`), with
golden cases in `tests/masp/`.

- The reference is masp 0.1.16 at `~/ps2dev/bin/masp`. It reads no stdin by default, so pass
  `/dev/stdin`. A pipe probe (`printf ... | masp -c ';' /dev/stdin`) settles rule questions.
- The corpus is ps2gl (`~/Repos/GitHub/ps2gl`): 14 programs, `vu1/*.vcl` plus
  `examples/tricked_out/billboard_renderer.vcl`. ps2stuff's build rule is
  `~/Repos/GitHub/ps2stuff/Makefile.work:401`.
- Recipe:
  1. Strip with ps2stuff's `sed`, feed the result to masp and to `vclpp -m` with the same
     flags, normalize (drop comments and blank lines, collapse whitespace), and compare.
  2. Run the full chain (`sed | APP | sed 's/\[\([0-9]\)\]/_\1/g ; s/\[\([w-zW-Z]\)\]/\1/g' | openvcl | cpp | dvp-as`)
     with each tool; the objects must `cmp` identical.
  3. One step: `vclpp -m -x -f -Ivu1` → openvcl → dvp-as. 13 of 14 matched. `indexed` came out
     4 pairs shorter because openvcl proved two numeric addresses don't alias. That is a
     correct schedule, not a bug.
- Pitfalls: macOS `/usr/bin/cpp` is clang in traditional mode and keeps `//`, so use
  `mips64r5900el-ps2-elf-gcc -E -P -x c`. `dvp-objdump -d` can't disassemble VU code, so use
  `cmp` or `dvp-objdump -s`.
- masp quirks: macro names ignore case, but parameters and `\&`/`.equ` names don't. `\name`
  takes the longest name and leaves an unknown one as written. `\@` counts from 0. A
  positional argument after a keyword argument is an error. `.equ` substitutes inside quotes.
  A column-1 word is a label (masp adds the colon), and `name .macro` defines a macro. masp
  rewrites GASP data and listing directives itself (`.sdata` → `.byte`, `.align 4` → `4`).
  `.end` in an include ends everything.
- Deliberate differences (documented in the README): vclpp keeps labels masp drops, passes
  `.align` through, treats `name.xyz` as code, and makes errors fatal.

## Open follow-ups

- Teach the VS Code grammar (`src/tools/vscode_extensions/ps2-vcl`) the vclpp 2 directives
  and `##`.
- Possibly a `-MD` depfile and per-invocation unique labels.
