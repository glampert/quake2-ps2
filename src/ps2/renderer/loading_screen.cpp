/* ================================================================================================
 * File: loading_screen.cpp
 * Brief: When the loading screen is drawn, and what its status bar says. See loading_screen.h.
 *        The drawing itself is PS2_DrawLoadingScreen, in ref.cpp with the rest of the 2D.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/loading_screen.h"
#include "ps2/debug/load_trace.h"

#include <cstdio>
#include <tamtypes.h>

namespace {

// Each loading screen frame waits for a vsync to be shown. Redrawing at most this often keeps
// that to a few percent of a load made of small files.
constexpr int kRedrawIntervalMsec = 200;

// A file this large takes a while to read from a USB drive (roughly 1 MB/s), so the bar names it
// as soon as it starts, however recently the screen was redrawn.
constexpr int kLargeFileBytes = 256 * 1024;

static bool s_active = false;
static int  s_beginMsec = 0;
static int  s_lastDrawMsec = 0;
static int  s_drawCount = 0;
static int  s_fileCount = 0;
static u32  s_bytesRead = 0;
static char s_fileName[MAX_QPATH];

void Draw()
{
    const int msec = Sys_Milliseconds();
    const u32 tenthsOfMb = s_bytesRead / ((1024u * 1024u) / 10u);

    char status[MAX_QPATH + 64];
    std::snprintf(status, sizeof(status), "%s  [%d files, %u.%u MB, %d s]",
                  (s_fileName[0] != '\0') ? s_fileName : "Loading...",
                  s_fileCount, tenthsOfMb / 10u, tenthsOfMb % 10u, (msec - s_beginMsec) / 1000);

    // A frame that couldn't be drawn (the world loader holds the chain) doesn't count, so the
    // first file after the .bsp parse redraws at once.
    if (PS2_DrawLoadingScreen(status) != 0)
    {
        s_lastDrawMsec = Sys_Milliseconds();
        s_drawCount += 1;
    }
}

} // namespace

extern "C" {

void PS2_LoadingScreenBegin()
{
    s_active      = true;
    s_beginMsec   = Sys_Milliseconds();
    s_drawCount   = 0;
    s_fileCount   = 0;
    s_bytesRead   = 0;
    s_fileName[0] = '\0';
    Draw();
}

void PS2_LoadingScreenEnd()
{
    if (s_active)
    {
        PS2_LOAD_TRACE("Loading screen: %d frames drawn over %d files and %u KB, in %d ms",
                       s_drawCount, s_fileCount, s_bytesRead / 1024u, Sys_Milliseconds() - s_beginMsec);
    }
    s_active = false;
}

void PS2_LoadingScreenNoteFile(const char * fileName, const int lengthBytes)
{
    if (!s_active)
    {
        return;
    }

    std::snprintf(s_fileName, sizeof(s_fileName), "%s", fileName);
    s_fileCount += 1;
    s_bytesRead += (lengthBytes > 0) ? static_cast<u32>(lengthBytes) : 0u;

    if ((Sys_Milliseconds() - s_lastDrawMsec) >= kRedrawIntervalMsec || lengthBytes >= kLargeFileBytes)
    {
        Draw();
    }
}

} // extern "C"
