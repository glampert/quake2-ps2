#pragma once
/* ================================================================================================
 * File: gs.h
 * Brief: Minimal double-buffered Graphics Synthesizer front-end used by the PS2
 *        renderer. Owns the framebuffers and the per-frame DMA/GIF packet, and
 *        exposes what the 2D overlay needs: clear/flip, solid rectangles, and
 *        textured rectangles sampling VRAM-resident textures. The VU-accelerated
 *        3D path builds on top of this.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <cstdint>

namespace ps2::tex { struct Texture; }

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

// Adds a solid rectangle to the current frame. Alpha below 255 blends with the
// framebuffer (255 = fully opaque, unblended).
void FillRect(int x, int y, int w, int h,
              std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

// Uploads a texture's pixels to GS VRAM (allocating the VRAM space) and fills
// in its libdraw descriptor. Synchronous; call outside Begin/EndFrame. The
// pixels must stay valid in EE RAM for later re-uploads.
void UploadTexture(tex::Texture & texture);

// Selects the VRAM-resident texture sampled by subsequent DrawTexturedRect
// calls. Redundant sets are dropped, so calling per-draw is fine.
void SetTexture(const tex::Texture & texture);

// Adds a textured rectangle to the current frame, sampling the SetTexture()
// texture over texel range [u0,v0]..[u1,v1]. 'brightness' modulates the texel
// colour: 128 = unchanged, 255 = ~2x. Texels with alpha 0 are cut out by the
// alpha test (console font transparency).
void DrawTexturedRect(int x, int y, int w, int h,
                      int u0, int v0, int u1, int v1, std::uint8_t brightness);

} // namespace ps2::gs
