/* ================================================================================================
 * File: video_mode.cpp
 * Brief: NTSC/PAL selection and the per-standard values the renderer and the debug screen share.
 *        See video_mode.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/video_mode.h"

#include <graph.h>          // GRAPH_MODE_NTSC, GRAPH_MODE_PAL, GRAPH_MODE_HDTV_*
#include <gs_privileged.h>  // GS_SET_DISPLAY
#include <kernel.h>         // _GetGsDxDyOffset, GetSyscallHandler
#include <rom0_info.h>      // GetRomName
#include <syscallnr.h>      // __NR__GetGsDxDyOffset

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

// A digital output's mode, from gsKit's table (gsInit.c, gsKit_set_buffer_attributes): the CRT
// mode code, the DISPLAY origin, and the mode's width in VCK and height in raster lines. gsKit's
// origins for 720p and 1080i were set on hardware for its hi-res modes; libgraph's own table puts
// them elsewhere (420/40 and 300/120).
struct DtvInfo
{
    const char * name;
    int          graphMode;
    s16          crtMode;
    bool         interlaced; // 1080i: interlaced FRAME mode, each field the whole framebuffer
    int          startX;
    int          startY;
    int          widthVck;
    int          heightLines;
};

constexpr DtvInfo kDtv480p  = { "480p",  GRAPH_MODE_HDTV_480P,  0x50, false, 232, 35, 1440, 480  };
constexpr DtvInfo kDtv720p  = { "720p",  GRAPH_MODE_HDTV_720P,  0x52, false, 306, 24, 1280, 720  };
constexpr DtvInfo kDtv1080i = { "1080i", GRAPH_MODE_HDTV_1080I, 0x51, true,  236, 38, 1920, 1080 };

const DtvInfo & Dtv(const Output output)
{
    PS2_Assert(output != Output::Interlaced);
    return (output == Output::Dtv1080i) ? kDtv1080i : (output == Output::Dtv720p) ? kDtv720p : kDtv480p;
}

static bool     s_hasSelection = false;
static Standard s_selected     = Standard::Ntsc;
static Output   s_output       = Output::Interlaced;

// ps2_screen_x/y, in pixels and rows (see the header for DISPLAY's units of them).
constexpr int kVckPerPixel       = 2560 / 640;
constexpr int kRasterLinesPerRow = 2;

static int s_screenOffsetX = 0;
static int s_screenOffsetY = 0;

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
    Output output = Output::Interlaced;

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
        // The digital outputs are 59.94 Hz whatever the console is, and keep its own standard for
        // the screens drawn outside the game.
        standard = ConsoleStandard();
        if (setting != nullptr && Q_stricmp(setting, "480p") == 0)
        {
            output = Output::Dtv480p;
        }
        else if (setting != nullptr && Q_stricmp(setting, "720p") == 0)
        {
            output = Output::Dtv720p;
        }
        else if (setting != nullptr && Q_stricmp(setting, "1080i") == 0)
        {
            output = Output::Dtv1080i;
        }
        else
        {
            known = (setting != nullptr && Q_stricmp(setting, "auto") == 0);
        }
    }

    if (recognized != nullptr)
    {
        *recognized = known;
    }

    s_selected     = standard;
    s_output       = output;
    s_hasSelection = true;
    return standard;
}

Output SelectedOutput()
{
    return s_output;
}

const char * OutputName(const Standard standard, const Output output)
{
    if (output == Output::Interlaced)
    {
        return (standard == Standard::Pal) ? "576i" : "480i";
    }
    return Dtv(output).name;
}

bool IsProgressive(const Output output)
{
    return output == Output::Dtv480p || output == Output::Dtv720p;
}

int OutputGraphMode(const Standard standard, const Output output)
{
    return (output == Output::Interlaced) ? GraphMode(standard) : Dtv(output).graphMode;
}

u64 DtvDisplay(const Output output, const int width, const int height)
{
    const DtvInfo & mode = Dtv(output);
    PS2_Assert(width > 0 && height > 0 && width <= mode.widthVck && height <= mode.heightLines);

    // The magnification that fits the framebuffer: whole VCK per pixel, whole lines per row.
    const int magH = mode.widthVck / width;
    const int magV = mode.heightLines / height;

    int x = mode.startX;
    int y = mode.startY;

    // The board's own offsets for the mode, on the consoles whose BIOS has the syscall (gsKit's
    // check, and libgraph's graph_get_screen_offsets elsewhere).
    if (GetSyscallHandler(__NR__GetGsDxDyOffset) != nullptr)
    {
        int dx = 0, dy = 0, dw = 0, dh = 0;
        _GetGsDxDyOffset(mode.crtMode, &dx, &dy, &dw, &dh);
        x += dx;
        y += dy;
    }

    // Centred in what the magnification leaves: 480p's 640 pixels at 2 VCK are 1280 of the line's
    // 1440, and its 448 rows 16 short of 480 either side.
    const int displayW = magH * width;
    const int displayH = magV * height;
    x += (mode.widthVck - displayW) / 2;
    y += (mode.heightLines - displayH) / 2;

    // ps2_screen_x/y in the mode's own units: a pixel is magH VCK, a row magV lines.
    x += s_screenOffsetX * magH;
    y += s_screenOffsetY * magV;

    // MAGH and MAGV hold the magnification less one. Interlaced: keep the odd/even start where it
    // is, as gsKit does, and since FRAME mode scans the whole framebuffer in each field, the
    // vertical magnification a field sees is one step less again (gsKit's MagV--): 1080i's 540
    // rows over 1080 lines are a MAGV of 0.
    int magVReg = magV - 1;
    if (mode.interlaced)
    {
        y &= ~1;
        magVReg -= 1;
    }
    PS2_Assert(magH >= 1 && magVReg >= 0);

    const u32 dxField = static_cast<u32>(x);
    const u32 dyField = static_cast<u32>(y);
    const u32 magHReg = static_cast<u32>(magH - 1);
    const u32 dwField = static_cast<u32>(displayW - 1);
    const u32 dhField = static_cast<u32>(displayH - 1);
    return GS_SET_DISPLAY(dxField, dyField, magHReg, static_cast<u32>(magVReg), dwField, dhField);
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

int FramebufferHeight(const Standard standard, const Output output, const int requested)
{
    if (output == Output::Dtv480p)
    {
        return VisibleLines(Standard::Ntsc);
    }

    const int lines = VisibleLines(standard);
    return (requested <= 0 || requested > lines) ? lines : requested;
}

bool FieldRateIsPal(const Standard standard, const Output output)
{
    return standard == Standard::Pal && output == Output::Interlaced;
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
    return Info(standard).displayX + ScreenAdjustX();
}

int DisplayY(const Standard standard)
{
    // In FIELD mode each field's lines land on alternate raster lines, so the origin doubles -
    // less the first line, as graph_set_screen computes it.
    return ((Info(standard).fieldOriginY - 1) * 2) + ScreenAdjustY();
}

void SetScreenOffset(const int x, const int y)
{
    s_screenOffsetX = x;
    s_screenOffsetY = y;
}

int ScreenOffsetX()
{
    return s_screenOffsetX;
}

int ScreenOffsetY()
{
    return s_screenOffsetY;
}

int ScreenAdjustX()
{
    return s_screenOffsetX * kVckPerPixel;
}

int ScreenAdjustY()
{
    return s_screenOffsetY * kRasterLinesPerRow;
}

} // namespace ps2::video
