---
paths:
  - "src/ps2/**"
  - "src/tools/scripts/frame_log/**"
---

# Performance: EE codegen, measuring in PCSX2, recipes

## Where things stand

- Goal: a smooth 60 fps through the whole `ps2_perftest` capture. It currently has **0
  dropped frames**. Reference `build/baselines/vwep.flog`: EE work mean 5.4 ms, p99 9.3 ms,
  max 11.6 ms (debug).
- What it took: soft-float doubles in the engine C (`-fsingle-precision-constant` +
  `math_c.h`, 59 → 1 drops); cheaper effects (`Com_FxRand`, an incremental rail spiral,
  inline `V_AddParticle`); preloading mid-level assets (player weapon fire sounds, all view
  weapons in `CL_RegisterTEntModels`); and cheaper file I/O (hashed pak directory, unbuffered
  pak reads, file-sized stdio buffers). A mid-level asset load is the usual cause of a dropped
  frame.
- A release-build EE codegen audit already landed: per-draw chunk heads copied with lq/sq
  (EntShadow -46%, EntGeom -19%), world gather through `ReserveVerts`/`CommitVerts` plus a
  local cursor, a BSP walk that passes clip flags down the tree with an inline corner test
  instead of `BoxOnPlaneSide` (-32%), and the particle gather through a local cursor with
  `sq`. Release View went from 3405 to 2614 µs.
- **Parked: background loading** of mid-level models/skins (a loader thread doing I/O,
  decode, and a Loading state). It is the only fix left for DM player joins and dropped world
  weapons. Decode matters as much as I/O: v_rail's md2+skin cost ~8.9 ms, of which only
  ~1.7 ms was I/O. The design is in the user's plan file
  `~/.claude/plans/i-want-you-to-binary-spindle.md`.
