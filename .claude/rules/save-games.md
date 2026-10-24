---
paths:
  - "src/ps2/save/**"
  - "src/game/g_save.c"
  - "src/server/sv_ccmds.c"
  - "src/client/menu.c"
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
- **The devices (2026-10-23, the Quake port's policy of 2026-10-21 brought over):**
  `ps2_savedevice` is `mc` (the default), or the boot drive's `host`, `usb` or `hdd`; a value
  naming a drive the game didn't boot from means the card, and the Game menu's "save device" row
  steps between the card and the boot drive. Every device holds the same slot archives, on a
  drive in `<gamedir>/save/`, and every device gets the save notice. Loads read the selected
  device only. config.cfg lives with the saves (below). A missing card or a failed save is told
  to the player (below). Until then it was `host` (the default) or `mc` under the emulator only,
  and always the card on a console.
- **The devices' code:** `memcard.cpp` (libmc) and `drive.cpp`, the old host-file Device, over
  stdio for all three drives: newlib sends `host:` to the ROM FILEIO and `mass:`/`pfs0:` to
  fileXio. Its kind comes from `FS_Gamedir()`'s prefix. After each file written or deleted it
  calls `ps2::sys::SyncGameDataDevice` (the hard disk's PFS keeps metadata cached otherwise); on
  an unpatched host: FILEIO a `remove()` leaves a directory, which it removes. It asks no free
  space (no driver here says): a full drive fails the write. The Quake port made its
  `drive.cpp` from this port's host-file Device, and this one is that version.
- **Engine hooks** (`q_common.h`, all tagged): `Sys_SaveNoticeBegin`/`End` in `SV_Savegame_f`,
  around the level written into the working set and the slot stored; `Sys_SaveStoreSlot`,
  `Sys_SaveRestoreSlot`, `Sys_SaveListSlots` put a notice up of their own unless one is.
  `Sys_SaveAbortStreams` (from `SV_Shutdown`, which every error drop runs) also takes down a
  notice an unwind left up. The Game menu's row reads `Sys_SaveDriveName` and
  `Sys_SaveTargetIsCard` and sets `Sys_SaveUseCard`; config.cfg follows on leaving the page, as
  after any menu change (`M_WriteChangedConfig` in menu.c, from `M_PopMenu` and `M_ForceMenuOff`:
  the config's text against a copy taken as the menu opened, so a console switched off rather
  than quit keeps a changed setting or bind, and a visit that changed nothing writes nothing).
  The load and save menus ask `Sys_SaveMenuProblem`
  and draw `Sys_SaveMenuDrawProblem`; the failure popups add `Sys_SaveSwitchAdvice`.
- miniz is the `src/tools/miniz` submodule (pinned 3.1.2), compiled `-O3` in both configs.
  `save/miniz_cfg/miniz_export.h` stands in for the CMake-generated header and carries
  `TDEFL_LESS_MEMORY`, which changes struct sizes. Everything must agree on it.
- The card API is generic (named files in `mc0:/Q2PS2/`).
- Slot 2 and card formatting are pending (README).

## Format

- A slot is `<slot>_a.q2s` / `<slot>_b.q2s`, in `mc0:/Q2PS2/` on the card and `<gamedir>/save/`
  on a drive, written to the *other* file, after which the older one is deleted. The newest
  valid sequence wins. There is no rename on rom0 MCSERV or fio; FAT and PFS have one, but the
  drives keep the card's scheme. The archive is CRC-checked throughout and read and verified
  fully before the running game is touched.
- The archive header carries the game's `G_SaveFingerprint`, so an incompatible slot is
  refused up front with a "different version" popup. See `engine-c.md` for the name-hash
  tables and when to bump `SAVE_FORMAT_VERSION`.

## The save notice (`save_notice.cpp`)

- **The user's request (the Quake port's of 2026-10-20 and 2026-10-21, here 2026-10-23):** while
  a save goes to its device or comes back off it, the notice PS2 games show: "Data is being saved
  to the Memory Card in slot 1. Do not remove the Memory Card or reset/switch off the console."
  ("loaded from" for a load), with the music faded to silence before the device is touched.
  Every save device gets it, named in its second line ("USB drive.", "hard disk drive.", "host
  filesystem.") with its own warning ("Do not remove the USB drive"; "Do not reset or switch off
  the console." for the hard disk and the host). The menus' text box (`M_DrawTextBox`), the
  first two lines in conchars' other colour (`Menu_DrawStringDark`) and the warning white, sized
  for the longest line any device has, so it is the same box. It replaced `SCR_SetBusyNotice`,
  which only card saves had, and which by the code couldn't show with `ps2_gs_latency 1` (the
  default): its frame was left for the next `BeginFrame` to present, after the save.
- **Every read and write of a device's data goes under it:** saves, the autosave, loads, the
  menus' slot listing, `ps2_saveinfo`, config.cfg's reads and writes of the save device, the
  icon files. `memcard.cpp` and `drive.cpp` assert it at each method that touches the data
  (`EnsureSaveDir`, `List`, `FileSize`, `Open`, `Read`, `Write`, `Close`, `Delete`), so a debug
  build stops at any new path that forgets. Only `Probe` goes without it, as it decides whether
  there is a device to put the notice up for. A drive's config.cfg goes through stdio, not the
  Device: written under the notice when it is the save device's copy, read without one (it is
  game data, read at boot before the save device is known, in milliseconds).
