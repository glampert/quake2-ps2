#pragma once
/* ================================================================================================
 * File: gs.h
 * Brief: Graphics Synthesizer front-end: video mode, the two framebuffers and the z-buffer, the
 *        CLUTs, texture uploads over the GIF channel, and the GIF register values and primitives
 *        the renderer emits. Emission goes through the caller's GifWriter - the command buffer
 *        and the frame belong to ps2::rs.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/gif_writer.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vram.h"

#include <tamtypes.h>
#include <gs_gp.h>

namespace ps2::gs {

// ------------------------------------------------------------------------------------------------
// Drawing contexts
// ------------------------------------------------------------------------------------------------

// The GS's two drawing contexts. A frame renders into one of them and the next into the other;
// TEST/TEX0/TEX1/ALPHA/ZBUF are per-context registers, and a primitive's CTXT bit picks which
// set it draws under, so every register write and every prim tag in a frame must name its one.
enum struct DrawContext : int
{
    Ctx0 = 0,
    Ctx1 = 1,
};

// The bare index, for libdraw's context parameter and the prim CTXT bit.
constexpr int Index(const DrawContext ctx) { return static_cast<int>(ctx); }

// The context the next frame draws into.
constexpr DrawContext NextDrawContext(const DrawContext ctx)
{
    return (ctx == DrawContext::Ctx0) ? DrawContext::Ctx1 : DrawContext::Ctx0;
}

// Address of a context-indexed GS register: the per-context registers come in pairs, context
// 1's one past context 0's.
constexpr u64 ContextReg(const int reg, const DrawContext ctx)
{
    return static_cast<u64>(reg + Index(ctx));
}

// ------------------------------------------------------------------------------------------------
// Bring-up
// ------------------------------------------------------------------------------------------------

struct Config
{
    const u32 * palette;          // 256 RGBA entries; the palette every indexed image samples
    float       intensity;        // lit-CLUT brightening, >= 1 (below that would darken)
    int         width, height;    // framebuffer dimensions, in pixels
    bool        framebuffer16Bit; // 16-bit halves the framebuffers, at 5:5:5 colour
};

// Brings up the GS: allocates the framebuffers and z-buffer, initialises the video mode (auto
// NTSC/PAL), programs both drawing contexts and builds and uploads the CLUTs. Call once. The
// config is latched - it fixes the VRAM layout, so none of it can change afterwards.
void Init(const Config & cfg);

// ------------------------------------------------------------------------------------------------
// State the emitters and the builders below read. In the header only so they can be inline;
// gs.cpp's Init is the one writer, apart from the 2D binding.
// ------------------------------------------------------------------------------------------------

namespace detail {
struct State
{
    int width, height;               // as configured
    framebuffer_t framebuffer[2];    // one per drawing context: the pair that alternates
    zbuffer_t zbuffer;               // where the z-buffer is, its format, and the real z-test
    vram::Address globalPaletteClut; // the three fixed CLUTs, outside the texture heap
    vram::Address litPaletteClut;
    vram::Address alphaRampClut;

    // Texture bound for 2D draws in the current context, for dropping redundant TEX0 writes.
    // The atlas when a scrapped image was bound; see ResolveBind2D.
    const tex::Texture * currentTex;
};
extern State g_state;
} // namespace detail

Q_ALWAYS_INLINE int Width()  { return detail::g_state.width;  }
Q_ALWAYS_INLINE int Height() { return detail::g_state.height; }

// ------------------------------------------------------------------------------------------------
// GS register values
// ------------------------------------------------------------------------------------------------

// ZBUF for the frame's z-buffer. 'maskDepthWrites' sets ZMSK: depth still tests, but nothing is
// written - for blended draws that must not occlude what comes after them.
Q_ALWAYS_INLINE u64 MakeZBuf(const bool maskDepthWrites)
{
    // Note ZBUF wants a word address >> 11, unlike TEX0's >> 6.
    return GS_SET_ZBUF(detail::g_state.zbuffer.address >> 11, detail::g_state.zbuffer.zsm,
                       maskDepthWrites ? 1 : 0);
}

// TEST: the alpha test that cuts out transparent texels, plus the real z-test. Same thing
// libdraw's draw_enable_tests programs, for the paths that write TEST themselves.
Q_ALWAYS_INLINE u64 MakePixelTests()
{
    return GS_SET_TEST(DRAW_ENABLE, ATEST_METHOD_NOTEQUAL, 0x00, ATEST_KEEP_FRAMEBUFFER,
                       DRAW_DISABLE, DRAW_DISABLE,
                       DRAW_ENABLE, static_cast<int>(detail::g_state.zbuffer.method));
}

// Which blend equation a draw writes into ALPHA. Opaque draws write one too - the register is
// ignored while the prim's ABE bit is off, and a known value beats whatever was left behind -
// and Blend is what they use.
enum class BlendMode
{
    Blend,       // (Cs - Cd) * As / 128 + Cd : the ordinary translucency
    Additive,    // (Cs -  0) * As / 128 + Cd : flares and glows
    Modulate,    // (Cd -  0) * As / 128 +  0 : scales the framebuffer, adds nothing of its own
    ModulateAdd, // (Cd -  0) * As / 128 + Cs : that, plus the draw's own colour on top
};

// ALPHA for 'mode'.
Q_ALWAYS_INLINE u64 MakeAlphaBlend(const BlendMode mode)
{
    switch (mode)
    {
    // Additive keeps C = As rather than the fixed 0x80 that would spell GL_ONE literally: at
    // As = 0x80 the two are identical, and routing the source alpha through the equation lets a
    // caller fade an additive primitive per vertex without a third mode.
    case BlendMode::Additive :
        return GS_SET_ALPHA(BLEND_COLOR_SOURCE, BLEND_COLOR_ZERO,
                            BLEND_ALPHA_SOURCE, BLEND_COLOR_DEST, 0x80);

    case BlendMode::Modulate :
        return GS_SET_ALPHA(BLEND_COLOR_DEST, BLEND_COLOR_ZERO,
                            BLEND_ALPHA_SOURCE, BLEND_COLOR_ZERO, 0x80);

    // The D term is what makes this one possible - plain Modulate leaves it zero, so the pass's
    // vertex colour was going spare. Its one user is the lit lightmap pass; see BlendModeFor.
    case BlendMode::ModulateAdd :
        return GS_SET_ALPHA(BLEND_COLOR_DEST, BLEND_COLOR_ZERO,
                            BLEND_ALPHA_SOURCE, BLEND_COLOR_SOURCE, 0x80);

    case BlendMode::Blend :
    default :
        return GS_SET_ALPHA(BLEND_COLOR_SOURCE, BLEND_COLOR_DEST,
                            BLEND_ALPHA_SOURCE, BLEND_COLOR_DEST, 0x80);
    }
}

// TEX1: the texture's filter modes.
Q_ALWAYS_INLINE u64 MakeTex1(const tex::Texture & texture)
{
    return GS_SET_TEX1(LOD_USE_K, 0,
                       tex::GsMagFilter(texture.magFilter),
                       tex::GsMinFilter(texture.minFilter),
                       LOD_MIPMAP_REGISTER, 0, 0);
}

// GS VRAM word address of the 256-entry CLUT an indexed format samples through. Palette8 takes
// the global palette, or the intensity-brightened copy of it when 'lit' - an image something is
// going to multiply back down, a wall under its lightmap or a skin under its shade colour.
// Alpha8 takes the alpha ramp. Invalid for the direct-colour formats, which sample no CLUT.
Q_ALWAYS_INLINE vram::Address ClutAddress(const tex::PixelFormat format, const bool lit)
{
    switch (format)
    {
    case tex::PixelFormat::Palette8 :
        return lit ? detail::g_state.litPaletteClut : detail::g_state.globalPaletteClut;
    case tex::PixelFormat::Alpha8 :
        return detail::g_state.alphaRampClut;
    default :
        return vram::Address::Invalid;
    }
}

// TEX0: where the texture is in VRAM, how it is laid out there, and the CLUT it samples through
// when it is an indexed format. The texture must be resident.
u64 MakeTex0(const tex::Texture & texture, bool lit);

// ------------------------------------------------------------------------------------------------
// Presentation
// ------------------------------------------------------------------------------------------------

// Puts the framebuffer of drawing context 'ctx' on screen, at the next vsync.
void PresentFramebuffer(DrawContext ctx);

// ------------------------------------------------------------------------------------------------
// Texture VRAM
// ------------------------------------------------------------------------------------------------

// DMAs the texture's pixels into the VRAM it has already been assigned, synchronously over the
// GIF channel, and clears its dirty flag. The caller owns residency: VRAM must be allocated and
// any hazard over it resolved (see rs::EnsureTextureResident, which is what calls this).
void UploadTexture(const tex::Texture & texture);

// Returns the texture's VRAM to the heap (no-op when not resident). For dynamic textures whose
// useful life has ended; it self-heals via re-upload if bound again later.
void ReleaseTexture(const tex::Texture & texture);

// Compacts the texture heap by evicting everything resident (see vram::Defragment); the textures
// re-upload packed on their next bind. Call on level changes, after releasing the outgoing
// level's textures - the holes they leave behind can otherwise fail an allocation while free
// VRAM remains.
void DefragVramHeap();

// ------------------------------------------------------------------------------------------------
// 2D texture binding
// ------------------------------------------------------------------------------------------------

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
Q_ALWAYS_INLINE Bind2D ResolveBind2D(const tex::Texture & texture)
{
    PS2_Assert(texture.type != tex::ImageType::Null && texture.pixels != nullptr);

    // A scrapped image is a window onto a shared atlas: the atlas is what gets bound and made
    // resident, and the draw's texel coordinates shift into it.
    const bool isInScrapAtlas = (texture.atlas != nullptr);
    const tex::Texture & bindTex = isInScrapAtlas ? *texture.atlas : texture;

    Bind2D bind;
    bind.texture = &bindTex;
    bind.originU = isInScrapAtlas ? texture.atlasX : 0;
    bind.originV = isInScrapAtlas ? texture.atlasY : 0;

    // Two different images in the same scrap resolve to the same atlas, so the second correctly
    // needs no TEX0 write - but it still draws from its own corner, which is why the origins are
    // set either way.
    bind.needsBind = (&bindTex != detail::g_state.currentTex) || bindTex.dirtyPixels;
    return bind;
}

// Forgets the current 2D binding, so the next EmitTextureBind rewrites TEX0. Required whenever
// something else has written TEX0 for the same drawing context since - which a VU1 batch does
// on every chunk - and whenever the bound texture's VRAM may have been recycled.
Q_ALWAYS_INLINE void Invalidate2DBinding() { detail::g_state.currentTex = nullptr; }

// ------------------------------------------------------------------------------------------------
// GIF emission. Each writes through the caller's cursor; 'ctx' is the drawing context being
// rendered into, which every context-indexed register and the prim CTXT bit must match.
// ------------------------------------------------------------------------------------------------

// Upper bounds on what one emission appends, for the caller sizing its block.
constexpr int kClearQwords        = 64;
constexpr int kFillRectQwords     = 64;
constexpr int kTextureBindQwords  = 16;
constexpr int kTexturedRectQwords = 8;
constexpr int kBegin2DQwords      = 8;

// Colour + depth clear of the whole framebuffer, as a z=0 sprite with an always-pass z-test.
// Re-arms depth writes and sets the dither enable, neither of which the clear itself touches.
void EmitClear(GifWriter & w, DrawContext ctx, const u8 color[3], bool dither);

// The state a 2D overlay section opens with: always-pass z-test so it lands on top of the 3D,
// and a re-armed ZBUF write mask in case a blended 3D batch masked it.
void EmitBegin2D(GifWriter & w, DrawContext ctx);

// A solid rectangle. Alpha below 255 blends with the framebuffer (255 = opaque, unblended).
void EmitFillRect(GifWriter & w, DrawContext ctx, int x, int y, int width, int height,
                  u8 r, u8 g, u8 b, u8 a);

// TEX0/TEX1 for 'bind', which becomes the current 2D binding. Only call when bind.needsBind.
void EmitTextureBind(GifWriter & w, DrawContext ctx, const Bind2D & bind);

// A textured rectangle sampling the bound texture over texel range [u0,v0]..[u1,v1], shifted by
// the bind's atlas origin. 'brightness' modulates the texel colour per RGB channel: 128 leaves
// it unchanged, 255 doubles it. Texels with alpha 0 are cut out by the alpha test.
void EmitTexturedRect(GifWriter & w, DrawContext ctx, int x, int y, int width, int height,
                      int u0, int v0, int u1, int v1, const Bind2D & bind,
                      const u8 brightness[3]);

} // namespace ps2::gs
