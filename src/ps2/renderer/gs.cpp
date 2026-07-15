/* ================================================================================================
 * File: gs.cpp
 * Brief: Double-buffered Graphics Synthesizer front-end. See gs.h.
 *
 *  Modelled on the ps2sdk libdraw "font"/"cube" samples: two 32-bit framebuffers
 *  in VRAM, one displayed while the other is drawn, using the two GS drawing
 *  contexts. draw_setup_environment programs each context so screen coordinates
 *  are direct top-left pixels.
 *
 *  Frame structure: BeginFrame() clears color and depth immediately (its own
 *  DMA transfer), the VU1 3D world arrives over PATH1, and the 2D overlay
 *  accumulates inside the Begin2D()/End2D() section, sent at End2D() to draw
 *  on top with an always-pass z-test. 2D and 3D never interleave; the section
 *  boundaries are asserted.
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
#include "ps2/renderer/render_packet.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vram.h"
#include "ps2/builtin/builtin.h" // global_palette

#include <dma.h>
#include <gs_psm.h>
#include <graph.h>
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>

namespace ps2::gs {
namespace {

constexpr int kWidth  = 640;
constexpr int kHeight = 448;

// Per-frame packet headroom. Worst observed 2D load is a full console of text
// (~2200 glyphs at 4 qwords each); 32K qwords (512 KB) leaves ample margin.
constexpr int kPacketQwords = 32768;

// Scratch packet for synchronous texture uploads (DMA chain tags only; the
// pixel data is referenced in place).
constexpr int kTexUploadQwords = 128;

// The color+depth clear, sent as its own transfer at the top of each frame.
constexpr int kClearQwords = 128;

static framebuffer_t s_frame[2];
static zbuffer_t     s_zbuffer;

static RenderPacket s_framePacket[2];   // double-buffered per-frame packets
static RenderPacket s_texUploadPacket;  // scratch packet for texture uploads
static RenderPacket s_clearPacket;      // per-frame color+depth clear

static int s_drawCtx   = 1; // which framebuffer/context we render into this frame
static int s_packetIdx = 0; // which frame packet is being filled

static bool s_frameStarted = false;
static bool s_in2D         = false;

// Screen clean color. Distinctive dark blue.
static u8 s_clear[3] = { 0x20, 0x20, 0x38 };

// Set when a VRAM allocation evicted a texture: draws already queued (or still
// rasterising) may reference the freed range, so the next upload must sync the
// GS first. Sticky until a GS-idle point - a block freed early in the frame
// can be handed out later without a new eviction.
static bool s_vramReuseHazard = false;

// Texture bound in the current 2D section.
static const tex::Texture * s_currentTex = nullptr;

// The global-palette CLUT: Quake's shared 8-bit palette, uploaded once at Init
// to a fixed VRAM spot (16x16 PSMCT32 image = 4 blocks) that every Palette8
// texture's TEX0 points at. s_clutData holds the entries in the GS's CSM1
// arrangement: within each 32-entry group the two middle 8-entry blocks swap
// (index bits 3 and 4 exchange).
alignas(16) static u32 s_clutData[256];
static vram::Address s_clutVramAddr = vram::Address::Invalid;

// Pixel stride the texture occupies VRAM with (the TEX0 TBW and transfer DBW).
// 8-bit textures must use a multiple of 128 (TBW must be even for PSMT8/4);
// other formats use their width as-is.
inline int TextureStridePixels(const tex::Texture & texture, int psm)
{
    if (psm == GS_PSM_8)
    {
        return (texture.width + 127) & ~127;
    }
    return texture.width;
}

inline RenderPacket & FramePacket()
{
    return s_framePacket[s_packetIdx];
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

int Width()  { return kWidth; }
int Height() { return kHeight; }

int CurrentContext()
{
    return s_drawCtx;
}

int DepthTestMethod()
{
    return static_cast<int>(s_zbuffer.method);
}

void SetClearColor(u8 r, u8 g, u8 b)
{
    s_clear[0] = r;
    s_clear[1] = g;
    s_clear[2] = b;
}

void Init()
{
    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // Two 32-bit framebuffers.
    // TODO: Consider more compact framebuffer formats to leave more vram for textures (RGB16?).
    s_frame[0].width   = kWidth;
    s_frame[0].height  = kHeight;
    s_frame[0].mask    = 0;
    s_frame[0].psm     = GS_PSM_32;
    s_frame[0].address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_PSM_32, GRAPH_ALIGN_PAGE));

    s_frame[1]         = s_frame[0];
    s_frame[1].address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_PSM_32, GRAPH_ALIGN_PAGE));

    // Z-buffer for the 3D world; larger depth = closer (the projection maps the
    // near plane to 0xFFFF), hence GREATER_EQUAL. 16-bit z leaves ~1.27 MB of
    // VRAM for textures. It must be the *signed* 16-bit format: the GS pairs
    // PSMCT32 color with the Z32/Z24/Z16S column - plain Z16 only works with
    // CT16 color buffers.
    s_zbuffer.enable  = DRAW_ENABLE;
    s_zbuffer.method  = ZTEST_METHOD_GREATER_EQUAL;
    s_zbuffer.mask    = 0;
    s_zbuffer.zsm     = GS_ZBUF_16S;
    s_zbuffer.address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_ZBUF_16S, GRAPH_ALIGN_PAGE));

    // The global-palette CLUT lives with the fixed allocations (a 16x16
    // PSMCT32 image, 256 words); the streamed texture heap takes everything
    // after it, rounded up to a page so its footprint math stays page-aligned
    // (the rest of the CLUT's page is unused).
    const int clutVramAddr = graph_vram_allocate(16, 16, GS_PSM_32, GRAPH_ALIGN_BLOCK);
    vram::Init((clutVramAddr + ArrayLength(s_clutData) + 2047) & ~2047);
    s_clutVramAddr = vram::Address(clutVramAddr);

    // Display framebuffer 0 first; auto-detects NTSC/PAL.
    graph_initialize(static_cast<int>(s_frame[0].address), kWidth, kHeight, GS_PSM_32, 0, 0);

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
    pkt.SetupEnvironment(0, s_frame[0], s_zbuffer);
    pkt.TextureWrapping(0, wrap);
    pkt.SetupEnvironment(1, s_frame[1], s_zbuffer);
    pkt.TextureWrapping(1, wrap);
    pkt.Finish();

    pkt.SendNormal();
    dma_wait_fast();
    draw_wait_finish();

    // Build and upload the global-palette CLUT (it never changes). The CSM1
    // reorder swaps entry index bits 3 and 4 - the arrangement the GS reads
    // the CLUT buffer with (see ps2stuff GS::ReorderClut).
    for (int i = 0; i < ArrayLength(s_clutData); ++i)
    {
        const int csm1 = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
        s_clutData[csm1] = global_palette[i];
    }

    RenderPacket & upload = s_texUploadPacket;
    upload.Reset();
    upload.TextureTransfer(s_clutData, 16, 16, GS_PSM_32, s_clutVramAddr, 64);
    upload.TextureFlush();

    upload.SendChain();
    dma_wait_fast();

    s_drawCtx   = 1;
    s_packetIdx = 0;
}

vram::Address GlobalClutAddress()
{
    return s_clutVramAddr;
}

void BeginFrame()
{
    PS2_AssertMsg(!s_frameStarted, "BeginFrame: frame already started!");
    s_frameStarted = true;

    s_packetIdx ^= 1;

    // The clear goes out immediately as its own transfer instead of riding the
    // deferred 2D packet: the VU1 3D world arrives over PATH1 mid-frame and
    // must land on an already-cleared framebuffer. The z=0 sprite with an
    // ALLPASS z-test clears color and depth in one pass (0 = farthest).
    RenderPacket & clear = s_clearPacket;
    clear.Reset();

    draw_disable_blending(); // draw_clear must overwrite, never blend
    clear.DisableTests(s_drawCtx, s_zbuffer);
    clear.Clear(s_drawCtx,
                0.0f, 0.0f,
                static_cast<float>(kWidth), static_cast<float>(kHeight),
                static_cast<int>(s_clear[0]), static_cast<int>(s_clear[1]), static_cast<int>(s_clear[2]));
    clear.EnableTests(s_drawCtx, s_zbuffer); // restore the real z-test for the 3D world
    clear.Finish();

    clear.SendNormal();
    dma_wait_fast();
    draw_wait_finish();

    // The GS is idle now, so nothing queued can reference reused VRAM anymore.
    s_vramReuseHazard = false;
    vram::BeginFrame();
}

void Begin2D()
{
    PS2_AssertMsg(s_frameStarted, "Begin2D outside Begin/EndFrame!");
    PS2_AssertMsg(!s_in2D, "Begin2D called twice!");
    s_in2D       = true;
    s_currentTex = nullptr; // the TEX0 dedupe state is per 2D section

    // The 2D overlay accumulates here and goes out at End2D, after the 3D
    // world has drawn: always-pass z-test so it lands on top.
    RenderPacket & pkt = FramePacket();
    pkt.Reset();
    pkt.DisableTests(s_drawCtx, s_zbuffer);
}

void End2D()
{
    PS2_AssertMsg(s_in2D, "End2D without Begin2D!");
    s_in2D = false;

    RenderPacket & pkt = FramePacket();
    pkt.Finish();

    dma_wait_fast();
    pkt.SendNormal();
    draw_wait_finish();

    s_vramReuseHazard = false; // GS idle again
}

bool In2DMode()
{
    return s_in2D;
}

void FillRect(int x, int y, int w, int h, u8 r, u8 g, u8 b, u8 a)
{
    PS2_AssertMsg(s_in2D, "FillRect outside the 2D section!");

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
        if (w >= kWidth / 2)
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

        dma_wait_fast();
        pkt.SendNormal();
        draw_wait_finish();

        pkt.Reset(); // GS registers persist; keep accumulating into the same packet
    }
    else
    {
        RenderPacket & pkt = s_texUploadPacket;
        pkt.Reset();
        pkt.Finish();

        dma_wait_fast();
        pkt.SendNormal();
        draw_wait_finish();
    }
    s_vramReuseHazard = false;
}

void EnsureTextureResident(const tex::Texture & texture)
{
    PS2_Assert(texture.type != tex::ImageType::Null && texture.pixels != nullptr);

    if (texture.vramAddr != tex::Texture::kNotResident)
    {
        vram::Touch(texture); // protect from eviction until the next frame
        return;
    }

    const int psm       = tex::GsPsm(texture.format);
    const int sizeWords = vram::TextureFootprintWords(texture.width, texture.height, psm);

    bool evicted = false;
    const vram::Address addr = vram::Allocate(texture, sizeWords, &evicted);

    s_vramReuseHazard |= evicted;
    if (s_vramReuseHazard)
    {
        SyncGsBeforeVramReuse();
    }

    texture.vramAddr = addr;

    // Fill the libdraw descriptor used when binding. The stride (TEX0's TBW)
    // differs from the width for narrow 8-bit textures; the page-grid footprint
    // already covers the rounding.
    const int stride = TextureStridePixels(texture, psm);

    texture.texbuf.address         = static_cast<unsigned int>(addr);
    texture.texbuf.width           = static_cast<unsigned int>(stride);
    texture.texbuf.psm             = static_cast<unsigned int>(psm);
    texture.texbuf.info.width      = draw_log2(static_cast<unsigned int>(texture.width));
    texture.texbuf.info.height     = draw_log2(static_cast<unsigned int>(texture.height));
    texture.texbuf.info.components = static_cast<unsigned char>(tex::GsComponents(texture.components));
    texture.texbuf.info.function   = static_cast<unsigned char>(tex::GsFunction(texture.function));

    // Synchronous DMA upload; the chain references the pixels in EE RAM.
    // TODO: TextureTransfer has no EnsureSpace - revisit the 128-qword scratch
    // packet if large streamed assets ever exceed its chain-tag headroom.
    RenderPacket & pkt = s_texUploadPacket;
    pkt.Reset();
    pkt.TextureTransfer(texture.pixels, texture.width, texture.height, psm, addr, stride);
    pkt.TextureFlush();

    pkt.SendChain();
    dma_wait_fast();

    Com_DPrintf("VRAM: uploaded '%s' (%dx%d, %d KB)\n", texture.name,
                texture.width, texture.height, sizeWords * 4 / 1024);
}

void SetTextureFor2D(const tex::Texture & texture)
{
    PS2_AssertMsg(s_in2D, "SetTextureFor2D outside the 2D section!");
    PS2_Assert(texture.type != tex::ImageType::Null && texture.pixels != nullptr);

    if (&texture == s_currentTex)
    {
        return; // already bound (and made resident) this section
    }

    EnsureTextureResident(texture);
    s_currentTex = &texture;

    RenderPacket & pkt = FramePacket();
    pkt.EnsureSpace(16);

    lod_t lod;
    lod.calculation   = LOD_USE_K;
    lod.max_level     = 0;
    lod.mag_filter    = static_cast<unsigned char>(tex::GsMagFilter(texture.magFilter));
    lod.min_filter    = static_cast<unsigned char>(tex::GsMinFilter(texture.minFilter));
    lod.mipmap_select = LOD_MIPMAP_REGISTER;
    lod.l             = 0;
    lod.k             = 0.0f;

    clutbuffer_t clut;
    if (texture.format == tex::PixelFormat::Palette8)
    {
        // Reload the on-chip CLUT cache from the global palette on every bind:
        // cheap (1 KB) at the 2D path's bind rate. TODO: CLUT_COMPARE_CBP0
        // skips redundant reloads - worthwhile once world textures bind per-surface.
        clut.address      = static_cast<unsigned int>(s_clutVramAddr);
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

    texbuffer_t texbuf = texture.texbuf; // libdraw wants a mutable pointer

    pkt.TextureSampling(s_drawCtx, lod);
    pkt.TextureBuffer(s_drawCtx, texbuf, clut);
}

void DrawTexturedRect(int x, int y, int w, int h,
                      int u0, int v0, int u1, int v1, u8 brightness)
{
    PS2_AssertMsg(s_in2D, "DrawTexturedRect outside the 2D section!");
    PS2_AssertMsg(s_currentTex != nullptr, "DrawTexturedRect without SetTextureFor2D!");

    RenderPacket & pkt = FramePacket();
    pkt.EnsureSpace(8);

    texrect_t rect;
    rect.v0.x = static_cast<float>(x);
    rect.v0.y = static_cast<float>(y);
    rect.v0.z = 0u;
    rect.t0.u = static_cast<float>(u0);
    rect.t0.v = static_cast<float>(v0);
    rect.v1.x = static_cast<float>(x + w);
    rect.v1.y = static_cast<float>(y + h);
    rect.v1.z = 0u;
    rect.t1.u = static_cast<float>(u1);
    rect.t1.v = static_cast<float>(v1);

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
    PS2_AssertMsg(!s_in2D, "EndFrame inside the 2D section - End2D missing!");
    s_frameStarted = false;

    graph_wait_vsync();
    graph_set_framebuffer_filtered(static_cast<int>(s_frame[s_drawCtx].address),
                                   static_cast<int>(s_frame[s_drawCtx].width),
                                   static_cast<int>(s_frame[s_drawCtx].psm), 0, 0);

    s_drawCtx ^= 1; // draw into the other buffer next frame

    vram::EndFrame();
}

} // namespace ps2::gs
