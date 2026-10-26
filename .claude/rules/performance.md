---
paths:
  - "src/ps2/**"
  - "src/tools/scripts/frame_log/**"
---

# Performance: EE codegen, measuring in PCSX2, recipes

## Where things stand

- Goal: a smooth 60 fps through the whole `ps2_perftest` capture. It currently has **0
  dropped frames**. Reference `build/baselines/vwep.flog`: EE work mean 5.4 ms, p99 9.3 ms,
  max 11.6 ms (debug). The SDK update of 2026-10-19 changed no stage by more than 0.1%
  (SndMix -0.9%, which wanders anyway): `sdk-986ff32.flog` against `sdk-329e2bb.flog`, the
  current reference, same code. The save devices, the menu work and the brightness curves
  (through 8d3d452, 2026-10-24) moved no rendering stage by more than 0.3% (`menu-gamma.flog`
  against it; EntShade +1.0%, the per-model light curve, 0.9 µs). Its Input column read 26.8 µs
  against 19.9 because that run polled the emulated USB keyboard and the reference's never
  brought the driver up: check `USB keyboard input initialised.` in both emulogs before reading
  anything into Input.
- What it took: soft-float doubles in the engine C (`-fsingle-precision-constant` +
  `math_c.h`, 59 → 1 drops); cheaper effects (`Com_FxRand`, an incremental rail spiral,
  inline `V_AddParticle`); preloading mid-level assets (player weapon fire sounds, all view
  weapons in `CL_RegisterTEntModels`); and cheaper file I/O (hashed pak directory, unbuffered
  pak reads, file-sized stdio buffers). A mid-level asset load is the usual cause of a dropped
  frame.
- A release-build EE codegen audit already landed: per-draw chunk heads copied with lq/sq
  (EntShadow -46%, EntGeom -19%), world gather through `ReserveVerts`/`CommitVerts` plus a
  local cursor, a BSP walk that passes clip flags down the tree with an inline corner test
  instead of `BoxOnPlaneSide` (-32%), and the particle gather through a local cursor with
  `sq`. Release View went from 3405 to 2614 µs.
- **Active: background loading** (design rewritten 2026-10-26 from the console baselines below:
  one loader thread for mid-level models, skins, pics and sounds, plus a read-ahead stream for
  cinematics and demos; stages 1-8). The design is in
  [misc/docs/plans/background-loading.md](../../misc/docs/plans/background-loading.md). The
  PCSX2-era note that decode mattered as much as I/O was wrong on hardware: the drive is 95% of a
  mid-level load there. **Stage 1 landed 2026-10-26**: the vsync wait sleeps on the vblank
  interrupt (gs.cpp, see gs-renderer.md). Its PCSX2 A/B against `ps2_vsync_spin 1`
  (`pcsx2-vsync-sleep-2026-10-26.flog` against `-spin-`) matched to 0.1% on every rendering
  column, 0 drops, EE work mean 4556 against 4560 µs; the one `compare_flog` flag was ClParse's
  mean (93 against 22 µs), a single 1.3 s level-change row that the sleep run's capture charged to
  ClParse and the spin run's to no column, the kind of attribution jitter a `gamemap` row has.
  Nothing a PCSX2 capture can see should change at this stage: the gain is the idle time the EE
  now hands to the other threads, which only shows once they have work. **Stage 2 landed the same
  day**: `ps2::aio` (fs/async_io.cpp), `FS_LocateFile` and the `ps2_testaio` self-test, no
  clients yet; PCSX2 passed it (testing-pcsx2.md). From a pkz the loader landed 5.9 MB in 1.5 s
  unthrottled, 11 MB/s inside `read()` on host files, and ran while the main thread sat in its
  own `FS_LoadFile` calls. **Stage 3 (the cinematic read-ahead stream) landed the same day** too:
  PCSX2 can't show its gain (host reads are instant), only that nothing broke: against the sleep
  capture, the cinematic rows' FsIo went from 958 to 824 µs mean and p95 1418 to 1101 (the stdio
  refills gone; what is left is the copy out of the ring), Cinematic and Frame unchanged
  (`pcsx2-readahead-2026-10-26.flog`); with `ps2_aio_throttle_kbps 900`, a USB stick's rate, the
  capture is the same to the row (`pcsx2-readahead-throttled-2026-10-26.flog`: the ring keeps up),
  and at 200 KB/s, below the bitrate, playback starves as designed (every cinematic row waits in
  FsIo, no failure). The frame log gained `aioBytes` and `aioQ`. The console USB capture is the
  gate that matters and is still to run. **Stage 4 (sounds) landed the same day**: in the
  throttled PCSX2 perf run (`pcsx2-sounds-throttled-2026-10-26.flog`) `rg_hum.wav`'s open row lost
  its FsIo and the sound was ready five frames later (`FLOG#open,5012` / `FLOG#ready,5017`), the
  one mid-level sound the run loads. `FLOG#ready,<frame>,<name>` is new: pair it with the `#open`
  of the same name for request-to-ready latency. **Stage 5 (textures) the same day**: in the same
  throttled run (`pcsx2-textures-throttled-2026-10-26.flog`) `pics/a_slugs.pcx` opened on 5012 and
  was ready on 5018, its row's ClParse 5.6 → 0.3 ms and FsIo 2876 → 5 µs. **Stage 6 (models) the same day**
  (`pcsx2-models-throttled-2026-10-26.flog`): the slugs model's row 4758 went from 4.8 ms ClParse
  and 2.8 ms FsIo to 0.4 ms and 4 µs, the model ready on 4760 and its skin on 4761. With that,
  every mid-level load the perf run makes (model, skin, HUD pic, sound) is off the frame in
  PCSX2; the console captures (stage 8) are what remains.
