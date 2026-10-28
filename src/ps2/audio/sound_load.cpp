/* ================================================================================================
 * File: sound_load.cpp
 * Brief: Background sound loads. See sound_load.h.
 *
 *  A small table of reads in flight, each with the sfx it is for, its staging buffer (64-byte
 *  aligned, the file rounded up, tag Aio) and the file's location, which a loose file keeps open
 *  until the read is done. Everything here runs on the main thread; the loader only ever sees the
 *  staging buffer and the descriptor.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/audio/sound_load.h"
#include "ps2/fs/async_io.h"
#include "ps2/system/heap.h"
#include "ps2/debug/profile.h"    // ReadCycles
#include "ps2/renderer/profile.h" // FrameLogNoteReady

#include <cstdio>
#include <cstring>

namespace ps2::audio {
namespace {

constexpr int kMaxPending = 8;

struct PendingSound
{
    sfx_s *       sfx = nullptr; // null: the entry is free
    aio::Ticket   ticket;
    fs_location_t loc;
    u8 *          staging      = nullptr;
    u32           stagingBytes = 0;
    int           length       = 0;
    char          name[MAX_QPATH];
};

static PendingSound s_pending[kMaxPending];
static int          s_numPending = 0;

// The read is over, one way or the other: builds the cache from the staging buffer, or reports,
// and frees the entry.
void Finish(PendingSound & p)
{
    const aio::State state  = aio::Poll(p.ticket);
    const int        result = aio::Result(p.ticket);

    if (state == aio::State::Done && result == p.length) [[likely]]
    {
        PS2_SoundFinishLoad(p.sfx, p.staging, p.length); // reports a bad WAV itself
    }
    else
    {
        Com_Printf("WARNING: %s: background read failed (%d); the sound stays silent.\n", p.name, result);
    }

    aio::Release(p.ticket);
    heap::Free(p.staging, p.stagingBytes, heap::MemTag::Aio);
    FS_ReleaseLocation(&p.loc);
    debug::FrameLogNoteReady(p.name);

    p.sfx     = nullptr;
    p.staging = nullptr;

    PS2_Assert(s_numPending > 0);
    --s_numPending;
}

} // namespace

int PumpLoads(const u32 budgetCycles)
{
    if (s_numPending == 0)
    {
        return 0;
    }

    const CpuCycles start = ReadCycles();
    int finished = 0;
    for (PendingSound & p : s_pending)
    {
        if (p.sfx == nullptr)
        {
            continue;
        }
        const aio::State state = aio::Poll(p.ticket);
        if (state != aio::State::Done && state != aio::State::Failed)
        {
            continue;
        }
        Finish(p);
        ++finished;
        if (ReadCycles() - start > budgetCycles)
        {
            break;
        }
    }
    return finished;
}

} // namespace ps2::audio

// ------------------------------------------------------------------------------------------------
// C API
// ------------------------------------------------------------------------------------------------

using namespace ps2;
using namespace ps2::audio;

extern "C" int PS2_SoundLoadAsync(sfx_s * const sfx, const char * const path)
{
    if (!aio::Enabled() || s_numPending == kMaxPending)
    {
        return 0;
    }

    PendingSound * entry = nullptr;
    for (PendingSound & p : s_pending)
    {
        if (p.sfx == nullptr)
        {
            entry = &p;
            break;
        }
    }
    if (entry == nullptr)
    {
        return 0;
    }

    fs_location_t loc;
    const int length = FS_LocateFile(path, &loc);
    if (length <= 0 || loc.fd < 0)
    {
        if (length >= 0)
        {
            FS_ReleaseLocation(&loc);
        }
        return 0; // missing, empty or unlocatable: the synchronous path answers as it always did
    }

    const u32 stagingBytes = aio::RoundUp64(static_cast<u32>(length));
    u8 * const staging     = static_cast<u8 *>(heap::TryAllocAligned(heap::MemAlign(64), stagingBytes, heap::MemTag::Aio));
    if (staging == nullptr)
    {
        FS_ReleaseLocation(&loc);
        return 0;
    }

    aio::Request request;
    request.kind      = (loc.kind == FS_LOC_PKZ_DEFLATED) ? aio::Kind::InflateWhole : aio::Kind::Read;
    request.prio      = aio::Priority::Sound;
    request.fd        = loc.fd;
    request.offset    = loc.offset;
    request.bytes     = loc.bytes;
    request.dest      = staging;
    request.destBytes = stagingBytes;
    request.rawBytes  = loc.rawBytes;

    const aio::Ticket ticket = aio::Submit(request);
    if (!ticket.IsValid())
    {
        heap::Free(staging, stagingBytes, heap::MemTag::Aio);
        FS_ReleaseLocation(&loc);
        return 0;
    }

    entry->sfx          = sfx;
    entry->ticket       = ticket;
    entry->loc          = loc;
    entry->staging      = staging;
    entry->stagingBytes = stagingBytes;
    entry->length       = length;
    std::snprintf(entry->name, sizeof(entry->name), "%s", path);
    ++s_numPending;
    return 1;
}

extern "C" int PS2_SoundLoadPending(const sfx_s * const sfx)
{
    if (s_numPending == 0)
    {
        return 0;
    }
    for (const PendingSound & p : s_pending)
    {
        if (p.sfx == sfx)
        {
            return 1;
        }
    }
    return 0;
}

extern "C" void PS2_SoundLoadsDrain(void)
{
    for (PendingSound & p : s_pending)
    {
        if (p.sfx != nullptr)
        {
            aio::Wait(p.ticket);
            Finish(p);
        }
    }
}
