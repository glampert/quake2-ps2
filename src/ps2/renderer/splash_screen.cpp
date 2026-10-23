/* ================================================================================================
 * File: splash_screen.cpp
 * Brief: The boot splash. See splash_screen.h.
 *
 *  It makes the display calls gs::Init makes - libgraph's interlaced FIELD mode with the flicker
 *  filter - then three GIF transfers: the drawing environment and a black clear, the logo's texels
 *  as an RGBA32 texture, and the logo four times its size with bilinear filtering. The output
 *  stays off until all of it is drawn. Nothing outlives it but the picture: the packets and texels
 *  share one heap block, freed once the GIF has read them, and VRAM is used at fixed spots
 *  libgraph's allocator never hears of, so gs::Init lays out its own from the start of VRAM.
 *
 *  ShowImage draws another picture under the logo the same way, at another fixed spot: the save
 *  notice, while the boot reads config.cfg off the memory card before the renderer exists.
 *  HideImage draws the splash again.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/splash_screen.h"
#include "ps2/renderer/gif_writer.h"
#include "ps2/builtin/builtin.h"
#include "ps2/system/heap.h"

#include <cstdio>
#include <cstring>
#include <dma.h>
#include <graph.h>
#include <gs_psm.h>
#include <kernel.h> // SyncDCache

namespace ps2::splash {
namespace {

// gs::Init's first framebuffer in its default layout (ps2_fb_width 640, ps2_fb_height 0,
// ps2_fb_16bit 1, ps2_video_mode auto): the start of VRAM, the standard's full height, 16-bit.
constexpr int kFrameWidth    = 640;
constexpr int kFramePsm      = GS_PSM_16;
constexpr int kFrameVramAddr = 0;

// VRAM word addresses past the tallest such framebuffer, 640x512 at 16 bits, which takes 80 pages
// of 2048 words: the logo's texels, the z-buffer the environment asks for, never tested or
// written, and ShowImage's texels, which take up to 10 pages (320x64 at 32 bits).
constexpr int kVramPageWords   = 2048;
constexpr int kTextureVramAddr = 80 * kVramPageWords;
constexpr int kZBufferVramAddr = 82 * kVramPageWords; // past the two pages of a 64x64 RGBA32 texture
constexpr int kImageVramAddr   = 96 * kVramPageWords;
constexpr int kImageMaxWidth   = 320;
constexpr int kImageMaxHeight  = 64;

// The logo is 36x42 texels; four times that is 144x168, over a third of an NTSC screen's height.
constexpr int kScale = 4;

// ShowImage's picture goes this far below the logo.
constexpr int kImageGap = 16;

// Palette index 255 is Quake's see-through one. Here it is the black of the background.
constexpr int kTransparentIndex = 255;

// What the logo's colours are multiplied by, each channel clamping at full. 1 draws the palette's
// own colours, as the main menu does, which looked dark and faded as a splash on black. The logo
// is dark to begin with - its channels average 33 of 255 and none passes 71, so nothing clips
// below about 3.5 - and the 16-bit framebuffer then truncates each channel to 5 bits, a step of 8,
// with no dither here.
constexpr float kColorIntensity = 1.7f;

// The three transfers' room in the block, in qwords. They take: the environment (16) and a clear
// in 20 strips (29), plus the end of the packet (1); the texel upload's chain (9) and the texture
// flush (3); the texture's registers (6), the sprite (4) and the end of the packet (1).
constexpr int kEnvironmentQwords = 64;
constexpr int kUploadQwords      = 32;
constexpr int kDrawQwords        = 32;
constexpr int kPacketQwords      = kEnvironmentQwords + kUploadQwords + kDrawQwords;

// How long to wait for each transfer, in polls of the GIF channel's CHCR. They take microseconds;
// a GIF a launcher left wedged would never finish them, and the boot goes on without the splash
// rather than hang on it.
constexpr int kDmaWaitPolls = 1 << 22;

static bool            s_shown         = false;
static bool            s_retired       = false; // gs::Init owns VRAM: nothing more is drawn here.
static int             s_shownHeight   = 0;
static video::Standard s_shownStandard = video::Standard::Ntsc;

// A palette colour with its three colour channels scaled by kColorIntensity.
u32 ScaleColor(const u32 color)
{
    u32 scaledColor = color & 0xFF000000u;
    for (u32 shift = 0; shift < 24; shift += 8)
    {
        const float channel = static_cast<float>((color >> shift) & 0xFFu) * kColorIntensity;
        scaledColor |= ((channel >= 255.0f) ? 255u : static_cast<u32>(channel)) << shift;
    }
    return scaledColor;
}

bool SendAndWait(qword_t * const data, const int qwords, const bool chain)
{
    if (chain)
    {
        dma_channel_send_chain(DMA_CHANNEL_GIF, data, qwords, 0, 0);
    }
    else
    {
        dma_channel_send_normal(DMA_CHANNEL_GIF, data, qwords, 0, 0);
    }
    return dma_channel_wait(DMA_CHANNEL_GIF, kDmaWaitPolls) == 0;
}

// The power of two a texture's side is addressed through, as TEX0 takes it: the log2 of the
// smallest one at least `size`.
int ExtentLog2(const int size)
{
    int log2 = 0;
    while ((1 << log2) < size)
    {
        ++log2;
    }
    return log2;
}

// A picture to draw: `width` x `height` RGBA32 texels uploaded to `vramAddr`, and the sprite they
// go on, its corners as libdraw takes them.
struct Picture
{
    int   width;
    int   height;
    int   vramAddr;
    float x0, y0;
    float x1, y1;
    bool  clear;    // A black clear of the screen first.
    bool  bilinear; // Filtered between texels, rather than point sampled.
    bool  alpha;    // Texels with alpha 0 left out by the environment's alpha test; drawn black otherwise.
};

// Draws a picture in three GIF transfers: the drawing environment (and the clear), the texels'
// upload as an RGBA32 texture, and the sprite. `fill` writes the texels. The GS addresses the
// texture through the powers of two past its size, in a buffer as wide as the 64 texels a PSMCT32
// TBW counts in, and region clamping keeps the taps inside the picture, so nothing past it is
// ever sampled. False if there is no memory for it or the GIF didn't take the transfers.
template<typename FillTexels>
bool DrawPicture(const Picture & picture, const FillTexels & fill)
{
    const int numTexels   = picture.width * picture.height;
    const int bufferWidth = (picture.width + 63) & ~63;

    // The packets first, then the texels, all in one cache-line aligned block.
    const size_t packetBytes = static_cast<size_t>(kPacketQwords) * sizeof(qword_t);
    const size_t blockBytes  = packetBytes + (static_cast<size_t>(numTexels) * sizeof(u32));
    void * const block = heap::TryAllocAligned(heap::MemAlign(64), blockBytes, heap::MemTag::Renderer);
    if (block == nullptr)
    {
        std::printf("Splash: no memory for a %dx%d picture, skipped.\n", picture.width, picture.height);
        return false;
    }

    qword_t * const packet = static_cast<qword_t *>(block);
    u32 * const texels = static_cast<u32 *>(static_cast<void *>(static_cast<u8 *>(block) + packetBytes));
    fill(texels);

    // The upload's chain REFs the texels where they are, and a chain send writes back only the
    // chain's own tags (see ps2-platform.md).
    SyncDCache(texels, texels + numTexels);

    framebuffer_t frame = {};
    frame.address = kFrameVramAddr;
    frame.width   = kFrameWidth;
    frame.height  = static_cast<unsigned int>(s_shownHeight);
    frame.psm     = kFramePsm;
    frame.mask    = 0;

    zbuffer_t zbuffer = {};
    zbuffer.enable  = DRAW_DISABLE;
    zbuffer.method  = ZTEST_METHOD_ALLPASS;
    zbuffer.address = kZBufferVramAddr;
    zbuffer.zsm     = GS_ZBUF_16;
    zbuffer.mask    = 1;

    gs::GifWriter environment(packet, kEnvironmentQwords);
    environment.SetupEnvironment(0, frame, zbuffer);
    if (picture.clear)
    {
        environment.Clear(0, 0.0f, 0.0f, static_cast<float>(kFrameWidth), static_cast<float>(s_shownHeight), 0, 0, 0);
    }
    environment.EndGifPacket();

    gs::GifWriter upload(packet + kEnvironmentQwords, kUploadQwords);
    upload.TextureTransfer(texels, picture.width, picture.height, GS_PSM_32, vram::Address(picture.vramAddr),
                           bufferWidth);
    upload.TextureFlush();

    texbuffer_t texture = {};
    texture.address         = static_cast<unsigned int>(picture.vramAddr);
    texture.width           = static_cast<unsigned int>(bufferWidth);
    texture.psm             = GS_PSM_32;
    texture.info.width      = static_cast<unsigned char>(ExtentLog2(picture.width));
    texture.info.height     = static_cast<unsigned char>(ExtentLog2(picture.height));
    texture.info.components = picture.alpha ? TEXTURE_COMPONENTS_RGBA : TEXTURE_COMPONENTS_RGB;
    texture.info.function   = TEXTURE_FUNCTION_DECAL;

    clutbuffer_t clut = {};
    clut.storage_mode = CLUT_STORAGE_MODE1;
    clut.load_method  = CLUT_NO_LOAD;

    lod_t lod = {};
    lod.calculation   = LOD_USE_K;
    lod.max_level     = 0;
    lod.mag_filter    = picture.bilinear ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
    lod.min_filter    = picture.bilinear ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
    lod.mipmap_select = LOD_MIPMAP_REGISTER;
    lod.l             = 0;
    lod.k             = 0.0f;

    texwrap_t wrap = {};
    wrap.horizontal = WRAP_REGION_CLAMP;
    wrap.vertical   = WRAP_REGION_CLAMP;
    wrap.minu       = 0;
    wrap.maxu       = picture.width - 1;
    wrap.minv       = 0;
    wrap.maxv       = picture.height - 1;

    texrect_t rect = {};
    rect.v0.x    = picture.x0;
    rect.v0.y    = picture.y0;
    rect.v0.z    = 0u;
    rect.t0.u    = 0.0f;
    rect.t0.v    = 0.0f;
    rect.v1.x    = picture.x1;
    rect.v1.y    = picture.y1;
    rect.v1.z    = 0u;
    rect.t1.u    = static_cast<float>(picture.width);
    rect.t1.v    = static_cast<float>(picture.height);
    rect.color.r = 0x80;
    rect.color.g = 0x80;
    rect.color.b = 0x80;
    rect.color.a = 0x80;
    rect.color.q = 1.0f;

    gs::GifWriter draw(packet + kEnvironmentQwords + kUploadQwords, kDrawQwords);
    draw.TextureBuffer(0, texture, clut);
    draw.TextureSampling(0, lod);
    draw.TextureWrapping(0, wrap);
    draw_disable_blending(); // Read when the sprite is emitted (see gs-renderer.md).
    draw.RectTextured(0, rect);
    draw.EndGifPacket();

    // One after another, as the upload goes as a chain and the rest as plain transfers.
    const bool drawn = SendAndWait(packet, environment.QwordCount(), /*chain=*/false) &&
                       SendAndWait(packet + kEnvironmentQwords, upload.QwordCount(), /*chain=*/true) &&
                       SendAndWait(packet + kEnvironmentQwords + kUploadQwords, draw.QwordCount(), /*chain=*/false);
    if (!drawn)
    {
        // The block stays allocated: a transfer that never finished may yet read it.
        std::printf("Splash: the GIF didn't take its transfers, skipped.\n");
        return false;
    }

    heap::Free(block, blockBytes, heap::MemTag::Renderer);
    return true;
}