- **Emergency chain drains: fixed 2026-10-11** (`cmdbuf::Reserve`'s overflow path). An overflow
  used to kick what was built, wait for the GS to draw all of it and rebuild in the same half, so
  the EE sat idle. On a console that cost ~5 ms per overflowing frame, and under NTSC those
  frames were 80% of the dropped ones. Now it kicks and carries on in the other half, which that
  kick has already waited out (`s_inFlightHalf` guards the one case where `Kick` sends nothing).
  The halves also grew from 512 to 768 KB, and the world arena by 512 KB with them. In PCSX2 the
  perf demos no longer overflow at all (the largest chain is 656 KB). A stress build with 256 KB
  halves drew the same work with no assert or stall: 8,191 overflows in 7,679 frames, up to 3 in
  one frame. On the console under NTSC (same settings as before) it removed every drain: 533
  frames to 0. View p95 fell from 15.2 to 6.4 ms and Entities p95 from 11.6 to 3.3, but
  dropped frames only went from 6.8% to 5.8% (56.1 to 56.7 fps). What drops now is views of
  6,000+ triangles, 48% of them, at ~18 ms: 9.1 ms of EE work plus 9.0 ms of GsWait. Each frame
  is kicked once, at its end, so **the GS idles while the EE builds a frame and the EE then
  waits while the GS draws it**. The overflow's mid-frame kick used to overlap the two by
  accident; at 768 KB nothing overflows.
- **A deliberate kick after the world pass** (`rs::KickBuilt` in `view::RenderFrame`,
  2026-10-11) lets the GS draw the walls, lightmaps and sky while the EE builds the entities,
  particles, translucent surfaces and 2D. It never waits mid-frame: it's skipped while anything
  is in flight. Same console, NTSC, 768 KB halves: **59.9 fps over gameplay** (from 56.7),
  steady frames dropped **0.4%** (27, from 5.8%), and frames without 1.7 ms to spare 4.0% (from
  9.1%). GsWait fell from 5.3 to 3.4 ms a frame. The perf test reached frame 7212 of PCSX2's
  7911, against 6835. Back at 512 KB halves with the kick it made no difference: 59.8 fps, 0.38%
  dropped. 11.5% of frames overflowed, at 17.3 ms with 2.8 ms of vsync left. The halves stay at
  768 KB anyway, as margin for views heavier than the perf demos': an overflowing frame is the
  one most likely to be near the edge already.
- **PAL with both fixes** (same console, `ps2_video_mode pal`, profiling release, 2026-10-11),
  against the PAL capture from before them: 50.1 fps over gameplay (from 49.9), two-field frames
  8 of 5890 (from 40), frames without 1.7 ms to spare 0.2% (from 4.6%), no drains (from 657).
  GsWait fell from 5.5 to 3.5 ms, vsync slack rose from 8.4 to 10.1, and EE work held at
  6.2-6.4. The 8 are the heaviest views of the run (7.6K-9.9K triangles, half of them waste2's
  49-entity fight near frame 4560), where EE work plus GsWait reaches 20-22 ms.
- **The demo hitches are the demo's own reads.** Every console capture has the same 7 frames of
  250-650 ms. Two are mid-demo asset loads (the slugs ammo model; the railgun hum with
  `a_slugs.pcx`). The other five are SV_Frame refilling the demo stream's 64 KB stdio buffer
  (`FS_SetStreamBuffer`) from `pak0.pak` on USB: demo1 is 132 KB and demo2 141 KB, so each
  plays two refills, and the demo switch adds demo2's first fill. One costs 135-630 ms,
  because it waits out a pending music read (`audio.md`). Server holds the time, one row
  before the slow Frame (see `frame_budget.py`). Only demo playback streams like this.

- **The console baseline of 2026-10-26** (the user's PAL console, profiling release f7c9918 with the
  I/O log, launched from the HDD; `build/baselines/console-hdd-2026-10-26.flog`, the raw
  `frame_log_hdd_2026-10-26.txt` and the io_logs beside it; `frame_budget.py --pal`):
  - Demos: 0.24% of steady frames dropped (14 of 5867). The slow rows are the known classes: the
    demo stream's 64 KB refills (Server 66-72 ms, charged a row early), one audsrv submit RPC that
    waited 42 ms on the IOP, and the two mid-demo loads.
  - **Mid-level loads are I/O even from the HDD:** the slugs ammo model and skin cost 43 ms of
    ClParse, 41 ms of it FsIo; `a_slugs.pcx` plus `rg_hum.wav` 61 ms, 58 ms of it FsIo. PCSX2 had
    shown decode as the larger half (plan notes in `misc/docs/plans/background-loading.md`); on a
    console the drive is 95% of it, and USB reads are slower again. A loader thread for the I/O is
    the fix that scales; decode comes second.
  - **Cinematics:** the Huffman decode alone (`Cinematic` minus `FsIo`) is 9.9 ms per cinematic
    frame on eou1_ (max 11) and 13.0 ms p50, 19.9 p95, 35.4 max on ntro, against ~3 ms in PCSX2:
    3-4x, a cache-bound tree walk. A 64 KB stdio refill from the HDD is 9.6 ms when the drive's fast
    path answers and 42-54 ms when its slow one does (the two HDD speeds above); refill frames run
    39-43 ms p50 and up to 88 ms, so a third of ntro's rows (989 of 9069) take two fields even from
    the HDD. No frame reached the 106 ms audsrv ring except at the cinematic's start, but `sndFree`
    was within 2400 bytes of empty in 23 rows: on USB, where a refill is 83-113 ms (2-8 cluster
    commands plus 64 KB at ~900 KB/s), the same frame is the stutter the user hears.
  - **The same ELF launched from the USB stick froze** after the first load. `io_log_usb` shows the
    base2 load complete (31.7 s, 12.26 MB read at 504 KB/s overall; files over 200 KB at 620-680
    KB/s, files under 8 KB at a median 140 KB/s, 13 ms each), then nothing: `frame_log.txt` holds
    only the `FLOG#map` marker, so the freeze fell between the load's end and the first 64-frame
    batch, 1.3 s into demo1. **The same ELF built with `PS2_QUAKE_ALWAYS_RESET_IOP=1` ran the whole
    capture from the stick**, so the launcher's reused USB drivers are the cause (see
    [ps2-platform.md](ps2-platform.md)). No USB keyboard was attached. A release build has no
    exception handler, so a crash and a hang both end as a frozen screen; the debug
    `quake2_dbg_diag_2026-10-26.elf` in `build/baselines/` (exception report, frame and I/O logs)
    is there to narrow it to usbd or usbhdfsd if that is ever worth a run.
