/* ================================================================================================
 * File: sys.cpp
 * Brief: Sys_* platform seam for the PS2 - timing, fatal-error handling, console
 *        output and the (static) game-module hookup. Filesystem enumeration and
 *        console input are not available on the target and are stubbed.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/qcommon.h"
#include "ps2/debug/scr_print.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <ctime>

#include <kernel.h> // SleepThread

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

int Sys_Milliseconds()
{
    static clock_t s_base = 0;
    static bool s_initialized = false;

    const clock_t now = clock();
    if (!s_initialized)
    {
        s_base = now;
        s_initialized = true;
    }

    const long long ticks = static_cast<long long>(now - s_base);
    curtime = static_cast<int>((ticks * 1000LL) / static_cast<long long>(CLOCKS_PER_SEC));
    return curtime;
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void Sys_Init()
{
    // The ps2dev crt0 already brings up SIF/RPC and the main thread, so nothing
    // is required here yet. IOP module loading (pad, audio) will be added with
    // those subsystems.
    Com_Printf("------- Sys_Init (PS2) -------\n");
}

void Sys_Quit()
{
    Qcommon_Shutdown();
    std::fflush(stdout);
    std::exit(0);
}

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
    ps2::debug::ScrPrintf("*******************************\n");
    ps2::debug::ScrPrintf("Sys_Error:\n%s\n", tempbuff);
    ps2::debug::ScrPrintf("*******************************\n");

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
    std::fputs(string, stdout);
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
