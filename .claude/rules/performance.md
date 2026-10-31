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
  PCSX2.
- **The USB capture with the loader (2026-10-27)**: the same console and stick, the profiling
  release of stages 1-6 with the IOP reset (`quake2_profile_aio_2026-10-26.elf`), wLaunchELF
  `mass:` launch; `console-usb-aio-2026-10-27.flog`, raw `frame_log_usb_aio_2026-10-27.txt` and
  the analysis against `console-usb-2026-10-26.flog` by `FLOG#map` segment (the user opened the
  menu again during demo1, rows 3093-3284, and its config write is row 3700's 1.55 s).
  - **Cinematics: every refill frame is gone.** Rows over the 106 ms audsrv ring went from 86 of
    817 (eou1_) and 1235 of 7387 (ntro) to one each, the cinematic's open. FsIo on cinematic
    rows: p95 93 ms → 0.45-0.63 ms (the copy out of the ring), max 1.8 s → 1.2 ms. The loader
    ran 1-7 requests deep through ntro (`aioQ`), landed 80 MB, and the ring never waited. The
    decode rows are unchanged: 15 ms of EE work on eou1_ and 10-28 ms on ntro (the Huffman
    decode plus `cin::DrawFrame`'s 3.7 ms re-upload), 25-35 ms of wall time in both captures,
    so 54-74% of them span two fields either way. That is what stage 7 would move off the frame;
    it never reaches the ring, and a 14 fps picture doesn't show it, so stage 7 stays deferred.
    The cinematic starts cost what they did (eou1_ 2.8 → 1.0 s for the open and table, ntro
    1.7 s both times); they are the "4-6 s per cinematic" item, not the stutter.
  - **Pickups:** the slugs model and skin row went from 90.7 ms of ClParse and a 99.9 ms frame to
    0.5 ms and 19.9 ms, the model ready 9 frames (180 ms) after its request and the skin 11;
    `a_slugs.pcx` plus `rg_hum.wav` from 146.8 ms and a 163 ms frame to 0.5 ms and 19.8 ms, both
    ready 5 frames later, the frame that finished them 27.6 ms. Steady demo frames dropped 0.24%
    against 0.21%; the demo stream's own refills are as before (Server 172 and 333 ms against 125
    and 346: the pkz deflates `.dm2`, so it gets no read-ahead).
  - **The sleeping vsync:** worst wake-to-DISPFB 3 µs over 18261 presents.
  - **`io_log.txt` was empty (three loads, 0 files), as in the 10-26 HDD and reset runs:** the perf
    run issues its loads on a disconnected client, so no plaque goes up and the I/O log, which
    is the plaque's lifetime, sees nothing. The frozen 10-26 run caught its load only because the
    client was still connected when `demomap` ran. Measure loads from the menu, as the A/B
    recipe below says.
  - `sndFree` reads 7520 or 8460 (of 9400) on isolated single rows every 1-3 s in every segment of
    both captures, at normal 20 ms frames: a sampling artefact of the ring report, not an
    underrun. Judge underruns by `Frame` over the ring.
- **The HDD capture with the loader (2026-10-28)**: the same console, `quake2_profile_aio_2026-10-28.elf`
  launched from the HDD; `console-hdd-aio-2026-10-28.flog`, raw `frame_log_hdd_aio_2026-10-28.txt`,
  against `console-hdd-2026-10-26.flog` by `FLOG#map` segment. The user opened the menu in base2
  (rows 229-518: its pics load synchronously by design, 60-64 ms of FsIo each).
  - **Cinematics: the main thread no longer reads.** FsIo on cinematic rows p95 9.6 ms → 0.46
    (eou1_) and 9.7 → 0.63 ms (ntro), max 716 and 55 ms → 1.2 ms; ntro's 1237 refills had cost the
    main thread 13.8 s. Rows over 30 ms: eou1_ 20.8 → 15.3%, ntro 26.2 → 21.6%. What is left is
    decode (p50 9.9 and 12.7 ms) plus `cin::DrawFrame`'s 3.7 ms of Ui, over a field on its own
    for the heavy ntro frames. eou1_'s start (map marker to first picture) 2.46 → 1.71 s, ntro's
    2.55 → 2.65 s.
  - **New on HDD: a background read stalls the audio submit.** 54 ntro rows and 3 eou1_ rows had
    `SndMix` of 8-44 ms (1.37 s in all on ntro), every one a row where a loader chunk landed: 2.2%
    of the 2455 chunk rows, whose SndMix p99 is 24 ms against 0.6 ms on the others. The USB capture
    has none (p99 1.3 ms on chunk rows). Gameplay rows show the same stall with no loader request
    (aioQ 0): 7 rows in the 10-26 HDD capture, 9 now, which fits the music reader's reads (the
    10-26 note's "audsrv submit RPC that waited 42 ms on the IOP"). The likely mechanism, from the
    SDK sources: on the IOP audsrv's RPC thread and fileXio's both run at priority 40, with no time
    slicing, and the HDD path holds the CPU in places (dev9's DMA busy-waits by design,
    `ps2dev9.c`: "thread switching hurts throughput greatly"), so `audsrv_available` and
    `audsrv_play_audio` wait out part of the read. Where the time goes wasn't measured. At ≤44 ms
    it never empties the 106 ms ring; it costs a field. Plan §12.4 has the fix.
  - **Pickups:** the slugs model and skin row ClParse 42.9 → 0.48 ms, the model ready 2 frames
    after its request and the skin 2; `a_slugs.pcx` plus `rg_hum.wav` 61.0 → 0.44 ms, ready 3-4
    frames later. But the next frame's submit waited 29 and 27 ms behind those reads, so the frames
    went from 61 and 83 ms to 49 ms each: one dropped field, not two or three.
  - The demo stream's refills are unchanged (Server 60-63 ms against 65-72: `.dm2` is deflated).
    Worst wake-to-DISPFB 10 µs over 18189 presents. `io_log.txt` empty again, for the reason above.
- **Plan §12.3 and §12.4 in PCSX2 (2026-10-29, debug, `ps2_aio_throttle_kbps 900`,
  `ps2_sound_defer 2`; `pcsx2-defer-cinconv-throttled-2026-10-29.flog` against
  `pcsx2-models-throttled-2026-10-26.flog`):** `cin::DrawFrame` converting and uploading only on a
  new picture took cinematic rows' Ui from 5.5 ms to 0.72 mean (4 µs p50, 3.0 ms on the one row in
  four that converts) and their EE work from 9.3-9.4 to 4.0-4.2 ms. The held-back submit: 4626 of
  ntro's 12141 rows and 324 of eou1_'s 1165, never two in a row, `sndFree` max and p99 the same as
  without it (4700, 3760), no frame past the ring but the starts'. A first run with a 40 ms limit
  held submits back for 67 ms in a row, hence 30. The console captures are the gate.