- **The USB baseline of 2026-10-26** (same console, the reset build above, wLaunchELF `mass:`
  launch, the 32 KB-cluster stick; `console-usb-2026-10-26.flog` and `frame_log_usb_reset_2026-10-26.txt`
  in `build/baselines/`). The user opened the Options menu during demo1's first seconds (rows
  373-453 load the menu pics, 1546 is the config write on leaving it), so its frame numbers don't
  match the HDD capture's; compare by `FLOG#map` segment.
  - Demos: 0.21% of steady frames dropped, like the HDD. The demo stream's 64 KB refill is
    125 ms (Server, row 2974). **The pickups: slugs model and skin 91 ms, `a_slugs.pcx` plus
    `rg_hum.wav` 147 ms** (ClParse, I/O nearly all of it), 2.1x and 2.4x the HDD, 5-7 PAL fields,
    the second past the audsrv ring.
  - **Cinematics: a 64 KB stdio refill is 93 ms** (p50 of 971 undisturbed refills; the model's
    2 cluster commands plus 64 KB at ~900 KB/s said 83), and every refill frame runs 120-130 ms,
    past the 106 ms ring: 86 of eou1_'s 88 refills and 1236 of ntro's 1240. That is the stutter,
    about 6 times a second on ntro. Decode is the HDD's 10-13 ms.
  - **The stick stalls its reads for ~0.5 s after a write.** For 4-28 rows after each frame-log
    dump (a ~20 KB append) the refills take 420-600 ms instead of 93, then drop back at once:
    269 of ntro's 1240 refills, exactly the 2-3 after each dump. A capture's own writes poison a
    fifth of its cinematic rows, so judge refills by the p50 or by rows more than 30 past a dump
    (the dump rows are the gaps in the frame numbering). The same applies to any write the game
    makes on a stick (autosave, config.cfg, logs): the half second after it reads slowly.
  - **Each cinematic start and end cost 4-6 s on USB**, more than the stutter did: 0.3 s for
    `pics/loading.pcx`, 1.6 s to open the file and read its 64 KB table and first frame, and
    2.8 s of `CL_RegisterSounds` reloading the temp-entity sounds after a `snd_restart`, with 1.7 s
    of the same at the end. The restart came from `SCR_PlayCinematic`'s rate check (see
    [engine-c.md](engine-c.md)); it is now up-only, so the eight 11 kHz cinematics no longer pay it.