// The logo's top edge on the screen, centred.
int LogoTop()
{
    return (s_shownHeight - (m_main_logo_height * kScale)) / 2;
}

// The logo, four times its size and centred on a black screen.
bool DrawLogo()
{
    const int drawWidth  = m_main_logo_width * kScale;
    const int drawHeight = m_main_logo_height * kScale;
    const int drawX      = (kFrameWidth - drawWidth) / 2;
    const int drawY      = LogoTop();

    Picture logo = {};
    logo.width    = m_main_logo_width;
    logo.height   = m_main_logo_height;
    logo.vramAddr = kTextureVramAddr;
    logo.x0       = static_cast<float>(drawX);
    logo.y0       = static_cast<float>(drawY);
    logo.x1       = static_cast<float>(drawX + drawWidth);
    logo.y1       = static_cast<float>(drawY + drawHeight);
    logo.clear    = true;
    logo.bilinear = true;
    logo.alpha    = false;

    return DrawPicture(logo, [](u32 * const texels) {
        const int numTexels = m_main_logo_width * m_main_logo_height;
        for (int i = 0; i < numTexels; ++i)
        {
            const int index = m_main_logo_data[i];
            texels[i] = (index == kTransparentIndex) ? 0u : ScaleColor(global_palette[index]);
        }
    });
}

} // namespace

