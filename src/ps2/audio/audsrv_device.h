#pragma once
/* ================================================================================================
 * File: audsrv_device.h
 * Brief: PCM output device over the audsrv IOP driver. The AudsrvDevice class owns the
 *        IOP-side bring-up (the libsd + audsrv IRX modules) and the streaming session,
 *        exposing audsrv's queue as "how many bytes fit right now" plus "take these".
 *        It knows nothing about Quake: the sound seam (snd.cpp) drives a single static
 *        instance and MixRing (mix_ring.h) does the bookkeeping around it.
 *
 *        The SPU2 is only reachable from the IOP - there is no EE-side mapping of its
 *        registers - so every byte the mixer produces crosses SIF through audsrv. All
 *        of its entry points are blocking RPCs, but none of the ones used here waits on
 *        buffer space, so the whole path stays on the main thread.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::audio {

class AudsrvDevice final
{
public:
    // 16-bit stereo is the only sensible output format: audsrv's upsampler table has
    // no 22050/8/stereo entry, and it is what the mixer's fast path (S_TransferStereo16)
    // produces anyway. Everything downstream is upsampled to the SPU2's native 48kHz.
    static constexpr int kChannels   = 2;
    static constexpr int kSampleBits = 16;
    static constexpr int kFrameBytes = kChannels * (kSampleBits / 8); // one stereo pair

    // Starts libsd and audsrv on the IOP (once per run) and opens a stream at
    // sampleRateHz. Returns false when audio can't be brought up - a missing IOP
    // driver only costs us sound, so the caller is expected to carry on silently.
    bool Init(int sampleRateHz);
    void Shutdown();

    bool Available() const { return m_ready; }

    // Free space in audsrv's IOP ring buffer, rounded down to whole stereo frames and
    // with one frame held back (see the guard note in the .cpp). Zero when the queue
    // is full, which is the normal steady state - it is what paces submission.
    int FreeBytes() const;

    // audsrv's IOP ring in bytes at the rate Init picked (about 106 ms); 0 before Init.
    int RingBytes() const { return m_ringBytes; }

    // Appends to the IOP ring. sizeBytes must be a multiple of kFrameBytes and no
    // larger than the FreeBytes() reading it was derived from. Returns the number of
    // bytes actually accepted, which can be smaller; anything beyond that is dropped
    // by audsrv, so the caller must resume from there rather than assume it all went.
    int Enqueue(const u8 * data, int sizeBytes);

    // Silences the output ahead of a stretch in which nothing will be enqueued (see
    // sound_hold.h): plays out what is queued, rewrites the whole ring with silence and
    // stops audsrv, which otherwise keeps looping the ring through an underrun. Blocks for
    // about the ring's length, ~106 ms. Does nothing if already held.
    void Hold();

    // Ends a Hold. Stopped, audsrv's read cursor stands still in a full ring, and only an
    // enqueue restarts it - into room there isn't. So this puts the cursors back where
    // Init left them, with half the ring free and nothing in it but the hold's silence.
    // Does nothing unless held.
    void Release();

    // Hold and Release in one: the output goes silent now and stays stopped, with nothing
    // stale left to replay, until the next Enqueue starts it again. For a stretch that may
    // or may not come back through a hold (see PS2_SoundSilence).
    void Silence();

    bool Held() const { return m_held; }

private:
    // The IOP's sound driver (ps2::sys::StartSoundDriver), then audsrv_init(). One-shot:
    // a second audsrv_init() would leak the EE-side RPC thread it spawns.
    static bool StartIopSide();

    // audsrv_set_format for the stream Init opened, which also resets audsrv's ring.
    bool SetFormat();

    bool m_ready        = false;
    bool m_held         = false;
    bool m_silent       = false; // Nothing but silence in the ring: a Hold's, and no Enqueue since.
    int  m_sampleRateHz = 0;
    int  m_ringBytes    = 0; // audsrv's IOP ring at m_sampleRateHz; see Init.
};

} // namespace ps2::audio