## Compressed packs (`pakN.pkz`) and load time

- **What a pkz changes** (PCSX2 `host:`, debug, I/O log, 2026-10-11, first 8 MapCycle loads):
  - Bytes read per load fall to 55-65% (base1 16.4 → 10.7 MB, base2 8.4 → 4.6 MB). base1.bsp
    was delivered at 1.44x its size, since the loaders reread lumps. It was read from the drive
    at 0.53x, which is 1.3x its compressed size: restarts reread input.
  - Inflating costs 0.6-1.3 s per load, about 10-13 MB/s of output on PCSX2's EE. Expect less
    on a console, where EE work runs ~1.56x PCSX2's.
  - Another 0.3-0.9 s per load goes to file calls outside reads and inflating: copying out of
    the dictionary, opening stored streams and, in debug builds, CRC-checking every pkz read.
    The split between them wasn't measured.
  - In PCSX2 a pkz load is therefore *slower* (base1 4.2 → 6.1 s), because `host:` reads are
    nearly free there.
- **On a console it wins by ~40%** (release + I/O log, USB stick, PAL console, 2026-10-11;
  `io_log.txt` from one boot each, the pkz renamed away for the pak run):

  | Load | pak | pkz | File calls pak → pkz | Read from the drive |
  | --- | --- | --- | --- | --- |
  | base2 | 58.6 s | 36.5 s | 51.5 → 30.3 s | 18.40 → 12.26 MB |
  | waste2 | 29.2 s | 17.2 s | 23.4 → 13.2 s | 9.35 → 5.22 MB |

  - Inflating took 1.2 s of base2 (0.6 s of waste2), about 12 MB/s of deflated output: 3% of
    the load. The rest of the file time is waiting for the drive.
  - The stick is bandwidth-bound: one large read runs 460-580 KB/s from either pack (base2.bsp
    from the pak, 2.83 MB in 4.9 s). Time follows bytes read.
  - Small files got cheaper by more than their bytes: wall textures 19.4 → 8.5 ms each,
    `.pcx` 62 → 20 ms, models 212 → 90 ms. The cause wasn't isolated. fileXio splits a read
    into a destination that isn't 64-byte aligned into a head, a middle and a tail read
    (`fileXio_iop.c`); the pkz inflates from an aligned input buffer, and the pak reads
    straight into `Z_Malloc` memory.
  - Sounds are now most of a pkz load: base2's 177 WAVs were 17.0 of its 28.4 s of reading.
    Deflate leaves them at 87-90% (16-bit 22 kHz PCM), so most are stored. Deflating a sample
    delta instead (16-bit: low and high byte planes) takes the 218 WAVs of base2 and waste2
    from 90% to 71% of their size (11.42 → 8.07 MB, stored below a 10% saving), about 1.7 MB
    less to read on base2. That needs an unfilter step in `PS2_PkzRead` and a format flag.
  - Seeks barely reread: only the collision loader's BSP stream read more than its file's
    compressed size (1.07 MB for 0.92 MB on base2).
