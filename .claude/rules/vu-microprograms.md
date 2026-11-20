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

`oxide-vusl --warnings-as-errors --header=build/vu/<name>.vusl.h` → `dvp-as`. oxide-vusl comes
from the `src/tools/oxide-vcl` submodule, built by cargo into `build/tools/oxide-vcl/`; it
compiles VUSL to VCL and hands that to oxide-vcl in oxide mode with every optimization. Its
docs, there: `docs/VUSL.md` (the language, the cost of each construct, the header) and
`docs/VUSL_COMPILER.md`. The output is config-independent and lives in `build/vu/`: the `.vsm`
and `.o`, and the header. There are two programs, `textured_triangles.vusl` and
`particles.vusl`, which share `frame.vush`.

- **Check `make`'s exit status.** oxide-vusl writes all its files or none, but a failed VU
  build leaves the previous `.vsm`/`.o` in place. If a VU change seems not to take effect,
  compare the `.vsm` mtime with the `.vusl`.
- **The header is the binding.** vu1.h includes `<name>.vusl.h` and takes the programs' VU
  addresses, batch offsets and the particle ceiling (`kBatchMaxCount`, solved by the compiler
  from the layout) from it, and static-asserts its own structs (FrameConstants, DrawVertex,
  LightConstants, LerpConstants, WarpConstants, ParticleVertex) against the generated ones. A
  layout changed in a `.vusl` fails the build there, not on screen. The C++ objects wait for
  the headers on a clean build (an order-only prerequisite), and `-MMD` tracks them after.
  vu1.cpp also asserts that both programs' `kInstructions` fit micro memory.
- `--warnings-as-errors` covers the preprocessor and the compiler alike. The notes oxide-vusl
  prints for values kept in the work area (`'iwindow_5' does not fit in the VI registers`)
  are not warnings.
- `--emit-vcl=FILE` writes the generated VCL, for reading what a construct became:
  `build/tools/oxide-vcl/release/oxide-vusl --emit-vcl=/tmp/t.vcl -o /tmp/t.vsm src/ps2/renderer/vu1progs/textured_triangles.vusl`.
- dvp-as's **"operand out of range" warning on a branch is harmless**. Branch offsets are 11
  bits and dvp-as keeps the low bits, which reach the same instruction because VU1 micro
  memory is exactly 2048 instructions and the PC wraps. textured_triangles' copies of its
  loop put some branches 1071 instructions back.
- **Immediates:** oxide mode's `imm` fix rewrites or rejects an immediate that does not fit
  its field, where dvp-as would truncate it silently (an `iaddi` of -18 once assembled as
  +14).

## Writing VUSL for these programs

Measured against the hand-written VCL these programs replaced (2026-11-20), on oxide-vcl's
test scenes for them (`crates/oxide-vcl/tests/support/scenes.rs`, run in its VU model): the
packets are identical and the cycles 1.4% more over the triangle scenes, +0.9% for particles.
What moved the triangle program, each from its own A/B:

- **Hold the batch's modes in locals before the loop**: `int16 colorMode`, `int16 format`,
  and the warp flag as `bool warped = batch.warp != 0`. A mode nothing in the loop writes is
  what lets oxide-vcl's `unswitch` copy the loop per colour mode and vertex format. A `bool`
  pays its compare once; an `int16 warped` tested `warped != 0` in the loop paid an `isub` per
  triangle (60 cycles over the 30-triangle wall).
- **Write the triangle out before its clip and cull decisions**, in the same block as the
  transform, and let the slow path take the output back (`cursor -= 3`). The stores fill the
  transform's empty slots. Branching to the fast path first cost the wall +54%.
- **Leave the slow path's test a flag** set on each format's side, with one `if (slow)` after
  them. `continue` out of the format sides stopped `unswitch` copying the loop per mode (1190
  instructions instead of 1806), and the model scene lost 10%.
- **Don't hoist a constant across the loop.** A `-1.0` held in a local for the clamps cost
  the keyframe path 5%: one more VF register live across the loop. A literal costs a `loi`
  where it is used.
