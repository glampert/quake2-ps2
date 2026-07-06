#pragma once
/* ================================================================================================
 * File: gs.h
 * Brief: Minimal double-buffered Graphics Synthesizer front-end used by the PS2
 *        renderer. Owns the framebuffers and the per-frame DMA/GIF packet, and
 *        exposes just enough to clear the screen, draw 2D solid rectangles, and
 *        flip. Textured 2D and the VU-accelerated 3D path build on top of this.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <cstdint>

namespace ps2::gs {

// Brings up the GS: allocates two 32-bit framebuffers, initialises the video
// mode (auto NTSC/PAL) and sets up both drawing contexts. Call once.
void Init();

int Width();
int Height();

// Background colour used by BeginFrame()'s screen clear.
void SetClearColor(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Per-frame lifecycle: BeginFrame() starts a fresh packet and clears the back
// buffer; EndFrame() finalises, kicks the DMA, waits and flips to the front.
void BeginFrame();
void EndFrame();

// Adds a solid (optionally alpha-blended) rectangle to the current frame.
void FillRect(int x, int y, int w, int h,
              std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

} // namespace ps2::gs