- **The console captures of §12.3 and §12.4 (2026-10-29)**: `quake2_profile_aio_2026-10-29.elf`
  from the HDD and from USB (`console-{hdd,usb}-aio-2026-10-29.flog`, raw
  `frame_log_{hdd,usb}_aio_2026-10-29.txt`), against `console-hdd-aio-2026-10-28.flog` and
  `console-usb-aio-2026-10-27.flog` by segment, no menu detour in either.
  - **Cinematic Ui 3.68 ms → 0.67 mean (13 µs p50)**, on both devices; EE work on cinematic rows
    7.1 → 4.7 ms (eou1_) and 8.4-8.5 → 5.6 ms (ntro). ntro's mean frame went from 21.1-21.2 to
    19.9 ms, the 50 Hz cadence; eou1_ was there already (19.5-19.8). A decode row still runs ~30 ms
    and the next ~10 (the GS latency pipeline catching up), so rows over 30 ms stay at ~20%, but
    the pairs no longer lose a field.
  - **HDD submit stalls (SndMix > 8 ms): 66 → 38**, gameplay 9 → 2, ntro 54 → 32 (max 44 → 37 ms),
    chunk rows' SndMix p99 24 → 18 ms. 882 submits were held back, never two in a row; 849 of them
    (96%) were followed by a clean submit. 30 of the 38 left came right after a hold, a read still
    out 20 ms later (HDD reads past ~40 ms, the drive's slow path); the other 8 followed a frame
    over 30 ms, when the rule doesn't hold back, or the cinematic's start. `sndFree` max on the
    gameplay segments 8460 → 6580: the ring came no nearer empty.
  - **The pickups no longer drop a field on HDD**: the slugs model's frames 30.5 + 9.8 ms (from 49
    + 12), `a_slugs.pcx` plus `rg_hum.wav` 28.9 + 17.0 ms (from 49 + 12); ready 2-3 frames after the
    request. On USB both pickups' frames are ~20 ms; no submit held back there (`ps2_sound_defer` 1).
  - Worst wake-to-DISPFB 5 µs (HDD) and 3 µs (USB).
  - **Open: the submit costs more on cinematic rows since this build.** `Sound` (SNDDMA_Submit's
    two audsrv RPCs) on cinematic rows went from ~0.40 ms mean (0.39 p50) to 0.92-1.00 ms (1.0 p50)
    on both devices; gameplay rows are unchanged (~0.41). It eats ~0.6 ms of §12.3's 3 ms. PCSX2
    shows the same step (227 → 429-564 µs). Not understood: the frames are shorter since §12.3, and
    the wait is on the IOP's side of the RPC; `ps2_sound_defer` is off on USB, so it isn't the hold.
