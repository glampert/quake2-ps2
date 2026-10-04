---
paths:
  - "src/ps2/renderer/vu1progs/**"
  - "src/ps2/renderer/vu1.h"
  - "src/ps2/renderer/vu1.cpp"
  - "src/ps2/renderer/clip.h"
  - "src/ps2/math/vec_mat.h"
  - "src/tools/vu-checker/**"
---

# VU microprograms and VU0 inline asm

## Pipeline (Makefile rule `build/vu/%.o`)

`vclpp -I vu1progs -Wundef -Werror -j` → `openvcl` → `openvcl -c` (source names, for the
regalloc check) → `dvp-as` → `check_vu_code.py` (vu-checker submodule), which deletes the
`.o` on failure. The output is config-independent and lives in `build/vu/`. There are two
programs, `textured_triangles.vcl` and `particles.vcl`, with shared `vu_common.i`/`vu_clip.i`.

- **A failed VU build is easy to miss.** openvcl's messages don't match `error:`. Check
  `make`'s exit status. If a VU change seems not to take effect, compare the `.vsm` mtime
  with the `.vcl`.
- dvp-as's **"operand out of range" warnings are expected**. Branch offsets are 11 bits, and
  dvp-as keeps the low bits. That is correct only because VU1 micro memory is exactly 2048
  instructions and the PC wraps. The `branches` check proves each lands mod 2048.
- The six checks: `crossloop`, `regalloc` (reaching definitions), `loopvar`, `immediates`
  (dvp-as silently truncates), `latency` (clip flag/Q across branches), `branches`. Each
  exists because a silent toolchain failure reached the screen.

## openvcl traps (all silent or cryptic)

- **Masked + unmasked writes to one VF register across a branch are not ordered as written.**
  `move vCol, vf00` before an `ibeq` and `move.w vCol, x` after it ended with the
  *initialiser's* `.w`. Keep writes to one register lane-disjoint (`move.xyz` + `move.w`), or
  write the value once on a path with no second writer.
- openvcl rejects a read of a lane that isn't written on every path, even an unreachable one
  (`Read-attempt from uninitialized float register`). The message is trustworthy: it caught
  an unassigned `.w`. But the obvious fix, an all-lane initialiser, creates the trap above.
  Mask the initialiser to the lanes the other writer does *not* touch.
- **A loop-carried value whose last write is mid-loop is treated as dead to the loop bottom.**
  openvcl sizes live ranges by source order and ignores the back edge, so it handed the
  pointer's VI to three other values. Keep a loop-carried pointer's update at the loop
  bottom. Use a spilled stride if the step varies. `regalloc` now catches this.
- **About 13 allocatable VI registers** (`-j` emits `.init_vi VI02..VI14`, and VI01 usually
  belongs to `fcand`). Past that you get `Register allocation ran out of registers` with no
  location. Fixes: merge counters with disjoint live ranges, read verdicts straight from
  `vi01`, and reuse one register for a running sum and its result.
- `clipw.xyz a, b` is rejected. Write `clipw.xyz fJudge, fPos[w]`, with an explicit `[w]`.

## vclpp and linking

- vclpp is the pinned submodule build. An old vclpp on `PATH` mangles macro bodies that
  contain comments.
- `#vuprog NAME` → `.name NAME` → openvcl emits `NAME_CodeStart`/`NAME_CodeEnd`. Declare
  those with `PS2_DECLARE_VU_MICROPROGRAM` at namespace scope. An `extern "C"` inside an
  anonymous namespace gets internal linkage and won't resolve against `.vudata`. Use
  `u32`/`unsigned int` for the symbol types, not `uint32_t`.
- Since vclpp 2, `#define`s nest and `#macro`s may invoke each other (recursion is an error).
  Label-placing macros take one prefix and paste it (`lbl##Loop`, `lbl##NoWarp`). Constant
  expressions are folded only with `-x`, which the Makefile doesn't pass, so openvcl gets
  operands like `1010 + 0(vi00)` as written.

## VU memory layout (vu1.h)

Chunks are up to 90 vertices (60 for MD2) in a double buffer, `XTOP` flips per `MSCAL`, and
there are two output windows XGKICKed alternately. The clip scratch is 956..1009, and the
light block starts at 1010. The batch tag block is 7 qwords with no room to grow (see
[gs-renderer.md](gs-renderer.md)).

## Writing VCL

- Above every non-trivial `#macro` and `#vuprog`, after the prose, add an indented
  `;`-commented **C-like pseudo-code** block: `vec4`/`mat4`/`qword` types, `//` comments, and
  control flow that mirrors the real code (`do/while` for a bottom-tested `ibne` loop).
  `textured_triangles.vcl` sets the format. Comment-only top-level `;` blocks pass through
  vclpp/openvcl safely.

## Debugging a microprogram

Read the generated `.vsm` as a hypothesis, not as evidence. Four disassembly theories failed
in a row on one bug, and two probes settled it:

- **Readout channel:** paint a value into the vertex colour (`addw.xyz vCol, vf00, <reg>`) and
  read the grey level on screen. A value that saturates the blend hides what's behind it.
- **Byte dump:** `sq` the register to a scratch qword nothing else writes (the spare qword of
  clip scratch), then print it from the EE with
  `ps2::debug::DumpVu1DataMemory(what, addrQw, count)` (`debug/pipeline_dump.h`, debug only),
  gated to every N frames. A probe inside a macro lands in *every* expansion, so check the
  shape of the data before trusting a sample.

## VU0 macro-mode inline asm (GCC 15)

- Syntax: `$vf4`/`$ACC` register prefixes and **no** `.xyzw` dest suffix
  (`vmulax $ACC, $vf5, $vf1`). See the `__GNUC__ > 3` branch of ps2sdk `ee/math3d/src/math3d.c`.
- GAS inserts a `nop` between `mfc1` and a reader of its GPR. Put independent work there.
- Passing a float as `"r"(bits_to_u32(t))` makes GCC `mfc1` right after the `div.s` and stall
  for the whole divide. Pass `"f"(t)` and `mfc1` inside the asm after the loads.
- One LerpTo-style asm per qword serializes on FMAC latency. Fusing N qwords into one block
  lets the subtracts issue back to back, and the results stay bit-identical.
- vec_mat.h's VU0 asm has no host fallback. Prove rewrites with the test-ELF recipe in
  [performance.md](performance.md).
