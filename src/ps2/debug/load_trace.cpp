/* ================================================================================================
 * File: load_trace.cpp
 * Brief: Level loading trace. See load_trace.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/load_trace.h"

#if PS2_QUAKE_LOAD_TRACE

#include <cstdarg>
#include <cstdio>

extern "C" void PS2Quake_LoadTrace(const char * fmt, ...)
{
    static int s_previousMsec = -1;

    char message[512];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    message[sizeof(message) - 1] = '\0';

    const int msec  = Sys_Milliseconds();
    const int delta = (s_previousMsec < 0) ? 0 : (msec - s_previousMsec);
    s_previousMsec = msec;

    Com_Printf("[load +%5d ms] %s\n", delta, message);
}

#endif // PS2_QUAKE_LOAD_TRACE