void Show()
{
    const int startMsec = Sys_Milliseconds();

    // No config has been read yet, so this is the console's own standard (see video_mode.h).
    const video::Standard standard = video::Selected();
    const int frameHeight = video::VisibleLines(standard);
    s_shownHeight   = frameHeight;
    s_shownStandard = standard;

    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);

    // gs::Init's display calls, with the output off until the logo is drawn: the framebuffer holds
    // whatever was last in that VRAM.
    graph_set_mode(GRAPH_MODE_INTERLACED, video::GraphMode(standard), GRAPH_MODE_FIELD, GRAPH_ENABLE);
    graph_disable_output();
    graph_set_screen(0, 0, kFrameWidth, frameHeight);
    graph_set_bgcolor(0, 0, 0);
    graph_set_framebuffer_filtered(kFrameVramAddr, kFrameWidth, kFramePsm, 0, 0);

    if (!DrawLogo())
    {
        return;
    }

    graph_enable_output();

    s_shown = true;
    std::printf("Splash: the id logo is up (%s, %dx%d) after %d ms.\n", video::Name(standard), kFrameWidth,
                frameHeight, Sys_Milliseconds() - startMsec);
}

bool IsShownIn(const int vramAddress, const int width, const int height, const int psm, const video::Standard standard)
{
    return s_shown && vramAddress == kFrameVramAddr && width == kFrameWidth && height == s_shownHeight &&
           psm == kFramePsm && standard == s_shownStandard;
}

