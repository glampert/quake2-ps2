---
paths:
  - "src/ps2/system/**"
  - "src/ps2/renderer/vram.*"
  - "src/ps2/renderer/texture.*"
  - "src/ps2/renderer/model*"
  - "src/ps2/renderer/image_load.*"
  - "src/ps2/tests/map_cycle.*"
---

# Memory budget (32 MB EE RAM)

- A single program-wide dlmalloc heap (`system/heap.h`) carries per-tag accounting
  (`ps2::heap::MemTag`). Kernel, ELF image and stack are booked as `MemTag::ElfSys`, so the
  tags add up to the whole 32 MB. dlmalloc's page size is pinned to 4096 because ps2sdk's
  `sysconf` fails (see `ps2-platform.md`).
- **The worst moment is a map transition**, not any map. The outgoing level's data can
  overlap the incoming one's, and every out-of-memory failure in this port has happened there.
  Watch `PEAK`/`NEW PEAK` and the per-transition "load peak" line from `ps2_testmaps 1`.
- `MapCycle` prints a level's line after `EndRegistration`, plus a second "load peak" line
  with that transition's own heap-window peak and its per-tag split. Summaries from before
  that change hold mid-transition Tex/Mdl snapshots, so don't compare them to current ones.

## Measured history (MapCycle worst moment, of 32 MB)

- The view-weapon preload took it from 29.76 to 30.29 MB.
- **Free-before-load** (`CL_PrepRefresh` touch-only pass → `FreeUnregistered` → real pass;
  walls likewise in `LoadTexInfo`): 30.33 → **28.89 MB**. The textures+models overlap during
  power1's load had been 10.91 MB against a 7.76 MB steady max. The worst moment is now city3's
  own content (7.81 MB tex+mdl, 5.12 MB audio), with zero reloads.
- Wall mipmaps (WAL levels 1-3 on POT walls): 28.89 → 29.26 MB (max tex+mdl 7.81 → 8.17).
  `ps2_mipmaps 0` reproduces the old numbers.
- CD music: 29.31 MB (~25 KB static).
- CD music's WAV fallback moved the stream buffers to the heap (`MemTag::Music`, the `Mus`
  column), held only while a track plays: the loading plaque stops music, so transitions
  never carry them. Static dropped to ~12 KB. On the code of 2026-10-05 the worst moment is
  city3's steady state either way: **29.44 MB** with `.adp` tracks (2 × 8 KB buffers) and
  **29.54 MB** with every track falling back to a 44.1 kHz WAV (2 × 64 KB), 1.98 MB still
  free. Exactly the 112 KB of buffer difference.
- **Fixed segregated heaps for textures/models were rejected.** A partition must cover
  max(tex+mdl) + max(everything else), and those peak at different moments, so it costs about
  1.2 MB more than fragmentation does. Fragmentation (arena minus live peak) measured
  0.67-1.55 MB. If it ever matters, the follow-up is a top-down compacting tex/model region
  that shares the gap with dlmalloc.
- `pics/*` are never freed. A stray key press that opens a menu shows up as extra Tex in a
  cycle (e.g. `pics/inventory.pcx`, 48 KB).
- Load-time and debug-only sources build `-Os` (`SIZE_OPT_CXX_SRC`), which saves ~9 KB of
  `.text` for level data.

Reference summaries live in `build/baselines/` (local, untracked): `mapcycle-fbl0` (before
free-before-load), `mapcycle-fbl1`, `mapcycle-mip0`/`mip1`, `mapcycle-vwep`,
`mapcycle-cdmusic`, `mapcycle-cdmusic2` (ADPCM, current code), `mapcycle-cdwav` (WAV only).
