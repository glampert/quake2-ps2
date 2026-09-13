#pragma once
/* ================================================================================================
 * File: gs.h
 * Brief: Graphics Synthesizer front-end: video mode, the two framebuffers and the z-buffer, the
 *        CLUTs, texture uploads over the GIF channel, and the GIF register values and primitives
 *        the renderer emits. Emission goes through the caller's GifWriter - the command buffer
 *        and the frame belong to ps2::rc.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/gif_writer.h"
#include "ps2/renderer/vram.h"

#include <tamtypes.h>

namespace ps2::tex { struct Texture; enum class PixelFormat : u8; }

namespace ps2::gs {

// Brings up the GS: allocates the framebuffers and z-buffer, initialises the video mode (auto
// NTSC/PAL), programs both drawing contexts and uploads the CLUTs. Call once.
void Init();

int Width();
int Height();

// The configured z-test method (a libdraw ZTEST_METHOD_* value), for paths that program the
// TEST register themselves.
int DepthTestMethod();

// The ZBUF register value for the frame's z-buffer. 'maskDepthWrites' sets the ZMSK bit: depth
// reads still test, but nothing is written - for blended draws that must not occlude.
u64 ZBufData(bool maskDepthWrites);

// GS VRAM word address of the 256-entry CLUT a texture samples through: the global palette for
// Palette8, the intensity-brightened copy of it when the image is one something will multiply
// back down (tex::TakesIntensity), and the alpha ramp for Alpha8. All live at fixed spots
// outside the texture heap. Invalid for the direct-colour formats, which sample no CLUT.
vram::Address ClutAddressFor(const tex::Texture & texture);

// The ps2_intensity value the lit palette CLUT holds, floored at 1. Formats that cannot sample
// through that CLUT scale their own texels by this instead.
float IntensityScale();

// Background colour for the frame clear.
void SetClearColor(u8 r, u8 g, u8 b);

// Puts the framebuffer of drawing context 'drawCtx' on screen, at the next vsync.
void PresentFramebuffer(int drawCtx);

// --------------------------------------------------------------------------------------------
// Texture VRAM
// --------------------------------------------------------------------------------------------

// DMAs the texture's pixels into the VRAM it has already been assigned, synchronously over the
// GIF channel, and clears its dirty flag. The caller owns residency: VRAM must be allocated and
// any hazard over it resolved (see rc::EnsureTextureResident, which is what calls this).
void UploadTexture(const tex::Texture & texture);

// Returns the texture's VRAM to the heap (no-op when not resident). For dynamic textures whose
// useful life has ended; it self-heals via re-upload if bound again later.
void ReleaseTexture(const tex::Texture & texture);

// Compacts the texture heap by evicting everything resident (see vram::Defragment); the textures
// re-upload packed on their next bind. Call on level changes, after releasing the outgoing
// level's textures - the holes they leave behind can otherwise fail an allocation while free
// VRAM remains.
void DefragVramHeap();

// --------------------------------------------------------------------------------------------
// 2D texture binding
// --------------------------------------------------------------------------------------------

// What a 2D draw of some image actually binds.
struct Bind2D
{
    const tex::Texture * texture;  // the atlas, when the request was a scrapped image
    int  originU, originV;         // texel shift into the atlas; zero otherwise
    bool needsBind;                // false when it is already bound and its pixels are clean
};

// Resolves a 2D draw's image to the texture the GS will sample: a scrapped image binds its
// atlas and draws from its corner of it. Pure - it reads the current binding to answer
// 'needsBind' but changes nothing, so a caller that decides not to draw has changed nothing
// either. The caller must make the texture resident before EmitTextureBind.
Bind2D ResolveBind2D(const tex::Texture & texture);

// Forgets the current 2D binding, so the next EmitTextureBind rewrites TEX0. Required whenever
// something else has written TEX0 for the same drawing context since - which a VU1 batch does
// on every chunk - and whenever the bound texture's VRAM may have been recycled.
void Invalidate2DBinding();

// --------------------------------------------------------------------------------------------
// GIF emission. Each writes through the caller's cursor; 'drawCtx' is the GS drawing context
// being rendered into, which every context-indexed register and the prim CTXT bit must match.
// --------------------------------------------------------------------------------------------

// Upper bounds on what one emission appends, for the caller sizing its block.
constexpr int kClearQwords        = 64;
constexpr int kFillRectQwords     = 64;
constexpr int kTextureBindQwords  = 16;
constexpr int kTexturedRectQwords = 8;
constexpr int kBegin2DQwords      = 8;

// Colour + depth clear of the whole framebuffer, as a z=0 sprite with an always-pass z-test.
// Re-arms depth writes and sets the dither enable, neither of which the clear itself touches.
void EmitClear(GifWriter & w, int drawCtx, bool dither);

// The state a 2D overlay section opens with: always-pass z-test so it lands on top of the 3D,
// and a re-armed ZBUF write mask in case a blended 3D batch masked it.
void EmitBegin2D(GifWriter & w, int drawCtx);

// A solid rectangle. Alpha below 255 blends with the framebuffer (255 = opaque, unblended).
void EmitFillRect(GifWriter & w, int drawCtx, int x, int y, int width, int height,
                  u8 r, u8 g, u8 b, u8 a);

// TEX0/TEX1 for 'bind', which becomes the current 2D binding. Only call when bind.needsBind.
void EmitTextureBind(GifWriter & w, int drawCtx, const Bind2D & bind);

// A textured rectangle sampling the bound texture over texel range [u0,v0]..[u1,v1], shifted by
// the bind's atlas origin. 'brightness' modulates the texel colour per RGB channel: 128 leaves
// it unchanged, 255 doubles it. Texels with alpha 0 are cut out by the alpha test.
void EmitTexturedRect(GifWriter & w, int drawCtx, int x, int y, int width, int height,
                      int u0, int v0, int u1, int v1, const Bind2D & bind,
                      const u8 brightness[3]);

} // namespace ps2::gs
