/* ================================================================================================
 * File: async_io.cpp
 * Brief: The background loader thread. See async_io.h.
 *
 *  Ownership of a slot moves with its state: the main thread owns a Free slot and a Done or
 *  Failed one, the loader a Queued or Running one. Each side writes a slot's fields and then its
 *  state last, behind a compiler barrier, and reads the state first; the two threads share one
 *  in-order CPU, so that is all the ordering the handoff needs (the kernel switches threads, it
 *  never runs them at once). Nothing here locks.
 *
 *  Only the loader reads the drive. It keeps where the descriptor it last read is, so the chunks
 *  of one request skip their lseek: on USB every call to the IOP's file server costs a SCSI
 *  command's latency, and a 32 KB chunk is one command when it starts on a cluster. A request's
 *  first chunk seeks unless the client marked it sequential (Request::sequential: the music and
 *  the read-ahead ring, whose requests follow one another on a descriptor only the loader
 *  moves), and then only when the cache says the position is elsewhere - another request on the
 *  same pak descriptor came between, or the stream wrapped. Unmarked, a request always seeks:
 *  a descriptor number can come back from a close as a different file, and the main thread may
 *  have read a file before handing it over.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/fs/async_io.h"
#include "ps2/fs/async_io_c.h"
#include "ps2/system/heap.h"
#include "ps2/debug/profile.h" // ReadCycles

#include <cstdint> // std::uintptr_t
#include <cstring>
#include <delaythread.h>
#include <kernel.h>
#include <miniz.h>
#include <unistd.h>

namespace ps2::aio {
namespace {

// ------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------

constexpr int kNumSlots   = 32;
constexpr int kStackBytes = 16 * 1024;

// The sequence a slot carries in its ticket. A Release moves the slot on, so a stale ticket
// never matches. 0 is never issued: a default Ticket's.
struct Slot
{
    volatile State state = State::Free;
    Priority       prio  = Priority::Asset;
    Kind           kind  = Kind::Read;
    u32            sequence  = 0;
    int            fd        = -1;
    u32            offset    = 0;
    u32            bytes     = 0;
    u32            rawBytes  = 0;
    u8 *           dest      = nullptr;
    u32            destBytes = 0;
    bool           sequential = false;
    volatile u32   bytesDone = 0; // loader writes, main reads
    volatile int   result    = 0; // loader writes, main reads
    volatile u32   ioCycles  = 0; // loader writes, main reads
};

// The one inflate in flight: one tinfl state and one input piece. An InflateWhole owns it from its
// first chunk to its last; Reads interleave, another InflateWhole waits.
struct Workspace
{
    alignas(64) u8 input[kChunkBytes]; // a SIF DMA target: whole cache lines
    tinfl_decompressor inflator;
    u32 inStart; // the input holds compressed bytes [inStart, inStart + inBytes) of the entry
    u32 inBytes;
    u32 inPos;
    u32 outPos;
    int owner;   // slot index, or -1
};

static Slot s_slots[kNumSlots];
alignas(16) static u8 s_stack[kStackBytes];

static s32  s_threadId     = -1;
static s32  s_workSema     = -1; // one signal per Submit; the loader drains all it can per wake
static s32  s_doneSema     = -1; // one signal per chunk landed; waiters re-check their slot
static bool s_threadFailed = false;
static u32  s_nextSequence = 1;

static Workspace * s_workspace       = nullptr;
static bool        s_workspaceFailed = false;

static const cvar_t * s_aioEnabled   = nullptr; // ps2_aio
static const cvar_t * s_throttleKbps = nullptr; // ps2_aio_throttle_kbps

// Loader-only: where the last read left its descriptor, and which slot made it.
static int s_cacheSlot = -1;
static int s_cacheFd   = -1;
static u32 s_cachePos  = 0;

// Set while the loader is inside lseek/read (or a throttle's stand-in for one). See FileCallInFlight.
static volatile bool s_inFileCall = false;

// Monotonic, loader-written; TakeFrameStats hands out the differences since its last call.
static volatile u32 s_statBytes    = 0;
static volatile u32 s_statChunks   = 0;
static volatile u32 s_statIoCycles = 0;

// The compiler may not move a slot's field writes past its state write, nor hoist a field read
// above the state read. The CPU is in order and the threads never overlap, so this is enough.
Q_ALWAYS_INLINE void CompilerBarrier()
{
    asm volatile("" ::: "memory");
}

Q_ALWAYS_INLINE bool InFlight(const State state)
{
    return state == State::Queued || state == State::Running;
}

Q_ALWAYS_INLINE bool IsTicketCurrent(const Ticket & ticket)
{
    return ticket.slot >= 0 &&
           ticket.slot < kNumSlots &&
           s_slots[ticket.slot].sequence == ticket.sequence &&
           s_slots[ticket.slot].state != State::Free;
}

// ------------------------------------------------------------------------------------------------
// Loader thread
// ------------------------------------------------------------------------------------------------

// The end of the chunk that starts at 'pos': the next kChunkBytes boundary of the file, or the
// range's end, whichever is first.
u32 ChunkEnd(const u32 pos, const u32 rangeEnd)
{
    const u32 boundary = (pos / kChunkBytes + 1u) * kChunkBytes;
    return (boundary < rangeEnd) ? boundary : rangeEnd;
}

// Reads [pos, pos + bytes) of fd into dest, for slot 'slot'. The cached position stands for the
// slot's own earlier chunks, and for any slot's when the request is sequential. 0, or a kErr code.
int ReadRange(const int slot, const bool sequential, const int fd, const u32 pos, u8 * dest, const u32 bytes,
              volatile u32 & ioCycles)
{
    if ((slot != s_cacheSlot && !sequential) || fd != s_cacheFd || pos != s_cachePos)
    {
        s_inFileCall = true;
        const off_t at = lseek(fd, static_cast<off_t>(pos), SEEK_SET);
        s_inFileCall = false;
        if (at != static_cast<off_t>(pos))
        {
            s_cacheSlot = -1;
            s_cacheFd   = -1; // where a failed call left it is anyone's guess
            return kErrSeek;
        }
        s_cacheFd  = fd;
        s_cachePos = pos;
    }
    s_cacheSlot = slot; // a sequential request's chunks may have skipped the seek above

    u32 left = bytes;
    while (left != 0)
    {
        const CpuCycles start = ReadCycles();
        s_inFileCall = true;
        const ssize_t n = read(fd, dest, left);
        s_inFileCall = false;
        const u32 cycles = ReadCycles() - start;
        ioCycles += cycles;
        s_statIoCycles = s_statIoCycles + cycles;
        s_statChunks   = s_statChunks + 1u;
        if (n <= 0)
        {
            s_cacheSlot = -1;
            s_cacheFd   = -1; // where a failed call left it is anyone's guess
            return kErrRead;
        }
        dest       += n;
        left       -= static_cast<u32>(n);
        s_cachePos += static_cast<u32>(n);
    }
    return 0;
}

void Finish(Slot & s, const int result)
{
    s.result = result;
    CompilerBarrier();
    s.state = (result < 0) ? State::Failed : State::Done;
}

// One chunk of a Read.
void RunReadChunk(const int slot, Slot & s)
{
    const u32 done = s.bytesDone;
    const u32 pos  = s.offset + done;
    const u32 n    = ChunkEnd(pos, s.offset + s.bytes) - pos;

    const int err = ReadRange(slot, s.sequential, s.fd, pos, s.dest + done, n, s.ioCycles);
    if (err != 0)
    {
        Finish(s, err);
        return;
    }

    s.bytesDone  = done + n;
    s_statBytes  = s_statBytes + n;
    if (s.bytesDone == s.bytes)
    {
        Finish(s, static_cast<int>(s.bytes)); // else it stays Running for the next chunk
    }
}

// One piece of an InflateWhole: the next input piece read and inflated straight into dest.
// Mirrors pkz_pack.cpp's InflateWhole (the synchronous path), one loop iteration per call; the
// ps2_testaio self-test compares the two byte for byte.
void RunInflateChunk(const int slot, Slot & s)
{
    Workspace & ws = *s_workspace;
    if (ws.owner != slot)
    {
        tinfl_init(&ws.inflator);
        ws.owner   = slot;
        ws.inStart = 0;
        ws.inBytes = 0;
        ws.inPos   = 0;
        ws.outPos  = 0;
    }

    if (ws.inPos == ws.inBytes && ws.inStart + ws.inBytes < s.bytes)
    {
        ws.inStart += ws.inBytes;
        const u32 pos = s.offset + ws.inStart;
        ws.inBytes    = ChunkEnd(pos, s.offset + s.bytes) - pos;
        ws.inPos      = 0;

        const int err = ReadRange(slot, false, s.fd, pos, ws.input, ws.inBytes, s.ioCycles);
        if (err != 0)
        {
            ws.owner = -1;
            Finish(s, err);
            return;
        }
    }

    const bool moreInput = (ws.inStart + ws.inBytes < s.bytes);
    size_t inSize  = ws.inBytes - ws.inPos;
    size_t outSize = s.rawBytes - ws.outPos;

    const tinfl_status status =
        tinfl_decompress(&ws.inflator, ws.input + ws.inPos, &inSize, s.dest, s.dest + ws.outPos, &outSize,
                         static_cast<mz_uint32>(TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (moreInput ? TINFL_FLAG_HAS_MORE_INPUT : 0)));

    ws.inPos   += static_cast<u32>(inSize);
    ws.outPos  += static_cast<u32>(outSize);
    s.bytesDone = ws.outPos;
    s_statBytes = s_statBytes + static_cast<u32>(outSize);

    if (status == TINFL_STATUS_DONE)
    {
        ws.owner = -1;
        Finish(s, (ws.outPos == s.rawBytes) ? static_cast<int>(s.rawBytes) : kErrInflate);
        return;
    }
    if (status < TINFL_STATUS_DONE || status == TINFL_STATUS_HAS_MORE_OUTPUT ||
       (status == TINFL_STATUS_NEEDS_MORE_INPUT && !moreInput))
    {
        ws.owner = -1;
        Finish(s, kErrInflate); // bad data, longer than recorded, or truncated
        return;
    }
    // Otherwise it stays Running for the next piece.
}

// Claims a picked request: Queued becomes Running, so Cancel can tell a request the loader has
// not touched (dropped at once) from one it is reading (waited for). Interrupts are off across
// the check and the store, since the main thread can preempt this thread between any two
// instructions and Cancel's own Queued -> Free store must not be overwritten.
bool Take(Slot & s)
{
    DIntr();
    const State state = s.state;
    if (state == State::Queued)
    {
        s.state = State::Running;
    }
    EIntr();
    return InFlight(state);
}

// The Queued or Running request to serve next: the lowest priority value, then the oldest. A
// Queued InflateWhole waits while another owns the workspace.
Slot * PickBest(int & slotOut)
{
    Slot * best = nullptr;
    int bestSlot = -1;
    const int inflateOwner = (s_workspace != nullptr) ? s_workspace->owner : -1;

    for (int i = 0; i < kNumSlots; ++i)
    {
        Slot & s = s_slots[i];
        const State state = s.state;
        if (!InFlight(state))
        {
            continue;
        }

        CompilerBarrier(); // the fields were written before the state
        (void)state;

        if (s.kind == Kind::InflateWhole && inflateOwner >= 0 && inflateOwner != i)
        {
            continue;
        }

        if (best == nullptr || s.prio < best->prio || (s.prio == best->prio && s.sequence < best->sequence))
        {
            best     = &s;
            bestSlot = i;
        }
    }

    slotOut = bestSlot;
    return best;
}

// ps2_aio_throttle_kbps: sleeps after a chunk for what a USB stick would have taken, so PCSX2's
// instant host reads behave like the drive. The cvar's value is a float in memory, which is all
// the loader reads of it (it never calls into the cvar system).
void Throttle(const u32 bytes)
{
    const float kbps = (s_throttleKbps != nullptr) ? s_throttleKbps->value : 0.0f;
    if (kbps <= 0.0f)
    {
        return;
    }

    // A SCSI command's latency plus the transfer. The read it stands for is still out, as far as
    // FileCallInFlight's callers are concerned.
    const float micros = 5000.0f + (static_cast<float>(bytes) * 1000.0f) / kbps;
    s_inFileCall = true;
    DelayThread(static_cast<int>(micros));
    s_inFileCall = false;
}

void LoaderLoop(void * const)
{
    for (;;)
    {
        WaitSema(s_workSema);

        for (;;)
        {
            int slot = -1;
            Slot * const s = PickBest(slot);
            if (s == nullptr)
            {
                break;
            }
            if (!Take(*s))
            {
                continue; // cancelled between the pick and the take
            }

            const u32 before = s->bytesDone;
            if (s->kind == Kind::Read)
            {
                RunReadChunk(slot, *s);
            }
            else
            {
                RunInflateChunk(slot, *s);
            }

            SignalSema(s_doneSema);
            Throttle(s->bytesDone - before);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Main-thread setup
// ------------------------------------------------------------------------------------------------

void RegisterCvars()
{
    if (s_aioEnabled == nullptr)
    {
        s_aioEnabled   = Cvar_Get("ps2_aio", "1", 0);
        s_throttleKbps = Cvar_Get("ps2_aio_throttle_kbps", "0", 0);
    }
}

// Starts the thread on the first request: a run that never loads mid-level pays nothing.
bool EnsureThread()
{
    if (s_threadId >= 0)
    {
        return true;
    }
    if (s_threadFailed)
    {
        return false;
    }
    s_threadFailed = true; // until everything below succeeded

    // One priority below the main thread (a higher number): it only runs while the main thread
    // sleeps, and the main thread takes the CPU back at once when an interrupt readies it.
    ee_thread_status_t mainStatus = {};
    if (ReferThreadStatus(GetThreadId(), &mainStatus) < 0) [[unlikely]]
    {
        Com_Printf("WARNING: aio: can't read the main thread's priority; loading synchronously.\n");
        return false;
    }

    ee_sema_t sema  = {};
    sema.init_count = 0;
    sema.max_count  = 255;
    s_workSema      = CreateSema(&sema);
    s_doneSema      = CreateSema(&sema);

    ee_thread_t thread      = {};
    thread.func             = reinterpret_cast<void *>(&LoaderLoop);
    thread.stack            = s_stack;
    thread.stack_size       = kStackBytes;
    thread.gp_reg           = &_gp;
    thread.initial_priority = mainStatus.current_priority + 1;

    if (s_workSema >= 0 && s_doneSema >= 0)
    {
        s_threadId = CreateThread(&thread);
    }

    if (s_threadId < 0 || StartThread(s_threadId, nullptr) < 0) [[unlikely]]
    {
        Com_Printf("WARNING: aio: can't start the loader thread; loading synchronously.\n");
        if (s_threadId >= 0)
        {
            DeleteThread(s_threadId);
            s_threadId = -1;
        }
        if (s_workSema >= 0)
        {
            DeleteSema(s_workSema);
            s_workSema = -1;
        }
        if (s_doneSema >= 0)
        {
            DeleteSema(s_doneSema);
            s_doneSema = -1;
        }
        return false;
    }

    s_threadFailed = false;
    Com_DPrintf("aio: loader thread started at priority %d.\n", thread.initial_priority);
    return true;
}

bool EnsureWorkspace()
{
    if (s_workspace != nullptr)
    {
        return true;
    }
    if (s_workspaceFailed)
    {
        return false;
    }

    s_workspace = static_cast<Workspace *>(
        heap::TryAllocAligned(heap::MemAlign(64), sizeof(Workspace), heap::MemTag::Aio));

    if (s_workspace == nullptr) [[unlikely]]
    {
        s_workspaceFailed = true; // reported once; deflated entries load synchronously from here
        Com_Printf("WARNING: aio: no memory for the inflate workspace (%u bytes); deflated files load synchronously.\n",
                   static_cast<unsigned>(sizeof(Workspace)));
        return false;
    }

    s_workspace->owner = -1;
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

bool Enabled()
{
    RegisterCvars();
    return s_aioEnabled->value != 0.0f && !s_threadFailed;
}

Ticket Submit(const Request & request)
{
    if (!Enabled())
    {
        return {};
    }

    const u32 deliverBytes = (request.kind == Kind::InflateWhole) ? request.rawBytes : request.bytes;
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(request.dest) & 63u) == 0, "aio::Submit: dest is not 64-byte aligned");
    PS2_AssertMsg(request.destBytes >= RoundUp64(deliverBytes), "aio::Submit: dest is smaller than the rounded size");
    PS2_Assert(request.fd >= 0);

    if (deliverBytes == 0 || request.bytes == 0)
    {
        return {}; // nothing to read: the synchronous path handles an empty file as it always did
    }
    if (!EnsureThread())
    {
        return {};
    }
    if (request.kind == Kind::InflateWhole && !EnsureWorkspace())
    {
        return {};
    }

    for (int i = 0; i < kNumSlots; ++i)
    {
        Slot & s = s_slots[i];
        if (s.state != State::Free)
        {
            continue;
        }

        s.prio      = request.prio;
        s.kind      = request.kind;
        s.sequence  = s_nextSequence++;
        s.fd        = request.fd;
        s.offset    = request.offset;
        s.bytes     = request.bytes;
        s.rawBytes  = request.rawBytes;
        s.dest      = static_cast<u8 *>(request.dest);
        s.destBytes = request.destBytes;
        s.sequential = (request.kind == Kind::Read) && request.sequential;
        s.bytesDone = 0;
        s.result    = 0;
        s.ioCycles  = 0;

        if (s_nextSequence == 0) // wrap around guard.
        {
            s_nextSequence = 1;
        }

        CompilerBarrier();
        s.state = State::Queued;
        SignalSema(s_workSema);

        return Ticket{ i, s.sequence };
    }

    return {}; // every slot taken
}

State Poll(const Ticket ticket, u32 * const bytesDone)
{
    if (!IsTicketCurrent(ticket))
    {
        if (bytesDone != nullptr)
        {
            *bytesDone = 0;
        }
        return State::Free;
    }

    const Slot & s = s_slots[ticket.slot];
    const State state = s.state;
    CompilerBarrier();

    if (bytesDone != nullptr)
    {
        *bytesDone = s.bytesDone;
    }
    return state;
}

int Result(const Ticket ticket)
{
    if (!IsTicketCurrent(ticket))
    {
        return kErrNoThread;
    }

    const Slot & s = s_slots[ticket.slot];
    const State state = s.state;
    CompilerBarrier();

    return InFlight(state) ? 0 : s.result;
}

u32 IoCycles(const Ticket ticket)
{
    return IsTicketCurrent(ticket) ? s_slots[ticket.slot].ioCycles : 0u;
}

void Wait(const Ticket ticket)
{
    if (!IsTicketCurrent(ticket))
    {
        return;
    }

    const Slot & s = s_slots[ticket.slot];

    // The done semaphore counts every chunk any request landed, so a wake
    // may be another's, or stale: each one re-checks.
    while (InFlight(s.state))
    {
        WaitSema(s_doneSema);
    }
}

void WaitProgress(const Ticket ticket, const u32 minBytes)
{
    if (!IsTicketCurrent(ticket))
    {
        return;
    }

    const Slot & s = s_slots[ticket.slot];

    while (InFlight(s.state) && s.bytesDone < minBytes)
    {
        WaitSema(s_doneSema);
    }
}

void Release(const Ticket ticket)
{
    if (!IsTicketCurrent(ticket))
    {
        return;
    }

    Slot & s = s_slots[ticket.slot];
    PS2_AssertMsg(!InFlight(s.state), "aio::Release of a request still in flight");

    CompilerBarrier();
    s.state = State::Free;
}

void Cancel(const Ticket ticket)
{
    if (!IsTicketCurrent(ticket))
    {
        return;
    }

    Slot & s = s_slots[ticket.slot];

    // Queued -> Free is this thread's store alone: the loader claims a request under DIntr (Take),
    // so it either saw Queued before this and is Running, or sees Free and skips it.
    if (s.state == State::Queued)
    {
        CompilerBarrier();
        s.state = State::Free;
        return;
    }

    while (InFlight(s.state))
    {
        WaitSema(s_doneSema);
    }

    CompilerBarrier();
    s.state = State::Free;
}

void Drain()
{
    if (s_threadId < 0)
    {
        return;
    }

    for (const Slot & s : s_slots)
    {
        while (InFlight(s.state))
        {
            WaitSema(s_doneSema);
        }
    }

    // Stale chunk signals, so the next Wait sleeps rather than spinning through them.
    while (PollSema(s_doneSema) >= 0)
    {
    }
}

bool FileCallInFlight()
{
    return s_inFileCall;
}

bool Pending()
{
    if (s_threadId < 0)
    {
        return false;
    }

    for (const Slot & s : s_slots)
    {
        if (InFlight(s.state))
        {
            return true;
        }
    }

    return false;
}

Stats TakeFrameStats()
{
    static u32 s_lastBytes    = 0;
    static u32 s_lastChunks   = 0;
    static u32 s_lastIoCycles = 0;

    const u32 bytes    = s_statBytes;
    const u32 chunks   = s_statChunks;
    const u32 ioCycles = s_statIoCycles;

    Stats stats;
    stats.bytesLanded = bytes - s_lastBytes;
    stats.chunks      = chunks - s_lastChunks;
    stats.ioCycles    = ioCycles - s_lastIoCycles;
    stats.queueDepth  = 0;

    for (const Slot & s : s_slots)
    {
        if (InFlight(s.state))
        {
            ++stats.queueDepth;
        }
    }

    s_lastBytes    = bytes;
    s_lastChunks   = chunks;
    s_lastIoCycles = ioCycles;
    return stats;
}

} // namespace ps2::aio

// ------------------------------------------------------------------------------------------------
// C hooks (async_io_c.h)
// ------------------------------------------------------------------------------------------------

extern "C" void PS2_AsyncIoDrain(void)
{
    ps2::aio::Drain();
}
