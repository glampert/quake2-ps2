#pragma once
/* ================================================================================================
 * File: splash_screen.h
 * Brief: The boot splash: the id logo, centred on black, up within the first milliseconds of
 *        main(). Bringing up the IOP drivers and finding the game data can take seconds on a
 *        console, and until the renderer starts the screen would hold whatever the launcher left.
 *        Drawn once, with nothing but the ELF: the logo (pics/m_main_logo.pcx) is a built-in.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/system/video_mode.h"

#include <tamtypes.h>

namespace ps2::splash {

// Brings the GS up just enough for the logo, draws it and leaves it on screen. It sets the GS up
// as gs::Init will in its default layout - the console's own standard, and the logo drawn into
// what becomes gs::Init's first framebuffer - so the picture stays up until the game presents its
// first frame. Call once, first thing in main(), with the heap up. A splash that can't be drawn
// (no heap for its texels, a GIF that never takes the transfers) is skipped, never fatal.
void Show();

// Whether the splash is on screen in a framebuffer laid out like this one: at this VRAM word
// address, this wide and tall, in this format and TV standard. gs::Init asks about its first
// framebuffer. One laid out any other way would show the splash's texels read the wrong way, so
// it starts black instead.
bool IsShownIn(int vramAddress, int width, int height, int psm, video::Standard standard);

// Whether ShowImage can draw: the splash is up, and gs::Init hasn't taken over.
bool CanShowImage();

// Draws an image under the logo, centred, one texel to a pixel as the engine's 2D draws: the save
// notice while the boot reads config.cfg off the memory card, which Qcommon_Init does before the
// renderer exists (save/save_notice.h). `texels` are `width` x `height` RGBA, those with alpha 0
// left out. False, drawing nothing, if it can't (CanShowImage) or the image doesn't fit.
bool ShowImage(const u32 * texels, int width, int height);

// Takes the image down again: the splash as Show drew it. Nothing happens once gs::Init has
// taken over.
void HideImage();

// gs::Init calls it as it lays out VRAM, which the splash's drawing would trample from then on.
void Retire();

} // namespace ps2::splash
