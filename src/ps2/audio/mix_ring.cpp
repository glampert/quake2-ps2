/* ================================================================================================
 * File: mix_ring.cpp
 * Brief: The Quake mixer's paint buffer and the submit cursor over it. See mix_ring.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/mix_ring.h"
#include "ps2/debug/profile.h" // ReadCycles
#include <cstring>

namespace ps2::audio {

void MixRing::Reset()
{
    std::memset(m_buffer, 0, sizeof(m_buffer));
    m_submittedFrames = 0;
}

int MixRing::PositionInSamples() const
{
    // Unsigned so the fold stays well defined once the cursor runs past INT_MAX -
    // which takes about 27 hours of play at 22050Hz, but costs nothing to get right.
    const unsigned int frames  = static_cast<unsigned int>(m_submittedFrames);
    const unsigned int samples = frames * static_cast<unsigned int>(AudsrvDevice::kChannels);
    return static_cast<int>(samples & (static_cast<unsigned int>(kSamples) - 1u));
}

void MixRing::Drain(AudsrvDevice & device, const int paintedFrames)
{
    const int pending = PendingFrames(paintedFrames);
    if (pending <= 0)
    {
        return;
    }
    Commit(Send(device, m_buffer, m_submittedFrames, pending));
}

MixRing::SendResult MixRing::Send(AudsrvDevice & device, const u8 * const buffer, const int startFrames, int pendingFrames)
{
    SendResult result;

    // FreeBytes() is already frame aligned and holds back the guard frame, so this
    // is exactly what audsrv will accept without dropping anything.
    const CpuCycles queryStart = ReadCycles();
    result.freeBytes           = device.FreeBytes();
    result.queryCycles         = ReadCycles() - queryStart;
    const int freeFrames       = result.freeBytes / AudsrvDevice::kFrameBytes;
    if (freeFrames < pendingFrames)
    {
        pendingFrames = freeFrames;
    }

    int frames = startFrames;
    while (pendingFrames > 0)
    {
        const int offsetFrames = frames & (kFrames - 1);

        int chunkFrames = kFrames - offsetFrames; // up to the end of the ring
        if (chunkFrames > pendingFrames)
        {
            chunkFrames = pendingFrames;
        }

        const int chunkBytes      = chunkFrames * AudsrvDevice::kFrameBytes;
        const CpuCycles playStart = ReadCycles();
        const int sentBytes       = device.Enqueue(buffer + (offsetFrames * AudsrvDevice::kFrameBytes), chunkBytes);
        result.playCycles        += ReadCycles() - playStart;
        if (sentBytes <= 0)
        {
            break;
        }

        const int sentFrames = sentBytes / AudsrvDevice::kFrameBytes;
        frames            += sentFrames;
        result.sentFrames += sentFrames;
        pendingFrames     -= sentFrames;

        // A short write means the IOP ring filled up despite the FreeBytes() reading
        // - the rest was dropped, not queued. Stop here and pick it up next frame.
        if (sentBytes != chunkBytes)
        {
            break;
        }
    }

    return result;
}

void MixRing::Commit(const SendResult & result)
{
    m_submittedFrames += result.sentFrames;
    m_lastFreeBytes    = result.freeBytes;

    m_sendTimes.queryCycles = (result.queryCycles > m_sendTimes.queryCycles) ? result.queryCycles : m_sendTimes.queryCycles;
    m_sendTimes.playCycles  = (result.playCycles > m_sendTimes.playCycles) ? result.playCycles : m_sendTimes.playCycles;
}

MixRing::SendTimes MixRing::TakeSendTimes()
{
    const SendTimes times = m_sendTimes;
    m_sendTimes           = {};
    return times;
}

} // namespace ps2::audio
