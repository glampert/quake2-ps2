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
// 294.912MHz. The unsigned subtraction in ~EventScoped() stays correct across one
// wrap, which caps a single measured scope at ~14.5s - far beyond anything you
// would put a scope timer around.
using Time = u32;

inline Time ReadCycles()
{
    return get_mips_cop_reg(0, static_cast<u32>(COP0_REG_Count));
}

// ------------------------------------------------------------------------------------------------
// Profile events
// ------------------------------------------------------------------------------------------------

// One named measurement site, accumulated across every call.
//
// Deliberately a plain aggregate: PS2_PROFILE_SCOPED declares it as a
// function-local static, and an all-constant initializer keeps it out of the
// guarded-static path. (-fno-threadsafe-statics drops the locking but a
// non-constant initializer would still cost a guard load and branch on entry.)
struct Event final
{
    const char * name;
    u64          totalCycles;
    Time         minCycles;
    Time         maxCycles;
    u32          callCount;
    Event *      next; // Dump registry chain; linked on first hit.
    bool         linked;
};

#define PS2_PROFILE_EVENT_INIT(label) { (label), 0, ~0u, 0, 0, nullptr, false }

// Links an event into the registry. Cold - runs once per site, ever.
void ProfileRegister(Event * ev) __attribute__((cold));

// Nominal EE core clock. ProfileCalibrate() replaces this with a measured value.
constexpr u32 kNominalCyclesPerMillisec = 294912;

// RAII probe. Reads the counter on entry, folds the delta into the event on
// exit. Everything after the closing read is outside the measured window.
class EventScoped final
{
public:
    explicit EventScoped(Event * ev)
        : m_event(ev)
        , m_start(ReadCycles())
    { }

    ~EventScoped()
    {
        const Time elapsed = ReadCycles() - m_start;
        Event * const ev = m_event;

        ev->totalCycles += elapsed;
        ev->callCount   += 1;

        if (elapsed < ev->minCycles) { ev->minCycles = elapsed; }
        if (elapsed > ev->maxCycles) { ev->maxCycles = elapsed; }

        // One predictable branch; taken exactly once per site.
        if (!ev->linked) [[unlikely]] { ProfileRegister(ev); }
    }

    EventScoped(const EventScoped &) = delete;
    EventScoped & operator = (const EventScoped &) = delete;

private:
    Event * const m_event;
    const Time    m_start;
};

// ------------------------------------------------------------------------------------------------
// Control
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE

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
// want to convert a raw Time delta themselves.
u32 ProfileCyclesPerMillisec();

#else // PS2_QUAKE_PROFILE

// No-ops stubs.
inline void ProfileCalibrate() {}
inline void ProfileReset() {}
inline void ProfileDump(void (*)(const char *, ...)) {}
inline u32 ProfileCyclesPerMillisec() { return kNominalCyclesPerMillisec; }

#endif // PS2_QUAKE_PROFILE

} // namespace ps2::debug

// ------------------------------------------------------------------------------------------------
// Macros
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE

    #define PS2_PROFILE_CONCAT_(a, b) a##b
    #define PS2_PROFILE_CONCAT(a, b)  PS2_PROFILE_CONCAT_(a, b)

    #define PS2_PROFILE_SCOPED(label)                                         \
        static ps2::debug::Event PS2_PROFILE_CONCAT(s_profEvent, __LINE__) =  \
            PS2_PROFILE_EVENT_INIT(label);                                    \
        const ps2::debug::EventScoped PS2_PROFILE_CONCAT(profScope, __LINE__) \
            (&PS2_PROFILE_CONCAT(s_profEvent, __LINE__))

    // Same thing, labelled with the enclosing function's name.
    #define PS2_PROFILE_FUNCTION() PS2_PROFILE_SCOPED(__func__)

#else // PS2_QUAKE_PROFILE

    #define PS2_PROFILE_SCOPED(label) (void)sizeof(label)
    #define PS2_PROFILE_FUNCTION()    (void)0

#endif // PS2_QUAKE_PROFILE
