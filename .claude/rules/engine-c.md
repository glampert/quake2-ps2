---
paths:
  - "src/client/**"
  - "src/common/**"
  - "src/game/**"
  - "src/server/**"
---

# id's engine C (`client/`, `common/`, `game/`, `server/`)

## Editing rules

- Keep id's code as close to untouched as possible. Platform work belongs in `src/ps2`
  behind a seam. When an engine change is unavoidable, keep it minimal and tag it:
  `// [PS2_QUAKE]: <why>` (about 180 tags exist; follow their style).
- This code is C89 (`-std=gnu89 -fcommon`) with a lenient warning set, built with
  `-fno-strict-aliasing -fsingle-precision-constant`. None of the backend's C++ style or
  `-Werror` rules apply here.
- **No double FPU on the EE.** Unsuffixed constants are made float by the flag, and libm calls
  are redirected by `ps2/math/math_c.h` (via `q_shared.h`). Don't add `double` math or
  `<math.h>` double calls to engine code.
- `Sys_Error` is not `[[noreturn]]`, and its declaration in the game header stays as it is.
- Seams the backend implements: `refexport_t` (the renderer), `Sys_*`, `NET_*` (loopback
  only), `IN_*` (plus the four `IN_Rumble*` hooks in cl_fx.c, cl_parse.c, cl_ents.c and
  cl_scrn.c), `SNDDMA_*`, `CDAudio_*`, `Sys_Save*` (`q_common.h`).
- A changed `.c` here needs only `make`. There is no separate engine build.

## Known engine quirks and bugs

- **`COM_Parse` writes `com_token[MAX_TOKEN_CHARS] = 0`, one byte past the array**, after
  truncating a 128+ char token. It corrupts whatever comes next (it hit `_BigShort`).
  It is unfixed and was reported to the user. Avoid long quoted tokens in scripts.
- `Sys_FindFirst` is a stub on this port (always nullptr), so `FS_ExecAutoexec` never runs
  `autoexec.cfg`, and anything else that enumerates files gets nothing.
- `wait` runs twice per frame (`Cbuf_Execute` in `Qcommon_Frame` and `CL_SendCommand`).
- Quake II has no `nomonsters`. `deathmatch` and `cheats` are `CVAR_LATCH`, and `sv_init.c`
  forces `maxclients` to 8 under deathmatch.
- The client frame interleaves **2D → 3D → 2D** (`SCR_UpdateScreen`), with no hook at the
  boundaries. The renderer handles this itself (see `gs-renderer.md`).

## Level loading and registration

- `CL_PrepRefresh` does free-before-load: it first runs its registration calls in touch-only
  mode (`re.SetRegistrationTouchOnly`), then `re.FreeUnregistered`, then the real pass. This
  keeps the old level's unused assets from overlapping the new level's in memory (see
  `memory-budget.md`).
- Every view weapon is preloaded in `CL_RegisterTEntModels` (id only did three), and player
  weapon fire sounds are preloaded with the level. Mid-level loads drop frames, so prefer
  preloading for anything that appears at a predictable moment.
- `CDAudio_Play` is called by `CL_PrepRefresh` when loading is done. It is a useful event
  hook for scripted tests.

## Save game duty

The game saves function and `mmove_t` pointers as FNV-1a hashes of their names, through
tables generated from the game objects at build time
(`src/tools/scripts/gen_save_tables.py` → `build/<cfg>/gen/g_save_tables.c`). It runs `nm`
over the game objects and a `^mmove_t` regex over the sources, skipping `#if 0` blocks.
`G_SaveFingerprint` covers struct sizes, field tables, `itemlist` and `SAVE_FORMAT_VERSION`.

- After touching `g_save.c` or any saved game struct, run `ps2_testsaves 1`.
- **Bump `SAVE_FORMAT_VERSION` in `g_save.c`** for layout changes the fingerprint can't see,
  such as a field whose meaning changed but whose size didn't.
