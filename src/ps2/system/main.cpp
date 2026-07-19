/* ================================================================================================
 * File: main.cpp
 * Brief: PS2 application entry point. Sets the filesystem base path, boots the
 *        Quake II common layer, then runs the frame loop forever.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"

int main()
{
    // Qcommon_Init wants an argv[]; synthesise a minimal one.
    static char s_arg0[] = "quake2.elf";
    static char * s_argv[] = { s_arg0, nullptr };

    // Locate the game data - host: under PCSX2, USB mass: on a real console
    // (which needs the IOP module bring-up) - before Qcommon_Init runs
    // FS_InitFilesystem. A build with -DPS2_FS_BASE_PATH=\"...\" pins the
    // base path and skips the detection, for debugging.
#ifdef PS2_FS_BASE_PATH
    FS_SetDefaultBasePath(PS2_FS_BASE_PATH);
#else
    FS_SetDefaultBasePath(ps2::sys::DetectBasePathAndBootIop());
#endif

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

        Qcommon_Frame(frametime);
        oldtime = newtime;
    }
}
