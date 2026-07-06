/* ================================================================================================
 * File: main.cpp
 * Brief: PS2 application entry point. Sets the filesystem base path, boots the
 *        Quake II common layer, then runs the frame loop forever.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/qcommon.h"

// Default filesystem prefix. PCSX2 exposes the ELF's directory as "host:"; a real
// console typically loads from USB mass storage ("mass:"). Override at build time
// with -DPS2_FS_BASE_PATH=\"...\" if needed.
#ifndef PS2_FS_BASE_PATH
    #define PS2_FS_BASE_PATH "host:"
#endif

int main()
{
    // Qcommon_Init wants an argv[]; synthesise a minimal one.
    static char s_arg0[] = "quake2.elf";
    static char * s_argv[] = { s_arg0, nullptr };

    FS_SetDefaultBasePath(PS2_FS_BASE_PATH);

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
