# Testing: PCSX2, built-in tests, host harnesses

## PCSX2 setup and logs

- App: `/Applications/PCSX2.app/Contents/MacOS/PCSX2`, launched as `PCSX2 -batch -elf <elf>`
  (`make run`). `host:` maps to the ELF's directory, and `make run` symlinks
  `build/<config>/baseq2` → the repo's `baseq2/`.
- Development through `host:` needs `[EmuCore] HostFs = true` in
  `~/Library/Application Support/PCSX2/inis/PCSX2.ini`. Disable it to test HDD/USB loading.
  Game stdout needs `[Logging] EnableIOPConsole = true`,
  because ps2sdk stdout goes through IOP fio, plus `EnableFileLogging = true` to land in
  `~/Library/Application Support/PCSX2/logs/emulog.txt`. Engine lines carry a `[Q2]` prefix.
- **Edit PCSX2.ini only while PCSX2 is closed.** It rewrites the file on exit.
- Every launch overwrites `emulog.txt`. Copy it out before the next launch if you need it.
- PCSX2 memory card slot 1: `~/Library/Application Support/PCSX2/memcards/Mcd001.ps2`.
  `Slot1_Enable = false` in the ini simulates "no card". Format notes are in
  [save-games.md](save-games.md).
- `[USB1] Type = hidkbd` attaches a host-passthrough USB keyboard. It reports itself as JIS,
  and its boot `Missing host mapping for QKey` warnings are harmless. It sends HID usage
  `0x34` for the host's `` ` `` key, never `0x35`.
- `Pad: DS2 Config Finished ... VS: Normal - VL: Normal` in the log means the vibration motors
  were mapped (`padSetActAlign`).

## Working with the user's machine

- Claude can't screenshot PCSX2 (`screencapture` is denied) or send it keystrokes (`osascript`
  is denied). Menus, popups, icons and anything visual need the user's eyes. Say so instead
  of guessing.
- The user may be at the machine while a run is up, and may close it. Don't relaunch PCSX2
  after they close a run without a reason.
- Runs read and rewrite `baseq2/config.cfg` (gitignored; the quit writes archived cvars back).
  **Back it up before a scripted or perf run and restore it after.** The user's config has
  `ps2_savedevice "mc"`. Start every run of a comparison from the same config.

## Scripting a session

`baseq2/autoexec.cfg` is silently ignored. `FS_ExecAutoexec` only execs it if `Sys_FindFirst`
finds it, and the PS2 `Sys_FindFirst` (src/ps2/system/sys.cpp) always returns nullptr.

- Put commands behind the commented-out `Cbuf_AddText(...)` near the end of `Qcommon_Init`
  (src/common/common.c), or define aliases in `baseq2/config.cfg`. Revert both afterwards.
- Without `alias d1 ""` (or a first segment that replaces it), the `d1` attract loop takes over.
- Working recipe: alias definitions in config.cfg (no execution line), then
  `Cbuf_AddText("<first alias>\n")` right after the `d1` block in `Qcommon_Init`, with a first
  segment of `killserver;wait;demomap demo1.dm2` (this mirrors `ps2_perftest`).

Three traps. Each one looks like a game crash, and all of them reproduce at HEAD:

1. **A quoted token of 128+ chars corrupts memory.** id's `COM_Parse` truncates at
   `MAX_TOKEN_CHARS` but then writes `com_token[128] = 0`, one byte past the array. In this
   build that lands on `_BigShort`, and the next `BigShort()` jumps into `fire_bfg` (TLB miss,
   `addr=0x54`). Keep alias values well under 128 chars. Build wait chains hierarchically
   (`w10` = 10 waits, `w100` = 10× `w10`). The engine bug itself is unfixed.
2. **Never leave `wait`s queued across a map/demo load.** The server's `precache` stufftext
   queues behind them, the client runs `CL_PrepRefresh` before `CM_LoadMap`, and the renderer
   asserts "Loading a world model the collision model is not holding!". End a segment at
   `map`/`demomap` and continue from an event, e.g. a temporary `Cbuf_AddText` of the next
   alias from `CDAudio_Play`, which `CL_PrepRefresh` calls once loading is done.
3. **`wait` runs twice per frame** (`Cbuf_Execute` in both `Qcommon_Frame` and
   `CL_SendCommand`), and much more often with no map up. Frame-count waits are a rough clock.

