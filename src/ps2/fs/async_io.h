#pragma once
/* ================================================================================================
 * File: async_io.h
 * Brief: The background loader: one thread that reads file ranges, and inflates deflated pack
 *        entries, into buffers the main thread owns, so a mid-level asset load or a cinematic's
 *        stream never holds a frame on the drive. The design is misc/docs/plans/background-loading.md.
 *
 *        Every file read is a SIF RPC that sleeps its caller until the IOP's file server answers:
 *        41 ms for a cluster-aligned 32 KB chunk on a USB stick, under 10 ms on the HDD. Issued
 *        from the main thread that is a dropped frame, and on a console the drive is 95% of a
 *        mid-level load. Here the main thread resolves where a file lies (FS_LocateFile in
 *        filesys.c: a descriptor, an offset and a size, with no read) and hands the range to
 *        this thread, which runs one priority below it: it never preempts frame work, and gets
 *        the CPU whenever the main thread sleeps, which since the sleeping vsync wait is 8-11 ms
 *        of every frame. The kernel does not time-slice, so the loader's own CPU work (tinfl)
 *        can hold the CPU only until the next interrupt wakes the main thread.
 *
 *        Requests run a chunk at a time, kChunkBytes aligned to the container file's offset (one
 *        SCSI command per chunk on 32 KB clusters), and the best (priority, sequence) request is
 *        re-picked after every chunk: a cinematic chunk waits at most one chunk behind a model.
 *        A main-thread file call meanwhile queues behind the chunk in flight in the SDK's file
 *        client, for the same one chunk at most.
 *
 *        Rules. The loader touches its request slots, the descriptors it is handed (it never
 *        opens or closes one), lseek/read, its one inflate workspace and tinfl. It never
 *        allocates, prints, or touches the engine, the caches or the profiler: errors are codes
 *        in the request, printed by the main thread with the name it kept. The main thread, while
 *        a request is pending, must not free or write its destination, close its descriptor or
 *        sweep the cache slot that owns it; the points that free or close behind a read call
 *        Drain() first (PS2_BeginRegistration, S_BeginRegistration, S_Shutdown, FS_SetGamedir,
 *        a read-ahead stream's close). Destinations are SIF DMA targets: 64-byte aligned and
 *        sized in whole 64-byte units, owning their cache lines (see ps2-platform.md).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::aio {

// A chunk: one read request to the drive. 32 KB clusters (the user's stick) and fileXio's 64 KB
// IOP buffer both take it whole when it starts on a 32 KB boundary of the file.
constexpr u32 kChunkBytes = 32u * 1024u;

constexpr u32 RoundUp64(const u32 n)
{
    return (n + 63u) & ~63u;
}

enum class Kind : u8
{
    Read,         // Copy [offset, offset + bytes) of fd into dest.
    InflateWhole, // Inflate the deflate stream at [offset, offset + bytes) of fd into dest: rawBytes out.
};

// Lower is served first, re-picked at every chunk boundary. A stream's next slot ahead of a
// sound someone is about to need ahead of a model that can draw a few frames late.
enum class Priority : u8
{
    Stream,
    Sound,
    Asset,
};

enum class State : u8
{
    Free,    // Nobody's. Poll() of a stale or invalid ticket.
    Queued,  // Submitted, not started.
    Running, // Some chunks landed: bytesDone says how many bytes of dest are final.
    Done,    // All of it landed; Result() is the bytes delivered.
    Failed,  // Result() is a negative code; dest holds nothing usable.
};

// Result() codes.
constexpr int kErrSeek     = -1; // lseek to the range failed
constexpr int kErrRead     = -2; // read() failed or hit the end of the file early
constexpr int kErrInflate  = -3; // tinfl rejected the data, or it inflated to the wrong size
constexpr int kErrNoThread = -4; // the loader could not start: nothing was read

struct Request
{
    Kind     kind      = Kind::Read;
    Priority prio      = Priority::Asset;
    int      fd        = -1;
    u32      offset    = 0; // the range in fd: for InflateWhole the compressed range
    u32      bytes     = 0;
    void *   dest      = nullptr; // 64-aligned; destBytes >= RoundUp64(what is delivered)
    u32      destBytes = 0;
    u32      rawBytes  = 0; // InflateWhole: the inflated size, what is delivered. Read: ignored (bytes).
};

// What Submit hands back. A default one is invalid; one from a Released slot goes stale, and
// Poll() answers Free for it, so a client that forgets a ticket cannot read another's result.
struct Ticket
{
    int slot     = -1;
    u32 sequence = 0;

    bool IsValid() const { return slot >= 0; }
};

// Queues a request. Returns an invalid ticket when the loader is off (ps2_aio 0), its thread
// couldn't start, every slot is taken, or (InflateWhole) its workspace couldn't be allocated:
// the client then loads synchronously, as it always did. Asserts the destination's alignment.
Ticket Submit(const Request & request);

// The request's state. bytesDone, when asked for, is how many bytes of dest are final: a stream
// may consume a Running request up to there.
State Poll(Ticket ticket, u32 * bytesDone = nullptr);

// Bytes delivered, or a negative code; meaningful once Poll says Done or Failed.
int Result(Ticket ticket);

// The ReadCycles() the request spent inside read(), for the frame log; meaningful once Done.
u32 IoCycles(Ticket ticket);

// Sleeps until the request is Done or Failed. The loader runs meanwhile, as it does whenever the
// main thread sleeps.
void Wait(Ticket ticket);

// Sleeps until at least minBytes of dest are final, or the request ends.
void WaitProgress(Ticket ticket, u32 minBytes);

// Gives the slot back. Must follow Done or Failed: a pending request's buffer is still being
// written. Nothing to do for an invalid or stale ticket.
void Release(Ticket ticket);

// Waits until no request is Queued or Running. Done ones keep their slots until their owners
// Release them. Call it before freeing or closing anything a request might still touch.
void Drain();

// True while any request is Queued or Running: a main-thread file call made now waits behind
// the chunk in flight (the keyboard poll's guard, beside MusicReadPending()).
bool Pending();

// False when ps2_aio is 0 or the thread failed to start: every client loads synchronously.
bool Enabled();

// What the loader did since the last call, for the frame log.
struct Stats
{
    u32 bytesLanded; // bytes delivered into destinations
    u32 chunks;      // read() calls made
    u32 ioCycles;    // ReadCycles() spent inside them
    u32 queueDepth;  // requests Queued or Running right now
};
Stats TakeFrameStats();

} // namespace ps2::aio