- **The music on the loader (plan §12.1), PCSX2 (2026-10-29, debug, `ps2_aio_throttle_kbps 900`;
  `pcsx2-music-aio-throttled-2026-10-29.flog` against `pcsx2-models-throttled-2026-10-26.flog`):**
  on the demo segments Music 138 µs mean (unchanged), SndMix 415-418 (416-419), Input 27 (27),
  no frame over the ring; the music's requests land ~26 KB/s through the loader, one 32 KB request
  every ~1.25 s, each in two reads (the stereo groups sit 2 KB off the chunk boundary).
- **The music on the loader, console USB (2026-10-29, the profiling release, `in_keyboard 1`;
  `console-usb-music-aio-2026-10-29.flog` against `console-usb-aio-2026-10-29.flog`):** no Input
  stall (max 0.64 ms; the poll's ~0.2 ms rows on 6% of gameplay frames, 5% before), Music
  131 µs mean against 138 and its worst row 0.59 ms against 1.38 (the reader's own reads are gone
  from main), one 32 KB request landing every 65 frames (1.3 s) in `aioBytes`, mostly as a 30 + 2
  KB pair. Steady drops 10 against 9, the same two kinds: the demos' own refills (Server
  314-391 ms) and waste2's heavy view (frames ~4585-4595, chain-bound).
- **The music on the loader, console HDD (2026-10-29, same ELF, `in_keyboard` off;
  `console-hdd-music-aio-2026-10-29.flog` against `console-hdd-aio-2026-10-29.flog`): a
  regression.** Blocked submits on gameplay rows (`Sound` over 3 ms, each right after a -2 hold)
  went from 2 to 13. 11 are music requests: one in ~6 of its 32 KB requests (every ~385 frames)
  is still out after the one-frame hold, and the submit then waits 4-13 ms behind it (frames
  23-32 ms). The reader's 8 KB reads were short enough for the hold. The two left are the slugs
  pickup's first read (6.6 ms here, 11.0 before), as in the earlier capture. The ring never came
  near empty at any of them (≥ 3.7 KB, ~42 ms, left at the reply), and each pair of rows still
  spans two fields, so the log shows neither an underrun nor a lost field; the stutter heard at
  the pickups from the HDD is not explained by it yet. USB: no hold, one 3.5 ms submit.