## Built-in tests (debug builds; set the cvar via config.cfg or the Qcommon_Init hook)

- `ps2_testmaps 1`: MapCycle over all 39 maps. Prints per-map memory lines plus a "load peak"
  line per transition. ~15 min. See [memory-budget.md](memory-budget.md).
- `ps2_testsaves 1|2|3`: save/level-change/load on host files (1) or the memory card (2), and
  load-across-a-rebuild (3). Ends in `SaveTest: PASS`/`FAIL`. Run 1 after touching
  `g_save.c` or saved game structs.
- `ps2_perftest 1`: unattended frame-log capture of the demo loop. See
  [performance.md](performance.md).
- `ps2_testcube 1` (VU1 path smoke test), `ps2_testcin 1` (cinematics).
- **The USB boot branch:** run a copy of the ELF from a directory with no `baseq2/` (e.g. the
  scratchpad), with HDD disabled. The `host:` probe misses, so the IOP reset, optional HDD
  probe, BDM module chain and 10 s `mass0:`-`mass9:` poll all run, ending on the
  "No game data found!" screen with
  `USB volumes mounted: none` when no emulated USB drive is attached. The run stops before
  `Qcommon_Init` and never touches `config.cfg`. Its
  `argv[0]` is an absolute host path, longer than the 50-char ELF folder cap, so only the
  root is searched.
- **PCSX2 emulated USB storage (3.20 debug runtime verified via uLaunchELF):**
  `[USB2] Type = Msd`, `Msd_subtype = 0` (Iomega Zip-100 / Generic), and
  `Msd_ImagePathMsd = /absolute/path/usb.img` attach a file-backed mass-storage device;
  USB1 can remain `hidkbd`. PCSX2 uses raw 512-byte sectors and opens the image `r+b`,
  so use a writable raw disk image; an MBR/FAT32 layout is a conservative test choice.
  Put `baseq2/` at its root for a directly launched host ELF. Disable `[EmuCore] HostFs`
  or launch from a folder without host game data, since the successful host probe skips
  storage bring-up. Also disable HDD if it has a matching installation: HDD takes
  precedence over USB. The embedded BDM modules probe `mass0:` through `mass9:`.
  Attaching the image doesn't launch its ELF; loading the ELF itself from `mass:` needs
  a homebrew launcher such as wLaunchELF. See PCSX2's
  [usb-msd.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/USB/usb-msd/usb-msd.cpp)
  and [USB.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/USB/USB.cpp).
- **Creating a USB image on macOS:** `hdiutil create -size 256m -fs 'MS-DOS FAT32'
  -layout MBRSPUD -volname Q2PS2DEBUG -srcfolder <package-folder> -format UDTO
  -nospotlight <output>.cdr`, then rename `.cdr` to `.img`. Despite the UDTO format's
  "DVD/CD master" name, the explicit filesystem/layout produce raw MBR/FAT32 sectors,
  without a DMG trailer or an ISO9660 filesystem. The 3.20 debug package produced a
  256 MiB image with `quake2.elf` and `baseq2/` at the root; its partition/boot signatures,
  FAT copies, and SHA-256 hashes of all four packaged files were verified by reading the
  FAT32 image directly. Launch `mass:/quake2.elf` in uLaunchELF. The user verified this
  on 2026-10-07; the log reports `mass0:/baseq2` ready after 600 ms and reads from its pak.
  Initial debug-run loading-screen summaries report 120-131 seconds. These are instrumented
  observations from one session, not a storage-throughput benchmark. Its emulator log was
  preserved as `github_rel_3.20/q2ps2_debug_3.20_usb-emulog.txt` before another launch could
  overwrite it. Keep build, map, config, and logging settings matched in comparisons.
- **Reusable test USB image:** `python3 src/tools/scripts/make_usb_image.py` packages the
  existing stripped debug/release ELFs at the volume root as `quake2_debug.elf` and
  `quake2_release.elf`, beside `baseq2/`; unstripped ELFs are omitted. It copies the working
  data/config except `baseq2/pak0/` when `pak0.pak` is present, and includes only `.adp`
  files under `baseq2/music/` (case-insensitive, including subdirectories). macOS `hdiutil`
  produces an auto-sized writable MBR/FAT32 image with volume name `Q2PS2` at
  `build/quake2-usb.img`; `--output`, `--size-mib`, and `--force` override defaults.
  The initial full-data run produced an 832 MiB image: raw FAT32 verification checked all
  96 included files by SHA-256, with no missing/extra files or directories. Sixteen fixture
  cases cover selection, sizing, errors and safe replacement. Both ELFs now sit next to
  the shared data, requiring no folder fallback. Close PCSX2 before replacing an image.