bool CanShowImage()
{
    return s_shown && !s_retired;
}

bool ShowImage(const u32 * const texels, const int width, const int height)
{
    const int x = (kFrameWidth - width) / 2;
    const int y = LogoTop() + (m_main_logo_height * kScale) + kImageGap;
    if (!CanShowImage() || width > kImageMaxWidth || height > kImageMaxHeight || x < 0 || y + height > s_shownHeight)
    {
        return false;
    }

    // libdraw's sprite takes its far corner as the last pixel it covers, not the one past it (its
    // END_OFFSET), so the corner is pulled in by one: one texel to a pixel, point sampled, as the
    // engine draws the same picture with the renderer up.
    Picture image = {};
    image.width    = width;
    image.height   = height;
    image.vramAddr = kImageVramAddr;
    image.x0       = static_cast<float>(x);
    image.y0       = static_cast<float>(y);
    image.x1       = static_cast<float>(x + width - 1);
    image.y1       = static_cast<float>(y + height - 1);
    image.clear    = false;
    image.bilinear = false;
    image.alpha    = true;

    return DrawPicture(image, [=](u32 * const out) {
        std::memcpy(out, texels, static_cast<size_t>(width * height) * sizeof(u32));
    });
}

void HideImage()
{
    if (CanShowImage())
    {
        DrawLogo();
    }
}

void Retire()
{
    s_retired = true;
}

} // namespace ps2::splash
