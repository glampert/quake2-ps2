/* ================================================================================================
 * File: main.cpp
 * Brief: PS2 application entry point. Sets the filesystem base path, boots the
 *        Quake II common layer, then runs the frame loop forever.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"
#include "ps2/renderer/render_profile.h"
#include "ps2/debug/exception_handler.h"

int main()
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

    // Qcommon_Init wants an argv[]; synthesise a minimal one.
    static char s_arg0[] = "quake2.elf";
    static char * s_argv[] = { s_arg0, nullptr };

    // Locate the game data - host: under PCSX2, USB mass: on a real console
    // (which needs the IOP module bring-up) - before Qcommon_Init runs
    // FS_InitFilesystem. A build with -DPS2_FS_BASE_PATH=\"...\" pins the
    // base path and skips the detection, for debugging.
#ifdef PS2_FS_BASE_PATH
    FS_SetDefaultBasePath(PS2_FS_BASE_PATH);
#else // PS2_FS_BASE_PATH
    FS_SetDefaultBasePath(ps2::sys::DetectBasePathAndBootIop());
#endif // PS2_FS_BASE_PATH

    Qcommon_Init(1, s_argv);

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

        oldtime = newtime;
    }
}
