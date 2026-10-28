# Testing: PCSX2, built-in tests, host harnesses

The detail lives in four files this one points to. Read the one you need before a run:

- [testing-recipes.md](testing-recipes.md): scripted screenshot sessions (the `CDAudio_Play`
  hook, what repeats across builds, measuring a shot), driving menus without keys, building an
  older commit beside the tree.
- [testing-storage-boot.md](testing-storage-boot.md): the USB and HDD boot paths in PCSX2
  (emulated stick and HDD images, launcher states, scripted runs from the stick), the
  background-loader test, and the storage/log host harnesses.
- [crash-triage.md](crash-triage.md): the pipeline hang report and how to provoke it, VU1 dump
  forensics, the exception handler and how to test it.
- [host-harnesses.md](host-harnesses.md): the fake-header recipes for renderer, client-side and
  pkz sources, the EE FPU emulation, ASan tricks, seeded-mutant proving.

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
  is denied), **but the game can screenshot itself**: `screenshot` reads the last finished
  frame out of GS VRAM into `baseq2/scrnshot/quakeNN.tga` (the first number free), and
  `screenshot <name>` into `scrnshot/<name>.tga`. Convert with
  `sips -s format png baseq2/scrnshot/<name>.tga --out <scratchpad>/<name>.png` and Read the
  PNG; delete `baseq2/scrnshot/` afterwards. `ps2_debug_overlays 0` and `ps2_show_fps 0` keep
  the panels out of a shot. Outside what the game draws (the PS2 BIOS, PCSX2's own UI, the
  memory card browser's icon) anything visual still needs the user's eyes. Worked recipes are
  in [testing-recipes.md](testing-recipes.md).
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
- Menus can be driven without a screen or keys, through `Key_Event` or a temporary `tkey`
  command: see [testing-recipes.md](testing-recipes.md).

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

## Built-in tests (debug builds; set the cvar via config.cfg or the Qcommon_Init hook)

- `ps2_testmaps 1`: MapCycle over all 39 maps. Prints per-map memory lines plus a "load peak"
  line per transition. ~15 min, each level's autosave holding the save notice up a second. See
  [memory-budget.md](memory-budget.md).
- On a console without a keyboard, the Options menu's debug page (debug_menu.cpp) starts
  `ps2_perftest` and `ps2_testmaps`, toggles `ps2_logfile` and `ps2_test_quit`, runs the IOP
  and save reports, and takes a screenshot without the menu in it.
- **All three tests quit when they finish unless `ps2_test_quit` is 0** (tests/test_quit.cpp).
  The save test quits too; it puts `ps2_savedevice` back first, so the config the quit writes
  keeps the user's `"mc"`. Each quit writes `config.cfg` (host) and its card copy: restore both
  after a run. At 0, several tests can run back to back in one session, each started when the
  one before ended.
- `ps2_testsaves 1|2|3`: save/level-change/load on the boot drive (1: host files under PCSX2),
  then the memory card too (2), and load-across-a-rebuild from the boot drive (3). Ends in
  `SaveTest: PASS`/`FAIL`. Run 1 after touching `g_save.c` or saved game structs, and 2 after
  touching the save devices or the notice. It runs from the main loop, between frames: its slot
  checks read the device under the save notice. `set ps2_testsaves "2"` at the end of a copy of
  the config starts it; the quit drops the line again.
- `ps2_perftest 1`: unattended frame-log capture of the demo loop. See
  [performance.md](performance.md).
- `ps2_testcube 1` (VU1 path smoke test), `ps2_testcin 1` (cinematics).
- `ps2_testaio 1`: the background loader self-test, ending in `AioTest: PASS`/`FAIL`. Details,
  and `ps2_aio_throttle_kbps` for pacing it like a USB stick, in
  [testing-storage-boot.md](testing-storage-boot.md).
- The USB and HDD boot paths (emulated stick and HDD images, launcher states, scripted runs
  from the stick) are in [testing-storage-boot.md](testing-storage-boot.md) too.

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
  the log. Anything new printed on a fatal path must use it, not `printf`. Any new fatal-path
  screen must go through `ScrInit`. The hang report, VU1 dump forensics and the exception
  handler are in [crash-triage.md](crash-triage.md).
- **`quake2.log` stamps count from `main()`** (`Sys_Milliseconds` is first called there). The
  `IOP boot:` lines, printed at `Sys_Init`, give each boot step's time.
- Debug builds print an EE exception report (cause, EPC, BadVAddr, stack). Resolve addresses
  against **the same build's** `quake2_unstripped.elf`:
  `mips64r5900el-ps2-elf-addr2line -f -C -e build/debug/quake2_unstripped.elf <addr>` or
  `build/tools/symbolize < emulog.txt`.
- PCSX2 logs `TLB Miss ... [store]` for a null store or load and skips the access without
  raising the exception, so a null access never reaches the handler there.
- **Known flake, never game code:** a `TLB Miss` in `_request_end` (ps2sdk `sifrpc.c`,
  `SIF_CMD_RPC_END`) during `host:` file I/O, usually right after a
  `PackFile: host:/baseq2/pak0.pak` line. The signature is one to three
  `TLB Miss, pc=<same> addr=0x10|0x18 [load|store]` lines; a second face is a wild `pc` below
  `.text` (0x100000), e.g. `pc=0x82000 addr=0x0`, usually fatal. **Re-run before investigating.** Across three identical
  39-map cycles, one died, one was clean, and one logged it and still finished. The renderer
  can't cause it: VIF1 chains only read RAM. A capture from a run that logged it is still valid.
  How to resolve the moving `pc` is in [crash-triage.md](crash-triage.md).

## Host harnesses (runtime logic only; `make` stays the compile check)

- A backend source runs on the host by `#include`-ing the **.cpp** (so the test can walk
  anonymous-namespace statics) against a `fakeinc/` of stub headers, compiled with clang under
  ASan/UBSan. The header sets per subsystem, and the proven recipes, are in
  [host-harnesses.md](host-harnesses.md).
- **Emulate the EE FPU** in any math harness: `1/sqrt(0)` must give FLT_MAX, not inf (see
  [ps2-platform.md](ps2-platform.md)). Otherwise target-only bugs won't reproduce.
- Code with EE/VU0 inline asm can't run on the host. Use the standalone test ELF recipe in
  [performance.md](performance.md).