- **Open the first window through `batch.windows[0]`**, a constant offset from XTOP. Through
  the `window` ref, which lives in the work area, each of the seven tag stores reloaded it.
- Changes that should save instructions often don't: these blocks are bound by their
  dependency chains, and the schedule moves by ±30 cycles with any edit. Measure each one.
- **The clip flags land four cycles after a `clipw`**: `any()` and `adc()` read them, and
  the compiler requires the `clipw`s to be the last ones in the same block.
- **A cut must interpolate what the GS would have.** Anything per-vertex that is not linear
  across the triangle (the water warp) has to be evaluated at the corners before the clipper
  runs, so the cut lerps finished values, as GL's clipper does. Warping the vertex the cut
  made samples the ripple where no neighbour does: on a 64-unit leaf's edges, computed, 3.2
  texels off the neighbours at the median and 8 at worst. The water warp did that until
  2026-10-11. Fixing it alone changed nothing visible on the console, where the large S/T
  of water was the artifact the user saw (see [gs-renderer.md](gs-renderer.md)). Every
  turbulent triangle goes through `warpCorners` in the clipper's buffer, cut or not, and out
  through the fan. A new non-linear per-vertex term belongs there too.
- **Store order through two refs**: the seed's stores and `warpCorners`, which writes over
  their S and T through another base, keep a `#pragma barrier` between them.

To A/B a program change, compile both versions and run them on the scenes: the programs suite
in oxide-vcl (`crates/vusl/tests/programs.rs`, whose `report` test prints cycles per scene)
does it for its own copies of these programs, against the corpus VCL. PCSX2 shows none of it
(see [performance.md](performance.md)).

## Linking

- `#pragma program(NAME)` → oxide-vusl emits `NAME_CodeStart`/`NAME_CodeEnd`. Declare those
  with `PS2_DECLARE_VU_MICROPROGRAM` at namespace scope. An `extern "C"` inside an anonymous
  namespace gets internal linkage and won't resolve against `.vudata`. Use `u32`/`unsigned int`
  for the symbol types, not `uint32_t`.

## VU memory layout (vu1.h)

Chunks are up to 90 vertices (60 for MD2) in a double buffer, `XTOP` flips per `MSCAL`, and
there are two output windows XGKICKed alternately. The clip scratch is 956..1009: the
clipper's buffers from 957 and the program's work area (its `layout(spill)` parameter) at
1008-1009, where oxide-vcl keeps the VI values that do not fit in registers. 956 is spare. The
light block starts at 1010. The batch tag block is 7 qwords with no room to grow (see
[gs-renderer.md](gs-renderer.md)). The programs' `main` parameters are where these addresses
are written; vu1.h takes them from the generated headers.

- **A write past the program's half breaks only on a console.** VIF1 unpacks the next batch
  into the other half while the program runs, so an overrun from the lower half lands on that
  batch's header. PCSX2 runs the program to its end at the `MSCAL`, before the next unpack,
  which then overwrites the overrun: every PCSX2 run is clean. The particle program wrote past
  its half from f2f0452 (2026-09-21) until 2026-10-10 (6 qwords at 77 particles per batch). Its
  fit check counted the header, the input and 5 qwords per sprite, but not the 7 tag qwords it
  copies to the packet head. A full batch made the next count 0x7FFF, and the first
  big particle burst hung the frame on hardware. Size a layout from the program's last write.

## Debugging a microprogram

Read the generated VCL and `.vsm` as a hypothesis, not as evidence. Four disassembly theories
failed in a row on one bug, and two probes settled it:

- **Readout channel:** paint a value into the vertex colour (in mode 1, `col.xyz` of
  `drawCorner`) and read the grey level on screen. A value that saturates the blend hides
  what's behind it.
- **Byte dump:** store the value to a scratch qword nothing else writes (956, the clip
  scratch's spare qword, as a `layout(address = 956) inout` parameter), then print it from the
  EE with `ps2::debug::DumpVu1DataMemory(what, addrQw, count)` (`debug/pipeline_dump.h`, debug
  only), gated to every N frames. A probe in a function lands in *every* call, since each is
  inlined, so check the shape of the data before trusting a sample.

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
