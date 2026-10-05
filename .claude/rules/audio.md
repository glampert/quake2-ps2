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
  two read buffers from `TryAllocAligned(64)` under `MemTag::Music`, allocated when a track
  opens and freed when it closes (the loading plaque stops music, so they are never held
  across a level load). The main thread yields with `RotateThreadReadyQueue` after
  queueing. Decode tops `s_rawsamples` up to `paintedtime + 7680` in `CDAudio_Update` (after
  `S_Update`), through the cinematics' raw-sample channel, so it costs no extra SIF bandwidth.
  The loop to the ambient track uses stream pass counts, and the wraps are seamless (verified
  bit-exact on target by hash). Behaviour follows id's `cd_win.c` (`cd_loopcount`,
  `cd_looptrack`).
- **WAV fallback (the user's request, for whoever skips `make music`):** per search directory,
  `trackNN.adp`, then `trackNN.wav`, then `Track%02d.wav` (as `make music` accepts; FAT and
  macOS hosts ignore case anyway). The format comes from the header (`"Q2MU"` or RIFF/WAVE),
  not the extension. Only a *missing* file falls through: one that is malformed or can't get
  its buffers (`OpenResult::Unusable`) is reported once and leaves the track silent. WAV is
  16-bit PCM, mono or stereo, 4-48 kHz, walked chunk by chunk (LIST etc. skipped,
  `WAVE_FORMAT_EXTENSIBLE` read, data length clamped to the file size). Buffers grow with the
  byte rate: ~3/8 s each in 4 KB pages, 8-64 KB, so a 44.1 kHz stereo rip holds 2 × 64 KB
  (vs 2 × 8 KB for ADPCM).
- **Rate conversion** (in `cd_audio.cpp`, for any format): equal rates copy straight in;
  exactly double goes through `half_band.h`; anything else through the linear resampler. The
  half-band is 23 taps, Kaiser beta 6.8, Q14 `{5103, -1425, 591, -232, 72, -13}` around a 0.5
  centre: -0.14 dB at 8 kHz, -6 dB at the new Nyquist (inherent), ≥67 dB down for anything
  that folds below 7 kHz. Shorter filters measured: 11 taps -40 dB, 15 taps -50 dB above
  15 kHz. **Codegen:** both channels in one loop spilled (46 live samples, 113 instructions
  per output frame); one channel at a time with the taps spelled out (`-O2` leaves a loop over
  them rolled, reloading each tap) is ~49 per channel. A host harness (ASan + UBSan, `-O1`,
  `-O3` and `-fsanitize=implicit-conversion,integer`) fuzzes the API with the uncommitted input
  poisoned, checks every output bit-exact against an int64 convolution, and drives the
  accumulator to its peak: ±755,749,620, 35% of int32.
- **Cost (PCSX2, debug), Music event:** ADPCM about 138 µs per frame mean (171 µs p50); that
  came from keeping the ADPCM history in locals (to dodge `-fno-strict-aliasing` spills) and
  unrolling per byte. A 44.1 kHz WAV about 274 µs mean (348 µs p50), the decimator being most
  of it. SndMix dropped by 60 µs when music arrived (it copies the ring instead of a memset).
  0 dropped frames either way. Static footprint ~12 KB (the 8 KB reader stack is a static
  member, so it stays in .bss; as an object member the constant-initialized stream put it in
  .data).
- **Verified on target by hash** (scripted, no map needed: music plays at the console): ADPCM
  wraps; a 44.1 kHz WAV looped 3 passes through the decimator; an extensible mono 22 kHz WAV
  with an oversized data length; a forced allocation failure (warning, silence, no leak); and
  `.adp` winning over a `.wav` beside it.
- Audio bring-up failure is not fatal (`ps2_disable_sound 1` turns it off on purpose). The
  libsd/audsrv IRX load is one-shot.

**After music changes:** re-run `ps2_perftest` (compare the Music event with
`build/baselines/cdmusic-on4.flog` for ADPCM, `cdwav-on.flog` for WAV) and a MapCycle
(`mapcycle-cdmusic2.summary.txt` for ADPCM, `mapcycle-cdwav.summary.txt` with the `.adp`
files moved out). Scripted music tests hit the traps in `testing-pcsx2.md`.
