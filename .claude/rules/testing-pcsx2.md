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
- **`PCSX2 -batch` exits 30-90 s after the game quits.** The quit writes config.cfg, then
  restarts the EE, which emulog shows as `# Restart.` (only then, never at boot). A script can
  close PCSX2 at that line.
- **A `baseq2/pak0.pkz` (from `make pkz`) is loaded instead of `pak0.pak`.** The log says
  `Added packfile host:/baseq2/pak0.pkz (3307 files, compressed)`. To test the `.pak` path,
  move the pkz out of `baseq2/`. Don't leave a stale pkz behind after changing the pak.
- PCSX2 memory card slot 1: `~/Library/Application Support/PCSX2/memcards/Mcd001.ps2`.
  `Slot1_Enable = false` in the ini simulates "no card". Format notes are in
  [save-games.md](save-games.md).
- `[USB1] Type = hidkbd` attaches a host-passthrough USB keyboard. It reports itself as JIS,
  and its boot `Missing host mapping for QKey` warnings are harmless. It sends HID usage
  `0x34` for the host's `` ` `` key, never `0x35`. `in_keyboard` defaults to 1 in debug builds
  only, so a release build ignores it unless the config sets `in_keyboard "1"`.
- `Pad: DS2 Config Finished ... VS: Normal - VL: Normal` in the log means the vibration motors
  were mapped (`padSetActAlign`).

## Working with the user's machine

- Claude can't screenshot PCSX2 (`screencapture` is denied) or send it keystrokes (`osascript`
  is denied), **but the game can screenshot itself** (since 2026-10-12): `screenshot` reads the
  last finished frame out of GS VRAM into `baseq2/scrnshot/quakeNN.tga` (the first number
  free), and `screenshot <name>` into `scrnshot/<name>.tga`. Convert with
  `sips -s format png baseq2/scrnshot/<name>.tga --out <scratchpad>/<name>.png` and Read the
  PNG; delete `baseq2/scrnshot/` afterwards. Outside what the game draws (the PS2 BIOS,
  PCSX2's own UI, the memory card browser's icon) anything visual still needs the user's eyes.
- **A screenshot recipe that worked** (2026-10-12): a temporary block in `CDAudio_Play` that
  runs once, queuing `alias w10 "wait;..."` (ten waits), `alias w100 "w10;..."` (ten w10s),
  then `w100;w100;w100;screenshot demo;menu_main;w100;screenshot menu`: the attract demo with
  its HUD, then the main menu over it. `screenshot splash` queued in `Qcommon_Init` before
  `d1` catches the boot splash, since no frame has been presented yet and frame 0 still holds
  it (with a 16-bit framebuffer; a 32-bit one comes back black, cleared by `gs::Init`).
  `ps2_debug_overlays 0` and `ps2_show_fps 0` keep the panels out of a shot. A 16-bit
  framebuffer reads back at 5 bits a channel. Each shot is 860 KB at 640x448.
- `pcrtc_offsets = true` under `[EmuCore/GS]` (on in the user's ini) makes PCSX2 move the picture
  as the GS DISPLAY registers say (`ps2_screen_x`/`ps2_screen_y`); off, it never moves. `screenshot`
  reads VRAM, so it never shows the shift either way: `developer 1` logs each change (`GS: picture
  +8 pixels, -4 rows: DISPLAY DX 684, DY 42.`), and the rest wants eyes, on a TV above all.
- **A scripted shot usually repeats across builds:** the same queued script from the
  `CDAudio_Play` hook mostly shoots the same demo frame, so two builds' shots can compare pixel
  for pixel. The brightness sliders were checked that way (2026-10-24): the defaults' shot
  matched HEAD's in all 286,720 pixels, two runs back to back. A later run of the same build
  landed a few frames off (the weapon bob and a soldier moved, the walls identical), so host
  timing can move the demo too; compare a static region then, or prove a table change in a host
  harness. Anything that waits a frame out (`rs::FinishFrameInFlight`, a brightness change)
  shifts the demo for the shots after it. Shots are uncompressed 24-bit TGAs (18-byte
  header, BGR rows), so a mean pixel value is a few lines of Python: the demo's first shot read
  20.1 at the defaults, 64.0 at `vid_gamma 0.5`, 88.9 with `vid_contrast 2` too, 8.2 at 1.3.
- **Particles in the attract demo:** a series of shots from the `CDAudio_Play` hook, one every
  two `w100`s, catches the first soldier's blood in shots 3, 4 and 7 (the 4th under the red
  damage flash) and blaster sparks in 5 (2026-10-25). The `r_particles` looks were checked that
  way, as pairs five frames apart (`r_particles 2;w10;screenshot cN;r_particles 1;w10;screenshot
  hN`); without PIL, a 160x120 crop around the centre upscaled 4x by a few lines of Python (TGA
  rows in, PNG out through `zlib`) makes the squares readable.