- **USB with 32 KB clusters, and HDD** (2026-10-12, same console, release + I/O log, 64 KB
  fileXio buffer, one boot each; the stick reformatted from 8 to 32 KB clusters):

  | Load, setup | pak | pkz |
  | --- | --- | --- |
  | base2, USB 8 KB clusters, 16 KB buffer (above) | 58.6 s | 36.5 s |
  | base2, USB 32 KB clusters, 64 KB buffer | 40.4 s | 29.9 s |
  | base2, HDD (PFS), 64 KB buffer | 15.8 s | 14.8 s |
  | waste2, USB 8 KB clusters, 16 KB buffer (above) | 29.2 s | 17.2 s |
  | waste2, USB 32 KB clusters, 64 KB buffer | 20.6 s | 14.2 s |
  | waste2, HDD (PFS), 64 KB buffer | 7.9 s | 7.5 s |

  - **USB:** large reads went from ~580 to 650-790 KB/s (base2.bsp from the pak: 4.9 → 3.7 s).
    Fitting both setups' large reads to *time = commands × c + bytes / B* gives c ≈ 5 ms per
    USB command and B ≈ 900 KB/s, so whole-cluster commands are already near USB 1.1's ceiling.
    The pkz gained less (−18% against −31%): it reads 32 KB at a time from unaligned offsets,
    which is usually two partial-cluster commands, and its bytes were already fewer. Small
    files still cost ~4-5 ms of command overhead each, ~2.5 s over base2's 498 files.
  - **HDD:** twice as fast as the best USB setup, and compression barely matters (−5%):
    inflating (1.2 s on base2) costs about what the smaller reads save. Reads run at one of
    two speeds, ~1.35 MB/s or 6-8 MB/s, with little in between. The same file read twice gets
    the same speed (base2's sounds in loads 1 and 5 of the pak run). The fast ones follow a
    read of the bytes just before them in the pack (the gunner sounds after the first one;
    pkz `v_bas3` straight after `v_bas1`, `comp_hum3` after `comp_hum1`), which looks like the
    drive's own read-ahead. The renderer's pass over a BSP right after the collision loader's
    ran 6 MB/s against 1.6 (pak). ~1.35 MB/s from the platters is low for any hard drive.
    The user's bay holds a SATA adapter with a Hitachi drive of about 390 GB; the adapter, or
    the transfer mode ps2atad settles on with it, is the unexamined suspect.
  - **What it suggests:** order a pack's data by when the game loads it (from MapCycle load
    lists) rather than id's order, so a level's reads run forward through the pack. On HDD
    more reads would come from the drive's read-ahead; on USB, small neighbours would share
    commands and BDM's 4 KB read cache (`bd_cache.c`). Also cluster-aligned 32 KB reads in the
    pkz reader (one whole-cluster command each), and the WAV delta filter (fewer bytes).
- **Console A/B recipe.** Use a release build with the log on:
  `rm -rf build/release/src && make release CONFIG_DEFS='-DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=0 -DNDEBUG -DPS2_QUAKE_IO_LOG=1'`,
  then the same `rm -rf` and a plain `make release` afterwards.
  - Build one USB image with `make_usb_image.py` and one with `--uncompressed-paks`.
  - Start each load from the menu (New Game) or by a level change. A `map` typed at the
    console, or a load from a disconnected client, raises no loading plaque and isn't logged.
  - The file is written only when a load ends. Check the in-game console (Select) for
    `I/O log: load N (...) written to <path>`, or a warning with the open/write/close results.
    A console with an HDD that holds the game data loads from it, so its log lands there.
    Move the HDD copy aside to measure USB: don't skip the HDD drivers (`ps2-platform.md`).
- **A stream's inflate step is capped at 2 KB of input** (`kStreamInflateStepBytes`).
  - Uncapped, the demo stream refilled its whole 32 KB dictionary in one frame: a 4.2 ms
    `Server` spike in PCSX2, against 1.25 ms with the pak. Capped, the worst was 1.5 ms, and
    slow frames matched the pak's (104 vs 107 of 7679).
  - tinfl's wrapping mode can't take a smaller *output* size: it derives the dictionary mask
    from it and returns `TINFL_STATUS_BAD_PARAM`.
- **Overlapping inflate with the reads** (a reader thread) would save at most the 3% inflate
  takes on a console.