- **HDD boot:** the current source embeds DEV9/ATAD/APA/PFS and probes HDD between `host:`
  and USB. ELFs packaged before HDD support need rebuilding.
  Enable `[DEV9/Hdd] HddEnable = true` and set `HddFile` to a PCSX2 HDD image. Create and
  format a new blank image with uLaunchELF's HDD Manager, create a main PFS partition
  (e.g. `+Q2PS2`, 512 MiB), and copy the new ELF and `baseq2/` side by side into it.
  With `HostFs = false`, launch it through uLaunchELF. The game resets the IOP and mounts
  PFS itself: the launcher mount does not survive. Successful bring-up logs
  `game data on pfs0:/.../baseq2 (HDD partition hdd0:+Q2PS2)` and keeps that mount alive.
  Canonical `hdd0:+Q2PS2:pfs:/dir/quake2.elf` and browser `hdd0:/+Q2PS2/dir/quake2.elf`
  paths retain the launch partition; bare `pfsN:` paths fall back to enumeration.
  The launch partition's folder/root comes first; other main PFS partitions are searched
  for the same folder before any roots. Missing HDD/drivers or no matching data falls
  through to USB. The user verified emulated PFS boot/reads on 2026-10-07 and observed
  noticeably faster loading than USB. The missing HDD log still needs session-specific
  verification after adding HDD-only `fileXioSync` after log closes; see the PFS
  metadata-flush trap in [ps2-platform.md](ps2-platform.md). Builds from ed03e18 up to the
  2026-10-09 fix also stopped every USB/HDD log after its header line: they required
  `close() == 0`, and iomanX returns the slot number (see ps2-platform.md). The fix was
  verified on emulated USB (uLaunchELF, `mass:quake2_debug.elf`): a full 1349-line
  `quake2.log` from boot to quit, and no logging warning in emulog.
- **Storage selection runtime harness:** include `iop_boot.cpp` with stub SDK headers
  and IOP/mount calls, then run from a directory holding folders literally named `pfs0:`,
  `mass0:`, `mass1:`... (macOS allows the colon). `fopen` then takes the probe paths as
  relative ones. The HDD implementation passed 84 cases under ASan/UBSan: launch-path
  parsing/bounds, host/HDD/USB precedence, partition filtering and mount lifetime,
  optional HDD failure fallback, mandatory initialization failures, and keyboard USB
  startup after HDD boot. `SyncGameDataDevice()` tests cover no-op before boot/host/USB/
  failed HDD, selected-mount sync and raw errors, and clearing stale state on another
  detection. This verifies selection logic, not the real drivers or DMA;
  `make` and `make release` remain the target compile/link checks.
- **HDD log sync runtime harness:** include `debug/log_file.cpp` with fake SDK headers,
  real host-backed open/seek/write/close wrappers and a mocked `SyncGameDataDevice()`.
  A host `close()` returns 0, unlike iomanX's slot number, so make the wrapper return a
  positive value on success or the harness can't see that trap;
  the boot harness checks its real fileXio implementation. Twenty-four ASan/UBSan cases
  verify header/append/fatal persistence ordering, no sync for host/USB or disabled
  logging, and disabling after open/seek/write/
  partial-write/close/sync failures. Every opened descriptor closes before HDD sync, even
  after a failed operation; an open failure never syncs. Real PFS persistence still needs
  an emulator run with the new ELF, checking `quake2.log` beside `baseq2/` after shutdown.

## Quiet map for renderer work

```
killserver ; deathmatch 1 ; cheats 1 ; map base1
```

Quake II has no `nomonsters`. `deathmatch 1` makes `monster_start` free every monster.
`deathmatch` and `cheats` are both `CVAR_LATCH`, so set them before `map`. `maxclients` is
forced to 8 under deathmatch, which is harmless. `cheats 1` is needed for
`noclip`/`give all`/`notarget` in DM. The BSP world is unchanged; only some items and
triggers differ. Alternatives: `notarget` in single player (monsters still animate and cost
draws), or `ps2_skip_entities 1` to drop every entity model from the draw.

