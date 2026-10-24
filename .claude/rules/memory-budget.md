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
- **The frame chain halves grew from 512 to 768 KB on 2026-10-11**, and the world arena (whose
  lump scratch holds both) from 7904 to 8416 KB. The worst moment rose to **30.08 MB**, city3
  again, ADPCM music, debug build. Every map's total rose 0.62-0.64 MB: the 0.5 MB arena plus
  ~0.13 MB of code added since 2026-10-05. The lowest free point fell from 1.96 to 1.12 MB, in
  the city3 -> strike load. All 39 maps still load, and the top chunk ended at 1.11 MB. An
  overflow no longer waits for the GS (see `performance.md`), so the halves can go back to
  512 KB if this headroom is ever needed: frames would overflow, cheaply, instead.
- **A compressed pack** (`baseq2/pak0.pkz`, MapCycle 2026-10-11, debug, against
  `mapcycle-chain768`):
  - All 39 maps load.
  - Every map's live total is +0.14 MB: tag `Pack`, 93 KB of directory tables (80 KB in
    release, which keeps no CRCs) plus the 40 KB whole-file workspace.
  - At a load peak `Pack` is 134 KB, or 206 KB with a deflated stream open (72 KB each, freed
    at close).
  - The worst moment rose from 30.08 to 30.26 MB (city3). The lowest sbrk headroom was 1.22 MB
    against 1.12 (strike).
  - The arena tracked the live increase, within ~0.07 MB apart from a transient bump. The
    end-of-pass top chunk was 55 KB against 1.11 MB, but arena-free plus headroom differed
    only by the 0.14 MB: where the free space lands is chaotic.
- **The 2026-10-19 SDK update** (ps2sdk `329e2bb`, newlib built with `HAVE_BLKSIZE`), same
  code, debug, pkz: the worst moment is 30.29 MB on both SDKs (city3's load; strike's load
  peak 30.24 MB), and every map's live total matches within the 0.01 MB the line shows. The
  end-of-pass layout differed (top chunk 819 KB against 161 KB, arena 24.54 against
  24.30 MB): a ~5 KB larger ELF moves where dlmalloc lands things.
- **The save devices, the menu work, the brightness curves and the screen offset** (12f2f9a to
  8d3d452 plus the menu config write, 2026-10-24, debug, pkz, `mapcycle-menu-gamma`): every
  map's live total and load peak rose 0.03-0.04 MB, the worst moment 30.29 to **30.33 MB**
  (city3), the lowest free point 0.92 to 0.86 MB (strike). All 39 maps load. The debug ELF grew
  20.7 KB of text (11.3 of it the save devices and notice) and 8.3 KB of bss, of which the
  brightness tables (`gamma.cpp`: two palettes, the light ramp and the light curve) are 3.3 KB
  and the video menu's slider and number rows about 2 KB. VRAM is unchanged: the fourth CLUT
  fits the CLUTs' page, so the texture heap stays 2408 KB.
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
`mapcycle-cdmusic`, `mapcycle-cdmusic2` (ADPCM, 512 KB chain halves), `mapcycle-cdwav` (WAV only),
`mapcycle-chain768` (ADPCM, 768 KB halves), `mapcycle-sdk-986ff32` and `mapcycle-sdk-329e2bb`
(pkz, the 2026-10-19 code on the old and new SDK), `mapcycle-menu-gamma` (the 2026-10-24 code).
