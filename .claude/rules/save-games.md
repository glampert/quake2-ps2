---
paths:
  - "src/ps2/save/**"
  - "src/game/g_save.c"
  - "src/tools/scripts/gen_save_tables.py"
  - "src/ps2/tests/save_test.*"
---

# Save games and config.cfg

The README's "Save games" section and `save/save_system.h` give the overview. Decisions and
facts behind them:

## Design (the user's decisions)

- id's `save/current/` working set lives in **RAM** as one miniz-deflated blob per file
  (`fopencookie` streams, so engine stdio is unchanged). It is ISO-ready, with no writable
  disc needed.
- The autosave (slot 0) goes to the device on every level change.
- `ps2_savedevice` is a text cvar, `"host"` or `"mc"`, default `"host"`. It is honoured only
  when `FS_Gamedir` starts with `host:`. On a console it is always the card.
- miniz is the `src/tools/miniz` submodule (pinned 3.1.2), compiled `-O3` in both configs.
  `save/miniz_cfg/miniz_export.h` stands in for the CMake-generated header and carries
  `TDEFL_LESS_MEMORY`, which changes struct sizes. Everything must agree on it.
- The card API is generic (named files in `mc0:/Q2PS2/`).
- Slot 2 and card formatting are pending (README).

## Format

- A slot is `<slot>_a.q2s` / `<slot>_b.q2s`, written to the *other* file, after which the
  older one is deleted. The newest valid sequence wins. There is no rename on rom0 MCSERV or
  fio. The archive is CRC-checked throughout and read and verified fully before the running
  game is touched.
- The archive header carries the game's `G_SaveFingerprint`, so an incompatible slot is
  refused up front with a "different version" popup. See `engine-c.md` for the name-hash
  tables and when to bump `SAVE_FORMAT_VERSION`.

## Measured (PCSX2, debug)

base1 `.sav` 327 KB → 25 KB, `.sv2` 133 KB → 2.3 KB. A two-level working set is 935 KB →
58 KB. The SaveData peak is 230 KB, 164 KB of which is the transient deflate state. A host
save takes ~170 ms. The first card save takes ~1.3 s, including driver start and the 33 KB
icon.

## Icon

- A 3D model of the quad damage pickup (`models/items/quaddama/tris.md2`, frame 0), chosen
  by the user over flat cut-outs. The cut-out code and its cvar were deleted.
- Parsed with `mod::ValidateMD2Header` + `mod::ExpandGLCmdsToTriangles` (`model_load.h`).
  Normals come from `bytedirs`, with axes X←Y, Y←−Z, Z←−X. The longest bbox side is 3.2,
  sitting on Y=0. Every triangle is emitted in both windings: 864 verts, 53.5 KB. Without the
  model, the fallback is a double-sided square.
- The BIOS browser reads the community-documented `.ico` layout (type 0x07, 4.12 fixed point,
  Y down, 0x80 colours) and **ignores the texture alpha bit**.
- `EnsureSaveIcons` rewrites icon files that differ from this build's, once per card per
  session.

## config.cfg policy (`save_api.cpp`: `Sys_SaveStoreConfig` / `Sys_SaveLoadConfig`)

All policy lives in the backend. The engine only builds the text (`open_memstream` +
`Cvar_WriteVariablesToFile`), and `exec config.cfg` asks the backend for it.

- **Write:** `<gamedir>/config.cfg` only when running from `host:` (**never** to
  `mass:`/USB), plus `mc0:/Q2PS2/config.cfg` whenever the save device is the card. The card
  write is skipped when unchanged.
- **Read:** emulator = the host file first, with the card as fallback. Console = **the card
  first**, then the USB `baseq2/config.cfg`.
- **Order:** binds in key-number order, then archived cvars sorted by name, so an unchanged
  setup writes the same bytes every quit. id wrote cvars in list order (newest first), and
  `exec config.cfg` creates most of them before registration, so every quit reversed them.
  That churned the committed file and defeated the card's skip-when-unchanged.
- An archived cvar that a run never registers is dropped from the file (id's behaviour):
  a `set` creates it with no flags. So **register every `CVAR_ARCHIVE` cvar in every build**,
  outside any `PS2_QUAKE_DEBUG`/`PS2_QUAKE_PROFILE` gate, even if nothing reads it there.
  `ps2_perftest` does this through `RegisterPerfTestCvar`. The game's cvars still drop if a
  run quits before any server starts, but the attract loop starts one within seconds.
- The console branch was only host-tested, because this setup can't boot from `mass:`. The
  host policy test uses directories literally named `host:`/`mass:` to catch stray writes.

## Testing

- `ps2_testsaves 1` (host), `2` (card), `3` (load a slot 7 save left by an earlier build).
  Start it with a temporary `Cbuf_AddText("set ps2_testsaves 1\n")` at the end of
  `Qcommon_Init`, then revert.
- `Slot1_Enable = false` in PCSX2.ini → "No memory card in MEMORY CARD slot 1." popups while
  the game keeps running.
- The PCSX2 card (`memcards/Mcd001.ps2`) is a raw 8 MB image: 528-byte pages (512 bytes + 16
  bytes of ECC), the superblock "Sony PS2 Memory Card Format", the FAT via `ifc_list`, and
  512-byte directory entries with the name at 0x40. An ~80-line Python reader can list and
  extract what the game wrote.
- Popups, menus and the icon need the user's eyes (no screenshots here).
- Memory card and FILEIO traps (MCSERV, `mcInit`, IOP flags, `remove()`→`mkdir`) are in
  `ps2-platform.md`.