- The user may be at the machine while a run is up, and may close it. Don't relaunch PCSX2
  after they close a run without a reason.
- Runs read and rewrite `baseq2/config.cfg` (tracked: the default the release packages ship; the
  quit writes archived cvars back, byte for byte the same when nothing changed).
  **Back it up before a scripted or perf run and restore it after.** The user's config has
  `ps2_savedevice "mc"`. Start every run of a comparison from the same config.
- **Every level a run loads autosaves slot 0 to the save device**, and with that config it is
  the PCSX2 card (`Mcd001.ps2`), which the user also uses for other projects (a `Q1PS2` folder).
  Back the card up before a run that loads a map, or set `ps2_savedevice "host"` in the
  backed-up config for the run (slot 0 then lands in `baseq2/save/`, replacing that one).

## Scripting a session

`baseq2/autoexec.cfg` is silently ignored. `FS_ExecAutoexec` only execs it if `Sys_FindFirst`
finds it, and the PS2 `Sys_FindFirst` (src/ps2/system/sys.cpp) always returns nullptr.

- Put commands behind the commented-out `Cbuf_AddText(...)` near the end of `Qcommon_Init`
  (src/common/common.c), or define aliases in `baseq2/config.cfg`. Revert both afterwards.
- Without `alias d1 ""` (or a first segment that replaces it), the `d1` attract loop takes over.
- Working recipe: alias definitions in config.cfg (no execution line), then
  `Cbuf_AddText("<first alias>\n")` right after the `d1` block in `Qcommon_Init`, with a first
  segment of `killserver;wait;demomap demo1.dm2` (this mirrors `ps2_perftest`).

