#pragma once
/* ================================================================================================
 * File: profile.h
 * Brief: Scoped CPU profiler for the EE. Drop PS2_PROFILE_SCOPED("name") into a
 *        function and its cost shows up in the `ps2_profile` console command.
 *        No allocation, no registration boilerplate, compiled out in release.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <tamtypes.h>
#include <mipscopaccess.h> // get_mips_cop_reg / COP0_REG_Count

namespace ps2::debug {

// Nominal EE core clock. ProfileCalibrate() replaces this with a measured value.
constexpr u32 kNominalCyclesPerMillisec = 294912;

enum ProfileFlags : u8
{
    kProfileNoFlags = 0,

    // Adds profile event to the on-screen profile overlay.
    kScreenOverlay  = 1 << 0,
};

// ------------------------------------------------------------------------------------------------
// Cycle counter
// ------------------------------------------------------------------------------------------------

// Raw EE core cycles, straight out of the COP0 Count register.
//
// Why this and not something else:
//   - mfc0 is a single instruction, register to register, no bus traffic. It is
//     the only clock cheap enough to wrap around a function without the probe
//     dominating what it measures.
//   - Sys_Milliseconds() has 1ms granularity - most functions worth profiling
//     land at zero - and each call costs an interrupt-disable pair plus two
//     uncached timer register reads.
//   - The R5900 performance counters (mfpc) are no cheaper: also one COP0-class
//     instruction, but they need PCCR programmed through mtps first, and their
//     emulator support is far less certain than Count's (Count is load-bearing
//     for the CP0 timer interrupt, so emulators have to implement it).
//
// Count is 32-bit and free-running, so it wraps roughly every 14.5 seconds at
// 294.912MHz. The unsigned subtraction in ~ProfileEventScoped() stays correct
// across one wrap, which caps a single measured scope at ~14.5s - far beyond
// anything you would put a scope timer around.
using CpuCycles = u32;

inline CpuCycles ReadCycles()
{
    return get_mips_cop_reg(0, static_cast<u32>(COP0_REG_Count));
}

#if PS2_QUAKE_PROFILE

// ------------------------------------------------------------------------------------------------
// Profile events
// ------------------------------------------------------------------------------------------------

// One named measurement site, accumulated across every call.
//
// Deliberately a plain aggregate: PS2_PROFILE_SCOPED declares it as a
// function-local static, and an all-constant initializer keeps it out of the
// guarded-static path. (-fno-threadsafe-statics drops the locking but a
// non-constant initializer would still cost a guard load and branch on entry.)
struct ProfileEvent final
{
    const char *   name;
    u64            totalCycles;
    CpuCycles      minCycles;
    CpuCycles      maxCycles;
    CpuCycles      frameCycles;
    CpuCycles      lastFrameCycles;
    u32            callCount;
    ProfileEvent * next;    // Profile registry chain; linked on first hit.
    bool           linked;
    ProfileFlags   flags;
    u8             sortKey; // Sorting key for the on screen overlay.

    u32 FrameMilliseconds() const;
};

#define PS2_PROFILE_EVENT_INIT(label, flags, sortKey) \
    { (label), 0, ~0u, 0, 0, 0, 0, nullptr, false, (flags), (sortKey) }

// Links an event into the registry. Cold - runs once per site, ever.
void ProfileRegister(ProfileEvent * ev) Q_COLD_FUNC;

// RAII probe. Reads the counter on entry, folds the delta into the event on
// exit. Everything after the closing read is outside the measured window.
class ProfileEventScoped final
{
public:
    explicit ProfileEventScoped(ProfileEvent * ev)
        : m_event(ev)
        , m_start(ReadCycles())
    { }

    ~ProfileEventScoped()
    {
        const CpuCycles elapsed = ReadCycles() - m_start;
        ProfileEvent * const ev = m_event;

        ev->totalCycles += elapsed;
        ev->frameCycles += elapsed;
        ev->callCount   += 1;

        if (elapsed < ev->minCycles) { ev->minCycles = elapsed; }
        if (elapsed > ev->maxCycles) { ev->maxCycles = elapsed; }

        // One predictable branch; taken exactly once per site.
        if (!ev->linked) [[unlikely]] { ProfileRegister(ev); }
    }

    ProfileEventScoped(const ProfileEventScoped &) = delete;
    ProfileEventScoped & operator = (const ProfileEventScoped &) = delete;

private:
    ProfileEvent * const m_event;
    const CpuCycles m_start;
};

// ------------------------------------------------------------------------------------------------
// Control
// ------------------------------------------------------------------------------------------------

// Measures the real COP0 Count rate against the EE bus timer (T2, which runs off
// the fixed 147.456MHz BUSCLK) so the reported milliseconds are right whether
// Count ticks at the nominal 294.912MHz or at whatever rate an emulator picks.
// Spins for a few milliseconds; call once, from Sys_Init. Skipping it just
// leaves the nominal rate in place.
void ProfileCalibrate();

// Zeroes every registered event's counters. The registry itself is kept.
void ProfileReset();

// Prints one line per event, ordered by total time descending.
void ProfileDump(void (*printer)(const char *, ...));

// Cycles measured by ProfileCalibrate, per millisecond. Exposed for callers that
// want to convert a raw CpuCycles delta themselves.
u32 ProfileCyclesPerMillisec();

// Closes the frame every event has been charging into: rolls frameCycles over to
// lastFrameCycles and clears it. Call once per frame, at the very top: probes
// that close late - a vsync wait trailing the last draw, a scope wrapping the
// whole frame - then still land in the frame they measured, and readers of
// lastFrameCycles get a completed frame rather than a half-filled current one.
void ProfileNewFrame();

// Head of the registry, for callers that want to walk the events themselves (the
// on-screen overlay). Ordered by first hit, most recent first, and stable once
// every site has been reached at least once. Null when nothing has been hit yet.
const ProfileEvent * ProfileEventList();

// Formats a cycle count as milliseconds with three decimals ("3.472"), and
// returns outBuff for use straight inside a printf argument list. Integer math
// only: %f would drag in soft-float doubles for a number two divides can build.
const char * ProfileFormatMillisec(CpuCycles cycles, char * outBuff, size_t outBuffSize);

#else // PS2_QUAKE_PROFILE

// No-ops stubs.
inline void ProfileCalibrate() {}
inline void ProfileReset() {}
inline void ProfileDump(void (*)(const char *, ...)) {}
inline u32 ProfileCyclesPerMillisec() { return kNominalCyclesPerMillisec; }
inline void ProfileNewFrame() {}
inline const struct ProfileEvent * ProfileEventList() { return nullptr; }
inline const char * ProfileFormatMillisec(CpuCycles, char * outBuff, size_t) { return outBuff; }

#endif // PS2_QUAKE_PROFILE

} // namespace ps2::debug

// ------------------------------------------------------------------------------------------------
// Macros
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE

    #define PS2_PROFILE_CONCAT_(a, b) a##b
    #define PS2_PROFILE_CONCAT(a, b)  PS2_PROFILE_CONCAT_(a, b)

    #define PS2_PROFILE_SCOPED(label, flags, sortKey)                                \
        static ps2::debug::ProfileEvent PS2_PROFILE_CONCAT(s_profEvent, __LINE__) =  \
            PS2_PROFILE_EVENT_INIT(label, ps2::debug::flags, sortKey);               \
        const ps2::debug::ProfileEventScoped PS2_PROFILE_CONCAT(profScope, __LINE__) \
            (&PS2_PROFILE_CONCAT(s_profEvent, __LINE__))

    // Same thing, labelled with the enclosing function's name.
    #define PS2_PROFILE_FUNCTION() PS2_PROFILE_SCOPED(__func__, kProfileNoFlags, 0)

#else // PS2_QUAKE_PROFILE

    #define PS2_PROFILE_SCOPED(label, flags, sortKey)
    #define PS2_PROFILE_FUNCTION()

#endif // PS2_QUAKE_PROFILE
