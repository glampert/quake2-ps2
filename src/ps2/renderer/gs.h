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

namespace ps2::tex { struct Texture; enum class PixelFormat : u8; }

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

// The ZBUF register value for the frame's z-buffer, for paths that program it
// themselves (the VU1 3D batches). 'maskDepthWrites' sets the ZMSK bit: depth
// reads still test, but nothing is written - for blended draws that must not
// occlude (projected shadows, translucent surfaces).
u64 ZBufData(bool maskDepthWrites);

// GS VRAM word address of the 256-entry CLUT a texture of this pixel format
// samples through: the global palette (Quake's shared 8-bit palette) for
// Palette8, and the alpha ramp (index = alpha, colour pinned at the modulate
// identity) for Alpha8, and for Palette8 either the plain global palette or the
// intensity-brightened copy of it, depending on what the image is for (see
// tex::TakesIntensity). All three live at fixed spots outside the texture heap.
// Returns vram::Address::Invalid for the direct-colour formats, which sample no CLUT.
vram::Address ClutAddressFor(const tex::Texture & texture);

// The ps2_intensity value the lit palette CLUT currently holds, floored at 1.
// Formats that cannot sample through that CLUT have to scale their own texels
// by this instead (see tex::TakesIntensity).
float IntensityScale();

// Background colour used by BeginFrame()'s screen clear.
void SetClearColor(u8 r, u8 g, u8 b);

// The most qwords either per-frame DMA packet has ever held, against the capacity
// both were allocated with. The two packets are the whole ps2::heap::MemTag::Renderer
// budget - shown as "DmaPeak" in the draw-stats overlay.
int FramePacketPeakQwords();
int FramePacketCapacityQwords();

// Per-frame lifecycle: BeginFrame() clears the back buffer (color + depth,
// sent immediately); EndFrame() flushes any pending 2D (see below), waits for
// vsync and flips to the front. 2D and 3D may be drawn in any order between them.
void BeginFrame();
void EndFrame();

// 2D draws (FillRect/SetTextureFor2D/DrawTexturedRect) accumulate into a
// deferred "pending batch" with an always-pass z-test, so it draws on top; the
// first primitive after a flush opens it lazily - callers just draw, no bracket.
// The batch is flushed (sent and waited on) automatically before the next 3D
// draw and by EndFrame(), which keeps its layering correct and its textures
// resident. Rarely needed directly; the VU1 3D path calls it before drawing so
// its triangles land under any 2D issued afterwards.
void FlushPending2D();

// True while a pending 2D batch is open (a 2D primitive has drawn since the last flush).
bool In2DMode();

// Adds a solid rectangle to the current frame. Alpha below 255 blends with the
// framebuffer (255 = fully opaque, unblended).
void FillRect(int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a);

// Ensures the texture's pixels are resident in GS VRAM: on a miss, allocates
// heap space (evicting the least-recently-bound textures when full) and
// DMA-uploads synchronously; when already resident it only refreshes the LRU
// stamp, so binding stays a plain register write - unless the pixels were
// marked dirty (Texture::MarkPixelsDirty), which re-uploads them in place
// (dynamic textures: cinematic frames, lightmaps/scrap). SetTextureFor2D
// and vu1::DrawTriangles call this implicitly; also usable to prefetch. The
// pixels must stay valid in EE RAM for re-uploads after eviction.
void EnsureTextureResident(const tex::Texture & texture);

// Returns the texture's VRAM to the heap (no-op when not resident). For
// dynamic textures whose useful life has ended (e.g. the cinematic frame when
// playback stops); it self-heals via re-upload if bound again later.
void ReleaseTexture(const tex::Texture & texture);

// Compacts the GS texture heap by evicting everything resident (see
// vram::Defragment); the textures re-upload packed on their next bind. Call on
// level changes, after releasing the outgoing level's textures - the holes they
// leave behind can otherwise fail an allocation while free VRAM remains.
void DefragVramHeap();

// Selects the texture sampled by subsequent DrawTexturedRect calls, uploading
// it first if needed (which may flush the accumulated 2D packet when reusing
// evicted VRAM). Redundant sets are dropped, so calling per-draw is fine.
void SetTextureFor2D(const tex::Texture & texture);

// Adds a textured rectangle to the current frame, sampling the SetTextureFor2D()
// texture over texel range [u0,v0]..[u1,v1]. 'brightness' modulates the texel
// colour: 128 = unchanged, 255 = ~2x, for each RGB channel. Texels with alpha 0
// are cut out by the alpha test (e.g. console font transparency).
void DrawTexturedRect(int x, int y, int w, int h,
                      int u0, int v0, int u1, int v1,
                      const u8 brightness[3]);

} // namespace ps2::gs
