/* ================================================================================================
 * File: main.cpp
 * Brief: PS2 application entry point. Sets the filesystem base path, boots the
 *        Quake II common layer, then runs the frame loop forever.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/splash_screen.h"
#include "ps2/debug/exception_handler.h"
#include "ps2/debug/log_file.h"
#include "ps2/tests/save_test.h"

int main(int argc, char ** argv)
{
#if PS2_QUAKE_DEBUG
    // First thing, ahead of even the memory accounting: a bad pointer any time
    // after this prints its cause, the faulting instruction and a call stack
    // instead of hanging the EE with three lines of emulator output. Costs
    // nothing until something faults, and is compiled out of release.
    ps2::debug::InstallExceptionHandlers();
#endif // PS2_QUAKE_DEBUG

    // Book the RAM we never get to allocate (EE kernel, ELF image, stack) against
    // ps2::heap::MemTag::ElfSys. Must happen before anything touches the heap, so that what the
    // tags add up to stays a faithful picture of the console's 32MB.
    ps2::heap::TagsAddSystemMem();

    // Sys_Milliseconds counts from its first call. Made here, it puts every log line's stamp and
    // the IOP boot report's times against the start of main(), the boot included.
    Sys_Milliseconds();

    // The id logo, on screen within milliseconds, while the IOP drivers come up and the game data
    // is found, which can take seconds on a console. It stays up until the game's first frame.
    ps2::splash::Show();

    // Locate the game data - host: first, HDD/PFS next, USB mass: last - before
    // Qcommon_Init runs FS_InitFilesystem. On HDD/USB it sits next to the ELF,
    // whose directory (and HDD partition) comes from the path the
    // loader passes as argv[0]. A build with -DPS2_FS_BASE_PATH=\"...\" pins
    // the base path and skips the detection, for debugging.
#ifdef PS2_FS_BASE_PATH
    const char * const basePath = PS2_FS_BASE_PATH;
#else // PS2_FS_BASE_PATH
    const char * const basePath = ps2::sys::DetectBasePathAndBootIop((argc > 0) ? argv[0] : nullptr);
#endif // PS2_FS_BASE_PATH

    FS_SetDefaultBasePath(basePath);

    // The log file goes next to baseq2/, and starts now so it covers all of Qcommon_Init.
    ps2::debug::LogFileOpen(basePath);

    Qcommon_Init(argc, argv);

    int oldtime = Sys_Milliseconds();
    for (;;)
    {
        int newtime;
        int frametime;
        do
        {
            newtime = Sys_Milliseconds();
            frametime = newtime - oldtime;
        } while (frametime < 1);

        // The whole frame - server, client, renderer and the vsync wait.
        {
            PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Frame);
            Qcommon_Frame(frametime);
        }

        // Deliberately outside the scope above: the dump is tens of milliseconds
        // of stdout and must not land in the timings it is reporting. The frame
        // it stretches is dropped rather than logged (see FrameLogFlush).
        ps2::debug::FrameLogFlush();

#if PS2_QUAKE_DEBUG
        // Save game test (cvar "ps2_testsaves 1", or 2 to include the memory card): saves,
        // loads and changes level through the console commands and checks what comes back.
        // Between frames rather than with the other tests at the end of one (PS2_EndFrame):
        // its checks read the save device, under the save notice, which draws a frame itself.
        ps2::test::RunSaveTest();
#endif // PS2_QUAKE_DEBUG

        oldtime = newtime;
    }
}
