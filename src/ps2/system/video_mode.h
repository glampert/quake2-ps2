#pragma once
/* ================================================================================================
 * File: video_mode.h
 * Brief: Which TV standard the GS drives, NTSC or PAL, and what follows from it: the CRT mode
 *        code, the visible lines, where the picture sits on the raster. Shared by the renderer
 *        (gs::Init) and the fatal-error debug screen (scr_print.cpp), so that an error drawn
 *        after the game chose a standard comes out in that same standard.
 *
 *        The standard is ps2_video_mode's to choose: "auto" (the console's own, from its
 *        BIOS ROM name), "ntsc" or "pal". A PAL console can drive NTSC timing, 480i at
 *        59.94 Hz, as PAL games' 60 Hz modes do; the TV has to take 60 Hz. The other way
 *        round needs a set that takes 50 Hz.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::video {

enum class Standard : u8
{
    Ntsc,
    Pal,
};

// The console's own standard: a PAL console carries an 'E' as the 5th character of its BIOS
// ROM name (as gsKit and libgraph read it).
Standard ConsoleStandard();

// Resolves a ps2_video_mode value - "auto", "ntsc" or "pal", in any case - to a standard and
// records it as Selected(). Anything else is taken as "auto"; *recognized says which it was,
// for the caller to report (this module prints nothing: the debug screen uses it too).
Standard SelectStandard(const char * setting, bool * recognized);

// The standard SelectStandard last chose, or the console's own before it has run - the
// boot-time screens draw before any config has been read.
Standard Selected();

// "NTSC" or "PAL", for the console.
const char * Name(Standard standard);

// Lines an interlaced FIELD mode picture shows: 448 for NTSC, 512 for PAL (libgraph's 224 and
// 256 per field, doubled).
int VisibleLines(Standard standard);

// The framebuffer height to run at: 'requested' (ps2_fb_height) when the standard can show it,
// clamped to VisibleLines otherwise, and VisibleLines itself for 0 - the default, so a PAL
// console gets its full 512 lines without anyone having to ask.
int FramebufferHeight(Standard standard, int requested);

// The video mode code SetGsCrt takes: 2 for NTSC, 3 for PAL.
s16 GsCrtMode(Standard standard);

// libgraph's id for it, for graph_set_mode: GRAPH_MODE_NTSC or GRAPH_MODE_PAL.
int GraphMode(Standard standard);

// Where an interlaced FIELD mode picture starts on the raster, as the GS DISPLAY register's
// DX/DY: libgraph's own offsets for the standard, DY already doubled for FIELD mode.
int DisplayX(Standard standard);
int DisplayY(Standard standard);

} // namespace ps2::video
