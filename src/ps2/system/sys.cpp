/* ================================================================================================
 * File: sys.cpp
 * Brief: Sys_* platform seam for the PS2 - timing, fatal-error handling, console
 *        output and the (static) game-module hookup. Filesystem enumeration and
 *        console input are not available on the target and are stubbed.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/scr_print.h"
#include "ps2/debug/profile.h"
#include "ps2/system/iop_boot.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>

#include <kernel.h> // SleepThread
#include <timer.h>  // GetTimerSystemTime / kBUSCLKBY256

extern "C" {

// Globals the engine expects the platform layer to own:
int curtime = 0;             // ms of the last Sys_Milliseconds (q_shared.h)
unsigned sys_frame_time = 0; // ms timestamp of the current input frame (cl_input.c)

// The Quake II game module is statically linked; call its entry point directly.
// Declared with void* here (ABI-compatible with game_export_t*(game_import_t*))
// to avoid pulling game/game.h into this C++ translation unit.
extern void * GetGameAPI(void * import);

// ------------------------------------------------------------------------------------------------
// Timing
// ------------------------------------------------------------------------------------------------

// Deliberately not clock(): the R5900 has no DMULT/DDIV, so every 64-bit
// multiply or divide - even by a constant - becomes a libgcc __muldi3 /
// __udivdi3 call. clock() pays two of those inside TimerBusClock2USec before
// we get a number, then the microseconds-to-milliseconds conversion pays a
// third. Reading the system timer directly and converting in 32-bit costs none.
int Sys_Milliseconds()
{
    // The EE system timer (T2) is clocked at BUSCLK/256, and GetTimerSystemTime
    // scales its count back up into BUSCLK units, so the low 8 bits are always
    // zero. Shifting them off recovers the raw 576000Hz tick - exactly 576 per
    // millisecond - which is also the timer's true 1.736us resolution.
    constexpr u32 kTicksPerMillisec = kBUSCLKBY256 / 1000; // 576

    static u64  s_lastBusClk    = 0;
    static u32  s_tickRemainder = 0;
    static int  s_millisecs     = 0;
    static bool s_initialized   = false;

    const u64 nowBusClk = GetTimerSystemTime();
    if (!s_initialized)
    {
        s_lastBusClk  = nowBusClk;
        s_initialized = true;
    }

    // Only the delta stays 64-bit (a single dsubu) and it narrows safely: 32
    // bits of 576kHz ticks is over two hours between calls.
    const u32 deltaTicks = static_cast<u32>((nowBusClk - s_lastBusClk) >> 8);
    s_lastBusClk = nowBusClk;

    // Carry the sub-millisecond remainder so truncation doesn't lose time.
    s_tickRemainder += deltaTicks;
    s_millisecs     += static_cast<int>(s_tickRemainder / kTicksPerMillisec);
    s_tickRemainder %= kTicksPerMillisec;

    curtime = s_millisecs;
    return curtime;
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void Sys_Init()
{
    // Nothing to do: IOP bring-up (reset + USB mass-storage modules when
    // needed) happens in main() via ps2::sys::DetectBasePathAndBootIop -
    // FS_InitFilesystem runs before Sys_Init and already needs file IO - and
    // the pad driver loads its rom0: modules later, at IN_Init.
    Com_Printf("------- Sys_Init (PS2) -------\n");

    Cmd_AddCommand("ps2_dump_iop_mods", []() {
        ps2::sys::PrintLoadedIopModules(40, &Com_Printf);
    });

#if PS2_QUAKE_PROFILE
    // Learn the real COP0 Count rate before any probe can fire (~8ms spin).
    ps2::debug::ProfileCalibrate();

    Cmd_AddCommand("ps2_profile", []() {
        ps2::debug::ProfileDump(&Com_Printf);
    });

    Cmd_AddCommand("ps2_profile_reset", []() {
        ps2::debug::ProfileReset();
    });
#endif // PS2_QUAKE_PROFILE
}

__attribute__((cold))
void Sys_Quit()
{
    Qcommon_Shutdown();
    std::fflush(stdout);
    std::exit(0);
}

__attribute__((cold))
void Sys_Error(const char * error, ...)
{
    va_list argptr;
    char tempbuff[2048];

    va_start(argptr, error);
    vsnprintf(tempbuff, sizeof(tempbuff), error, argptr);
    tempbuff[sizeof(tempbuff) - 1] = '\0';
    va_end(argptr);

    ps2::debug::ScrInit();
    ps2::debug::ScrSetTextColor(0xFF0000FF); // red text
    ps2::debug::ScrPrintf("***************************************************************\n");
    ps2::debug::ScrPrintf("Sys_Error:\n%s\n", tempbuff);
    ps2::debug::ScrPrintf("***************************************************************\n");

    // Draw the error to the screen and halt so the
    // message stays readable in the emulator/console.
    for (;;)
    {
        SleepThread();
    }
}

void * Sys_GetGameAPI(void * parms)
{
    return GetGameAPI(parms);
}

void Sys_UnloadGame()
{
    // Statically linked - nothing to unload.
}

// ------------------------------------------------------------------------------------------------
// Console I/O
// ------------------------------------------------------------------------------------------------

void Sys_ConsoleOutput(const char * string)
{
    std::printf("[Q2] %s", string);
}

char * Sys_ConsoleInput()
{
    return nullptr; // no interactive console on the PS2
}

void Sys_SendKeyEvents()
{
    // Controller polling will hook in here; for now just advance the input clock
    // so cl_input's timing stays sane.
    sys_frame_time = static_cast<unsigned>(Sys_Milliseconds());
}

// ------------------------------------------------------------------------------------------------
// Misc / stubs
// ------------------------------------------------------------------------------------------------

void Sys_AppActivate() {}
void Sys_CopyProtect() {}
char * Sys_GetClipboardData() { return nullptr; }

void Sys_Mkdir(const char * path) { (void)path; }

char * Sys_FindFirst(const char * path, unsigned musthave, unsigned canthave)
{
    (void)path; (void)musthave; (void)canthave;
    return nullptr;
}

char * Sys_FindNext(unsigned musthave, unsigned canthave)
{
    (void)musthave; (void)canthave;
    return nullptr;
}

void Sys_FindClose() {}

} // extern "C"
