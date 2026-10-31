/* ================================================================================================
 * File: sound_hold.h
 * Brief: Holds the sound output silent across a stretch the main loop can't run in - a save
 *        game or config.cfg going to its device or coming off it, a level load - instead of
 *        letting it stutter.
 *        NOTE: Shared header between C and C++.
 *
 *        audsrv's IOP thread plays its ring buffer (about 106 ms of audio) whether or not
 *        anything new has reached it: the read cursor never waits for the write cursor. So a
 *        main loop that stops feeding it for longer than that - a frame blocked on the memory
 *        card, or on the USB reads of a level load - gets the ring's last 106 ms played over
 *        and over until it comes back. That was the soundtrack's stutter on saves, config
 *        writes and level changes. Pausing the music alone would not have helped: the ring
 *        loops whatever it last held.
 *
 *        A hold plays out what audsrv has queued, rewrites its whole ring with silence and
 *        stops it (AudsrvDevice::Hold, about 106 ms). While it lasts, SNDDMA_Submit sends
 *        nothing, so the mixer's play position stands still and everything it paints - the
 *        music included - waits where it was. The first frame after the release carries on
 *        from that very sample, after the half ring of silence the release starts with. The
 *        save notice (save/save_notice.h) holds the output for as long as it is up, once the
 *        music has faded out (audio/cd_audio.h).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_AUDIO_SOUND_HOLD_H
#define PS2_AUDIO_SOUND_HOLD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Counted: holds the output across one blocking stretch. Every Begin needs its End. */
void PS2_SoundHoldBegin(void);
void PS2_SoundHoldEnd(void);

/* Silences the output now, unless a hold already has: flushes what is queued and stops audsrv,
   leaving nothing stale to replay, and the next frame's submit starts it again. For a stretch
   that may block with no hold over it - SCR_BeginLoadingPlaque's, which stops every sound and
   then often loads with no plaque up at all (the console is down, or the client is between
   servers). Whatever the frames between its loading steps submit afterwards is silence. */
void PS2_SoundSilence(void);

/* The loading plaque's hold, set and cleared with the plaque's own state rather than counted:
   the plaque can be begun again while it is up, and it comes down on a timeout as well as in
   SCR_EndLoadingPlaque. See renderer/loading_screen.cpp. */
void PS2_SoundSetLoadingHold(int held);

#ifdef __cplusplus
}

#include <tamtypes.h>

namespace ps2::audio {

// Whether the output is held now, by a PS2_SoundHoldBegin or by the loading plaque: nothing the
// mixer paints goes to audsrv until the release.
bool OutputHeld();

// For the frame log: the free bytes of audsrv's ring the last SNDDMA_Submit found before it
// topped it up (-1 while held, -2 when it sent nothing: the send before still out on the IOP, or held
// back behind a background read), and the ring's size. See FrameSample::sndFree in renderer/profile.cpp.
int SubmitFreeBytes();
int DeviceRingBytes();

// Also for the frame log: the ReadCycles() of the slowest free-space query and play RPC of the
// sends that finished since the last call, then starts over. See FrameSample::sndQueryCycles.
void TakeSubmitTimes(u32 & queryCycles, u32 & playCycles);

} // namespace ps2::audio
#endif /* __cplusplus */

#endif /* PS2_AUDIO_SOUND_HOLD_H */
