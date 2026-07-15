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

#include "ps2/renderer/vram.h"
#include <tamtypes.h>

namespace ps2::tex { struct Texture; }

namespace ps2::gs {

// Brings up the GS: allocates two 32-bit framebuffers, initialises the video
// mode (auto NTSC/PAL) and sets up both drawing contexts. Call once.
void Init();

int Width();
int Height();

// GS drawing context (0 or 1) being rendered into this frame. The 3D path
// needs it for every context-indexed register it touches (TEX0/TEX1/TEST and
// the prim CTXT bit) - using the wrong one draws into the displayed buffer.
int CurrentContext();

// The configured z-test method (a libdraw ZTEST_METHOD_* value), for paths
// that program the TEST register themselves (the VU1 3D batches).
int DepthTestMethod();

// GS VRAM word address of the 256-entry global-palette CLUT (Quake's shared
// 8-bit palette), uploaded once by Init() to a fixed spot outside the texture
// heap. PixelFormat::Palette8 textures sample through it.
vram::Address GlobalClutAddress();

// Background colour used by BeginFrame()'s screen clear.
void SetClearColor(u8 r, u8 g, u8 b);

// Per-frame lifecycle: BeginFrame() clears the back buffer (color + depth,
// sent immediately); EndFrame() waits for vsync and flips to the front.
// 3D drawing (the VU1 path) happens outside the 2D section below.
void BeginFrame();
void EndFrame();

// The 2D overlay section. All 2D draws (FillRect/SetTextureFor2D/DrawTexturedRect)
// must happen between Begin2D() and End2D(): they accumulate in a deferred
// packet with an always-pass z-test that End2D() sends and waits on. 2D and
// 3D never interleave - 2D outside the section (or 3D inside it) asserts.
void Begin2D();
void End2D();

// True between Begin2D() and End2D(); the 3D path asserts against it.
bool In2DMode();

// Adds a solid rectangle to the current frame. Alpha below 255 blends with the
// framebuffer (255 = fully opaque, unblended).
void FillRect(int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a);

// Ensures the texture's pixels are resident in GS VRAM: on a miss, allocates
// heap space (evicting the least-recently-bound textures when full) and
// DMA-uploads synchronously; when already resident it only refreshes the LRU
// stamp, so binding stays a plain register write. SetTextureFor2D and
// vu1::DrawTriangles call this implicitly; also usable to prefetch. The
// pixels must stay valid in EE RAM for re-uploads after eviction.
void EnsureTextureResident(const tex::Texture & texture);

// Selects the texture sampled by subsequent DrawTexturedRect calls, uploading
// it first if needed (which may flush the accumulated 2D packet when reusing
// evicted VRAM). Redundant sets are dropped, so calling per-draw is fine.
void SetTextureFor2D(const tex::Texture & texture);

// Adds a textured rectangle to the current frame, sampling the SetTextureFor2D()
// texture over texel range [u0,v0]..[u1,v1]. 'brightness' modulates the texel
// colour: 128 = unchanged, 255 = ~2x. Texels with alpha 0 are cut out by the
// alpha test (console font transparency).
void DrawTexturedRect(int x, int y, int w, int h,
                      int u0, int v0, int u1, int v1, u8 brightness);

} // namespace ps2::gs
