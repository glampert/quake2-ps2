/* ================================================================================================
 * File: profile.cpp
 * Brief: Registry, calibration and reporting for the scoped profiler. Everything
 *        here is cold - the hot path (two mfc0 reads and an accumulate) is inline
 *        in profile.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/profile.h"

#if PS2_QUAKE_PROFILE

#include <algorithm>
#include <cstdio>
#include <timer.h> // GetTimerSystemTime / kBUSCLKBY256

namespace ps2::debug {
namespace {

// Intrusive list of every event that has been hit at least once. Single-threaded
// by construction: the game runs on one EE thread and probes never fire from an
// interrupt handler.
static ProfileEvent * s_eventList = nullptr;

static u32 s_cyclesPerMillisec = kNominalCyclesPerMillisec;

// Cap on how many events one dump will order and print. Only the sort scratch is
// bounded - events past the cap are still counted, and the dump says so.
constexpr int kMaxDumpEvents = 64;

// Bus-timer ticks per millisecond, after GetTimerSystemTime's BUSCLK units are
// shifted down by the T2 prescaler. Same 576kHz base Sys_Milliseconds uses.
constexpr u32 kBusTicksPerMillisec = kBUSCLKBY256 / 1000; // 576

// Formats microseconds as "<ms>.<frac>" without touching the FPU. printf's %f
// would drag in soft-float doubles for a number we can build with two divides.
inline void FormatMillisec(u32 usec, char * outBuff, size_t outBuffSize)
{
    std::snprintf(outBuff, outBuffSize, "%u.%03u", usec / 1000u, usec % 1000u);
}

inline u32 CyclesToMicrosec(u64 cycles)
{
    if (s_cyclesPerMillisec == 0)
    {
        return 0;
    }

    // Scale up before dividing: cycles-per-microsecond would truncate 294.912 to
    // 294 and bias every number by 0.3%. Cold path, so the 64-bit divide (a
    // libgcc __udivdi3 call on the R5900) is fine - it is exactly what the
    // inline hot path exists to avoid.
    return static_cast<u32>((cycles * 1000u) / s_cyclesPerMillisec);
}

} // namespace

u32 ProfileEvent::FrameMilliseconds() const
{
    const u32 usec = CyclesToMicrosec(lastFrameCycles);
    return usec / 1000u;
}

Q_COLD_FUNC void ProfileRegister(ProfileEvent * ev)
{
    ev->linked  = true;
    ev->next    = s_eventList;
    s_eventList = ev;
}

u32 ProfileCyclesPerMillisec()
{
    return s_cyclesPerMillisec;
}

void ProfileCalibrate()
{
    // Count how many COP0 Count ticks elapse over a fixed bus-timer window. T2 is
    // driven by BUSCLK, which is fixed at 147.456MHz, so it is the trustworthy
    // side of the comparison; Count's rate is what we are trying to learn.
    constexpr u32 kWindowMillisec = 8;
    const u32 windowTicks = kBusTicksPerMillisec * kWindowMillisec;

    // Bail-out budget so a system timer that never advances (GetTimerSystemTime
    // returns 0 flat if InitTimer was never called) can't hang the boot here.
    const CpuCycles cycleBudget = kNominalCyclesPerMillisec * 100;

    const u64 busStart = GetTimerSystemTime();
    const CpuCycles cycleStart = ReadCycles();

    u64 busNow;
    CpuCycles cycles;
    do
    {
        busNow = GetTimerSystemTime();
        cycles = ReadCycles() - cycleStart;
    } while (static_cast<u32>((busNow - busStart) >> 8) < windowTicks && cycles < cycleBudget);

    const u32 busTicks = static_cast<u32>((busNow - busStart) >> 8);

    // The loop overshoots by however long one GetTimerSystemTime() call takes, so
    // scale by the window we actually observed rather than the one we asked for.
    if (busTicks != 0 && cycles != 0)
    {
        s_cyclesPerMillisec = static_cast<u32>((static_cast<u64>(cycles) * kBusTicksPerMillisec) / busTicks);
    }
}

void ProfileReset()
{
    for (ProfileEvent * ev = s_eventList; ev != nullptr; ev = ev->next)
    {
        ev->totalCycles     = 0;
        ev->minCycles       = ~0u;
        ev->maxCycles       = 0;
        ev->frameCycles     = 0;
        ev->lastFrameCycles = 0;
        ev->callCount       = 0;
    }
}

void ProfileNewFrame()
{
    // Walks every event, not just the displayed ones: an event that stops being
    // shown must not keep a half-accumulated frame around for when it comes back.
    for (ProfileEvent * ev = s_eventList; ev != nullptr; ev = ev->next)
    {
        ev->lastFrameCycles = ev->frameCycles;
        ev->frameCycles     = 0;
    }
}

const ProfileEvent * ProfileEventList()
{
    return s_eventList;
}

const char * ProfileFormatMillisec(CpuCycles cycles, char * outBuff, size_t outBuffSize)
{
    FormatMillisec(CyclesToMicrosec(cycles), outBuff, outBuffSize);
    return outBuff;
}

void ProfileDump(void (*printer)(const char *, ...))
{
    if (printer == nullptr)
    {
        return;
    }

    // Snapshot into an array so we can order the report without disturbing the
    // registry (and so a probe firing mid-dump can't splice the list under us).
    const ProfileEvent * sorted[kMaxDumpEvents];
    int count = 0;
    int total = 0;

    for (const ProfileEvent * ev = s_eventList; ev != nullptr; ev = ev->next)
    {
        ++total;
        if (count < kMaxDumpEvents)
        {
            sorted[count++] = ev;
        }
    }

    if (count == 0)
    {
        printer("No profile events recorded. Instrument a function with PS2_PROFILE_SCOPED().\n");
        return;
    }

    // Insertion sort by total cycles, descending.
    std::sort(sorted, sorted + count,
        [](const ProfileEvent * a, const ProfileEvent * b) -> bool
        {
            return a->totalCycles > b->totalCycles;
        });

    char totalBuff[32];
    char avgBuff[32];
    char minBuff[32];
    char maxBuff[32];

    printer("------- Profile (%u cycles/ms) -------\n", s_cyclesPerMillisec);
    printer("%-28s %8s %10s %10s %10s %10s\n", "event", "calls", "total ms", "avg ms", "min ms", "max ms");

    for (int i = 0; i < count; ++i)
    {
        const ProfileEvent * const ev = sorted[i];
        const u32 totalUsec = CyclesToMicrosec(ev->totalCycles);
        const u32 avgUsec = (ev->callCount != 0) ? (totalUsec / ev->callCount) : 0;

        // minCycles carries the ~0u sentinel until the first hit after a reset.
        const u32 minUsec = (ev->callCount != 0) ? CyclesToMicrosec(ev->minCycles) : 0;

        FormatMillisec(totalUsec, totalBuff, sizeof(totalBuff));
        FormatMillisec(avgUsec, avgBuff, sizeof(avgBuff));
        FormatMillisec(minUsec, minBuff, sizeof(minBuff));
        FormatMillisec(CyclesToMicrosec(ev->maxCycles), maxBuff, sizeof(maxBuff));

        printer("%-28s %8u %10s %10s %10s %10s\n",
                ev->name, ev->callCount, totalBuff, avgBuff, minBuff, maxBuff);
    }

    if (total > count)
    {
        printer("... and %d more event(s) not shown (kMaxDumpEvents is %d)\n", total - count, kMaxDumpEvents);
    }
}

} // namespace ps2::debug

#endif // PS2_QUAKE_PROFILE
