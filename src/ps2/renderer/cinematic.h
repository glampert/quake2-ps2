/* ================================================================================================
 * File: cinematic.h
 * Brief: Cinematic (.cin) frame rendering: expands the 8-bit frames the engine decodes
 *        (cl_cin.c) through the cinematic palette into an RGBA5551 texture and draws it
 *        as a linear-filtered fullscreen quad. Backs re.DrawStretchRaw and
 *        re.CinematicSetPalette (see ref.cpp).
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_RENDERER_CINEMATIC_H
#define PS2_RENDERER_CINEMATIC_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* cl_cin.c moved cin.pic on to the next decoded frame. DrawFrame converts and uploads only after
   this (or a new palette, buffer or size): the engine draws every client frame, the picture changes
   at 14 fps, and the pointer alone can't tell, since Z_Malloc hands the freed frame's block back. */
void PS2_CinematicFrameChanged(void);

#ifdef __cplusplus
}

#include <tamtypes.h>

namespace ps2::cin {

// Sets the 256-color palette the frames are expanded with, from a 768-byte RGB
// triplet table. NULL resets to the game's global palette - the engine sends
// that when playback stops (SCR_StopCinematic), so it also releases the frame
// texture's VRAM back to the heap.
void SetPalette(const u8 * palette);

// Draws the 8-bit cinematic frame ('data', cols x rows) stretched over (x,y)-(x+w,y+h), through
// the RGBA5551 frame texture. The texture is converted and re-uploaded only when the frame, the
// palette (or the brightness curve) or the size changed since the last call; otherwise the
// resident texture is drawn again. Frames taller than 256 rows are downsampled; columns
// resample to the fixed 256-px width. Must be called inside the GS 2D section; the engine calls
// it every frame while a cinematic plays.
void DrawFrame(int x, int y, int w, int h, int cols, int rows, const u8 * data);

} // namespace ps2::cin
#endif /* __cplusplus */

#endif /* PS2_RENDERER_CINEMATIC_H */
