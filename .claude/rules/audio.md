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
- **A main-thread file call waits out a music read.** The SDK file client and the IOP's file
  server take one request at a time. So anything the main thread reads or writes while the
  reader has a read out waits for it, and on a console USB drive that took ~300 ms. The USB
  keyboard poll is a `read()` on `usbkbd:dev` every frame. A 2026-10-10 console capture showed
  it stalling a frame every 640 ms: 83 s of 171 s of gameplay. It now skips while
  `audio::MusicReadPending()`, and the driver keeps the keys queued until then. A console run
  with the keyboard off confirmed the cause, and one with it on and these fixes confirmed the
  cure: no stalls either way. It also polls
  only four times a second unless a key arrived in the last 10 s or is held (keyboard.h), and
  `in_keyboard` is off by default in release builds. Keep any other per-frame file I/O off the
  main thread the same way. PCSX2's `host:` reads are too quick to show it. In PCSX2 a
  per-frame poll cost ~86 µs; with the idle polling the Input column averages 27 µs, against
  20 µs for the pad alone.
- **audsrv loops its ring through an underrun.** Its IOP play thread advances the read cursor
  whether or not anything was written (audsrv.c's `play_thread`), so a main loop that stops
  submitting for longer than the ring (10 feed blocks: 9400 bytes, ~106 ms at 22050 Hz) gets
  the last 106 ms played over and over. That was the stutter on saves, config writes and
  level changes from USB; the user heard it as the music stuttering. Pausing the music alone
  can't fix it. `AudsrvDevice::Hold` plays the queue out with silence until a whole ring has
  been rewritten, then `audsrv_stop_audio()` (which renders zeros and freezes the read cursor).
  Stopped, the ring is full, and only `audsrv_play_audio` restarts it, so `Release` calls
  `audsrv_set_format` again to reset the cursors with half the ring free. While held,
  `SNDDMA_Submit` sends nothing. The submit cursor is the mixer's play position, so the mixer
  stops a mixahead ahead and the music ring stops filling, and both resume on the same sample.
  Holds: the save notice, for as long as it is up (`save/save_notice.cpp`: every read and write
  of a save device, a save's from before the level-file compression on), and the plaque's
  lifetime (`PS2_LoadingScreenBegin/End`, timeout included). `SCR_BeginLoadingPlaque` also
  silences the output at once, for loads that skip the plaque (the console down, a new game
  between servers). It runs several times per load, so the device tracks whether only silence
  went in since its last flush and then stops without one. Verified in PCSX2 (`ps2_testsaves 2`,
  with temporary prints): every flush took 54-106 ms, audio came back after every release, and
  each load paid one flush. On a console with USB data the user found it much better
  (2026-10-11).
- **Fading the music out ahead of the save notice's hold (`cd_audio.h`, brought over from the
  Quake port, 2026-10-23):** the hold above stops the music dead where audsrv's queue ends, and
  starts it again just as abruptly. `HoldMusic` fades out what is already queued in
  `s_rawsamples` in place, ahead of the mixer (300-350 ms of it: the pump keeps the ring up to
  7680 frames ahead of `paintedtime`), so nothing decoded is dropped and `ReleaseMusic` picks up where
  the fade ended. The fade is 500 ms, or as long as what is queued if that is longer: past the
  queued frames the pump decodes on, the ramp laid over what it stores, and stores nothing past
  the fade's end. `HoldMusic` then paints the mix itself (`S_Update_`, every 5 ms) until
  `soundtime`, the submit cursor, is past the fade's end, so all of it has gone to audsrv; the
  hold that follows plays audsrv's queue out before it stops it. A held output (the plaque's)
  takes nothing from the mixer, so then there is no fade. `ReleaseMusic` arms a 1 s fade-in,
  laid on the first frames stored after it; closing the track drops it, so a load's new track
  starts at full volume. Sound effects aren't faded: they play on through the fade, then the hold
  cuts them.
- **What the mixer has painted ahead (`s_mixahead`, 0.2 s here, 0.1 s in the Quake port) plays at
  full volume before the fade begins**, and the fade starts at `paintedtime`. So `HoldMusic`'s
  wait is the lead plus the fade: 564-688 ms for a 500 ms fade over the save test's notices
  (PCSX2, debug, base1's track, 2026-10-23), with 6000-7400 of the fade's 11025 frames queued
  already. The lead also covers the main thread until the fade begins; `BeginSaveNotice` tops
  the mix up (`audio::MixAhead`) before the probe (whose first card access of a session starts
  MCMAN and MCSERV) and before the notice's frame.
- **The fade's gain** is Q14, applied as `((v >> 8) * g) >> 6`: a ring sample is 16 bits times
  up to 256 (`kUnityGain`), so the product stays under 2^29. The Quake port's host harness
  checked the same code (UBSan with `-fsanitize=integer`, 180 cases across the ring's wrap):
  within the gain's 1/16384, monotonic, ending 1/8192 from silence.
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