- **Order:** top the mix up (`audio::MixAhead`, `S_Update_`); probe (none, or unformatted: no
  notice, and the access fails with the reason); stop the rumble (`IN_RumbleStop`); top up again;
  draw a frame with the notice and show it at once; fade the music out (`audio::HoldMusic`); hold
  the output (`PS2_SoundHoldBegin`); then the device. Afterwards the notice stays until it has
  been up a second (`kMinShownMsec`), then the hold ends and the music fades back in. See
  [audio.md](audio.md) for the fade and the hold.
- **Where it is drawn (`ShowFrame`):** over the game in `PS2_EndFrame`, the screen dimmed under
  it, the frame presented at once whatever `ps2_gs_latency` says; with the loading plaque up, on
  the loading screen under the plaque (`ps2::loading::Redraw`); before `gs::Init`, on the boot
  splash under the id logo (`splash::ShowImage`). Inside a frame, or while the world loader holds
  the frame chain (`cmdbuf::LentToWorldLoad`), nothing can be drawn: `BeginSaveNotice` refuses
  and the device is left alone. Nor does `SCR_UpdateScreen` draw between `gs::Init` and
  `SCR_Init`, so the notice counts the frames `PS2_EndFrame` drew it in, and refuses when none
  was. No path refuses now.
- **The boot's read of the card's config.cfg comes in `Qcommon_Init`'s `exec`, before
  `Sys_Init` and the renderer**, so the splash draws it: the box and lines composed on the EE
  into one RGBA texture from the built-in conchars and palette, one texel to a pixel, point
  sampled, below the logo (the box's glyphs are opaque); its see-through corners left out by the
  alpha test. `gs::Init` calls `splash::Retire` before it lays out VRAM. The logo's own drawing
  is unchanged (RGB, bilinear, four times, the old sprite corner); the image takes libdraw's
  inclusive far corner (`END_OFFSET`), `x + width - 1`. The in-game 2D path passes `x + width`
  (`gs::EmitTexturedRect`), so every glyph there is drawn a pixel larger and resampled: the
  splash's box is the exact 1:1 one, and differs from the in-game box by that (2026-10-23).
- **Where it goes up:** a save in `SV_Savegame_f` before `SV_WriteLevelFile` (deflating the level
  into the working set already stands the game still, and the music would stop dead under it);
  the autosave in `Sys_SaveStoreSlot`, after `SV_Map`, under `SV_GameMap_f`'s plaque (`map`
  typed at a disconnected console raises none, and the notice is drawn over the console); a load
  in `Sys_SaveRestoreSlot`, before the plaque, over the game; the menus' listing in
  `Sys_SaveListSlots`, as `LoadGame_MenuInit`/`SaveGame_MenuInit` run, before the new menu is
  pushed, over the one before it.
- **The save test can't read the device from `PS2_EndFrame`, inside a frame**, where the other
  tests run: `RunSaveTest` runs from the main loop in `main.cpp`, between frames.