## Crash triage

- **On a console, `quake2.log` is the only report.** Fatal reports (pipeline hang dump, stack
  traces, out-of-memory stats) go through `ps2::debug::DumpPrintf` (log_file.h), to stdout and
  the log. Anything new printed on a fatal path must use it, not `printf`: the first hardware
  pipeline hang (2026-10-07) logged only `Sys_Error: Render pipeline hang: ... See the pipeline
  dump above.`, because the dump went to stdout alone.
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
- Debug builds print an EE exception report (cause, EPC, BadVAddr, stack). Resolve addresses
  against **the same build's** `quake2_unstripped.elf`:
  `mips64r5900el-ps2-elf-addr2line -f -C -e build/debug/quake2_unstripped.elf <addr>` or
  `build/tools/symbolize < emulog.txt`.
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
- **Known flake, never game code:** a `TLB Miss` in `_request_end` (ps2sdk `sifrpc.c`,
  `SIF_CMD_RPC_END`) during `host:` file I/O, usually right after a
  `PackFile: host:/baseq2/pak0.pak` line. The signature is one to three
  `TLB Miss, pc=<same> addr=0x10|0x18 [load|store]` lines: `cd->hdr.pkt_addr` is already null,
  one RPC completed twice under PCSX2's faked IOP HostFs. A second face of it is a wild `pc`
  below `.text` (0x100000), e.g. `pc=0x82000 addr=0x0`, which is usually fatal. The pc moves
  per build, so resolve it:
  ```sh
  mips64r5900el-ps2-elf-objdump -d build/debug/quake2_unstripped.elf > q2.dis
  L=$(grep -n "^  1ba87c:" q2.dis | cut -d: -f1)
  awk -v n="$L" 'NR<=n && /^[0-9a-f]+ </ {f=$0} NR==n {print f}' q2.dis | c++filt
  ```
  **Re-run before investigating.** Across three identical 39-map cycles, one died, one was
  clean, and one logged it and still finished. The renderer can't cause it: VIF1 chains only
  read RAM. A capture from a run that logged it is still valid.

## Host harnesses (runtime logic only; `make` stays the compile check)

- **Renderer sources:** in the scratchpad, make a `fakeinc/` with four headers and compile
  with `-Ifakeinc -I<repo>/src` (quoted includes miss the source's own dir, so `-I` order
  decides): `ps2/common.h` (`MAX_QPATH`, `PS2_QUAKE_DEBUG`, `Com_Printf`/`Com_DPrintf`/
  `Sys_Error` decls, `PS2_Assert`/`PS2_AssertMsg`), `tamtypes.h`, `gs_psm.h` (copy the
  `GS_PSM_*` values), `draw_buffers.h` (a `texbuffer_t`). Don't fake `texture.h`/`vram.h`.
  `#include` the **.cpp** so the test can walk anonymous-namespace statics. Stub `Sys_Error`
  as a counter (fatal paths `return` after it) and make the assert stub `exit(1)`. Use clang
  with ASan/UBSan.
- **Client-side sources** (e.g. input/rumble.cpp) compile against the *real* `client/client.h`
  chain on host clang in C++ mode. Make the fake `ps2/common.h` wrap
  `#include "common/q_common.h"` in `extern "C"`, define the globals the source touches, and
  fake only the hardware class.
- **Emulate the EE FPU** in any math harness: `1/sqrt(0)` must give FLT_MAX, not inf (see
  [ps2-platform.md](ps2-platform.md)). Otherwise target-only bugs won't reproduce.
- **ASan can't see an overrun that stays inside one object** (a member array reading into
  the next member). Heap-allocate the object under test, so writes past its last member hit
  the redzone, and poison what must not be read with `ASAN_POISON_MEMORY_REGION` from
  `<sanitizer/asan_interface.h>` (it handles a partial first granule), unpoisoning after the
  call. Give outputs exact-size heap buffers with canaries. Then prove the harness by seeding
  defects into a shadow copy of the header (`-I<mutant dir>` first): the `half_band.h`
  harness caught 7 of 7 (off-by-ones, an over-long memmove, a wrong tap, a 32-bit overflow).
- Code with EE/VU0 inline asm can't run on the host. Use the standalone test ELF recipe in
  [performance.md](performance.md).
