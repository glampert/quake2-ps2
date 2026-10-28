/* ================================================================================================
 * File: readahead_stream.cpp
 * Brief: The loader-fed sequential stream. See readahead_stream.h.
 *
 *  Slot i of the ring is the buffer ring + i * kSlotBytes. The slots are used round robin: 'head'
 *  is the oldest request, the one the reader is consuming, and the slot freed by consuming it is
 *  where the next range of the file is submitted, so the in-flight slots are always head, head+1,
 *  ... in file order. A request covers exactly one loader chunk - the range up to the next 32 KB
 *  boundary of the container file - so every slot lands in one read() and is either whole or not
 *  there yet.
 *
 *  A read that finds its head slot unlanded sleeps in aio::Wait; the loader runs meanwhile. The
 *  frame log charges that sleep to FsIo as it did the stdio refill, so a capture shows how often the
 *  ring ran dry. Seeks: ftell (SEEK_CUR 0) and a seek inside the consumed part of the head slot or
 *  forward within what has landed are served; anything else cancels the ring and restarts it at the
 *  target. Nothing seeks today.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

// newlib only declares fopencookie for GNU sources, and has to see this before any header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "ps2/common.h"
#include "ps2/fs/readahead_stream.h"
#include "ps2/fs/async_io.h"
#include "ps2/system/heap.h"
#include "ps2/debug/io_log.h"

#include <cstdio>
#include <cstring>

namespace ps2::fs {
namespace {

constexpr u32 kSlotBytes   = aio::kChunkBytes;
constexpr int kMaxSlots    = 16; // 512 KB
constexpr int kMinSlots    = 8;  // 256 KB, the fallback when 512 can't be had

#ifdef __LARGE64_FILES
using CookieOffset = _off64_t;
#else
using CookieOffset = off_t;
#endif

struct Slot
{
    aio::Ticket ticket;
    u32         fileOffset; // of its first byte, in the file (0-based)
    u32         bytes;      // the chunk's size; 0 when the slot holds nothing
};

struct Stream
{
    fs_location_t loc;
    u8 *          ring;
    u32           ringBytes;
    int           numSlots;
    Slot          slots[kMaxSlots];
    int           head;       // the slot the reader consumes next
    int           inFlight;   // slots holding a request, head onwards
    u32           nextOffset; // the next file offset to submit
    u32           position;   // the reader's position in the file
    bool          failed;
    char          name[MAX_QPATH];
};

static const cvar_t * s_readaheadKb = nullptr; // ps2_readahead_kb

u8 * SlotBuffer(const Stream & s, const int slot)
{
    return s.ring + static_cast<u32>(slot) * kSlotBytes;
}

// The end of the chunk that starts at file offset 'at': the next kChunkBytes boundary of the
// container file (loc.offset + at), or the file's end.
u32 ChunkEndAt(const Stream & s, const u32 at)
{
    const u32 containerPos = s.loc.offset + at;
    const u32 boundary     = (containerPos / aio::kChunkBytes + 1u) * aio::kChunkBytes - s.loc.offset;
    return (boundary < s.loc.bytes) ? boundary : s.loc.bytes;
}

// Submits the next range of the file into 'slot'. False when nothing is left or the loader declined.
bool SubmitSlot(Stream & s, const int slot)
{
    Slot & sl = s.slots[slot];
    sl.bytes  = 0;
    if (s.nextOffset >= s.loc.bytes)
    {
        return false;
    }

    const u32 end = ChunkEndAt(s, s.nextOffset);

    aio::Request request;
    request.kind      = aio::Kind::Read;
    request.prio      = aio::Priority::Stream;
    request.fd        = s.loc.fd;
    request.offset    = s.loc.offset + s.nextOffset;
    request.bytes     = end - s.nextOffset;
    request.dest      = SlotBuffer(s, slot);
    request.destBytes = kSlotBytes;

    sl.ticket = aio::Submit(request);
    if (!sl.ticket.IsValid())
    {
        return false;
    }
    sl.fileOffset = s.nextOffset;
    sl.bytes      = request.bytes;
    s.nextOffset  = end;
    s.inFlight   += 1;
    return true;
}

// Fills every free slot behind the in-flight ones.
void FillRing(Stream & s)
{
    while (s.inFlight < s.numSlots)
    {
        const int slot = (s.head + s.inFlight) % s.numSlots;
        if (!SubmitSlot(s, slot))
        {
            break;
        }
    }
}

// Takes the ring's requests back and empties it. The destination buffers are the stream's again.
void CancelRing(Stream & s)
{
    for (int i = 0; i < s.inFlight; ++i)
    {
        Slot & sl = s.slots[(s.head + i) % s.numSlots];
        aio::Cancel(sl.ticket);
        sl.ticket = aio::Ticket{};
        sl.bytes  = 0;
    }
    s.inFlight = 0;
}

// Restarts the ring at file offset 'at'.
void RestartAt(Stream & s, const u32 at)
{
    CancelRing(s);
    s.head       = 0;
    s.nextOffset = at;
    s.position   = at;
    FillRing(s);
}

Q_COLD_FUNC void Fail(Stream & s, const char * const what, const int code)
{
    if (!s.failed)
    {
        s.failed = true;
        Com_Printf("WARNING: read-ahead stream %s: %s (%d).\n", s.name, what, code);
    }
}

ssize_t StreamRead(void * cookie, char * dest, const size_t bytes)
{
    Stream & s = *static_cast<Stream *>(cookie);
    if (s.failed)
    {
        return -1;
    }

    const u32 left   = s.loc.bytes - s.position;
    const u32 wanted = (bytes < left) ? static_cast<u32>(bytes) : left;
    u32 copied = 0;

    while (copied < wanted)
    {
        if (s.inFlight == 0)
        {
            // Every slot was declined (the loader went off, or its ring filled up): read what is
            // left through the ring synchronously by resubmitting; if that fails too, give up.
            FillRing(s);
            if (s.inFlight == 0) [[unlikely]]
            {
                Fail(s, "the loader declined the next chunk", 0);
                return -1;
            }
        }

        Slot & sl = s.slots[s.head];
        PS2_Assert(sl.fileOffset <= s.position && s.position < sl.fileOffset + sl.bytes);

        aio::State state = aio::Poll(sl.ticket);
        if (state == aio::State::Queued || state == aio::State::Running)
        {
            // The ring ran dry: the main thread sleeps for this chunk, the loader runs.
            aio::Wait(sl.ticket);
            state = aio::Poll(sl.ticket);
        }
        if (state != aio::State::Done) [[unlikely]]
        {
            Fail(s, "a chunk failed", aio::Result(sl.ticket));
            return -1;
        }

        const u32 slotPos = s.position - sl.fileOffset;
        const u32 n       = (wanted - copied < sl.bytes - slotPos) ? (wanted - copied) : (sl.bytes - slotPos);
        std::memcpy(dest + copied, SlotBuffer(s, s.head) + slotPos, n);
        copied     += n;
        s.position += n;

        if (s.position == sl.fileOffset + sl.bytes)
        {
            // Consumed: the slot takes the next range of the file.
            aio::Release(sl.ticket);
            sl.ticket   = aio::Ticket{};
            sl.bytes    = 0;
            s.head      = (s.head + 1) % s.numSlots;
            s.inFlight -= 1;
            FillRing(s);
        }
    }
    return static_cast<ssize_t>(copied);
}

int StreamSeek(void * cookie, CookieOffset * offset, const int whence)
{
    Stream & s = *static_cast<Stream *>(cookie);
    if (s.failed)
    {
        return -1;
    }

    CookieOffset target = *offset;
    if (whence == SEEK_CUR)
    {
        target += static_cast<CookieOffset>(s.position);
    }
    else if (whence == SEEK_END)
    {
        target += static_cast<CookieOffset>(s.loc.bytes);
    }
    else if (whence != SEEK_SET)
    {
        return -1;
    }
    if (target < 0 || target > static_cast<CookieOffset>(s.loc.bytes))
    {
        return -1;
    }

    const u32 at = static_cast<u32>(target);
    if (at != s.position)
    {
        // Inside the head slot: just move. Anywhere else: start the ring over there.
        const Slot & sl = s.slots[s.head];
        if (s.inFlight > 0 && sl.fileOffset <= at && at < sl.fileOffset + sl.bytes)
        {
            s.position = at;
        }
        else
        {
            RestartAt(s, at);
        }
    }
    *offset = static_cast<CookieOffset>(s.position);
    return 0;
}

int StreamClose(void * cookie)
{
    Stream * const s = static_cast<Stream *>(cookie);
    const bool failed = s->failed;

    CancelRing(*s); // at most one chunk's wait
    FS_ReleaseLocation(&s->loc);
    heap::Free(s->ring, s->ringBytes, heap::MemTag::Aio);
    heap::Free(s, sizeof(Stream), heap::MemTag::Aio);
    return failed ? -1 : 0;
}

} // namespace
} // namespace ps2::fs

// ------------------------------------------------------------------------------------------------
// C API
// ------------------------------------------------------------------------------------------------

extern "C" int PS2_WantsReadAheadStream(const char * const filename)
{
    if (ps2::fs::s_readaheadKb == nullptr)
    {
        ps2::fs::s_readaheadKb = Cvar_Get("ps2_readahead_kb", "512", 0);
    }
    if (ps2::fs::s_readaheadKb->value <= 0.0f)
    {
        return 0;
    }
    const char * const dot = std::strrchr(filename, '.');
    return dot != nullptr && (std::strcmp(dot, ".cin") == 0 || std::strcmp(dot, ".dm2") == 0);
}

extern "C" FILE * PS2_OpenReadAheadStream(const char * const filename, const struct fs_location_s * const loc)
{
    using namespace ps2;
    using namespace ps2::fs;

    if (loc->fd < 0 || loc->kind == FS_LOC_PKZ_DEFLATED || loc->bytes == 0 || !aio::Enabled())
    {
        return nullptr;
    }

    // The ring: ps2_readahead_kb's worth of slots, 512 KB at most; half that if memory is short.
    int numSlots = static_cast<int>(s_readaheadKb->value) / static_cast<int>(kSlotBytes / 1024u);
    numSlots     = (numSlots > kMaxSlots) ? kMaxSlots : (numSlots < 1) ? 1 : numSlots;

    u8 * ring = nullptr;
    while (ring == nullptr && numSlots >= 1)
    {
        ring = static_cast<u8 *>(heap::TryAllocAligned(heap::MemAlign(64), static_cast<u32>(numSlots) * kSlotBytes, heap::MemTag::Aio));
        if (ring == nullptr)
        {
            numSlots = (numSlots > kMinSlots) ? kMinSlots : (numSlots / 2);
        }
    }

    if (ring == nullptr) [[unlikely]]
    {
        Com_Printf("WARNING: no memory for %s's read-ahead ring; streaming it from the drive.\n", filename);
        return nullptr;
    }

    auto * const s = static_cast<Stream *>(heap::TryAlloc(sizeof(Stream), heap::MemTag::Aio));
    if (s == nullptr) [[unlikely]]
    {
        heap::Free(ring, static_cast<u32>(numSlots) * kSlotBytes, heap::MemTag::Aio);
        return nullptr;
    }

    std::memset(static_cast<void *>(s), 0, sizeof(*s));
    s->loc       = *loc;
    s->ring      = ring;
    s->ringBytes = static_cast<u32>(numSlots) * kSlotBytes;
    s->numSlots  = numSlots;
    std::snprintf(s->name, sizeof(s->name), "%s", filename);

    cookie_io_functions_t io = {};
    io.read  = &StreamRead;
    io.seek  = &StreamSeek;
    io.close = &StreamClose;

    FILE * const file = fopencookie(s, "rb", io);
    if (file == nullptr) [[unlikely]]
    {
        heap::Free(ring, s->ringBytes, heap::MemTag::Aio);
        heap::Free(s, sizeof(Stream), heap::MemTag::Aio);
        return nullptr;
    }

    // Unbuffered: newlib then reads straight into the caller's memory, one callback per fread,
    // and the ring is the buffer.
    std::setvbuf(file, nullptr, _IONBF, 0);

    // Every slot in flight from the open: the first fread waits for one chunk, the rest stream
    // behind it.
    FillRing(*s);

#if PS2_QUAKE_IO_LOG
    Com_DPrintf("Read-ahead stream on %s: %d KB ring, %d chunks in flight.\n", filename, numSlots * 32, s->inFlight);
#endif // PS2_QUAKE_IO_LOG

    return file;
}
