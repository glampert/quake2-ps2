---
paths:
  - "src/ps2/audio/**"
  - "src/tools/host/musenc.cpp"
---

# Audio and CD music

The README's "Sound" section covers `AudsrvDevice` and `MixRing`. The decisions and
measurements behind CD music (`cd_audio.cpp`, `music_stream.*`, `spu_adpcm.h`,
`tools/host/musenc.cpp`, `make music`):

- **Format (the user's choice):** SPU2 ADPCM, 22050 Hz stereo (= `dma.speed`, so no
  resampling), 2048-byte chunk interleave so the files stay SPU2-voice-streamable later, and a
  2 KB `"Q2MU"` v1 header. Files are `baseq2/music/trackNN.adp`, always lowercase. The
  soundtrack's 10 WAVs (289 MB) encode to 41.36 MB, at 26-42 dB SNR (dense tracks lowest).
  musenc does the exact 2:1 decimation itself (255-tap Kaiser sinc). `spu_adpcm.h` is shared
  by game and encoder, so it uses `<cstdint>`.
- **Pipeline (the user's choice): decode on the EE**, not IOP → SPU2 voices. ps2snd.irx is
  buggy: it re-reads the header as ADPCM, `DeleteThread`s itself at EOF, and doesn't loop. And
  audsrv owns SPU2 core 1 and zeroes core 0's MVOL. A hardware-voice path means a custom IRX,
  which is deferred.
- **Mechanics:** a reader thread at the main thread's priority (1, so it never preempts),
  2 × 8 KB `alignas(64)` buffers. The main thread yields with `RotateThreadReadyQueue` after
  queueing. Decode tops `s_rawsamples` up to `paintedtime + 7680` in `CDAudio_Update` (after
  `S_Update`), through the cinematics' raw-sample channel, so it costs no extra SIF bandwidth.
  The loop to the ambient track uses stream pass counts, and the wraps are seamless (verified
  bit-exact on target by hash). Behaviour follows id's `cd_win.c` (`cd_loopcount`,
  `cd_looptrack`).
- **Cost (PCSX2, debug):** about 144 µs per frame mean for the Music event. That figure came
  after keeping the ADPCM history in locals (to dodge `-fno-strict-aliasing` spills) and
  unrolling per byte. SndMix dropped by 60 µs (it copies the ring instead of a memset). Net EE
  work rose 88 µs, with 0 dropped frames.
- Audio bring-up failure is not fatal (`ps2_disable_sound 1` turns it off on purpose). The
  libsd/audsrv IRX load is one-shot.

**After music changes:** re-run `ps2_perftest` against `build/baselines/cdmusic-base.flog` and
a MapCycle (compare with `mapcycle-cdmusic.summary.txt`). Scripted music tests hit the traps
in `testing-pcsx2.md`.
