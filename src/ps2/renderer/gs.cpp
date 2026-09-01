/* ================================================================================================
 * File: gs.cpp
 * Brief: Double-buffered Graphics Synthesizer front-end. See gs.h.
 *
 *  Modelled on the ps2sdk libdraw "font"/"cube" samples: two framebuffers in
 *  VRAM, one displayed while the other is drawn, using the two GS drawing
 *  contexts. draw_setup_environment programs each context so screen coordinates
 *  are direct top-left pixels.
 *
 *  ps2_fb_16bit picks their format. 16-bit (the default) costs 560 KB each
 *  instead of 1120 KB, which nearly doubles the texture heap below and halves
 *  the GS's color write and blend-read bandwidth, in exchange for 5:5:5 color -
 *  hardware dithering (ps2_fb_dither) covers most of the resulting banding.
 *
 *  Frame structure: BeginFrame() clears color and depth immediately (its own
 *  DMA transfer). 2D and 3D then draw in any order. 2D primitives accumulate
 *  into a deferred "pending batch" (always-pass z-test, so it lands on top);
 *  the first primitive after a flush opens it lazily. The batch is flushed to
 *  the GS - sent and waited on - automatically at each 2D->3D boundary (the
 *  VU1 path calls FlushPending2D() before drawing over PATH1, so its triangles
 *  land under any 2D issued afterwards) and once more by EndFrame(). Flushing
 *  at the boundary also keeps the deferred draws' textures resident: they are
 *  consumed before a later 3D upload can evict the VRAM they sample.
 *
 *  Textures stream on first bind into the VRAM left over after the
 *  framebuffers and z-buffer (~1.27 MB), managed by vram.cpp. While a texture
 *  is resident, binding it is just a TEX0/TEX1 register write - no DMA upload,
 *  no pipeline flush. When the heap fills, the least-recently-bound textures
 *  are evicted; uploads over reused VRAM first sync the GS so queued draws
 *  keep sampling the old texels, not the new ones.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/clut.h"
#include "ps2/renderer/render_packet.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vram.h"
#include "ps2/builtin/builtin.h" // global_palette

#include <dma.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <graph.h>
#include <kernel.h> // SyncDCache
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>

namespace ps2::gs {
namespace {

constexpr int kRenderWidth  = 640;
constexpr int kRenderHeight = 448;

// Per-frame packet headroom, in qwords. There are two of these (double buffered)
// and they are the whole ps2::heap::MemTag::Renderer budget, so the size is worth getting
// right rather than rounding up out of caution.
//
// Was 32K (512 KB each, 1 MB total), chosen against an estimate: a full console of
// text is ~2200 glyphs at 4 qwords. Measured instead - the DmaPeak counter in the
// draw-stats overlay reports RenderPacket::PeakQwords() - the real high-water
// across every stock map never passed 10,000. 15K keeps better than 50% headroom
// on that and gives back ~512 KB, which on a 32 MB console is most of a map's
// lightmap atlases.
//
// Overflow is not silent if this is ever too small: RenderPacket::EnsureSpace and
// the post-emission check in RenderPacket::Advance both Sys_Error naming this
// constant, in release as well as debug.
constexpr int kPacketQwords = 15 * 1024;

// Scratch packet for synchronous texture uploads (DMA chain tags only; the
// pixel data is referenced in place).
constexpr int kTexUploadQwords = 128;

// The color+depth clear, sent as its own transfer at the top of each frame.
constexpr int kClearQwords = 128;

static framebuffer_t s_frameBuffer[2];
static zbuffer_t     s_zbuffer;

static RenderPacket s_framePacket[2];   // double-buffered per-frame packets
static RenderPacket s_texUploadPacket;  // scratch packet for texture uploads
static RenderPacket s_clearPacket;      // per-frame color+depth clear

static int s_drawCtx   = 1; // which framebuffer/context we render into this frame
static int s_packetIdx = 0; // which frame packet is being filled

static bool s_frameStarted = false;
static bool s_in2D         = false;

// Screen clean color. Distinctive dark blue.
static u8 s_clearColor[3] = { 0x20, 0x20, 0x38 };

// ps2_fb_16bit picks the framebuffer format and is read once by Init (it fixes
// the whole VRAM layout, so it cannot change mid-run). ps2_fb_dither is sampled
// every frame instead, so the 5:5:5 banding it hides can be compared on the spot.
static const cvar_t * s_fb16Bit = nullptr;
static const cvar_t * s_enableDither = nullptr;

// The 4x4 ordered dither matrix the GS adds before truncating a pixel to 5 bits
// per channel: it trades banding for a fixed low-amplitude pattern.
//
// A DIMX field is 3-bit signed, so the usable range is -4..3 - eight levels for
// sixteen cells, which is why each level appears exactly twice. That also makes
// a zero mean impossible; this averages -0.5, a negligible half-level darkening.
// The arrangement is equivalent to the classic 4x4 Bayer matrix mapped into that
// range by (bayer >> 1) - 4: same level histogram, and the same mean difference
// between neighbouring cells (4.0), which is the property that actually spreads
// the quantisation error rather than clumping it.
constexpr signed char kDitherMatrix[16] =
{
    -4,  2, -3,  3,
     0, -2,  1, -1,
    -3,  3, -4,  2,
     1, -1,  0, -2,
};

// Set when a VRAM allocation evicted a texture: draws already queued (or still
// rasterising) may reference the freed range, so the next upload must sync the
// GS first. Sticky until a GS-idle point - a block freed early in the frame
// can be handed out later without a new eviction.
static bool s_vramReuseHazard = false;

// Texture bound in the current 2D section, and the texel offset draws through it
// must be shifted by - nonzero only while a scrap atlas is bound, where the bound
// texture is the atlas and the requested image is a sub-rectangle of it.
static const tex::Texture * s_currentTex = nullptr;
static int s_texOriginU = 0;
static int s_texOriginV = 0;

// The three CLUTs, all living at fixed VRAM spots outside the texture heap
// (see clut.h for their layout).
//
// The global palette is Quake's shared 8-bit palette. The lit palette is that
// same palette pre-brightened by 'intensity', and is what a Palette8 image
// samples through when something is going to multiply it back down - a wall
// under its lightmap, a skin under its shade colour. Everything drawn at face
// value (the HUD, the menus, the sky) keeps the unscaled one, which is the same
// split ref_gl makes when it skips intensity for it_pic and it_sky.
//
// The alpha ramp backs PixelFormat::Alpha8: the lightmap atlases (luxel
// intensity) and the generated particle images (their shape) both carry only an
// alpha signal and take their colour from the primitive.
static tex::Clut s_globalPaletteClut;
static tex::Clut s_litPaletteClut;
static tex::Clut s_alphaRampClut;

// ref_gl's 'intensity': how much every lit image is brightened before anything
// multiplies it back down. Read each frame so it can be dialled in on hardware;
// changing it rebuilds and re-uploads s_litPaletteClut (see RefreshLitPalette).
static const cvar_t * s_intensity = nullptr;
static float s_litPaletteScale = 0.0f; // what s_litPaletteClut currently holds

// Packs the matrix into the DIMX register: sixteen 3-bit signed fields at a
// 4-bit stride.
//
// Not GS_SET_DIMX - that macro masks each field with 0x3 rather than 0x7, so it
// truncates every 3-bit value to two bits and turns the negative half of the
// matrix into small positives (-4 becomes 0, -1 becomes 3). The result is a
// brightening bias instead of a dither. libdraw's draw_dither_matrix just
// forwards to the same macro, so it is no better.
constexpr u64 PackDitherMatrix(const signed char (&matrix)[16])
{
    u64 packed = 0;
    for (int i = 0; i < 16; ++i)
    {
        // Through u32 so a negative value keeps its two's complement bits.
        packed |= static_cast<u64>(static_cast<u32>(matrix[i]) & 0x7u) << (i * 4);
    }
    return packed;
}

inline RenderPacket & FramePacket()
{
    return s_framePacket[s_packetIdx];
}

// Bytes of EE RAM the texture's pixel buffer occupies (linear width*height texels).
inline int PixelBufferBytes(const tex::Texture & texture)
{
    return texture.width * texture.height * tex::BytesPerTexel(texture.format);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

int Width()  { return kRenderWidth; }
int Height() { return kRenderHeight; }

int CurrentContext()
{
    return s_drawCtx;
}

int DepthTestMethod()
{
    return static_cast<int>(s_zbuffer.method);
}

u64 ZBufData(bool maskDepthWrites)
{
    // Note ZBUF wants a word address >> 11, unlike TEX0's >> 6.
    return GS_SET_ZBUF(s_zbuffer.address >> 11, s_zbuffer.zsm, maskDepthWrites ? 1 : 0);
}

void SetClearColor(u8 r, u8 g, u8 b)
{
    s_clearColor[0] = r;
    s_clearColor[1] = g;
    s_clearColor[2] = b;
}

int FramePacketPeakQwords()
{
    const int a = s_framePacket[0].PeakQwords();
    const int b = s_framePacket[1].PeakQwords();
    return (a > b) ? a : b;
}

int FramePacketCapacityQwords()
{
    return kPacketQwords;
}

// Sends one or two CLUTs to their fixed VRAM addresses and waits for the
// transfer. Only ever called between frames, so it can take the shared upload
// packet without fighting the streamed texture uploads for it.
static void UploadCluts(const tex::Clut * first, const tex::Clut * second)
{
    RenderPacket & upload = s_texUploadPacket;
    upload.Reset();

    const tex::Clut * const cluts[] = { first, second };
    for (const tex::Clut * clut : cluts)
    {
        if (clut != nullptr)
        {
            upload.TextureTransfer(clut->entries, tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                   GS_PSM_32, clut->vramAddr, tex::Clut::kTransferWidth);
        }
    }
    upload.TextureFlush();

    upload.SendChain();
    upload.Wait();
}

// Rebuilds the lit palette when ps2_intensity has changed, so the value can be
// dialled in on hardware without a restart. A no-op on every frame that did not
// change it, which is all but a handful.
//
// Note this reaches Palette8 images only, which is every image the retail game
// ships. A PixelFormat::RGBA32 texture (a .tga replacement) carries the scale in
// its own texels instead and picks up a new value when it is next loaded - the
// same restart ref_gl needs for all of them.
static void RefreshLitPalette()
{
    // Below 1 would darken rather than brighten, which is not what the knob is
    // for and is what ref_gl's own floor at 1 says too.
    const float scale = (s_intensity->value < 1.0f) ? 1.0f : s_intensity->value;
    if (scale == s_litPaletteScale)
    {
        return;
    }

    s_litPaletteScale = scale;
    s_litPaletteClut.BuildFromPaletteScaled(global_palette, scale);
    UploadCluts(&s_litPaletteClut, nullptr);
}

void Init()
{
    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // Latched: the framebuffer format fixes the whole VRAM layout, so it is read
    // once here and a change only takes effect on the next run.
    s_fb16Bit       = Cvar_Get("ps2_fb_16bit",  "1", CVAR_ARCHIVE);
    s_enableDither  = Cvar_Get("ps2_fb_dither", "0", CVAR_ARCHIVE); // Skybox looks significantly worse with dithering on.
    const bool fb16 = (s_fb16Bit->value != 0.0f);

    // Two framebuffers. 16-bit halves them - 1120 KB each down to 560 KB - which
    // is where most of the texture heap's headroom comes from, at the cost of
    // 5:5:5 color (see the dither below) and one bit of destination alpha, which
    // nothing reads: every blend here scales by *source* alpha.
    const int framePsm = fb16 ? GS_PSM_16 : GS_PSM_32;

    s_frameBuffer[0].width   = kRenderWidth;
    s_frameBuffer[0].height  = kRenderHeight;
    s_frameBuffer[0].mask    = 0;
    s_frameBuffer[0].psm     = static_cast<unsigned int>(framePsm);
    s_frameBuffer[0].address = static_cast<unsigned int>(graph_vram_allocate(kRenderWidth, kRenderHeight, framePsm, GRAPH_ALIGN_PAGE));

    s_frameBuffer[1]         = s_frameBuffer[0];
    s_frameBuffer[1].address = static_cast<unsigned int>(graph_vram_allocate(kRenderWidth, kRenderHeight, framePsm, GRAPH_ALIGN_PAGE));

    // Z-buffer for the 3D world; larger depth = closer (the projection maps the
    // near plane to 0xFFFF), hence GREATER_EQUAL. Depth is 16-bit either way -
    // what changes is which 16-bit format, because the GS requires the color and
    // depth buffers to share a page layout: PSMCT32/24 pair with Z32/Z24/Z16S,
    // PSMCT16 pairs with Z16, PSMCT16S with Z16S. Mismatch them and depth sorting
    // breaks while color looks fine, which is a confusing way to find out.
    const int zPsm = fb16 ? GS_ZBUF_16 : GS_ZBUF_16S;

    s_zbuffer.enable  = DRAW_ENABLE;
    s_zbuffer.method  = ZTEST_METHOD_GREATER_EQUAL;
    s_zbuffer.mask    = 0;
    s_zbuffer.zsm     = static_cast<unsigned int>(zPsm);
    s_zbuffer.address = static_cast<unsigned int>(graph_vram_allocate(kRenderWidth, kRenderHeight, zPsm, GRAPH_ALIGN_PAGE));

    // All three CLUTs live with the fixed allocations; the streamed texture heap
    // takes everything after them, rounded up to a page so its footprint math
    // stays page-aligned (the rest of the last CLUT's page is unused).
    // graph_vram_allocate hands out increasing addresses, so the heap starts
    // past the last of the three.
    const int clutVramAddr      = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    const int litClutVramAddr   = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    const int alphaRampClutAddr = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    PS2_Assert(alphaRampClutAddr > litClutVramAddr && litClutVramAddr > clutVramAddr);

    vram::Init((alphaRampClutAddr + tex::Clut::kNumEntries + 2047) & ~2047);
    s_globalPaletteClut.vramAddr = vram::Address(clutVramAddr);
    s_litPaletteClut.vramAddr    = vram::Address(litClutVramAddr);
    s_alphaRampClut.vramAddr     = vram::Address(alphaRampClutAddr);

    // Display framebuffer 0 first; auto-detects NTSC/PAL.
    graph_initialize(static_cast<int>(s_frameBuffer[0].address), kRenderWidth, kRenderHeight, framePsm, 0, 0);

    s_framePacket[0].Init(kPacketQwords);
    s_framePacket[1].Init(kPacketQwords);
    s_texUploadPacket.Init(kTexUploadQwords);
    s_clearPacket.Init(kClearQwords);

    // Program both drawing contexts: context 0 -> frame 0, context 1 -> frame 1.
    // The environment defaults texture wrapping to CLAMP; Quake's DrawTileClear
    // addresses texels in screen space and needs REPEAT.
    texwrap_t wrap;
    wrap.horizontal = WRAP_REPEAT;
    wrap.vertical   = WRAP_REPEAT;
    wrap.minu = wrap.maxu = 0;
    wrap.minv = wrap.maxv = 0;

    RenderPacket & pkt = s_framePacket[0];
    pkt.SetupEnvironment(0, s_frameBuffer[0], s_zbuffer);
    pkt.TextureWrapping(0, wrap);
    pkt.SetupEnvironment(1, s_frameBuffer[1], s_zbuffer);
    pkt.TextureWrapping(1, wrap);

    // DIMX is global rather than per-context and never changes, so it is set up
    // once here; only the DTHE enable is rewritten per frame (see BeginFrame).
    pkt.SetRegister(static_cast<u64>(GS_REG_DIMX), PackDitherMatrix(kDitherMatrix));
    pkt.Finish();

    pkt.SendNormal();
    pkt.Wait();
    pkt.WaitFinish();

    // Build and upload the CLUTs. The two below never change again; the lit
    // palette is built by RefreshLitPalette, which also uploads it and runs
    // once here before anything can sample it.
    s_globalPaletteClut.BuildFromPalette(global_palette);
    s_alphaRampClut.BuildAlphaRamp();

    UploadCluts(&s_globalPaletteClut, &s_alphaRampClut);

    s_intensity = Cvar_Get("ps2_intensity", "2", CVAR_ARCHIVE);
    RefreshLitPalette();

    s_drawCtx   = 1;
    s_packetIdx = 0;
}

float IntensityScale()
{
    return s_litPaletteScale;
}

vram::Address ClutAddressFor(const tex::Texture & texture)
{
    switch (texture.format)
    {
    case tex::PixelFormat::Palette8 :
        return tex::TakesIntensity(texture.type) ? s_litPaletteClut.vramAddr : s_globalPaletteClut.vramAddr;
    case tex::PixelFormat::Alpha8 :
        return s_alphaRampClut.vramAddr;
    default :
        return vram::Address::Invalid;
    }
}

void BeginFrame()
{
    PS2_AssertMsg(!s_frameStarted, "BeginFrame: frame already started!");
    s_frameStarted = true;

    // Between frames is the only safe moment to rewrite a CLUT the GS samples.
    RefreshLitPalette();

    s_packetIdx ^= 1;

    // The clear goes out immediately as its own transfer instead of riding the
    // deferred 2D packet: the VU1 3D world arrives over PATH1 mid-frame and
    // must land on an already-cleared framebuffer. The z=0 sprite with an
    // ALLPASS z-test clears color and depth in one pass (0 = farthest).
    RenderPacket & clear = s_clearPacket;
    clear.Reset();

    draw_disable_blending(); // draw_clear must overwrite, never blend
    clear.DisableTests(s_drawCtx, s_zbuffer);

    // Re-arm depth writes: a blended VU1 batch (ZMSK = 1) may have been this
    // context's last word on ZBUF two frames ago, and draw_disable_tests only
    // touches TEST - without this the z=0 sprite would clear color but leave
    // stale depth behind.
    clear.SetRegister(static_cast<u64>(GS_REG_ZBUF + s_drawCtx), ZBufData(false));

    // Dithering hides the banding a 5:5:5 framebuffer would otherwise show on
    // smooth gradients. Rewritten every frame (one qword) purely so the cvar can
    // be flipped live to compare; it does nothing to a 32-bit framebuffer.
    const bool dither = (s_frameBuffer[0].psm == GS_PSM_16) && (s_enableDither->value != 0.0f);
    clear.SetRegister(static_cast<u64>(GS_REG_DTHE), GS_SET_DTHE(dither ? 1 : 0));

    clear.Clear(s_drawCtx,
                0.0f, 0.0f,
                static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight),
                static_cast<int>(s_clearColor[0]), static_cast<int>(s_clearColor[1]), static_cast<int>(s_clearColor[2]));
    clear.EnableTests(s_drawCtx, s_zbuffer); // restore the real z-test for the 3D world
    clear.Finish();

    clear.SendNormal();
    clear.Wait();
    clear.WaitFinish();

    // The GS is idle now, so nothing queued can reference reused VRAM anymore.
    s_vramReuseHazard = false;
    vram::BeginFrame();
}

// Opens the pending 2D batch on demand: the first 2D primitive after a flush
// (or after BeginFrame) lands here. Cheap no-op once the batch is already open.
static void Ensure2D()
{
    PS2_AssertMsg(s_frameStarted, "2D draw outside Begin/EndFrame!");
    if (s_in2D)
    {
        return;
    }
    s_in2D       = true;
    s_currentTex = nullptr; // the TEX0 dedupe state is per 2D batch
    s_texOriginU = 0;
    s_texOriginV = 0;

    // The 2D overlay accumulates here and goes out at the next flush, after any
    // 3D drawn so far: always-pass z-test so it lands on top. ZBUF is re-armed
    // too, in case a blended 3D batch (ZMSK = 1) drew before this batch opened.
    RenderPacket & pkt = FramePacket();
    pkt.Reset();
    pkt.DisableTests(s_drawCtx, s_zbuffer);
    pkt.SetRegister(static_cast<u64>(GS_REG_ZBUF + s_drawCtx), ZBufData(false));
}

void FlushPending2D()
{
    if (!s_in2D)
    {
        return; // nothing accumulated since the last flush
    }
    s_in2D = false;

    RenderPacket & pkt = FramePacket();
    pkt.Finish();

    pkt.Wait();
    pkt.SendNormal();
    pkt.WaitFinish();

    s_vramReuseHazard = false; // GS idle again
}

bool In2DMode()
{
    return s_in2D;
}

void FillRect(int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a)
{
    Ensure2D();

    RenderPacket & pkt = FramePacket();
    pkt.EnsureSpace(64);

    rect_t rect;
    rect.v0.x = static_cast<float>(x);
    rect.v0.y = static_cast<float>(y);
    rect.v0.z = 0u;
    rect.v1.x = static_cast<float>(x + w);
    rect.v1.y = static_cast<float>(y + h);
    rect.v1.z = 0u;
    rect.color.r = r;
    rect.color.g = g;
    rect.color.b = b;
    rect.color.q = 1.0f;

    if (a == 255)
    {
        // Fully opaque: plain overwrite.
        draw_disable_blending();
        rect.color.a = 0x80;
        pkt.RectFilled(s_drawCtx, rect);
    }
    else
    {
        // Translucent (fade screen and friends). GS alpha is 0..0x80 = 0..1.
        draw_enable_blending();
        rect.color.a = static_cast<u8>(a >> 1);

        // The GS is slow on very large polygons; libdraw recommends strips for
        // near-fullscreen fills.
        if (w >= kRenderWidth / 2)
        {
            pkt.RectFilledStrips(s_drawCtx, rect);
        }
        else
        {
            pkt.RectFilled(s_drawCtx, rect);
        }
        draw_disable_blending();
    }
}

// The GS may still be drawing - or hold queued draws that will sample - VRAM
// about to be overwritten by an upload into evicted space: flush anything
// queued and wait for the GS to go idle first. Inside the 2D section the frame
// packet itself carries the FINISH; otherwise a bare FINISH rides the scratch
// packet (VU1 batches are synchronous, but their DMA completing does not mean
// the GS has finished rasterizing them).
static void SyncGsBeforeVramReuse()
{
    if (s_in2D)
    {
        RenderPacket & pkt = FramePacket();
        pkt.Finish();

        pkt.Wait();
        pkt.SendNormal();
        pkt.WaitFinish();

        pkt.Reset(); // GS registers persist; keep accumulating into the same packet
    }
    else
    {
        RenderPacket & pkt = s_texUploadPacket;
        pkt.Reset();
        pkt.Finish();

        pkt.Wait();
        pkt.SendNormal();
        pkt.WaitFinish();
    }
    s_vramReuseHazard = false;
}

// Finds 'sizeWords' of VRAM for the texture, escalating when the heap is full.
//
// The normal path evicts the least-recently-bound textures, but never ones bound
// this frame: their draws may still be queued or rasterizing. When that leaves
// nothing to take, the pins are the only thing in the way - and the sole reason
// they exist is work still in flight. Draining the GS retires that work, after
// which dropping the pins is legitimate and the whole heap is fair game again.
// The frame still renders correctly; it just loses its pipelining, and anything
// evicted re-uploads when it is next bound.
//
// The last rung repacks the heap into one free block, so it can only come up
// short for a texture larger than the entire heap - which the caller rejects
// before ever getting here.
static vram::Address AllocateVramFor(const tex::Texture & texture, int sizeWords)
{
    bool evicted = false;

    vram::Address addr = vram::TryAllocate(texture, sizeWords, &evicted);
    s_vramReuseHazard |= evicted;

    if (addr == vram::Address::Invalid)
    {
        Com_DPrintf("VRAM: heap full mid-frame for '%s' (%d KB), draining the GS to unpin.\n",
                    texture.name, sizeWords * 4 / 1024);

        SyncGsBeforeVramReuse();
        s_currentTex = nullptr; // the 2D dedupe must not survive an eviction
        vram::UnpinAll();
        vram::NoteOomSync();

        addr = vram::TryAllocate(texture, sizeWords, &evicted);
        s_vramReuseHazard |= evicted;
    }

    if (addr == vram::Address::Invalid)
    {
        // Enough free words, just not contiguous. The GS is already idle from
        // the rung above, so the wholesale evict-and-repack is safe here.
        s_vramReuseHazard |= vram::Defragment();
        s_currentTex = nullptr;

        addr = vram::TryAllocate(texture, sizeWords, &evicted);
        s_vramReuseHazard |= evicted;
    }

    if (addr == vram::Address::Invalid) [[unlikely]]
    {
        vram::DumpAllBlocks();
        Sys_Error("GS VRAM allocation failed for '%s' (%d KB) even after draining and defragmenting!",
                  texture.name, sizeWords * 4 / 1024);
    }

    return addr;
}

void EnsureTextureResident(const tex::Texture & texture)
{
    PS2_Assert(texture.type != tex::ImageType::Null && texture.pixels != nullptr);

    // A scrapped image has no VRAM of its own and its pixels are a window into
    // the atlas: residency is the atlas's, and binding it here would upload the
    // whole atlas under the wrong name and sample from the wrong corner. Only
    // the 2D path can produce one, and it resolves the atlas before calling in.
    PS2_AssertMsg(texture.atlas == nullptr, "EnsureTextureResident on a scrapped image - bind its atlas!");

    const int psm    = tex::GsPsm(texture.format);
    const int stride = tex::TextureStridePixels(texture, psm);

    if (texture.vramAddr != tex::Texture::kNotResident)
    {
        if (!texture.dirtyPixels)
        {
            vram::Touch(texture); // protect from eviction until the next frame
            return;
        }

        // Dynamic texture with rewritten pixels: re-upload over its own block.
        // Draws queued earlier this frame would sample the new texels instead
        // of the ones they were issued with - drain the GS first. (Its block
        // is still owned, so the eviction/reuse hazard does not apply here.)
        const bool boundThisFrame = vram::BoundThisFrame(texture);
        vram::Touch(texture);
        if (boundThisFrame)
        {
            SyncGsBeforeVramReuse();
        }
    }
    else
    {
        const int sizeWords = vram::TextureFootprintWords(texture.width, texture.height, psm);

        // Nothing below can service a texture bigger than the whole heap, and
        // trying would evict the entire working set first and then report it as
        // a working-set problem. Say what is actually wrong instead.
        if (sizeWords > vram::HeapTotalWords()) [[unlikely]]
        {
            Sys_Error("Texture '%s' (%dx%d) needs %d KB of GS VRAM, but the whole texture heap is only %d KB!",
                      texture.name, texture.width, texture.height,
                      sizeWords * 4 / 1024, vram::HeapTotalWords() * 4 / 1024);
        }

        const vram::Address addr = AllocateVramFor(texture, sizeWords);

        // Queued or in-flight draws may still sample VRAM the allocation just
        // recycled; the upload below would pull it out from under them.
        if (s_vramReuseHazard)
        {
            SyncGsBeforeVramReuse();
        }

        texture.vramAddr = addr;

        Com_DPrintf("VRAM: uploaded '%s' (%dx%d, %d KB)\n", texture.name,
                    texture.width, texture.height, sizeWords * 4 / 1024);
    }

    if (texture.dirtyPixels)
    {
        // The CPU just wrote these pixels; part of them may still sit in the
        // data cache, and SendChain only writes back the chain-tag buffer, not
        // REF'd data - flush the range or the GS reads stale texels. Built-ins
        // are never dirty (the ELF loader wrote them) and skip this.
        void * pixels = const_cast<void *>(texture.pixels);
        SyncDCache(pixels, static_cast<u8 *>(pixels) + PixelBufferBytes(texture));
        texture.dirtyPixels = false;
    }

    // Synchronous DMA upload; the chain references the pixels in EE RAM.
    // TextureTransfer cannot EnsureSpace up front - only draw_texture_transfer
    // knows how many chain tags a given texture needs - but RenderPacket::Advance
    // checks afterwards and Sys_Errors, so a texture that outgrows this 128-qword
    // scratch packet says so instead of scribbling past it.
    RenderPacket & pkt = s_texUploadPacket;
    pkt.Reset();
    pkt.TextureTransfer(texture.pixels, texture.width, texture.height, psm, texture.vramAddr, stride);
    pkt.TextureFlush();

    pkt.SendChain();
    pkt.Wait();

    vram::NoteTextureUpload(); // for the debug overlay's per-frame upload count
}

void ReleaseTexture(const tex::Texture & texture)
{
    // Never leave the TEX0 dedupe pointing at a released texture: a rebind in
    // the same 2D section must go through EnsureTextureResident again, and the
    // cache may recycle the slot for a different image entirely.
    if (s_currentTex == &texture)
    {
        s_currentTex = nullptr;
    }

    if (texture.vramAddr == tex::Texture::kNotResident)
    {
        return;
    }

    vram::Free(texture);

    // Queued or in-flight draws may still sample the freed range; the next
    // upload that lands there must sync the GS first, same as an eviction.
    s_vramReuseHazard = true;
}

void DefragVramHeap()
{
    if (!vram::Defragment())
    {
        return;
    }

    // Every texture is non-resident now: the 2D dedupe would otherwise skip the
    // rebind of the current one and sample VRAM it no longer owns, and queued
    // draws may still reference the recycled heap, same as ReleaseTexture.
    s_currentTex      = nullptr;
    s_vramReuseHazard = true;
}

void SetTextureFor2D(const tex::Texture & texture)
{
    PS2_Assert(texture.type != tex::ImageType::Null && texture.pixels != nullptr);
    Ensure2D();

    // A scrapped image is a window onto a shared atlas: the atlas is what gets
    // bound and made resident, and the draw's texel coordinates shift into it.
    const bool isInScrapAtlas = (texture.atlas != nullptr);
    const tex::Texture & bindTex = isInScrapAtlas ? *texture.atlas : texture;

    // Update the origin before the dedupe below, not after: two different images
    // in the same scrap resolve to the same atlas, so the second one correctly
    // skips the TEX0 write - but it still has to draw from its own corner.
    s_texOriginU = isInScrapAtlas ? texture.atlasX : 0;
    s_texOriginV = isInScrapAtlas ? texture.atlasY : 0;

    if (&bindTex == s_currentTex && !bindTex.dirtyPixels)
    {
        return; // already bound (and made resident); EE RAM pixels not dirty.
    }

    EnsureTextureResident(bindTex);
    s_currentTex = &bindTex;

    RenderPacket & pkt = FramePacket();
    pkt.EnsureSpace(16);

    lod_t lod;
    lod.calculation   = LOD_USE_K;
    lod.max_level     = 0;
    lod.mag_filter    = static_cast<unsigned char>(tex::GsMagFilter(bindTex.magFilter));
    lod.min_filter    = static_cast<unsigned char>(tex::GsMinFilter(bindTex.minFilter));
    lod.mipmap_select = LOD_MIPMAP_REGISTER;
    lod.l             = 0;
    lod.k             = 0.0f;

    clutbuffer_t clut;
    const vram::Address clutAddr = ClutAddressFor(bindTex);
    if (clutAddr != vram::Address::Invalid)
    {
        // Reload the on-chip CLUT cache on every bind: cheap (1 KB) at the 2D
        // path's bind rate. TODO: CLUT_COMPARE_CBP0 skips redundant reloads -
        // worthwhile once world textures bind per-surface.
        clut.address      = static_cast<unsigned int>(clutAddr);
        clut.psm          = GS_PSM_32;
        clut.storage_mode = CLUT_STORAGE_MODE1;
        clut.start        = 0;
        clut.load_method  = CLUT_LOAD;
    }
    else
    {
        // Not palettized; the CLUT slots stay empty.
        clut.address      = 0;
        clut.psm          = 0;
        clut.storage_mode = CLUT_STORAGE_MODE1;
        clut.start        = 0;
        clut.load_method  = CLUT_NO_LOAD;
    }

    const int psm    = tex::GsPsm(bindTex.format);
    const int stride = tex::TextureStridePixels(bindTex, psm);

    // Fill the libdraw descriptor used when binding. The stride (TEX0's TBW)
    // differs from the width for narrow 8-bit textures; the page-grid footprint
    // already covers the rounding.
    texbuffer_t texbuf;
    texbuf.address         = static_cast<unsigned int>(bindTex.vramAddr);
    texbuf.width           = static_cast<unsigned int>(stride);
    texbuf.psm             = static_cast<unsigned int>(psm);
    texbuf.info.width      = tex::Log2(static_cast<u32>(bindTex.width));
    texbuf.info.height     = tex::Log2(static_cast<u32>(bindTex.height));
    texbuf.info.components = static_cast<unsigned char>(tex::GsComponents(bindTex.components));
    texbuf.info.function   = static_cast<unsigned char>(tex::GsFunction(bindTex.function));

    pkt.TextureSampling(s_drawCtx, lod);
    pkt.TextureBuffer(s_drawCtx, texbuf, clut);
}

void DrawTexturedRect(int x, int y, int w, int h,
                      int u0, int v0, int u1, int v1, u8 brightness)
{
    PS2_AssertMsg(s_currentTex != nullptr, "DrawTexturedRect without SetTextureFor2D!");
    PS2_AssertMsg(s_in2D, "DrawTexturedRect without an open 2D batch!");

    RenderPacket & pkt = FramePacket();
    pkt.EnsureSpace(8);

    // s_texOriginU/V both zero unless a scrap atlas is bound, in which case they shift the
    // coordinates from the image's own space into its corner of the atlas.
    texrect_t rect;
    rect.v0.x = static_cast<float>(x);
    rect.v0.y = static_cast<float>(y);
    rect.v0.z = 0u;
    rect.t0.u = static_cast<float>(u0 + s_texOriginU);
    rect.t0.v = static_cast<float>(v0 + s_texOriginV);
    rect.v1.x = static_cast<float>(x + w);
    rect.v1.y = static_cast<float>(y + h);
    rect.v1.z = 0u;
    rect.t1.u = static_cast<float>(u1 + s_texOriginU);
    rect.t1.v = static_cast<float>(v1 + s_texOriginV);

    // Modulate: 0x80 = 1.0, so 'brightness' 128 leaves texels unchanged. Vertex
    // alpha 0x80 likewise preserves texel alpha, which the alpha test then uses
    // to cut out transparent texels (e.g. the console font background).
    rect.color.r = brightness;
    rect.color.g = brightness;
    rect.color.b = brightness;
    rect.color.a = 0x80;
    rect.color.q = 1.0f;

    draw_disable_blending();
    pkt.RectTextured(s_drawCtx, rect);
}

void EndFrame()
{
    PS2_AssertMsg(s_frameStarted, "EndFrame without BeginFrame!");
    s_frameStarted = false;

    // Send whatever 2D accumulated since the last flush (the HUD/console overlay
    // in the common case) so it lands on top before the buffer is displayed.
    FlushPending2D();

    graph_wait_vsync();
    graph_set_framebuffer_filtered(static_cast<int>(s_frameBuffer[s_drawCtx].address),
                                   static_cast<int>(s_frameBuffer[s_drawCtx].width),
                                   static_cast<int>(s_frameBuffer[s_drawCtx].psm), 0, 0);

    s_drawCtx ^= 1; // draw into the other buffer next frame

    vram::EndFrame();
}

} // namespace ps2::gs
