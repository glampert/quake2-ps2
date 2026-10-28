---
paths:
  - "src/ps2/tests/**"
  - "src/ps2/debug/debug_menu.*"
  - "src/client/menu.c"
  - "src/client/qmenu.c"
---

# Testing recipes: scripted screenshots, menus without keys, older builds

Companion to [testing-pcsx2.md](testing-pcsx2.md), which has the session-scripting rules and
the four traps. Everything here was worked out in a real run on the dates given.

## Scripted screenshot sessions

- **A screenshot recipe that worked** (2026-10-12): a temporary block in `CDAudio_Play` that
  runs once, queuing `alias w10 "wait;..."` (ten waits), `alias w100 "w10;..."` (ten w10s),
  then `w100;w100;w100;screenshot demo;menu_main;w100;screenshot menu`: the attract demo with
  its HUD, then the main menu over it. `screenshot splash` queued in `Qcommon_Init` before
  `d1` catches the boot splash, since no frame has been presented yet and frame 0 still holds
  it (with a 16-bit framebuffer; a 32-bit one comes back black, cleared by `gs::Init`).
  A 16-bit framebuffer reads back at 5 bits a channel. Each shot is 860 KB at 640x448.
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

## Menus without a screen or keys

A temporary command that opens the page and calls `Key_Event(key, true, 0)` then
`Key_Event(key, false, 0)` per press goes through the real menu code; printing
`cls.key_dest`/`cls.state` around it shows where it left the client. Spin controls change with
left/right only: Enter does nothing on them (qmenu's `Menu_SelectItem`). The debug page was
tested this way (2026-10-12), fired from the `CDAudio_Play` hook. A `screenshot` after the
presses shows the page itself. A temporary `tkey <keynum>` command (one `Key_Event` press and
release) lets the queued script drive the pages itself, with a `w10` between a press and its
`screenshot`. keys.h: Enter 13, Escape 27, Up 128, Down 129, Left 130, Right 131. That checked
the main menu's four items, Down's wrap and two Options rows, whose cvars the script then
printed by name (2026-10-23). The controls page the same way (2026-10-25): `menu_keys`, two
Downs, Enter (13) for the grab, `tkey 204` (JOY2, circle's game key) to bind and `tkey 127`
(Backspace, triangle's menu key) to clear, a shot after each. What that can't reach is
input.cpp's choice of key per button, which needs a pad. Killing PCSX2 after the last shot,
rather than quitting, leaves config.cfg and the card unwritten. PCSX2 still rewrites its ini's
window state on the way out, so restore the ini too.

## Running tests back to back

With `ps2_test_quit 0`, one session ran a map cycle, the save test, the perf test and a second
map cycle back to back (2026-10-12, map list cut to two), each started when the one before
ended. The save test has quit on its own since 2026-10-12.

## Building an older commit beside the tree

For a size or codegen comparison against an older commit without touching the checkout (a test
may be running from `build/debug`): `git archive <commit> | tar -x -C <scratch>/<commit>`, then
`cp -R src/tools/{vclpp,vu-checker,miniz}/. <scratch>/<commit>/src/tools/<same>/` (an archive
holds no submodule contents), and `make -j4` there. The Makefile's paths are relative, so it
builds its own `build/tools/vclpp` and ELF. Two such builds took about four minutes each
alongside a running map cycle (2026-10-24). `mips64r5900el-ps2-elf-size` on the unstripped ELFs
then gives text/data/bss per commit.