Four traps. Each looks like a game crash or hang:

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
3. **`CDAudio_Play` runs while the level's loading plaque is still up.** It comes down at the
   first valid frame after. Stopping the attract loop from a command queued there, without
   `SCR_EndLoadingPlaque` (the console toggle calls it; `CL_Disconnect` doesn't), left the
   plaque up: no frame was drawn, so the map cycle, which runs from `PS2_EndFrame`, waited
   out the 150 s plaque timeout.
4. **`wait` runs twice per frame** (`Cbuf_Execute` in both `Qcommon_Frame` and
   `CL_SendCommand`), and much more often with no map up. Frame-count waits are a rough clock.

**Menus can be tested without a screen or keys.** A temporary command that opens the page and
calls `Key_Event(key, true, 0)` then `Key_Event(key, false, 0)` per press goes through the real
menu code; printing `cls.key_dest`/`cls.state` around it shows where it left the client. Spin
controls change with left/right only: Enter does nothing on them (qmenu's `Menu_SelectItem`).
The debug page was tested this way (2026-10-12), fired from the `CDAudio_Play` hook. A
`screenshot` after the presses shows the page itself. A temporary `tkey <keynum>` command (one
`Key_Event` press and release) lets the queued script drive the pages itself, with a `w10`
between a press and its `screenshot`. keys.h: Enter 13, Escape 27, Up 128, Down 129, Left 130,
Right 131. That checked the main menu's four items, Down's wrap and two Options rows, whose
cvars the script then printed by name (2026-10-23). The controls page the same way (2026-10-25):
`menu_keys`, two Downs, Enter (13) for the grab, `tkey 204` (JOY2, circle's game key) to bind
and `tkey 127` (Backspace, triangle's menu key) to clear, a shot after each. What that can't
reach is input.cpp's choice of key per button, which needs a pad. Killing PCSX2 after the last shot, rather
than quitting, leaves config.cfg and the card unwritten. PCSX2 still rewrites its ini's window
state on the way out, so restore the ini too.

## Built-in tests (debug builds; set the cvar via config.cfg or the Qcommon_Init hook)

- `ps2_testmaps 1`: MapCycle over all 39 maps. Prints per-map memory lines plus a "load peak"
  line per transition. ~15 min, each level's autosave holding the save notice up a second. See
  [memory-budget.md](memory-budget.md).
- On a console without a keyboard, the Options menu's debug page (debug_menu.cpp) starts
  `ps2_perftest` and `ps2_testmaps`, toggles `ps2_logfile` and `ps2_test_quit`, runs the IOP
  and save reports, and takes a screenshot without the menu in it.
- **All three tests quit when they finish unless `ps2_test_quit` is 0** (tests/test_quit.cpp).
  The save test quits too, since 2026-10-12; it puts `ps2_savedevice` back first, so the
  config the quit writes keeps the user's `"mc"`. Each quit writes `config.cfg` (host) and its
  card copy: restore both after a run. At 0, one session ran a map cycle, the save test, the perf
  test and a second map cycle back to back (2026-10-12, map list cut to two), each started when
  the one before ended.
- `ps2_testsaves 1|2|3`: save/level-change/load on the boot drive (1: host files under PCSX2),
  then the memory card too (2), and load-across-a-rebuild from the boot drive (3). Ends in
  `SaveTest: PASS`/`FAIL`. Run 1 after touching `g_save.c` or saved game structs, and 2 after
  touching the save devices or the notice. It runs from the main loop, between frames: its slot
  checks read the device under the save notice. `set ps2_testsaves "2"` at the end of a copy of
  the config starts it; the quit drops the line again.
- `ps2_perftest 1`: unattended frame-log capture of the demo loop. See
  [performance.md](performance.md).
- `ps2_testcube 1` (VU1 path smoke test), `ps2_testcin 1` (cinematics).
- **The USB boot branch:** run a copy of the ELF from a directory with no `baseq2/` (e.g. the
  scratchpad), with HDD disabled. The `host:` probe misses, so the IOP inventory (PCSX2's BIOS
  leaves nothing in the way, so no reset), optional HDD probe, BDM module chain and 10 s
  `mass0:`-`mass9:` poll all run, ending on the "No game data found!" screen with
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
  storage bring-up. A directly launched ELF (a host path) searches the HDD first, so
  also disable HDD if it has a matching installation; one launched from `mass:` never
  searches it. The embedded BDM modules probe `mass0:` through `mass9:`.
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
- **Scripted runs from the emulated stick, no launcher** (2026-10-19): launch a host ELF
  from a folder with no `baseq2/` (the `host:` probe misses; HDD off), with `[USB2] Type = Msd`
  on the image. A USB boot reads the stick's config.cfg when it says `ps2_savedevice "usb"`, and
  leaves the card alone then; with `"mc"` (the shipped default) it reads the card's first, so with
  a card in, the stick's test cvars are ignored and autosaves go to the card, unless
  **`Slot1_Enable = false`**. Put the cvars in the image's `baseq2/config.cfg`:
  `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -mountpoint <dir> <img>`,
  edit, `hdiutil detach <dir>`. Give each run an APFS clone of a pristine image (`cp -c`), so
  every run starts from the same bytes. Debug with the I/O log (`ps2_logfile "0"`), the first
  8 MapCycle loads from the pkz took 95.5 s in all (base1 22.4 s, 10.68 MB read); the boot
  found the data at 658 ms.
- **HDD boot:** the current source embeds DEV9/ATAD/APA/PFS and probes HDD between `host:`
  and USB. ELFs packaged before HDD support need rebuilding.
  Enable `[DEV9/Hdd] HddEnable = true` and set `HddFile` to a PCSX2 HDD image. Create and
  format a new blank image with uLaunchELF's HDD Manager, create a main PFS partition
  (e.g. `+Q2PS2`, 512 MiB), and copy the new ELF and `baseq2/` side by side into it.
  With `HostFs = false`, launch it through uLaunchELF. The game mounts PFS itself, first
  unmounting whatever a launcher's PFS driver left at `pfs0:`. Successful bring-up logs
  `game data on pfs0:/.../baseq2 (HDD partition hdd0:+Q2PS2)` and keeps that mount alive.
  Canonical `hdd0:+Q2PS2:pfs:/dir/quake2.elf` and browser `hdd0:/+Q2PS2/dir/quake2.elf`
  paths retain the launch partition; bare `pfsN:` paths fall back to enumeration.
  The launch partition's folder/root comes first; other main PFS partitions are searched
  for the same folder before any roots. Missing HDD/drivers or no matching data falls
  through to USB. An ELF launched from `mass:` skips the HDD altogether (since 2026-10-12). The user verified emulated PFS boot/reads on 2026-10-07 and observed
  noticeably faster loading than USB. The missing HDD log still needs session-specific
  verification after adding HDD-only `fileXioSync` after log closes; see the PFS
  metadata-flush trap in [ps2-platform.md](ps2-platform.md). Builds from ed03e18 up to the
  2026-10-09 fix also stopped every USB/HDD log after its header line: they required
  `close() == 0`, and iomanX returns the slot number (see ps2-platform.md). The fix was
  verified on emulated USB (uLaunchELF, `mass:quake2_debug.elf`): a full 1349-line
  `quake2.log` from boot to quit, and no logging warning in emulog.
- **Launcher states without a launcher** (2026-10-12): a small test ELF in the scratchpad brings
  the IOP up as a launcher would, then starts `mass:/quake2_debug.elf` from the USB image with
  libelf-loader's `LoadELFFromFileWithPartitionNoReset` (in `libelf-loader.a`, not declared in
  its header; declare it by hand). It links `-lelf-loader -lpatches -lpad -lmc -lfileXio
  -lkernel` with the IRXs from `$PS2SDK/iop/irx` embedded by `bin2c`, the way wLaunchELF's
  `Reset()` loads them. Launched with `PCSX2 -batch -elf`, it runs from `host:`. Each state it
  made, and what the game did:
  - wLaunchELF 2022-2026/05 (its BDM stack, ROM SIO2MAN and PADMAN, `mcman-old`/`mcserv-old`):
    no reset, every module reused, game data 44 ms after `main()` against 655 ms from a fresh
    IOP.
  - The same, with pad port 0 left open: its DMA rewrote the game's `.text` until the boot
    closed the port (read through the uncached segment, 100 ms apart, before and after).
  - The elf-loader with reset (wLaunchELF since 2026/05): the ROM's card drivers reused.
  - wLaunchELF 2019 (`usbhdfsd`): the data read through it.
  - ps2sdk's `sio2man` alone, and most of the IOP's memory taken: each reset, then booted.
  Keep the launcher's `padArea` buffer and the ELF's address ranges in mind when reading a
  probe: the launcher's addresses are the game's once it runs.
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

## Building an older commit beside the tree

For a size or codegen comparison against an older commit without touching the checkout (a test
may be running from `build/debug`): `git archive <commit> | tar -x -C <scratch>/<commit>`, then
`cp -R src/tools/{vclpp,vu-checker,miniz}/. <scratch>/<commit>/src/tools/<same>/` (an archive
holds no submodule contents), and `make -j4` there. The Makefile's paths are relative, so it
builds its own `build/tools/vclpp` and ELF. Two such builds took about four minutes each
alongside a running map cycle (2026-10-24). `mips64r5900el-ps2-elf-size` on the unstripped ELFs
then gives text/data/bss per commit.

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
- **`quake2.log` stamps count from `main()`** (since 2026-10-12, `Sys_Milliseconds` is first
  called there). Older console logs started counting at the boot's first USB poll, so their
  first stamp (about 0.65 s) leaves out the IOP reset and the module loads. The `IOP boot:`
  lines, printed at `Sys_Init`, give each boot step's time.
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
  fake only the hardware class. input.cpp's `IN_Move` (the stick dead zones) works this way
  with fake `ps2/input/pad.h` (settable stick floats plus libpad's `PAD_*` bits), `keyboard.h`,
  `rumble.h` and `ps2/renderer/profile.h`, and a `Cvar_Get` that merges flags like id's. Set
  the anonymous-namespace `s_gamepad` directly. Its 10,428 checks caught HEAD's 0.15 step
  response, rest drift from 20 counts up, and 5 of 5 seeded mutants (2026-10-11).
- **Emulate the EE FPU** in any math harness: `1/sqrt(0)` must give FLT_MAX, not inf (see
  [ps2-platform.md](ps2-platform.md)). Otherwise target-only bugs won't reproduce.
- **ASan can't see an overrun that stays inside one object** (a member array reading into
  the next member). Heap-allocate the object under test, so writes past its last member hit
  the redzone, and poison what must not be read with `ASAN_POISON_MEMORY_REGION` from
  `<sanitizer/asan_interface.h>` (it handles a partial first granule), unpoisoning after the
  call. Give outputs exact-size heap buffers with canaries. Then prove the harness by seeding
  defects into a shadow copy of the header (`-I<mutant dir>` first): the `half_band.h`
  harness caught 7 of 7 (off-by-ones, an over-long memmove, a wrong tap, a 32-bit overflow).
- **The pkz reader** (fs/pkz_pack.cpp) runs on the host the same way: include the `.cpp` with a
  fake `ps2/common.h` (MAX_QPATH/MAX_OSPATH/MAX_FILES_IN_PACK, Com_Printf, a Sys_Error that
  longjmps when a test expects it, and a `ps2::heap` that checks every Free's size and tag
  against its Alloc). macOS has no `fopencookie`: `#define fopencookie` and `setvbuf` to
  stand-ins *after* including `<cstdio>`, keep the cookie callbacks in a table, and call them
  the way newlib's unbuffered fread/fseek/ftell do. Link the miniz sources with the game's
  `save/miniz_cfg`. Keep the pack's path under `MAX_OSPATH`: stored files reopen the pack by
  name, and the scratchpad's absolute paths are longer (run from the data directory). The
  2026-10-11 harness checked all 3307 files of pak0 whole, 537 as streams through the BSP and
  MD2 loaders' seek patterns and 208K random reads/seeks, zips from Info-ZIP and Python's
  `zipfile` (local extra fields, a comment, directory entries), one damaged and five malformed
  packs, and caught 6 of 6 seeded mutants (restart index, stale dictionary, dictionary window,
  wrap copy, stale input, local extra field). macOS has no `timeout`: bound a mutant run with
  `perl -e 'alarm 600; exec @ARGV' ...`, and check the exit code isn't 127.
- Code with EE/VU0 inline asm can't run on the host. Use the standalone test ELF recipe in
  [performance.md](performance.md).