- **Emergency chain drains: fixed 2026-10-11** (`cmdbuf::Reserve`'s overflow path). An overflow
  used to kick what was built, wait for the GS to draw all of it and rebuild in the same half, so
  the EE sat idle. On a console that cost ~5 ms per overflowing frame, and under NTSC those
  frames were 80% of the dropped ones. Now it kicks and carries on in the other half, which that
  kick has already waited out (`s_inFlightHalf` guards the one case where `Kick` sends nothing).
  The halves also grew from 512 to 768 KB, and the world arena by 512 KB with them. In PCSX2 the
  perf demos no longer overflow at all (the largest chain is 656 KB). A stress build with 256 KB
  halves drew the same work with no assert or stall: 8,191 overflows in 7,679 frames, up to 3 in
  one frame. On the console under NTSC (same settings as before) it removed every drain: 533
  frames to 0. View p95 fell from 15.2 to 6.4 ms and Entities p95 from 11.6 to 3.3, but
  dropped frames only went from 6.8% to 5.8% (56.1 to 56.7 fps). What drops now is views of
  6,000+ triangles, 48% of them, at ~18 ms: 9.1 ms of EE work plus 9.0 ms of GsWait. Each frame
  is kicked once, at its end, so **the GS idles while the EE builds a frame and the EE then
  waits while the GS draws it**. The overflow's mid-frame kick used to overlap the two by
  accident; at 768 KB nothing overflows. Next lever: kick mid-frame on purpose.

## EE codegen facts

- **Count memory ops, not instructions.** ALU is the cheap half of this machine. A version
  that ran 9 *more* instructions per triangle but did fewer loads and stores was faster.
- **`-fno-strict-aliasing` makes gcc spill and reload pointers around stores.** A local
  `__restrict` copy *at the use site* fixes it
  (`vu1::LerpVertexBytes * const __restrict p = tri.pos;`). `__restrict` on struct/class
  members produces byte-identical code, so don't bother. It isn't a blanket sweep: check the
  disassembly per site, since it pays off where register pressure is high. Declare restrict
  locals *after* any flush call in the loop body. A cursor passed by reference has its
  address taken and stays in memory, so copy it into a local first.
- **ps2sdk `packet2_add_*` costs ~5 memory ops per word** (it reloads `packet->next` around
  every store). In hot emission, take a local `qword_t * __restrict q = pkt->next`, write whole
  qwords, and store `next` back once.
- **gcc never forms `lq`/`sq` itself.** Struct copies of `alignas(16)` types, `__int128`,
  `vector_size(16)`, `mode(TI)` and aligned `__builtin_memcpy` all lower to `ld`/`sd` pairs (a
  Mat4 is a two-trip loop). Use `ps2/qwords.h` (`CopyQwords`, `StoreQword`, `CopyDrawVertex`)
  at memory-to-memory sites. A transparent lq/sq `operator=` on Vec4/Mat4 was measured and
  rejected, because asm memory operands push register-promoted locals back to memory (+56%
  instructions in `DrawAliasMD2Entity`). The reasons are recorded in `vec_mat.h`'s header.
- `__builtin_ctz/clz` are libgcc calls on the EE. A per-batch ctz cost TexChains +9%.

## What a PCSX2 capture can and can't show

- **No EE cache emulation** (`EnableEECache = false`). Cache-locality work (prefetch, slimmer
  structs, UCAB/uncached buffers, reordering) reads as zero. Don't conclude it's worthless.
  To test it, turn the ini flag on (slow, edit with PCSX2 closed) or use hardware.
- **No GS-internal cost.** CLUT loads, fill rate, texture cache and overdraw are free. Only
  data moved (DMA/GIF qwords) and EE/VU1 instructions are charged. For GS-side changes,
  promise "no regression", never a measurable win.
- **PCSX2 charges roughly per instruction** (~2.5-3 cycles per EE instruction seemed to
  apply). Only a net instruction-count cut shows up. An MMI change that swapped 5 memory ops
  for 1 extra instruction measured *slower* (+3.1%) and was reverted, even though it should
  win on hardware.
- Same build, same capture: rendering stages reproduce within ~0.2 µs/frame, and demo1 ends on
  frame 4183 of 7911 every run. SndMix (~1 µs), Frame (~1 µs) and VSync (~6 µs) wander between
  identical runs. **Code layout alone shifts stages by ~1-2 µs** (0.5 KB of init-only code
  moved Particles by -1.9%). To attribute a delta under ~2%, compare against a same-build
  re-run, and if needed against a variant with HEAD's section sizes (`objdump -h`).
- Quote per-item ratios (e.g. EntGeom per triangle), not the EE mean. Run-to-run spread on
  the EE mean is about ±2%.
- PCSX2 timing ≠ hardware for I/O and FPU latency either.

## The first console capture (2026-10-10, debug build, USB)

The same debug ELF ran `ps2_perftest` on a console and in PCSX2. That test build also narrowed
the guard band to 0.25 through a test cvar (since removed), so the PCSX2 comparison used it too.

- **A PAL console runs at 50 Hz** with `ps2_video_mode auto`, the console's own standard. A frame
  there has 20 ms and the frame rate tops out at 50. The "~48 fps" seen on a release build was
  that cap with a few percent of frames taking two fields. PCSX2's BIOS here is NTSC. Use
  `frame_budget.py --pal` for a PAL capture, and `ps2_video_mode pal` reproduces the pacing in
  PCSX2: 6635 frames for the perf test's 7911, with no steady frame dropped.
- **EE work (Frame - VSync - GsWait) was 1.56x PCSX2's**, 7.2 against 4.6 ms. TexChains was
  1.95x, World 1.6x, LmChains 1.46x, EntGeom 1.3x, Sound and SndMix 1.65-2x. This is the cache
  PCSX2 doesn't model, and it puts real numbers on the cache bullet above.
- **GsWait: 5.4 ms a frame against 0.05.** On hardware the GS/VU1 side takes ~12-13 ms per
  frame for ~4K triangles. PCSX2 charges it nothing.
- **Emergency chain drains cost ~8.6 ms on hardware.** These are frames whose chain outgrows the
  512 KB half: ~9-12% of demo frames, the heavy views at ~6.8K triangles. On the console they
  averaged 28.7 ms against 20.1 ms for the rest. View rose to 17 ms, because the EE waits for
  the GS mid-frame. In PCSX2 the same frames cost ~2 ms more. They were the console's dropped
  frames.
- **259 hitches of ~314 ms (median), one every 640 ms: 83 s of the 171 s of gameplay.** No
  column held the time, and PCSX2 had none. The cause was the keyboard poll waiting out the
  music reader's USB reads: 640 ms is two 8 KB music buffers (see `audio.md`). Input now has its
  own column. **Confirmed** by a second capture with `in_keyboard 0` and the 0.8 band: 7 slow
  frames for 2.6 s of 121 s, and 49.0 fps over gameplay. 2.6% of steady frames were dropped.
  Of the 7 slow frames, two were mid-demo asset loads (the slugs ammo model, the railgun hum),
  one a 134 ms Server spike and one the demo switch; three left nothing in the log. A third
  capture, the profiling release with `in_keyboard 1`, confirmed the fix (no poll while a read
  is pending, and the idle polling). It had the same 7 slow frames, 2.9 s, and a keyboard poll
  cost ~221 µs on hardware, on 4.7% of frames. It's not known whether the reads take that long
  without the capture's own writes to the drive.
- **The profiling release on the PAL console is at the 50 Hz cap:** 49.9 fps over gameplay, 0.77%
  of steady frames dropped, and 3.6% with under 1.7 ms to spare. On the same ELF, EE work was
  6.2 ms against PCSX2's 3.8 (1.63x; TexChains and LmChains ~2x, World 1.77x, EntGeom 1.57x),
  and GsWait 5.5 ms against 0.04. Chain drains, on 11% of frames, averaged 21.4 ms with 3.2 ms
  of vsync left, against 9.0 for the rest: within a PAL field, but 18 ms of work would miss an
  NTSC one.
- **The same console forced to NTSC** (`ps2_video_mode ntsc`, 2026-10-11, the profiling release,
  448 lines) ran at 56.1 fps over gameplay, with 6.8% of steady frames dropped against 0.77% at
  50 Hz. The work per frame matched PAL's: EE 6.0 ms and GsWait 5.3 ms. Only the budget shrank.
  80% of the dropped frames had a chain drain (368 of 461), doing 19.3 ms of work against
  16.7 ms. The rest were heavy views (~6.2K triangles) at the GS's own limit.
- **The guard band's width costs the GS nothing measurable:** GsWait was 5.39 ms at 0.25 and
  5.42 ms at 0.8 on the console, with EE work about the same. So narrowing it buys no speed, and
  the test cvar was dropped.
- **Capturing on a console:** the frame log goes to stdout, which a console has nobody reading.
  Build with `PS2_QUAKE_FRAME_LOG_FILE=1` (profile.h, shipped as 0) and each 64-frame dump is
  also appended to `baseq2/frame_log.txt` with one open/write/close. A profiling release for a
  console capture:
  `rm -rf build/release/src && make release CONFIG_DEFS='-DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=1 -DPS2_QUAKE_FRAME_LOG_FILE=1 -DNDEBUG'`,
  then the same `rm -rf` and a plain `make release` afterwards. Keep `ps2_logfile` off for a
  capture, since every console print through it is a ~23 ms USB write. The perf test allows
  240 s for a USB map load.

## Capture recipe (`ps2_perftest`, ~2.5 min, debug build)

1. Back up `baseq2/config.cfg` and set `ps2_perftest "1"` in it. `make run`.
2. Wait for `PerfRun: complete` / `FLOG#end` in emulog, then copy `emulog.txt` into
   `build/baselines/<tag>.emulog.txt` before the next launch overwrites it.
3. Restore the config. The run sets `ps2_show_fps` and the like to 0, and those are
   `CVAR_ARCHIVE`, so the quit writes them back.
4. `src/tools/scripts/frame_log/summarize_flog.py <emulog> --rows build/baselines/<tag>.flog`,
   then `compare_flog.py <before> <after>`. `frame_budget.py` realigns Server/ClParse (they
   land one row early) and lists each `FLOG#open` load with its cost.

- If the notes show `pics/m_main_*.pcx` or `pause.pcx` mid-demo, a stray host key reached
  PCSX2's USB keyboard and opened the menu. Re-run with focus away from PCSX2.
- **Profiling a release build:** set `-DPS2_QUAKE_PROFILE=1` in the release `CONFIG_DEFS`,
  `rm -rf build/release/src`, `make release run` with `ps2_perftest 1`. Revert and rebuild
  clean afterwards.
- `build/baselines/` is untracked and local, holding captures, summaries and config backups.

## Codegen A/B across the backend (no tree edits)

- Baseline: `make release`, then snapshot `build/release/src/ps2/**/*.o` to the scratchpad.
- Take the exact compile line from `make -n <obj>` (or `make -n -W <file>`).
  `SIZE_OPT_CXX_SRC` objects get **`-Os` per object**, and an ad-hoc `-O3` shows dozens of
  spurious diffs. Always build a **control** set with unmodified headers first and require
  zero diffs against the baseline.
- To shadow a header, put the modified copy at `<dir>/ps2/math/vec_mat.h` and pass `-I<dir>`
  *before* `-Isrc`.
- To list every copy/assign site of a type, temporarily `T & operator=(const T &) = delete;` in
  the shadow and compile everything with `-fmax-errors=0`.
- Compare per function (instructions, loads, stores, lq/sq), normalizing branch targets.
  Static counts understate loops, so check loop bodies by hand.

## Proving an EE/VU0 asm rewrite on target (~6 s)

All of this happens in the scratchpad, with nothing added to the repo.

- The test `.cpp` includes the real header plus the HEAD version of the changed functions
  (`git show HEAD:<file>`) inside `namespace ref { ... }`. Qualify ref-internal calls
  (`ref::Foo`), since ADL on shared argument types makes them ambiguous.
- Use deterministic xorshift inputs, `memcmp` every output byte, and print `Tag: PASS/FAIL`.
  Call `SifInitRpc(0)` before `printf`, then `SleepThread()`.
- Build with the release flags. Link:
  `mips64r5900el-ps2-elf-g++ -T$PS2SDK/ee/startup/linkfile -O3 -o t.elf t.o <objs> -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128 -lkernel`
  (add `-lgraph -ldma` if needed).
- Back up emulog, run `PCSX2 -batch -elf t.elf` in a background shell with an `until grep`
  loop on the log (the verdict, or `TLB Miss|EXCEPTION`, plus a deadline), then `kill` the PID
  you started, since SleepThread never exits. Restore emulog.
- The same harness measured vsync timing: an NTSC field is 9609.6 T2 ticks = 59.94 Hz
  (`gs::PresentClock`).
