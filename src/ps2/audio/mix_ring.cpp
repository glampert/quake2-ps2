/* ================================================================================================
 * File: mix_ring.cpp
 * Brief: The Quake mixer's paint buffer and the submit cursor over it. See mix_ring.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/mix_ring.h"
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
    int pending = paintedFrames - m_submittedFrames;
    if (pending <= 0)
    {
        return;
    }

    // FreeBytes() is already frame aligned and holds back the guard frame, so this
    // is exactly what audsrv will accept without dropping anything.
    m_lastFreeBytes      = device.FreeBytes();
    const int freeFrames = m_lastFreeBytes / AudsrvDevice::kFrameBytes;
    if (freeFrames < pending)
    {
        pending = freeFrames;
    }

    while (pending > 0)
    {
        const int offsetFrames = m_submittedFrames & (kFrames - 1);

        int chunkFrames = kFrames - offsetFrames; // up to the end of the ring
        if (chunkFrames > pending)
        {
            chunkFrames = pending;
        }

        const int chunkBytes = chunkFrames * AudsrvDevice::kFrameBytes;
        const int sentBytes = device.Enqueue(m_buffer + (offsetFrames * AudsrvDevice::kFrameBytes), chunkBytes);
        if (sentBytes <= 0)
        {
            break;
        }

        const int sentFrames = sentBytes / AudsrvDevice::kFrameBytes;
        m_submittedFrames += sentFrames;
        pending -= sentFrames;

        // A short write means the IOP ring filled up despite the FreeBytes() reading
        // - the rest was dropped, not queued. Stop here and pick it up next frame.
        if (sentBytes != chunkBytes)
        {
            break;
        }
    }
}

} // namespace ps2::audio
