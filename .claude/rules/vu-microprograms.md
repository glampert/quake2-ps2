---
paths:
  - "src/ps2/renderer/vu1progs/**"
  - "src/ps2/renderer/vu1.h"
  - "src/ps2/renderer/vu1.cpp"
  - "src/ps2/renderer/clip.h"
  - "src/ps2/math/vec_mat.h"
---

# VU microprograms and VU0 inline asm

## Pipeline (Makefile rule `build/vu/%.o`)

`oxide-vcl -G -I vu1progs --vcl-boilerplate --warn-undef --mode=oxide --opt=all
--warnings-as-errors` → `dvp-as`. oxide-vcl is the `src/tools/oxide-vcl` submodule, built by
cargo into `build/tools/oxide-vcl/`; its docs (`docs/VCL_REFERENCE.md` there) cover the
language, the preprocessor and the optimizations. The output is config-independent and lives
in `build/vu/`, the `.vsm` beside each `.o`. There are two programs, `textured_triangles.vcl`
and `particles.vcl`, with shared `vu_common.i`/`vu_clip.i`.

- **Check `make`'s exit status.** A failed VU build leaves the previous `.vsm`/`.o` in place.
  If a VU change seems not to take effect, compare the `.vsm` mtime with the `.vcl`.
- `--warnings-as-errors` covers the preprocessor (`--warn-undef`: an `#if` on an undefined
  name) and the compiler alike.
- dvp-as's **"operand out of range" warning on a branch is harmless**. Branch offsets are 11
  bits and dvp-as keeps the low bits, which reach the same instruction because VU1 micro
  memory is exactly 2048 instructions and the PC wraps.
- **Immediates:** oxide mode's `imm` fix rewrites or rejects an immediate that does not fit
  its field, where dvp-as would truncate it silently (an `iaddi` of -18 once assembled as
  +14).

## Writing VCL for oxide-vcl

- **A register some path leaves unwritten can't be read**, even on a path that never runs
  (`'x.y' is read before anything writes it`). Write it on every path, as ClipTransform does
  its colour; the message names the register and lane.
- **Hold batch state in VI registers.** The allocator gives a value its real live range and
  has VI01-VI15 (`--vcl-boilerplate` declares `.init_vi_all`; it keeps VI01 free across
  `fcand`), so a flag read once per batch belongs in a register, not reloaded per triangle.
  The window state and the clipper's survivor count stay in their spill qwords because they
  are read once per window or per pass, not because of the allocator.
- **Let `unswitch` see a mode.** A branch on a register the loop never writes (`iColorMode`,
  `iFormat`) is decided once before the loop: the triangle loop runs in one of four copies,
  per colour mode and vertex format. Reloading such a register inside the loop, even from a
  constant header, makes it a write and keeps the branch.
- **Keep the hot path one block.** The scheduler packs within a block. A branch in the
  middle of the usual path splits it, so the triangle is written out before its clip and
  cull decisions (`EmitTriangle`) and the slow path takes the output advance back; the fan
  overwrites the same slots.
- **The clip flags land four cycles after a `clipw`**: judge (`fcand`) in the same block, and
  carry the verdict in `vi01` from there.
- **Store order through two address registers**: a store that must land after another to the
  same qword written through a different base register (the uncut water path's seed and
  `WarpCorners`) keeps a `--barrier` or a block boundary between them.
- `clipw.xyz a, b` is rejected. Write `clipw.xyz fJudge, fPos[w]`, with an explicit `[w]`.

## Linking

- `#vuprog NAME` → `.name NAME` → oxide-vcl emits `NAME_CodeStart`/`NAME_CodeEnd`. Declare
  those with `PS2_DECLARE_VU_MICROPROGRAM` at namespace scope. An `extern "C"` inside an
  anonymous namespace gets internal linkage and won't resolve against `.vudata`. Use
  `u32`/`unsigned int` for the symbol types, not `uint32_t`.
- `#define`s nest and `#macro`s may invoke each other (recursion is an error). Label-placing
  macros take one prefix and paste it (`lbl##Loop`, `lbl##NoWarp`). Constant expressions are
  folded only with `--fold-constants`, which the Makefile doesn't pass, so the compiler gets
  operands like `1010 + 0(vi00)` and `kWindowVerts - kWindowReserve` as written, and
  evaluates them itself.

## VU memory layout (vu1.h)

Chunks are up to 90 vertices (60 for MD2) in a double buffer, `XTOP` flips per `MSCAL`, and
there are two output windows XGKICKed alternately. The clip scratch is 956..1009, and the
light block starts at 1010. The batch tag block is 7 qwords with no room to grow (see
[gs-renderer.md](gs-renderer.md)).

- **A write past the program's half breaks only on a console.** VIF1 unpacks the next batch
  into the other half while the program runs, so an overrun from the lower half lands on that
  batch's header. PCSX2 runs the program to its end at the `MSCAL`, before the next unpack,
  which then overwrites the overrun: every PCSX2 run is clean. The particle program wrote past
  its half from f2f0452 (2026-09-21) until 2026-10-10 (6 qwords at 77 particles per batch). Its
  fit check counted the header, the input and 5 qwords per sprite, but not the 7 tag qwords it
  copies to the packet head. A full batch made the next count 0x7FFF, and the first
  big particle burst hung the frame on hardware. Size a layout from the program's last write.

## Writing VCL

- **A cut must interpolate what the GS would have.** Anything per-vertex that is not linear
  across the triangle (the water warp) has to be evaluated at the corners before the clipper
  runs, so the cut lerps finished values, as GL's clipper does. Warping the vertex the cut
  made samples the ripple where no neighbour does: on a 64-unit leaf's edges, computed, 3.2
  texels off the neighbours at the median and 8 at worst. The water warp did that until
  2026-10-11. Fixing it alone changed nothing visible on the console, where the large S/T
  of water was the artifact the user saw (see [gs-renderer.md](gs-renderer.md)). Every
  turbulent triangle now goes through `WarpCorners` in the clipper's buffer, cut or not, and
  out through the fan. A new non-linear per-vertex term belongs there too.
- Above every non-trivial `#macro` and `#vuprog`, after the prose, add an indented
  `;`-commented **C-like pseudo-code** block: `vec4`/`mat4`/`qword` types, `//` comments, and
  control flow that mirrors the real code (`do/while` for a bottom-tested `ibne` loop).
  `textured_triangles.vcl` sets the format. Comment-only top-level `;` blocks pass through
  the preprocessor and the compiler safely.

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
