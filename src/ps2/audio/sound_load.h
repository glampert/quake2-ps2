/* ================================================================================================
 * File: sound_load.h
 * Brief: Mid-level sound effects loaded through the background loader (fs/async_io.h), so a
 *        sound the server names for the first time while a level runs (a pickup's hum, a
 *        monster's first bark, the local player's sexed sounds) no longer costs the frame a
 *        file read: 58 ms for rg_hum.wav from the HDD, 147 ms from a USB stick, in the frame
 *        that asked for it. S_LoadSound hands the file to this when it is not registering a
 *        level; the read lands in a staging buffer, and the pump (fs/aio_pump.h) finishes it on
 *        the main thread - GetWavinfo, PS2_AudioMalloc, ResampleSfx, the half of S_LoadSound
 *        that became S_FinishLoadSound - early in the next frame, so the same frame's S_Update
 *        can mix it. A playsound whose sound is still loading waits in the pending list for up
 *        to half a second (snd_dma.c) and is dropped after that.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_AUDIO_SOUND_LOAD_H
#define PS2_AUDIO_SOUND_LOAD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct sfx_s;

/* Queues the read of 'path' (the file S_LoadSound would FS_LoadFile) for 'sfx'. 1 when queued:
   the sfx's cache arrives later, through the pump, and S_LoadSound returns NULL meanwhile. 0 when
   it can't be: the loader is off, the file is missing or not locatable, memory or a table slot is
   short - the caller then loads synchronously as before, which also reports a missing file. */
int PS2_SoundLoadAsync(struct sfx_s * sfx, const char * path);

/* True while a queued read for 'sfx' has not been finished yet. */
int PS2_SoundLoadPending(const struct sfx_s * sfx);

/* Finishes every queued read, waiting for the loader. Called where sfx slots are cleared or caches
   freed: S_BeginRegistration and S_Shutdown. */
void PS2_SoundLoadsDrain(void);

/* snd_mem.c: the part of S_LoadSound after the file read, on a WAV image in memory. 1 when the
   sfx now has a cache. The data stays the caller's. */
int PS2_SoundFinishLoad(struct sfx_s * sfx, unsigned char * data, int size);

#ifdef __cplusplus
}

namespace ps2::audio {

/* Finishes the reads that landed, oldest first, until 'budgetCycles' of ReadCycles() are spent
   (at least one when any has landed). Returns how many were finished. Main thread, once a frame,
   from aio::Pump. */
int PumpSoundLoads(unsigned budgetCycles);

} // namespace ps2::audio
#endif /* __cplusplus */

#endif /* PS2_AUDIO_SOUND_LOAD_H */
