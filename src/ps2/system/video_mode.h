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
 *        ps2_video_mode can also pick a digital TV output instead of the standard's interlaced
 *        picture: "480p", "720p" or "1080i", all at 59.94 Hz, for a set on component or an HDMI
 *        adapter that takes them. That is the Output below. The standard is then the console's
 *        own, and only the screens drawn outside the game (the boot splash, the fatal-error
 *        screen) use it: they stay in 480i or 576i, which any set that syncs to the others
 *        takes too.
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

// What the GS scans out: the standard's own interlaced picture (480i or 576i, in FIELD mode with
// the flicker filter, as ever), or one of the digital TV modes.
enum class Output : u8
{
    Interlaced, // the standard's: NTSC 480i or PAL 576i
    Dtv480p,    // 480p, 59.94 Hz progressive (SetGsCrt 0x50), the same 640x448 picture
    Dtv720p,    // 720p, 59.94 Hz progressive (0x52)
    Dtv1080i,   // 1080i, 59.94 Hz interlaced FRAME mode (0x51)
};

// The console's own standard: a PAL console carries an 'E' as the 5th character of its BIOS
// ROM name (as gsKit and libgraph read it).
Standard ConsoleStandard();

// Resolves a ps2_video_mode value - "auto", "ntsc", "pal", "480p", "720p" or "1080i", in any
// case - to a standard and an output, and records both as Selected() and SelectedOutput(). A
// digital output takes the console's standard. Anything else is taken as "auto"; *recognized
// says which it was, for the caller to report (this module prints nothing: the debug screen
// uses it too).
Standard SelectStandard(const char * setting, bool * recognized);

// The output SelectStandard last chose: Output::Interlaced before it has run.
Output SelectedOutput();

// "480i", "576i", "480p", "720p" or "1080i", for the console.
const char * OutputName(Standard standard, Output output);

// True for the outputs whose pictures are progressive: no FIELD mode, no flicker filter.
bool IsProgressive(Output output);

// The CRT mode code SetGsCrt takes for an output, through libgraph's id for it
// (graph_set_mode): GRAPH_MODE_HDTV_*. Interlaced is the standard's GraphMode.
int OutputGraphMode(Standard standard, Output output);

// The DISPLAY register for a digital output showing a framebuffer of this size, the way gsKit
// lays it out (gsInit.c): the mode's origin, plus the board's own offsets where the BIOS has the
// GetGsDxDyOffset syscall, the magnification that fits the framebuffer in the mode's width and
// height, and the picture centred in what is left over. With the screen offset below added. The
// interlaced output keeps libgraph's graph_set_screen, as before.
u64 DtvDisplay(Output output, int width, int height);

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
// console gets its full 512 lines without anyone having to ask. 480p takes NTSC's 448 whatever
// is asked: the same framebuffer as 480i, 16 lines short of the mode's 480 and centred in it,
// which keeps its VRAM layout and every page-aligned height the heap expects.
int FramebufferHeight(Standard standard, Output output, int requested);

// Fields a second the present clock counts in: 50 for PAL's 576i, 59.94 for everything else.
bool FieldRateIsPal(Standard standard, Output output);

// The video mode code SetGsCrt takes: 2 for NTSC, 3 for PAL.
s16 GsCrtMode(Standard standard);

// libgraph's id for it, for graph_set_mode: GRAPH_MODE_NTSC or GRAPH_MODE_PAL.
int GraphMode(Standard standard);

// Where an interlaced FIELD mode picture starts on the raster, as the GS DISPLAY register's
// DX/DY: libgraph's own offsets for the standard, DY already doubled for FIELD mode, with the
// screen offset below added.
int DisplayX(Standard standard);
int DisplayY(Standard standard);

// Where the picture sits on the TV, from ps2_screen_x and ps2_screen_y (ref.cpp): 'x' pixels to
// the right and 'y' framebuffer rows down of the standard's usual place, for a set that cuts an
// edge off. gs::Init brings the GS up with it and gs::PresentFramebuffer applies a change at the
// next vsync; the debug screen reads it through DisplayX/DisplayY.
void SetScreenOffset(int x, int y);
int  ScreenOffsetX();
int  ScreenOffsetY();

// The offset in DISPLAY's units, the adjustment graph_set_screen takes for the interlaced output:
// a pixel is 4 VCK across (libgraph's 2560 VCK line over the 640-pixel framebuffer, MAGH 3), and a
// framebuffer row is 2 raster lines down in FIELD mode, where each field shows every other row.
// DtvDisplay scales the offset to its own mode's magnification instead.
int ScreenAdjustX();
int ScreenAdjustY();

} // namespace ps2::video