- **Checked in PCSX2 (debug, 2026-10-23):** `ps2_testsaves 2` passed, each access under the
  notice, held to the second (1000-1009 ms) on host files, a card save 1678 ms. Screenshots taken
  as each notice went up (a scratch build) showed it over the game for a save and a load, on the
  host files and the card; over the Game menu for the slot listing; under the plaque on the
  loading screen for a `gamemap`'s autosave; over the console for `map` typed disconnected and
  for the quit's config.cfg write; and on the splash under the logo for the boot's read of the
  card's config (host config.cfg moved away).

## Telling the player

- **The load and save menus** look at the save device afresh as they open (`Sys_SaveListSlots`
  clears the error first), and when it can't be used draw a box over their slots
  (`Sys_SaveMenuDrawProblem`, in place of the status line): the reason in white ("No memory card
  in MEMORY CARD slot 1."), then "To use the <other device> instead, change save device in the
  Game menu." in the other colour. The other device is the boot drive from the card, the card
  from a drive. Cross on a slot meanwhile lists the slots again instead (a card put in since
  counts) and doesn't try the slot.
- **A save, a load or an autosave that fails** says so in its `M_Popup` ("Save failed", "Load
  failed", "Autosave failed", the last once a session), with the reason `SetError` recorded and
  the same advice. config.cfg's failures stay on the console: they come on quitting and on menu
  exits.
- The boxes' lines are wrapped at 36 characters, as `M_Popup`'s are.

## Measured (PCSX2, debug)

base1 `.sav` 327 KB → 25 KB, `.sv2` 133 KB → 2.3 KB. A two-level working set is 935 KB →
58 KB. The SaveData peak is 230 KB, 164 KB of which is the transient deflate state. A host
save takes ~170 ms. The first card save takes ~1.3 s, including driver start and the 33 KB
icon. Under the notice (2026-10-23) every access is held to a second at least, so a host save
or an autosave takes a second; the fade (~0.6-0.7 s) and the hold (~0.1 s) come inside it. The
save test's SaveData peak was 274 KB.

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

- **It lives with the saves (the user's choice, as in the Quake port).** On a console: written
  to the save device, the card's `mc0:/Q2PS2/config.cfg` or the drive's `<gamedir>/config.cfg`.
  Read at boot from the drive's when it says `ps2_savedevice` is that drive, the player's own
  then, and the card isn't touched; otherwise from the card's, the drive's standing in when the
  card has none. A switch from the drive back to the card writes the card's and rewrites the
  drive's once, so it stops naming the drive (`DriveConfigNamesDrive`); a switch to the drive
  needs nothing on the card, since the drive's wins. The Quake port rejected the card first (its
  notice at every boot even for a player on the drive, and the card's old copy winning over a
  switch), the drive always first (the drive's shipped config over the player's card), and the
  newest (no common clock between card, FAT and PFS).
- **Under the emulator nothing changed:** `<gamedir>/config.cfg` is always written and read
  first, and the card's copy is written when saves go to the card and read when there is no host
  file. The host write goes under the notice when host files are the save device. Every drive
  write (the emulator's too) is skipped when the file holds the same bytes, as the card's is.
- **The card's copy (`CardConfig`):** what the card's config.cfg holds is kept in RAM (a few
  KB): read off the card by the boot's `exec`, and updated by every write. A later
  `exec config.cfg` is answered from it, and a config equal to it isn't written, nor the card
  even looked at: each would put the save notice up for nothing. A probe that finds another
  card, or none, makes it stale (`Device::Generation`), and then a write reads the card's file to
  compare first. Under the emulator the boot reads the host file, so the first card write of a
  session compares against the card's file, under the notice, before writing.
- **The drive's copy** is kept the same way, from its first read; a console's choice of the
  drive or the card is made at the first read and kept (`ConfigFromDrive`). `ConfigValue` reads
  `ps2_savedevice` out of it: the last `set ps2_savedevice "<value>"` line, or one typed by hand
  without the `set` or the quotes.
- **Checked in PCSX2 (debug, 2026-10-23):** booted from a USB image (`make_usb_image.py`, the
  no-launcher recipe in `testing-pcsx2.md`) whose config said `ps2_savedevice "usb"`, with the
  card enabled: `ps2_testsaves 1` passed with the saves, autosaves and loads in
  `mass0:/baseq2/save/`, the quit wrote the stick's config under the notice, and the card was
  never touched (MCMAN never started, its image byte for byte the same). Switching to `mc` and
  quitting wrote the card's copy and rewrote the stick's to `"mc"`; the boot after read the
  card's under the notice on the splash. The hard disk the same way, from PCSX2's HDD image (its
  `+q2ps2` partition, the image cloned first and put back after): `ps2_testsaves 1` passed with
  the saves in `pfs0:/baseq2/save/` and the config written there, and the next boot, the card
  enabled, took the HDD's config without starting MCMAN, and passed again. With no card
  (`Slot1_Enable = false`), screenshots showed the save menu's box and the save, load and
  autosave popups, each with the advice.
- **Order:** binds in key-number order, then archived cvars sorted by name, so an unchanged
  setup writes the same bytes every quit. id wrote cvars in list order (newest first), and
  `exec config.cfg` creates most of them before registration, so every quit reversed them.
  That churned the committed file and defeated the skip-when-unchanged.
- An archived cvar that a run never registers is dropped from the file (id's behaviour):
  a `set` creates it with no flags. So **register every `CVAR_ARCHIVE` cvar in every build**,
  outside any `PS2_QUAKE_DEBUG`/`PS2_QUAKE_PROFILE` gate, even if nothing reads it there.
  `ps2_perftest` does this through `RegisterPerfTestCvar`. The game's cvars still drop if a
  run quits before any server starts, but the attract loop starts one within seconds.

## Testing

- `ps2_testsaves 1` (the boot drive: host files under PCSX2, the USB drive or the hard disk on a
  console), `2` (and the card), `3` (load a slot 7 save an earlier build left on the boot
  drive). Start it with a temporary `Cbuf_AddText("set ps2_testsaves 1\n")` at the end of
  `Qcommon_Init`, or a `set ps2_testsaves "2"` line in a copy of config.cfg, then revert.
- `Slot1_Enable = false` in PCSX2.ini simulates "no card".
- **The drives:** host files under PCSX2 as they are. A USB drive from `testing-pcsx2.md`'s image
  recipe, its `baseq2/config.cfg` saying `"usb"`: attach the image read/write between runs to set
  test cvars and read what a run left, and detach it before PCSX2 starts. The hard disk from
  PCSX2's HDD image, whose PFS nothing on this Mac writes: a scratch
  `Cbuf_AddText("ps2_savedevice hdd ; set ps2_testsaves 1\n")` at the end of `Qcommon_Init`
  arms the test, launched from a folder with no `baseq2/` and `HddEnable = true`; an APFS clone
  of the image (`cp -c`, instant) puts it back after. A switch of device and a quit, with no pad
  to press, is an alias in the config (`ps2_savedevice mc;w10;quit`) run by a scratch hook.
- The PCSX2 card (`memcards/Mcd001.ps2`) is a raw 8 MB image: 528-byte pages (512 bytes + 16
  bytes of ECC), the superblock "Sony PS2 Memory Card Format", the FAT via `ifc_list`, and
  512-byte directory entries with the name at 0x40. An ~80-line Python reader can list and
  extract what the game wrote.
- **`ps2_testsaves 2` failed two steps on a console boot at fabcf80** (PCSX2 booted from the USB
  image, 2026-10-12): the console boot ignored `ps2_savedevice host`, so slot 7 went to the card;
  after the test's two level changes it loaded as "The save game is damaged", and "back to host
  files" then found it invalid. Not investigated. Mode 1 now keeps slot 7 on the boot drive
  there too, so a rerun from the USB image says whether the card's damaged slot remains.
- **Screenshots of the notice** (`testing-pcsx2.md` has the general recipe): the `screenshot`
  command can't run from inside the save commands (it retokenises, and `SV_Savegame_f`'s
  `Cmd_Argv` would dangle), so a scratch build gives screenshot.cpp a named entry point and calls
  it in `BeginSaveNotice` right after `ShowFrame`. The splash's notice is caught by leaving it in
  frame 0 (no `HideImage`) and queuing `screenshot` in `Qcommon_Init` ahead of `d1`, with no host
  config.cfg so that the boot reads the card's. A level change's notice on the loading screen
  needs a `gamemap` from within a level: `map` from the disconnected console raises no plaque.
- Memory card and FILEIO traps (MCSERV, `mcInit`, IOP flags, `remove()`→`mkdir`) are in
  `ps2-platform.md`.