- **What sizes a drive read** (the IOP side of every `read()` on `mass:`/`pfs:`):
  - fileXio serves it in pieces of its IOP buffer, each an `iomanX_read` plus a DMA to the
    EE, waited out before the next. Stock size 16 KB. Boot now sets 64 KB
    (`PS2_QUAKE_FILEXIO_BUFFER_KB`, iop_boot.cpp), once the drive's modules are loaded. In
    PCSX2 both HDD and USB boots then still start audsrv and the keyboard.
  - PFS reads a whole run of contiguous zones in one transfer, up to the piece size
    (`fileTransfer`, pfs_fio.c). So on HDD the buffer sets the ATA transfer size.
  - FatFs (bdmfs_fatfs, R0.16) ends every direct read at a cluster's end (`f_read`'s "Clip at
    cluster boundary"). usbmass_bd caps a SCSI command at 128 sectors (64 KB). So a USB read
    is at most one cluster, whatever the buffer. The user's stick (FAT32, 15.8 GB) had
    **8 KB clusters** (macOS's default at that size), so its reads were 8 KB commands in the
    2026-10-11 A/B. On 2026-10-12 it was reformatted with 32 KB clusters
    (`newfs_msdos -F 32 -c 64`; `diskutil` can't choose a cluster size) for the next A/B.
  - BDM caches reads under 8 sectors (`bd_cache.c`, 32 × 4 KB), so FAT walks for seeks come
    mostly from cache. FatFs has `FF_USE_FASTSEEK 0` and `FF_FS_TINY 1`.
  - The pkz inflater reads its input in 32 KB pieces (`kReadInputBytes`,
    `kStreamInputBytes`), so a deflated file never asks for more than 32 KB at a time.

## EE codegen facts

- **Count memory ops, not instructions.** ALU is the cheap half of this machine. A version
  that ran 9 *more* instructions per triangle but did fewer loads and stores was faster.
- **`-fno-strict-aliasing` makes gcc spill and reload pointers around stores.** A local
  `__restrict` copy *at the use site* fixes it
  (`vu1::LerpVertexBytes * const __restrict p = tri.pos;`). `__restrict` on struct/class
  members produces byte-identical code, so don't bother. It isn't a blanket sweep: check the
  disassembly per site, since it pays off where register pressure is high. Declare restrict
  locals *after* any flush call in the loop body. A cursor passed by reference has its
  address taken and stays in memory, so copy it into a local first.
- **ps2sdk `packet2_add_*` costs ~5 memory ops per word** (it reloads `packet->next` around
  every store). In hot emission, take a local `qword_t * __restrict q = pkt->next`, write whole
  qwords, and store `next` back once.
- **gcc never forms `lq`/`sq` itself.** Struct copies of `alignas(16)` types, `__int128`,
  `vector_size(16)`, `mode(TI)` and aligned `__builtin_memcpy` all lower to `ld`/`sd` pairs (a
  Mat4 is a two-trip loop). Use `ps2/qwords.h` (`CopyQwords`, `StoreQword`, `CopyDrawVertex`)
  at memory-to-memory sites. A transparent lq/sq `operator=` on Vec4/Mat4 was measured and
  rejected, because asm memory operands push register-promoted locals back to memory (+56%
  instructions in `DrawAliasMD2Entity`). The reasons are recorded in `vec_mat.h`'s header.
- `__builtin_ctz/clz` are libgcc calls on the EE. A per-batch ctz cost TexChains +9%.

## What a PCSX2 capture can and can't show

- **No EE cache emulation** (`EnableEECache = false`). Cache-locality work (prefetch, slimmer
  structs, UCAB/uncached buffers, reordering) reads as zero. Don't conclude it's worthless.
  To test it, turn the ini flag on (slow, edit with PCSX2 closed) or use hardware.
- **No GS-internal cost.** CLUT loads, fill rate, texture cache and overdraw are free. Only
  data moved (DMA/GIF qwords) and EE/VU1 instructions are charged. For GS-side changes,
  promise "no regression", never a measurable win.
- **PCSX2 charges roughly per instruction** (~2.5-3 cycles per EE instruction seemed to
  apply). Only a net instruction-count cut shows up. An MMI change that swapped 5 memory ops
  for 1 extra instruction measured *slower* (+3.1%) and was reverted, even though it should
  win on hardware.
- Same build, same capture: rendering stages reproduce within ~0.2 µs/frame, and demo1 ends on
  frame 4183 of 7911 every run. SndMix (~1 µs), Frame (~1 µs) and VSync (~6 µs) wander between
  identical runs. **Code layout alone shifts stages by ~1-2 µs** (0.5 KB of init-only code
  moved Particles by -1.9%). To attribute a delta under ~2%, compare against a same-build
  re-run, and if needed against a variant with HEAD's section sizes (`objdump -h`).
- Quote per-item ratios (e.g. EntGeom per triangle), not the EE mean. Run-to-run spread on
  the EE mean is about ±2%.
- PCSX2 timing ≠ hardware for I/O and FPU latency either.

## The first console capture (2026-10-10, debug build, USB)

The same debug ELF ran `ps2_perftest` on a console and in PCSX2. That test build also narrowed
the guard band to 0.25 through a test cvar (since removed), so the PCSX2 comparison used it too.

- **A PAL console runs at 50 Hz** with `ps2_video_mode auto`, the console's own standard. A frame
  there has 20 ms and the frame rate tops out at 50. The "~48 fps" seen on a release build was
  that cap with a few percent of frames taking two fields. PCSX2's BIOS here is NTSC. Use
  `frame_budget.py --pal` for a PAL capture, and `ps2_video_mode pal` reproduces the pacing in
  PCSX2: 6635 frames for the perf test's 7911, with no steady frame dropped.
- **EE work (Frame - VSync - GsWait) was 1.56x PCSX2's**, 7.2 against 4.6 ms. TexChains was
  1.95x, World 1.6x, LmChains 1.46x, EntGeom 1.3x, Sound and SndMix 1.65-2x. This is the cache
  PCSX2 doesn't model, and it puts real numbers on the cache bullet above.
- **GsWait: 5.4 ms a frame against 0.05.** On hardware the GS/VU1 side takes ~12-13 ms per
  frame for ~4K triangles. PCSX2 charges it nothing.
- **Emergency chain drains cost ~8.6 ms on hardware.** These are frames whose chain outgrows the
  512 KB half: ~9-12% of demo frames, the heavy views at ~6.8K triangles. On the console they
  averaged 28.7 ms against 20.1 ms for the rest. View rose to 17 ms, because the EE waits for
  the GS mid-frame. In PCSX2 the same frames cost ~2 ms more. They were the console's dropped
  frames.
- **259 hitches of ~314 ms (median), one every 640 ms: 83 s of the 171 s of gameplay.** No
  column held the time, and PCSX2 had none. The cause was the keyboard poll waiting out the
  music reader's USB reads: 640 ms is two 8 KB music buffers (see `audio.md`). Input now has its
  own column. **Confirmed** by a second capture with `in_keyboard 0` and the 0.8 band: 7 slow
  frames for 2.6 s of 121 s, and 49.0 fps over gameplay. 2.6% of steady frames were dropped.
  Of the 7 slow frames, two were mid-demo asset loads (the slugs ammo model, the railgun hum)
  and four the demo's own reads (see the demo hitches above); one left nothing in the log. A third
  capture, the profiling release with `in_keyboard 1`, confirmed the fix (no poll while a read
  is pending, and the idle polling). It had the same 7 slow frames, 2.9 s, and a keyboard poll
  cost ~221 µs on hardware, on 4.7% of frames. It's not known whether the reads take that long
  without the capture's own writes to the drive.
- **The profiling release on the PAL console is at the 50 Hz cap:** 49.9 fps over gameplay, 0.77%
  of steady frames dropped, and 3.6% with under 1.7 ms to spare. On the same ELF, EE work was
  6.2 ms against PCSX2's 3.8 (1.63x; TexChains and LmChains ~2x, World 1.77x, EntGeom 1.57x),
  and GsWait 5.5 ms against 0.04. Chain drains, on 11% of frames, averaged 21.4 ms with 3.2 ms
  of vsync left, against 9.0 for the rest: within a PAL field, but 18 ms of work would miss an
  NTSC one.
- **The same console forced to NTSC** (`ps2_video_mode ntsc`, 2026-10-11, the profiling release,
  448 lines) ran at 56.1 fps over gameplay, with 6.8% of steady frames dropped against 0.77% at
  50 Hz. The work per frame matched PAL's: EE 6.0 ms and GsWait 5.3 ms. Only the budget shrank.
  80% of the dropped frames had a chain drain (368 of 461), doing 19.3 ms of work against
  16.7 ms. The rest were heavy views (~6.2K triangles) at the GS's own limit.
- **The guard band's width costs the GS nothing measurable:** GsWait was 5.39 ms at 0.25 and
  5.42 ms at 0.8 on the console, with EE work about the same. So narrowing it buys no speed, and
  the test cvar was dropped.
- **Capturing on a console:** the frame log goes to stdout, which a console has nobody reading.
  Build with `PS2_QUAKE_FRAME_LOG_FILE=1` (profile.h, shipped as 0) and each 64-frame dump is
  also appended to `baseq2/frame_log.txt` with one open/write/close. A profiling release for a
  console capture:
  `rm -rf build/release/src && make release CONFIG_DEFS='-DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=1 -DPS2_QUAKE_FRAME_LOG_FILE=1 -DNDEBUG'`,
  then the same `rm -rf` and a plain `make release` afterwards. Adding `-DPS2_QUAKE_IO_LOG=1`
  gives the same run an `io_log.txt` of every plaque-covered load (per-file read speeds, which
  calibrate the USB command-cost model above); its one write per load lands in the first frame
  after the plaque, so expect a spike in each demo's first row. Keep `ps2_logfile` off for a
  capture, since every console print through it is a ~23 ms USB write. The perf test allows
  240 s for a USB map load. Start it from the Options menu's debug page ("start perf test"),
  or with `ps2_perftest "1"` in the config.

## Capture recipe (`ps2_perftest`, ~6.5 min in PCSX2, debug build)

The run plays demo1 and demo2, then two cinematics through `gamemap`: `eou1_.cin` (20 s) and
`ntro.cin` (210 s), since 2026-10-25. The demos come first, so their rows keep the frame numbers
of older captures. Each cinematic gets a `FLOG#map` marker when its server comes up. A
cinematic's frame-read work is the `Cinematic` column (`PS2_PROF_CINEMATIC` around
`SCR_ReadNextFrame`: the frame's reads, FsIo nested inside, and its Huffman decode), and
`sndFree` is the free bytes of audsrv's ring at the frame's submit (-1 held; the header's
`FLOG#note` gives the ring's size, 9400 at 22 kHz). A `Frame` longer than the ring is a
certain audio underrun (audsrv loops its last 106 ms, `sound_hold.h`); `sndFree` shows how
close the others came, and loses meaning once the ring has run dry. The run's closing
`FLOG#note,present: worst stamp-to-DISPFB N us` is the sleeping vsync wait's worst wake-to-write
gap (gs.h `PresentClock::worstWriteTicks`); it has to stay well inside the ~1500 µs blanking
interval, and read 1 µs in PCSX2.

1. Back up `baseq2/config.cfg` and set `ps2_perftest "1"` in it. `make run`.
2. Wait for `PerfRun: complete` / `FLOG#end` in emulog, then copy `emulog.txt` into
   `build/baselines/<tag>.emulog.txt` before the next launch overwrites it.
3. Restore the config: the quit rewrites it. Since 2026-10-12 the run puts back the panel cvars
   and `developer` it changed before quitting (verified: the written config kept all five
   `ps2_show_*` at 1), so only `ps2_perftest` differs, set back to 0.
4. `src/tools/scripts/frame_log/summarize_flog.py <emulog> --rows build/baselines/<tag>.flog`,
   then `compare_flog.py <before> <after>`. `frame_budget.py` realigns Server/ClParse (they
   land one row early) and lists each `FLOG#open` load with its cost.

- If the notes show `pics/m_main_*.pcx` or `pause.pcx` mid-demo, a stray host key reached
  PCSX2's USB keyboard and opened the menu. Re-run with focus away from PCSX2.
- **Profiling a release build:** set `-DPS2_QUAKE_PROFILE=1` in the release `CONFIG_DEFS`,
  `rm -rf build/release/src`, `make release run` with `ps2_perftest 1`. Revert and rebuild
  clean afterwards.
- `build/baselines/` is untracked and local, holding captures, summaries and config backups.

## Codegen A/B across the backend (no tree edits)

- Baseline: `make release`, then snapshot `build/release/src/ps2/**/*.o` to the scratchpad.
- Take the exact compile line from `make -n <obj>` (or `make -n -W <file>`).
  `SIZE_OPT_CXX_SRC` objects get **`-Os` per object**, and an ad-hoc `-O3` shows dozens of
  spurious diffs. Always build a **control** set with unmodified headers first and require
  zero diffs against the baseline.
- To shadow a header, put the modified copy at `<dir>/ps2/math/vec_mat.h` and pass `-I<dir>`
  *before* `-Isrc`.
- To list every copy/assign site of a type, temporarily `T & operator=(const T &) = delete;` in
  the shadow and compile everything with `-fmax-errors=0`.
- Compare per function (instructions, loads, stores, lq/sq), normalizing branch targets.
  Static counts understate loops, so check loop bodies by hand.

## Proving an EE/VU0 asm rewrite on target (~6 s)

All of this happens in the scratchpad, with nothing added to the repo.

- The test `.cpp` includes the real header plus the HEAD version of the changed functions
  (`git show HEAD:<file>`) inside `namespace ref { ... }`. Qualify ref-internal calls
  (`ref::Foo`), since ADL on shared argument types makes them ambiguous.
- Use deterministic xorshift inputs, `memcmp` every output byte, and print `Tag: PASS/FAIL`.
  Call `SifInitRpc(0)` before `printf`, then `SleepThread()`.
- Build with the release flags. Link:
  `mips64r5900el-ps2-elf-g++ -T$PS2SDK/ee/startup/linkfile -O3 -o t.elf t.o <objs> -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128 -lkernel`
  (add `-lgraph -ldma` if needed).
- Back up emulog, run `PCSX2 -batch -elf t.elf` in a background shell with an `until grep`
  loop on the log (the verdict, or `TLB Miss|EXCEPTION`, plus a deadline), then `kill` the PID
  you started, since SleepThread never exits. Restore emulog.
- The same harness measured vsync timing: an NTSC field is 9609.6 T2 ticks = 59.94 Hz
  (`gs::PresentClock`).
