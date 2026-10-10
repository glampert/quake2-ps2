/* ================================================================================================
 * File: video_mode.cpp
 * Brief: NTSC/PAL selection and the per-standard values the renderer and the debug screen share.
 *        See video_mode.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/video_mode.h"

#include <graph.h>     // GRAPH_MODE_NTSC, GRAPH_MODE_PAL
#include <rom0_info.h> // GetRomName

namespace ps2::video {
namespace {

// What a standard fixes, from libgraph's own mode table (ee/graph/src/graph_mode.c): its
// SetGsCrt code, and for an interlaced picture the DISPLAY origin and the lines per field.
struct StandardInfo
{
    const char * name;
    s16          crtMode;
    int          graphMode;
    int          displayX;
    int          fieldOriginY; // libgraph's non-interlaced Y offset
    int          fieldLines;
};

constexpr StandardInfo kNtsc = { "NTSC", 2, GRAPH_MODE_NTSC, 652, 26, 224 };
constexpr StandardInfo kPal  = { "PAL",  3, GRAPH_MODE_PAL,  680, 37, 256 };

const StandardInfo & Info(const Standard standard)
{
    return (standard == Standard::Pal) ? kPal : kNtsc;
}

static bool     s_hasSelection = false;
static Standard s_selected     = Standard::Ntsc;

} // namespace

Standard ConsoleStandard()
{
    char romName[16] = {};
    GetRomName(romName);
    return (romName[4] == 'E') ? Standard::Pal : Standard::Ntsc;
}

Standard SelectStandard(const char * const setting, bool * const recognized)
{
    bool known = true;
    Standard standard;

    if (setting != nullptr && Q_stricmp(setting, "ntsc") == 0)
    {
        standard = Standard::Ntsc;
    }
    else if (setting != nullptr && Q_stricmp(setting, "pal") == 0)
    {
        standard = Standard::Pal;
    }
    else
    {
        known    = (setting != nullptr && Q_stricmp(setting, "auto") == 0);
        standard = ConsoleStandard();
    }

    if (recognized != nullptr)
    {
        *recognized = known;
    }

    s_selected     = standard;
    s_hasSelection = true;
    return standard;
}

Standard Selected()
{
    return s_hasSelection ? s_selected : ConsoleStandard();
}

const char * Name(const Standard standard)
{
    return Info(standard).name;
}

int VisibleLines(const Standard standard)
{
    return 2 * Info(standard).fieldLines;
}

int FramebufferHeight(const Standard standard, const int requested)
{
    const int lines = VisibleLines(standard);
    return (requested <= 0 || requested > lines) ? lines : requested;
}

s16 GsCrtMode(const Standard standard)
{
    return Info(standard).crtMode;
}

int GraphMode(const Standard standard)
{
    return Info(standard).graphMode;
}

int DisplayX(const Standard standard)
{
    return Info(standard).displayX;
}

int DisplayY(const Standard standard)
{
    // In FIELD mode each field's lines land on alternate raster lines, so the origin doubles -
    // less the first line, as graph_set_screen computes it.
    return (Info(standard).fieldOriginY - 1) * 2;
}

} // namespace ps2::video
