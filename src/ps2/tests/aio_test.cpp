/* ================================================================================================
 * File: aio_test.cpp
 * Brief: Background loader self-test. See aio_test.h.
 *
 *  Two passes over the same list, the second throttled. A pass submits every file at once (the
 *  ring holds 32, the list fewer), which exercises the scheduler's pick across priorities, then
 *  polls once a frame; the throttled pass also sleeps on one ticket through aio::Wait. Every frame
 *  with requests in flight makes one small FS_LoadFile on the main thread and times it: on a
 *  console that read queues behind the chunk in flight in the SDK's file client, and the design
 *  promises it waits one chunk at most.
 *
 *  The references come from FS_LoadFile at submit time, so both copies are compared from memory.
 *  Keep the list small enough for twice its bytes beside a running attract demo.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/common.h"
#include "ps2/tests/aio_test.h"
#include "ps2/tests/test_quit.h"
#include "ps2/fs/async_io.h"
#include "ps2/system/heap.h"

#include <cstring>

namespace ps2::test {
namespace {

// Engine paths. With pak0.pkz in place these are a mix of stored and deflated entries (pakzip
// stores what deflate saves under 10% on, mostly sounds); with pak0.pak every one is a pak range;
// the cinematic is a loose file in both setups. Priorities alternate so the pick has something to
// choose between.
struct Item
{
    const char *  name;
    aio::Priority prio;
};

const Item kItems[] = {
    { "pics/colormap.pcx",                  aio::Priority::Asset  },
    { "default.cfg",                        aio::Priority::Asset  },
    { "sound/world/ric1.wav",               aio::Priority::Sound  },
    { "sound/weapons/railgf1a.wav",         aio::Priority::Sound  },
    { "models/weapons/g_blast/tris.md2",    aio::Priority::Asset  },
    { "models/monsters/soldier/tris.md2",   aio::Priority::Asset  },
    { "models/monsters/soldier/skin.pcx",   aio::Priority::Asset  },
    { "textures/e1u1/floor1_1.wal",         aio::Priority::Asset  },
    { "env/unit1_rt.pcx",                   aio::Priority::Asset  },
    { "sound/misc/menu1.wav",               aio::Priority::Sound  },
    { "video/idlog.cin",                    aio::Priority::Stream },
    { "demos/demo1.dm2",                    aio::Priority::Stream },
    { "maps/base1.bsp",                     aio::Priority::Asset  },
};
constexpr int kNumItems = ArrayLength(kItems);

// The small read made on the main thread each frame while requests are in flight.
constexpr const char * kProbeFile = "pics/conchars.pcx";

struct Pending
{
    aio::Ticket   ticket;
    fs_location_t loc;
    void *        reference = nullptr; // FS_LoadFile's copy
    u8 *          dest      = nullptr;
    u32           destBytes = 0;
    int           length    = 0;
    int           submitMs  = 0;
    int           landedMs  = 0;
    bool          active    = false; // a ticket is out
    bool          skipped   = false; // missing, empty, or Submit declined it
};

enum class Phase : int
{
    Idle,
    Running,
    Done,
};

static Phase    s_phase        = Phase::Idle;
static int      s_pass         = 0; // 0: as the drive is; 1: throttled
static Pending  s_pending[kNumItems];
static int      s_passStartMs  = 0;
static int      s_failures     = 0;
static int      s_compared     = 0;
static int      s_kindsSeen    = 0; // bit per FS_LOC_* kind
static int      s_probeMaxMs   = 0;
static int      s_probeCount   = 0;
static u32      s_passBytes    = 0;
static u32      s_passChunks   = 0;
static u32      s_passIoCycles = 0;
static char     s_savedThrottle[32];
static bool     s_waitedOnce   = false;

const char * KindName(const int kind)
{
    switch (kind)
    {
    case FS_LOC_LOOSE:        return "loose";
    case FS_LOC_PAK:          return "pak";
    case FS_LOC_PKZ_STORED:   return "pkz-stored";
    case FS_LOC_PKZ_DEFLATED: return "pkz-deflated";
    default:                  return "?";
    }
}

void FreePending(Pending & p)
{
    if (p.reference != nullptr)
    {
        FS_FreeFile(p.reference);
        p.reference = nullptr;
    }
    if (p.dest != nullptr)
    {
        heap::Free(p.dest, p.destBytes, heap::MemTag::Aio);
        p.dest = nullptr;
    }
    if (p.loc.looseFile != nullptr)
    {
        FS_ReleaseLocation(&p.loc);
    }
    p.active = false;
}

// Submits every item of the list. The reference copy is read first, on the main thread.
void StartPass()
{
    s_passStartMs = Sys_Milliseconds();
    s_probeMaxMs  = 0;
    s_probeCount  = 0;
    s_waitedOnce  = false;
    s_passBytes = s_passChunks = s_passIoCycles = 0;
    (void)aio::TakeFrameStats(); // zero the window

    Com_Printf("AioTest: pass %d (%s): submitting %d files...\n", s_pass + 1,
               (s_pass == 0) ? "as the drive is" : "throttled to 900 KB/s", kNumItems);

    for (int i = 0; i < kNumItems; ++i)
    {
        Pending & p = s_pending[i];
        std::memset(static_cast<void *>(&p), 0, sizeof(p));
        p.ticket = aio::Ticket{};

        p.length = FS_LocateFile(kItems[i].name, &p.loc);
        if (p.length <= 0 || p.loc.fd < 0)
        {
            Com_Printf("AioTest:   %-36s %s\n", kItems[i].name, (p.length < 0) ? "missing, skipped" : "empty or unlocatable, skipped");
            p.skipped = true;
            FreePending(p);
            continue;
        }
        s_kindsSeen |= 1 << p.loc.kind;

        if (FS_LoadFile(kItems[i].name, &p.reference) != p.length)
        {
            Com_Printf("AioTest:   %-36s FS_LoadFile length differs from FS_LocateFile's!\n", kItems[i].name);
            ++s_failures;
            FreePending(p);
            continue;
        }

        p.destBytes = aio::RoundUp64(static_cast<u32>(p.length));
        p.dest      = static_cast<u8 *>(heap::TryAllocAligned(heap::MemAlign(64), p.destBytes, heap::MemTag::Aio));
        if (p.dest == nullptr)
        {
            Com_Printf("AioTest:   %-36s no memory for %u bytes, skipped\n", kItems[i].name, p.destBytes);
            p.skipped = true;
            FreePending(p);
            continue;
        }
        std::memset(p.dest, 0xA5, p.destBytes); // so a short read can't compare equal by luck

        aio::Request request;
        request.kind      = (p.loc.kind == FS_LOC_PKZ_DEFLATED) ? aio::Kind::InflateWhole : aio::Kind::Read;
        request.prio      = kItems[i].prio;
        request.fd        = p.loc.fd;
        request.offset    = p.loc.offset;
        request.bytes     = p.loc.bytes;
        request.dest      = p.dest;
        request.destBytes = p.destBytes;
        request.rawBytes  = p.loc.rawBytes;

        p.submitMs = Sys_Milliseconds();
        p.ticket   = aio::Submit(request);
        if (!p.ticket.IsValid())
        {
            Com_Printf("AioTest:   %-36s Submit declined (loader off or ring full), skipped\n", kItems[i].name);
            p.skipped = true;
            FreePending(p);
            continue;
        }
        p.active = true;
    }
}

// Compares a landed request with its reference, frees both and reports the line.
void FinishItem(const int i)
{
    Pending & p = s_pending[i];
    p.landedMs  = Sys_Milliseconds();

    const int result = aio::Result(p.ticket);
    bool ok = (result == p.length);
    if (ok)
    {
        ok = std::memcmp(p.dest, p.reference, static_cast<size_t>(p.length)) == 0;
    }
    // The 64-byte rounding past the file must be untouched by a Read (an inflate writes only
    // rawBytes too).
    for (u32 b = static_cast<u32>(p.length); ok && b < p.destBytes; ++b)
    {
        ok = (p.dest[b] == 0xA5);
    }

    const u32 ioCycles = aio::IoCycles(p.ticket);
    Com_Printf("AioTest:   %-36s %-12s %8d bytes  %5d ms  read %4u ms  %s\n", kItems[i].name, KindName(p.loc.kind), p.length,
               p.landedMs - p.submitMs, ioCycles / 294912u, ok ? "ok" : (result < 0 ? va("FAILED (code %d)", result) : "FAILED (mismatch)"));

    if (!ok)
    {
        ++s_failures;
    }
    ++s_compared;
    aio::Release(p.ticket);
    FreePending(p);
}

// Checks every ticket; true when the pass has nothing left in flight.
bool PollPass()
{
    bool anyActive = false;
    for (int i = 0; i < kNumItems; ++i)
    {
        Pending & p = s_pending[i];
        if (!p.active)
        {
            continue;
        }

        // The throttled pass sleeps on its first in-flight ticket once, to exercise Wait: the
        // loader runs while the main thread does.
        if (s_pass == 1 && !s_waitedOnce)
        {
            s_waitedOnce = true;
            aio::Wait(p.ticket);
        }

        const aio::State state = aio::Poll(p.ticket);
        if (state == aio::State::Done || state == aio::State::Failed)
        {
            FinishItem(i);
        }
        else
        {
            anyActive = true;
        }
    }

    const aio::Stats stats = aio::TakeFrameStats();
    s_passBytes    += stats.bytesLanded;
    s_passChunks   += stats.chunks;
    s_passIoCycles += stats.ioCycles;

    if (anyActive)
    {
        // What a frame pays for a file call while a chunk is in flight.
        void * probe = nullptr;
        const int start = Sys_Milliseconds();
        const int len   = FS_LoadFile(kProbeFile, &probe);
        const int ms    = Sys_Milliseconds() - start;
        if (len > 0)
        {
            FS_FreeFile(probe);
            ++s_probeCount;
            if (ms > s_probeMaxMs)
            {
                s_probeMaxMs = ms;
            }
        }
    }
    return !anyActive;
}

void EndPass()
{
    const int elapsedMs = Sys_Milliseconds() - s_passStartMs;
    const u32 ioMs      = s_passIoCycles / 294912u; // 294.912 MHz
    Com_Printf("AioTest: pass %d: %u KB landed in %d ms (%u KB/s wall), %u read() calls, %u ms inside them (%u KB/s); "
               "%d main-thread reads of %s, longest %d ms.\n",
               s_pass + 1, s_passBytes / 1024u, elapsedMs, (elapsedMs > 0) ? (s_passBytes / 1024u) * 1000u / static_cast<u32>(elapsedMs) : 0u,
               s_passChunks, ioMs, (ioMs > 0) ? (s_passBytes / 1024u) * 1000u / ioMs : 0u, s_probeCount, kProbeFile, s_probeMaxMs);

    aio::Drain(); // nothing left: a no-op that must return at once
    if (aio::Pending())
    {
        Com_Printf("AioTest: Pending() still true after the pass!\n");
        ++s_failures;
    }
}

// The declined-ticket path: with ps2_aio 0 a Submit must come back invalid.
void CheckDisabledSubmit()
{
    const bool wasEnabled = aio::Enabled();
    Cvar_Set("ps2_aio", "0");
    alignas(64) static u8 s_dummy[64];
    aio::Request request;
    request.fd        = 0;
    request.bytes     = 64;
    request.dest      = s_dummy;
    request.destBytes = 64;
    if (aio::Submit(request).IsValid() || aio::Enabled())
    {
        Com_Printf("AioTest: Submit accepted a request with ps2_aio 0!\n");
        ++s_failures;
    }
    Cvar_Set("ps2_aio", wasEnabled ? "1" : "0");
}

} // namespace

void RunAioTest()
{
    static const cvar_t * s_enabled = Cvar_Get("ps2_testaio", "0", 0);
    if (s_enabled->value == 0.0f || s_phase == Phase::Done)
    {
        return;
    }

    if (s_phase == Phase::Idle)
    {
        s_failures = 0;
        s_compared = 0;
        s_kindsSeen = 0;
        s_pass = 0;
        Com_Printf("AioTest: starting - %d files, two passes.\n", kNumItems);
        if (!aio::Enabled())
        {
            Com_Printf("AioTest: the loader is off (ps2_aio 0 or its thread failed): FAIL.\n");
            s_phase = Phase::Done;
            test::EndTest("AioTest", "ps2_testaio");
            return;
        }
        CheckDisabledSubmit();
        Com_sprintf(s_savedThrottle, sizeof(s_savedThrottle), "%s", Cvar_VariableString("ps2_aio_throttle_kbps"));
        s_phase = Phase::Running;
        StartPass();
        return;
    }

    if (!PollPass())
    {
        return;
    }
    EndPass();

    if (s_pass == 0)
    {
        s_pass = 1;
        Cvar_Set("ps2_aio_throttle_kbps", "900");
        StartPass();
        return;
    }

    Cvar_Set("ps2_aio_throttle_kbps", s_savedThrottle);
    Com_Printf("AioTest: source kinds seen:%s%s%s%s\n",
               (s_kindsSeen & (1 << FS_LOC_LOOSE)) ? " loose" : "",
               (s_kindsSeen & (1 << FS_LOC_PAK)) ? " pak" : "",
               (s_kindsSeen & (1 << FS_LOC_PKZ_STORED)) ? " pkz-stored" : "",
               (s_kindsSeen & (1 << FS_LOC_PKZ_DEFLATED)) ? " pkz-deflated" : "");
    Com_Printf("AioTest: %s - %d files compared, %d failures.\n", (s_failures == 0 && s_compared > 0) ? "PASS" : "FAIL", s_compared, s_failures);

    s_phase = Phase::Done;
    if (!test::EndTest("AioTest", "ps2_testaio"))
    {
        s_phase = Phase::Idle; // left up: the test can be started again
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
