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
 *  DMA transfer), the VU1 3D world arrives over PATH1 mid-frame, and the 2D
 *  overlay accumulates in the frame packet, sent at EndFrame() to draw on top
 *  with an always-pass z-test.
 *
 *  Textures are uploaded to the VRAM left over after the framebuffers and stay
 *  resident (~1.8 MB fits every built-in image at once), so switching textures
 *  mid-frame is just a TEX0/TEX1 register write in the frame packet - no DMA
 *  upload, no pipeline flush, and 2D draws happen in call order.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_packet.h"
#include "ps2/renderer/texture.h"

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
static bool s_2dFlushed    = false;
static const tex::Texture * s_currentTex = nullptr; // texture bound in the current frame packet

static std::uint8_t s_clear[3] = { 0x20, 0x20, 0x38 }; // distinctive dark blue

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

int CurrentContext() { return s_drawCtx; }

int DepthTestMethod() { return static_cast<int>(s_zbuffer.method); }

void SetClearColor(std::uint8_t r, std::uint8_t g, std::uint8_t b)
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
    s_frame[0].width   = kWidth;
    s_frame[0].height  = kHeight;
    s_frame[0].mask    = 0;
    s_frame[0].psm     = GS_PSM_32;
    s_frame[0].address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_PSM_32, GRAPH_ALIGN_PAGE));

    s_frame[1]         = s_frame[0];
    s_frame[1].address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_PSM_32, GRAPH_ALIGN_PAGE));

    // Z-buffer for the 3D world; larger depth = closer (the projection maps the
    // near plane to 0xFFFFFF), hence GREATER_EQUAL. 32-bit z leaves ~730 KB of
    // VRAM for textures - switch to GS_ZBUF_16S if that gets tight once real
    // game textures land.
    s_zbuffer.enable  = DRAW_ENABLE;
    s_zbuffer.method  = ZTEST_METHOD_GREATER_EQUAL;
    s_zbuffer.mask    = 0;
    s_zbuffer.zsm     = GS_ZBUF_32;
    s_zbuffer.address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_ZBUF_32, GRAPH_ALIGN_PAGE));

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

    s_drawCtx   = 1;
    s_packetIdx = 0;
}

void BeginFrame()
{
    PS2_AssertMsg(!s_frameStarted, "BeginFrame: frame already started!");
    s_frameStarted = true;
    s_currentTex   = nullptr; // texture state does not persist across packets

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

    // The 2D overlay accumulates here all frame and goes out at EndFrame, after
    // the 3D world has drawn: always-pass z-test so it lands on top.
    RenderPacket & pkt = FramePacket();
    pkt.Reset();
    pkt.DisableTests(s_drawCtx, s_zbuffer);
}

void FillRect(int x, int y, int w, int h,
              std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    PS2_AssertMsg(s_frameStarted, "FillRect outside Begin/EndFrame!");
    PS2_AssertMsg(!s_2dFlushed, "FillRect after Flush2D!");

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
        rect.color.a = static_cast<std::uint8_t>(a >> 1);

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

void UploadTexture(tex::Texture & texture)
{
    PS2_AssertMsg(!s_frameStarted, "UploadTexture must happen outside Begin/EndFrame!");
    PS2_AssertMsg(texture.vramAddr == tex::Texture::kNotResident, "Texture already resident!");
    PS2_Assert(texture.type != tex::ImageType::Null);

    const int psm  = tex::GsPsm(texture.format);
    const int addr = graph_vram_allocate(texture.width, texture.height, psm, GRAPH_ALIGN_BLOCK);
    PS2_AssertMsg(addr >= 0, "Out of GS VRAM for textures!");

    texture.vramAddr = addr;

    // Fill the libdraw descriptor used when binding.
    texture.texbuf.address         = static_cast<unsigned int>(addr);
    texture.texbuf.width           = static_cast<unsigned int>(texture.width);
    texture.texbuf.psm             = static_cast<unsigned int>(psm);
    texture.texbuf.info.width      = draw_log2(static_cast<unsigned int>(texture.width));
    texture.texbuf.info.height     = draw_log2(static_cast<unsigned int>(texture.height));
    texture.texbuf.info.components = static_cast<unsigned char>(tex::GsComponents(texture.components));
    texture.texbuf.info.function   = static_cast<unsigned char>(tex::GsFunction(texture.function));

    // Synchronous DMA upload; the chain references the pixels in EE RAM.
    RenderPacket & pkt = s_texUploadPacket;
    pkt.Reset();
    pkt.TextureTransfer(texture.pixels, texture.width, texture.height, psm, addr);
    pkt.TextureFlush();

    pkt.SendChain();
    dma_wait_fast();
}

void SetTexture(const tex::Texture & texture)
{
    PS2_AssertMsg(s_frameStarted, "SetTexture outside Begin/EndFrame!");
    PS2_AssertMsg(!s_2dFlushed, "SetTexture after Flush2D!");
    PS2_AssertMsg(texture.vramAddr != tex::Texture::kNotResident, "Texture not resident in VRAM!");
    PS2_Assert(texture.type != tex::ImageType::Null);

    if (&texture == s_currentTex)
    {
        return;
    }
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

    // No palettized formats in use, so the CLUT slots stay empty.
    clutbuffer_t clut;
    clut.address      = 0;
    clut.psm          = 0;
    clut.storage_mode = CLUT_STORAGE_MODE1;
    clut.start        = 0;
    clut.load_method  = CLUT_NO_LOAD;

    texbuffer_t texbuf = texture.texbuf; // libdraw wants a mutable pointer

    pkt.TextureSampling(s_drawCtx, lod);
    pkt.TextureBuffer(s_drawCtx, texbuf, clut);
}

void DrawTexturedRect(int x, int y, int w, int h,
                      int u0, int v0, int u1, int v1, std::uint8_t brightness)
{
    PS2_AssertMsg(s_frameStarted, "DrawTexturedRect outside Begin/EndFrame!");
    PS2_AssertMsg(!s_2dFlushed, "DrawTexturedRect after Flush2D!");
    PS2_AssertMsg(s_currentTex != nullptr, "DrawTexturedRect without SetTexture!");

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

void Flush2D()
{
    PS2_AssertMsg(s_frameStarted, "Flush2D outside Begin/EndFrame!");
    PS2_AssertMsg(!s_2dFlushed, "Flush2D called twice this frame!");
    s_2dFlushed = true;

    RenderPacket & pkt = FramePacket();
    pkt.Finish();

    dma_wait_fast();
    pkt.SendNormal();
    draw_wait_finish();
}

void EndFrame()
{
    PS2_AssertMsg(s_frameStarted, "EndFrame without BeginFrame!");

    if (!s_2dFlushed)
    {
        Flush2D();
    }
    s_frameStarted = false;
    s_2dFlushed    = false;

    graph_wait_vsync();
    graph_set_framebuffer_filtered(static_cast<int>(s_frame[s_drawCtx].address),
                                   static_cast<int>(s_frame[s_drawCtx].width),
                                   static_cast<int>(s_frame[s_drawCtx].psm), 0, 0);

    s_drawCtx ^= 1; // draw into the other buffer next frame
}

} // namespace ps2::gs
