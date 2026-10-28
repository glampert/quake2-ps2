---
paths:
  - "src/ps2/debug/**"
  - "src/ps2/renderer/vu1.*"
---

# Crash triage: the hang report, VU1 forensics, the exception handler

Companion to [testing-pcsx2.md](testing-pcsx2.md), which has the short form: `quake2.log` is
the only report on a console, how to symbolize an address, and the known `_request_end` flake.

## The pipeline hang report

- The first hardware pipeline hang (2026-10-07) logged only `Sys_Error: Render pipeline hang:
  ... See the pipeline dump above.`, because the dump went to stdout alone. That is why every
  fatal-path print goes through `ps2::debug::DumpPrintf` (log_file.h) now.
- **A pipeline hang on a console used to end on a solid green screen**, with no `Sys_Error`
  text drawn, though the log had the `Sys_Error` line. `ScrInit` reset the GS, then its PATH3
  transfers couldn't get through a GIF still held by the wedged pipeline, so `DmaWaitGif`
  spun. `ScrInit` now runs `ResetGraphicsPaths` first (scr_print.cpp), in libgs's
  `GsResetPath` order: stop DMA channels 1 and 2 under a DMAC suspend, then reset VIF1, VU1
  (if the thread has COP2) and the GIF. Confirmed on hardware 2026-10-09: the same hang now
  shows the error on screen. PCSX2 can't wedge the GIF, so only a console shows the
  difference. Any new fatal-path screen must go through `ScrInit`.
- **To exercise the pipeline hang report for real** (not a zero timeout, whose registers show
  a finished DMA): emit a DIRECT block whose GIF tag promises more than it carries, e.g.
  `GIF_SET_TAG(0x7FFF, 1, 0, 0, GIF_FLG_IMAGE, 0)` plus one qword, with the renderer's
  `OpenDirect`/`DirectCursor`/`SetDirectCursor`/`CloseDirect` on the Nth 3D frame, after
  `RenderWorldModel` so MSCALs precede it. PCSX2 wedges like hardware would: `D1_CHCR STR=1`,
  VIF1 `VGW=1` on a FLUSH, GIF `APATH=PATH2 OPH=1`, and the chain trail flags the packet.
  PCSX2 reads the GIF tag registers and VPU-STAT as 0; only a console fills those in.
- **VU1 memory that repeats one output shape** throughout means a microprogram looped on a
  corrupt count, its output wrapping around all 1024 qwords, constants included. In the
  2026-10-10 console log, five particle-sprite qwords filled all of it. Simulate the program's
  writes (start, stride, count, wrap at 1024) against the dumped memory. Only TOP 8 with count
  0x7FFF matched every qword, and it put the XGKICK on the qword the `GIF_TAG` registers held:
  no EOP anywhere, so PATH1 never closed. The cause was a particle batch overrun (see
  [vu-microprograms.md](vu-microprograms.md)). The dump now flags a particle count out of range.

## Log stamps

`quake2.log` stamps count from `main()` (since 2026-10-12, `Sys_Milliseconds` is first called
there). Older console logs started counting at the boot's first USB poll, so their first stamp
(about 0.65 s) leaves out the IOP reset and the module loads.

## The exception handler

- **The handler can't print from where it runs.** libeedebug calls it at exception level (EXL
  set, its own stack). No SIF RPC can complete there, and newlib's `printf` faults: its state
  pointer reads null. The old handler died that way: a burst of `TLB Miss` at 0x0-0x68 inside
  `printf`, `_vfprintf_r` and `__retarget_lock_acquire_recursive`, and no report. Now the
  handler only copies the frame's registers. It then points the frame's EPC and `$sp` at
  `ReportCrash`, which has its own 16 KB stack, and returns. libeedebug's `_ee_load_frame` +
  `eret` resumes the faulting thread there as ordinary thread code, which writes the log, the
  screen and stdout. Keep anything that needs the IOP out of `OnException`.
- To test the handler, add a temporary `teq $zero, $zero` (an unconditional trap, cause 13).
  `move $sp, $zero` before it gives a smashed stack. A null store or load won't do: PCSX2 logs
  `TLB Miss ... [store]` and skips the access without raising the exception. For the same
  reason the reporter's retry path (a fault while it walks the stack) can't be exercised in
  PCSX2.

## Resolving the known flake's `pc`

The `_request_end` flake (ps2sdk `sifrpc.c`, `SIF_CMD_RPC_END`) shows `cd->hdr.pkt_addr`
already null: one RPC completed twice under PCSX2's faked IOP HostFs. Its `pc` moves per build,
so resolve it against the same build:

```sh
mips64r5900el-ps2-elf-objdump -d build/debug/quake2_unstripped.elf > q2.dis
L=$(grep -n "^  1ba87c:" q2.dis | cut -d: -f1)
awk -v n="$L" 'NR<=n && /^[0-9a-f]+ </ {f=$0} NR==n {print f}' q2.dis | c++filt
```
