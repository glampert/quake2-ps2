#pragma once
/* ================================================================================================
 * File: cinematic.h
 * Brief: Cinematic (.cin) frame rendering: expands the 8-bit frames the engine decodes
 *        (cl_cin.c) through the cinematic palette into an RGBA5551 texture and draws it
 *        as a linear-filtered fullscreen quad. Backs re.DrawStretchRaw and
 *        re.CinematicSetPalette (see ref.cpp).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::cin {

// Sets the 256-color palette the frames are expanded with, from a 768-byte RGB
// triplet table. NULL resets to the game's global palette - the engine sends
// that when playback stops (SCR_StopCinematic), so it also releases the frame
// texture's VRAM back to the heap.
void SetPalette(const u8 * palette);

// Converts one 8-bit cinematic frame ('data', cols x rows) into the RGBA5551
// frame texture and draws it stretched over (x,y)-(x+w,y+h). Frames taller
// than 256 rows are downsampled; columns resample to the fixed 256-px width.
// Must be called inside the GS 2D section; the engine calls it every frame
// while a cinematic plays.
void DrawFrame(int x, int y, int w, int h, int cols, int rows, const u8 * data);

} // namespace ps2::cin