- **The submit on its own thread (plan §12.4, 2026-10-30), PCSX2** (debug, `ps2_aio_throttle_kbps
  900`; `pcsx2-sound-async-throttled-2026-10-30.flog` against
  `pcsx2-music-aio-throttled-2026-10-29.flog`): `Sound` 210 → 8 µs on the demos and 429 → 8 on the
  cinematics, SndMix (which holds it) 416 → 218 and 506 → 86; EE work per frame 4467 → 4340 µs
  (base2), 4609 → 4482 (waste2), 4081 → 3687 (eou1_) and 4220 → 3819 (ntro). The cinematic rows'
  extra submit cost of the 10-29 build (the open item above) is gone from the main thread with
  the rest. Music rose 137 → 201 µs and Input 27 → 29-34: the thread's copy and play RPC run when
  the free-space reply lands, which is during `CDAudio_Update`, right after the submit. No `-2`
  row (a send still out at the next frame), `sndFree` distribution and frames past the ring as
  before. `ps2_testsaves 1` passed with its music fades at 484-688 ms, as before. The console HDD
  capture is the gate.
- **The submit thread on the console, HDD (2026-10-31)** (`quake2_profile_soundasync_2026-10-30.elf`,
  `console-hdd-soundasync-2026-10-31.flog` against `console-hdd-music-aio-2026-10-29.flog`): rows
  with `Sound` over 3 ms 56 → 0 (gameplay 13 → 0), its worst row 32 ms → 125 µs; steady frames
  dropped 17 → 10 (`frame_budget.py --pal`), EE work p50 unchanged; SndMix 624-994 → 129-254 µs,
  Music 129 → 191, Input 29 → 32-36. The slugs model pickup no longer stretches a frame (26 ms
  before; 19.8-20.5 now). Sends still out at the next frame (`-2`): 55 runs over the run, 43 of
  one frame, 11 of two and one of four (waste2 frame 4727, behind a 32 KB music request that
  landed six frames after it went out). The `sndFree` distribution matches the 10-29 capture,
  readings of an empty ring (8460) included (3-5 per segment in both), but two of the new ones
  follow a send that was out: the four-frame run, and the `a_slugs.pcx`/`rg_hum.wav` pickup
  (frame 3986). Whether either is heard is the open ear check. From USB that ELF froze at the end
  of base2's load three times, the last with `ps2_sound_async 0` (`quake2.log` ends at
  `sky 256`): it lacked `PS2_QUAKE_ALWAYS_RESET_IOP=1`, so it ran on the launcher's usbhdfsd (the
  2026-10-26 freeze below). The USB capture needs `quake2_profile_soundasync_reset_2026-10-31.elf`.
  **USB, with that ELF (2026-10-31)** (`console-usb-soundasync-2026-10-31.flog` against
  `console-usb-music-aio-2026-10-29.flog`): rows with `Sound` over 1 ms 6603 → 0 (over 3 ms
  3 → 0), its worst row 33 ms → 132 µs; steady frames dropped 10 → 7, EE work mean 10182 →
  9929 µs; SndMix 624-1037 → 130-255 µs, Music 130 → 195, Input 35-45 → 32-35. Only 4 `-2`
  rows (two two-frame runs, both in cinematics). Both pickups now run at 17-23 ms frames with no
  empty-ring reading; on 10-29 the slugs model cost a 23 ms frame (a 3.5 ms submit) and
  `rg_hum.wav` an empty ring. The `sndFree` distribution matches. Readings that go from a full
  ring (≤ 1880 free) to an empty one (≥ 7520) across one short frame, which playback can't do,
  are as common as before (HDD 10 → 10, USB 50 → 48): an older quirk of the reading, or of
  audsrv's ring, not the thread's; not looked into. That run's I/O log recorded no loads (0
  files in each), so it has no load times.
  Since 2026-10-31 every build resets on a console boot unless built with
  `-DPS2_QUAKE_ALWAYS_RESET_IOP=0`; to check an ELF, `strings -a <elf> | grep -c "every console
  boot does"` gives 1 (an ELF from before then said `PS2_QUAKE_ALWAYS_RESET_IOP is set`).
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
