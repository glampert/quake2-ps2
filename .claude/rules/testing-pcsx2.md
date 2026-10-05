# Testing: PCSX2, built-in tests, host harnesses

## PCSX2 setup and logs

- App: `/Applications/PCSX2.app/Contents/MacOS/PCSX2`, launched as `PCSX2 -batch -elf <elf>`
  (`make run`). `host:` maps to the ELF's directory, and `make run` symlinks
  `build/<config>/baseq2` → the repo's `baseq2/`.
- `~/Library/Application Support/PCSX2/inis/PCSX2.ini` needs `[EmuCore] HostFs = true`
  (otherwise "No game data found"). Game stdout needs `[Logging] EnableIOPConsole = true`,
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

- Debug builds print an EE exception report (cause, EPC, BadVAddr, stack). Resolve addresses
  against **the same build's** `quake2_unstripped.elf`:
  `mips64r5900el-ps2-elf-addr2line -f -C -e build/debug/quake2_unstripped.elf <addr>` or
  `build/tools/symbolize < emulog.txt`.
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
