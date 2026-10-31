#pragma once
/* ================================================================================================
 * File: mix_ring.h
 * Brief: The buffer the stock Quake mixer paints into (dma.buffer), and the bookkeeping
 *        that turns it into something audsrv can be fed.
 *
 *        The engine wants a ring it can write anywhere in and a play cursor it can read
 *        back; audsrv wants a queue it can be handed chunks of. MixRing bridges the two
 *        exactly the way id's own waveOut backend (win32/snd_win.c) did: the position
 *        reported to the engine is how far we have *submitted*, not where the SPU2
 *        actually is. Because submission is paced by the device's free space, that
 *        cursor advances at precisely the playback rate, and - unlike a real play
 *        position - it is monotonic by construction. GetSoundtime() requires exactly
 *        that (it infers buffer wraps from the value decreasing), and it means a stall
 *        long enough to drain the queue, such as a level load, recovers on its own with
 *        no resync logic. What such a stall sounds like is another matter: audsrv loops
 *        its ring through it, which is what sound_hold.h is for. The cost is latency:
 *        s_mixahead plus the queue depth, which is the same deal the waveOut path shipped
 *        with.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/audsrv_device.h"

namespace ps2::audio {

class MixRing final
{
public:
    // 16384 stereo frames: 743ms at 22050Hz, comfortably more than the 0.2s s_mixahead
    // default asks for. Must stay a power of two - the mixer masks with dma.samples - 1
    // to find its write offset.
    static constexpr int kFrames    = 16384;
    static constexpr int kSamples   = kFrames * AudsrvDevice::kChannels;   // -> dma.samples
    static constexpr int kSizeBytes = kFrames * AudsrvDevice::kFrameBytes; // 64KB

    // Clears the buffer and rewinds the submit cursor. Call before handing Buffer()
    // to the engine.
    void Reset();

    // The mixer writes here directly; this is dma.buffer and stays valid for the
    // lifetime of the program.
    u8 * Buffer() { return m_buffer; }

    // SNDDMA_GetDMAPos: the submit cursor wrapped into the ring, counted in mono
    // samples (a stereo pair counts as two), which is the unit dma.samples is in.
    int PositionInSamples() const;

    // Hands the device everything painted but not yet submitted, as far as it will
    // take, splitting the copy where the ring wraps. paintedFrames is the engine's
    // `paintedtime`. Whatever doesn't fit stays put and goes out on a later frame.
    // Send and Commit in one, on the calling thread.
    void Drain(AudsrvDevice & device, int paintedFrames);

    // What one Send did: the frames the device took, and its FreeBytes() before it took them.
    // For the frame log, the ReadCycles() its two RPCs took: the free-space query, and the play
    // (both of them when the copy splits at the wrap). Either waits on the IOP while a file read
    // is out there.
    struct SendResult
    {
        int sentFrames  = 0;
        int freeBytes   = 0;
        u32 queryCycles = 0;
        u32 playCycles  = 0;
    };

    // The slowest query and play of the sends committed since the last call, in ReadCycles().
    struct SendTimes
    {
        u32 queryCycles = 0;
        u32 playCycles  = 0;
    };

    // Drain's work without the cursor: offers the device up to pendingFrames of buffer from
    // startFrames on (both in the submit cursor's free-running count), as far as its free space
    // goes. It reads the buffer and calls the device, nothing else, so the submit thread
    // (submit_thread.h) can run it while the main thread paints further on: the mixer only
    // writes past paintedtime, and the ring is far longer than a mixahead.
    static SendResult Send(AudsrvDevice & device, const u8 * buffer, int startFrames, int pendingFrames);

    // Applies a Send's result: advances the submit cursor, keeps the free bytes for LastFreeBytes.
    void Commit(const SendResult & result);

    int SubmittedFrames() const { return m_submittedFrames; }
    int PendingFrames(int paintedFrames) const { return paintedFrames - m_submittedFrames; }

    // The device's FreeBytes() as the last Drain found it, before it enqueued anything: what the
    // IOP had played out since the drain before. For the frame log (renderer/profile.cpp).
    int LastFreeBytes() const { return m_lastFreeBytes; }

    // SendTimes since the last call, then starts over. For the frame log.
    SendTimes TakeSendTimes();

private:
    // Total frames handed to the device since Reset(). Free-running; the mask in
    // PositionInSamples() is what folds it back into the ring.
    int m_submittedFrames = 0;
    int m_lastFreeBytes   = 0;
    SendTimes m_sendTimes;

    alignas(64) u8 m_buffer[kSizeBytes] = {};
};

} // namespace ps2::audio
