/* ================================================================================================
 * File: gs.cpp
 * Brief: Double-buffered Graphics Synthesizer front-end. See gs.h.
 *
 *  Two framebuffers in VRAM, one displayed while the other is drawn, one per GS drawing context,
 *  modelled on the ps2sdk libdraw samples. draw_setup_environment programs each context so that
 *  screen coordinates are direct top-left pixels.
 *
 *  Config::framebuffer16Bit picks their format. 16-bit costs 560 KB each instead of 1120 KB,
 *  which nearly doubles the texture heap and halves the GS's colour write and blend-read
 *  bandwidth, in exchange for 5:5:5 colour - hardware dithering covers most of the banding.
 *
 *  Textures stream on first bind into the VRAM left over after the framebuffers and z-buffer
 *  (~1.27 MB), managed by vram.cpp. While one is resident, binding it is a TEX0/TEX1 register
 *  write and nothing more. The frame, the command buffer and the residency policy belong to
 *  ps2::rs; the uploads here are synchronous and own a packet and channel of their own.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/clut.h"
#include "ps2/renderer/gif_writer.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vram.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/splash_screen.h"
#include "ps2/debug/profile.h"
#include "ps2/renderer/profile.h"
#include "ps2/system/heap.h"

#include <cstdint> // std::uintptr_t
#include <cstring> // memset
#include <optional>
#include <dma.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <graph.h>
#include <kernel.h> // SyncDCache, CreateSema/WaitSema/PollSema, iSignalSema, ExitHandler
#include <timer.h>  // GetTimerSystemTime / kBUSCLKBY256
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>
#include <gs_privileged.h> // GS_REG_CSR / GS_REG_BUSDIR
#include <vif_codes.h>
#include <vif_registers.h> // VIF1_STAT

namespace ps2::gs {
namespace {

// ------------------------------------------------------------------------------------------------
// GifPacket
// ------------------------------------------------------------------------------------------------

// A GIF packet with a buffer of its own, sent down the GIF channel by the EE. The frame's
// command buffer carries everything else; what is left for this are the transfers that cannot
// ride it - the one-time context setup in Init, which runs before the command buffer exists,
// and the synchronous texture and CLUT uploads.
class GifPacket final
{
public:
    GifPacket() = default;

    // Non-copyable: it owns its buffer.
    GifPacket(const GifPacket &) = delete;
    GifPacket & operator=(const GifPacket &) = delete;

    // Allocates the buffer. Call once, after the heap is up - not from a static constructor.
    void Init(const int maxQwords)
    {
        PS2_AssertMsg(m_base == nullptr, "GifPacket::Init called twice!");
        PS2_Assert(maxQwords > 0);

        // 64-byte (cache line) aligned and zeroed. Behind the tagged allocator, so it shows up
        // in the memory overlay.
        const size_t sizeBytes = static_cast<size_t>(maxQwords + kGuardQwords) * sizeof(qword_t);
        m_base = static_cast<qword_t *>(heap::AllocAligned(heap::MemAlign(64), sizeBytes, heap::MemTag::Renderer));
        std::memset(m_base, 0, sizeBytes);
        m_maxQwords = maxQwords;
    }

    // Rewinds to the start of the buffer and hands back the writer to build with.
    GifWriter & Begin()
    {
        PS2_AssertMsg(m_base != nullptr, "GifPacket::Begin before Init!");
        return m_writer.emplace(m_base, m_maxQwords);
    }

    // Sends what has been built as one normal transfer. Fire and forget; the wait is WaitGifChannel().
    void SendNormal()
    {
        dma_channel_send_normal(DMA_CHANNEL_GIF, m_base, BuiltQwords(), 0, 0);
    }

    // Sends it as a source-chain transfer, for the uploads whose payload is referenced by
    // chain tags the packet holds rather than copied into it.
    void SendChain()
    {
        dma_channel_send_chain(DMA_CHANNEL_GIF, m_base, BuiltQwords(), 0, 0);
    }

    // Waits until the GIF channel is usable again.
    // NOTE: assumes fast waits are enabled for it (see Init below).
    static void WaitGifChannel() { dma_wait_fast(); }

    // Waits for the FINISH event a GifWriter::Finish() armed.
    static void WaitFinish() { draw_wait_finish(); }

private:
    // Slack past m_maxQwords, under asserts only. The libdraw helpers report their size only by
    // returning the advanced cursor, so an overrun is caught after the fact - this is the room
    // that keeps the offending write inside our own allocation, so GifWriter::Advance halts on
    // it instead of it becoming heap corruption somebody debugs later. Comfortably larger than
    // any single emission: draw_texture_transfer's whole chain fits in the 128-qword packet.
    // A release build checks nothing and so has nothing to land in, and does not pay for it.
#if PS2_QUAKE_ASSERTS
    static constexpr int kGuardQwords = 256;
#else // !PS2_QUAKE_ASSERTS
    static constexpr int kGuardQwords = 0;
#endif // PS2_QUAKE_ASSERTS

    // Qwords Begin()'s writer has built, which is what a send transfers.
    Q_ALWAYS_INLINE int BuiltQwords() const
    {
        return m_writer.has_value() ? m_writer->QwordCount() : 0;
    }

    qword_t *                m_base      = nullptr;
    int                      m_maxQwords = 0;
    std::optional<GifWriter> m_writer;
};

static GifPacket s_texUploadPacket; // owns its buffer; sent over the GIF channel

// ------------------------------------------------------------------------------------------------
// Local helpers / constants
// ------------------------------------------------------------------------------------------------

// Scratch packet for the transfers that are still the EE's own: streamed texture
// and CLUT uploads (DMA chain tags only; the pixel data is referenced in place),
// the one-time context setup in Init - which runs before the frame chain exists -
// and the bare FINISH the VRAM-reuse sync needs outside a 2D section. Everything
// else the GS is told to do now goes through the frame chain.
constexpr int kTexUploadQwords = 128;

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

// The four CLUTs, all living at fixed VRAM spots outside the texture heap
// (see clut.h for their layout).
//
// The global palette is Quake's shared 8-bit palette. The lit palette is that
// same palette pre-brightened by 'intensity', and is what a Palette8 image
// samples through when something is going to multiply it back down - a wall
// under its lightmap, a skin under its shade colour. Everything drawn at face
// value (the HUD, the menus, the sky) keeps the unscaled one, which is the same
// split ref_gl makes when it skips intensity for it_pic and it_sky. Both are
// through the brightness and contrast curve (gamma.h).
//
// The alpha ramp backs PixelFormat::Alpha8, the generated particle images,
// which carry only their shape as an alpha and take their colour from the
// primitive. The light ramp backs PixelFormat::Light8, the lightmap atlases: the
// same, but the alpha is the luxel's light through the brightness curve.
static tex::Clut s_globalPaletteClut;
static tex::Clut s_litPaletteClut;
static tex::Clut s_alphaRampClut;
static tex::Clut s_lightRampClut;

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

// Sends the CLUTs to their fixed VRAM addresses and waits for the transfer. Called from Init,
// before a frame has ever started, and from UpdateCluts, between frames with the GS idle, so it
// can take the shared upload packet without fighting the streamed texture uploads for it and
// without having to fence anything.
//
// The lit palette reaches Palette8 images only, which is every image the retail game ships. A
// PixelFormat::RGBA32 texture (a .tga replacement) carries the intensity in its own texels instead
// and picks up a new value when it is next loaded - the same restart ref_gl needs for all of them.
void UploadCluts(const tex::Clut * const (&cluts)[4])
{
    GifWriter & upload = s_texUploadPacket.Begin();

    for (const tex::Clut * clut : cluts)
    {
        // The CPU has just built these entries and the chain REFs them in place. SendChain
        // writes back only the tag buffer, so without this the GS loads whatever RAM held: on a
        // console that can be a CLUT of zeros, which makes every palettized texel transparent
        // and leaves only untextured fills on screen. PCSX2 has no data cache to show it.
        u32 * const entries = const_cast<u32 *>(clut->entries);
        SyncDCache(entries, entries + tex::Clut::kNumEntries);

        upload.TextureTransfer(clut->entries, tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                               GS_PSM_32, clut->vramAddr, tex::Clut::kTransferWidth);
    }
    upload.TextureFlush();

    s_texUploadPacket.SendChain();
    GifPacket::WaitGifChannel();
}

// ------------------------------------------------------------------------------------------------
// The picture's place on the TV
// ------------------------------------------------------------------------------------------------

// What DISPLAY's origin was last written with: ps2_screen_x and ps2_screen_y's adjustments
// (video_mode.h), rewritten with the frame's DISPFB when they change.
static int s_screenAdjustX = 0;
static int s_screenAdjustY = 0;

void SetDisplayPosition()
{
    s_screenAdjustX = video::ScreenAdjustX();
    s_screenAdjustY = video::ScreenAdjustY();
    graph_set_screen(s_screenAdjustX, s_screenAdjustY, detail::g_state.width, detail::g_state.height);

    const video::Standard standard = video::Selected();
    Com_DPrintf("GS: picture %+d pixels, %+d rows: DISPLAY DX %d, DY %d.\n",
                video::ScreenOffsetX(), video::ScreenOffsetY(), video::DisplayX(standard), video::DisplayY(standard));
}

// Loads the global palette into the GS's CLUT buffer and sets CBP0 to it, so the
// CLUT_COMPARE_CBP0 in every indexed bind (see MakeTex0) starts from a known state.
// Nothing documents what CBP0 holds after a GS reset, and whatever ran before us
// may have left it on one of our CLUT addresses with its own palette in the buffer.
// The first bind through that CLUT would then compare equal and skip its load.
//
// Must follow the CLUT uploads, since it reads one. Anything that rewrites a CLUT
// in place later has to force a reload the same way, as UpdateCluts does: binds
// compare addresses, not contents.
void SeedClutBuffer()
{
    // TEX2 writes only TEX0's PSM and CLUT fields, so there is no texture to make up
    // for it, and the first bind rewrites the lot. The PSM has to be an indexed one,
    // or the GS ignores CLD.
    GifWriter & pkt = s_texUploadPacket.Begin();
    pkt.SetRegister(ContextReg(GS_REG_TEX2, DrawContext::Ctx0),
                    GS_SET_TEX2(GS_PSM_8, static_cast<u32>(s_globalPaletteClut.vramAddr) >> 6,
                                GS_PSM_32, CLUT_STORAGE_MODE1, 0, CLUT_LOAD_COPY_CBP0));
    pkt.EndGifPacket();

    s_texUploadPacket.SendNormal();
    GifPacket::WaitGifChannel();
}

// ------------------------------------------------------------------------------------------------
// Vsync wait
// ------------------------------------------------------------------------------------------------

// PresentFramebuffer used to spin on GS_CSR's VSINT bit (graph_wait_vsync), which held the EE for
// the 8-11 ms a frame leaves before its field ends. The main thread sleeps instead: the EE INTC's
// vblank-start line (INTC_VBLANK_S, the same edge the spin watched) signals this semaphore from
// an interrupt handler, and the kernel resumes the waiter at the handler's exit - microseconds,
// well inside the blanking interval the DISPFB write needs. The background loader (fs/async_io.h,
// the music's reads included) runs in the time the spin burned.
static s32           s_vblankSema      = -1;
static int           s_vblankHandlerId = -1;
static const cvar_t * s_vsyncSpin      = nullptr; // ps2_vsync_spin: 1 brings the spin back, for an A/B

static s32 VblankHandler(const s32 cause)
{
    (void)cause;
    // The semaphore's max count is one, so a vblank nobody waited for is remembered as a single
    // pending signal, and the overflow error a second one gets is harmless.
    iSignalSema(s_vblankSema);
    ExitHandler();
    return 0;
}

// Installs the handler; a failure leaves the spin in place, which is what every frame before
// 2026-10-26 used.
static void InitVsyncWait()
{
    s_vsyncSpin = Cvar_Get("ps2_vsync_spin", "0", 0);

    ee_sema_t sema  = {};
    sema.init_count = 0;
    sema.max_count  = 1;
    s_vblankSema    = CreateSema(&sema);
    if (s_vblankSema < 0)
    {
        Com_Printf("WARNING: GS: no vblank semaphore (%d); vsync waits spin.\n", static_cast<int>(s_vblankSema));
        return;
    }

    s_vblankHandlerId = graph_add_vsync_handler(&VblankHandler);
    if (s_vblankHandlerId < 0)
    {
        Com_Printf("WARNING: GS: no vblank handler (%d); vsync waits spin.\n", s_vblankHandlerId);
        DeleteSema(s_vblankSema);
        s_vblankSema = -1;
    }
}

// Returns at the next vblank start, the way graph_wait_vsync did.
static void WaitVsync()
{
    if (s_vblankSema < 0 || s_vsyncSpin->value != 0.0f)
    {
        graph_wait_vsync();
        return;
    }

    // A vblank that passed while the frame was still being built is not the one to present on,
    // any more than it was for the spin, which cleared VSINT before watching it: drain it so the
    // wait ends at the next edge.
    while (PollSema(s_vblankSema) >= 0) {}
    WaitSema(s_vblankSema);
}

// ------------------------------------------------------------------------------------------------
// Present clock
// ------------------------------------------------------------------------------------------------

static_assert(kPresentTicksPerSec == kBUSCLKBY256, "The present clock counts T2 ticks");

// See gs.h. Init sets what one field lasts on it, by the standard it set up.
static PresentClock s_presentClock;
static u32          s_ticksPerField;

// Stamps the present just made, counting its gap from the last one in fields, rounded to the
// nearest: a stamp sits microseconds past its field boundary, never half a field.
void StampPresent()
{
    // GetTimerSystemTime scales T2's count up to BUSCLK units, so the low byte is always zero (see
    // Sys_Milliseconds); shifting it off gives the timer's own 576 kHz ticks. The 32 bits kept wrap
    // every two hours or so, which the unsigned difference below does not mind.
    const u32 now = static_cast<u32>(GetTimerSystemTime() >> 8);

    // Two presents are never less than a field apart - each waits for a vsync of its own - so a
    // gap that rounds to none could only be a clock fault, and counts as the one field it was.
    u32 fields = 1;
    if (s_presentClock.frames != 0)
    {
        fields = (now - s_presentClock.lastTicks + (s_ticksPerField / 2u)) / s_ticksPerField;
        fields = (fields != 0) ? fields : 1;
    }

    s_presentClock.frames   += 1;
    s_presentClock.fields   += fields;
    s_presentClock.lastTicks = now;
}

// Called once the DISPFB write is out: how long after its stamp this present's write landed. The
// write has to fall inside the blanking interval (about 1.5 ms), so the worst case is the one
// number that says whether the sleeping wait wakes in time; it read 1 us in PCSX2.
void StampDisplayWrite()
{
    const u32 now   = static_cast<u32>(GetTimerSystemTime() >> 8);
    const u32 ticks = now - s_presentClock.lastTicks;
    if (ticks > s_presentClock.worstWriteTicks)
    {
        s_presentClock.worstWriteTicks = ticks;
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

// What the inline accessors, register builders and 2D resolve in gs.h read. Init fills it, and
// Invalidate2DBinding/EmitTextureBind move 'currentTex'; nothing else writes it.
namespace detail { State g_state; }

void Init(const Config & cfg)
{
    PS2_Assert(cfg.palette != nullptr && cfg.litPalette != nullptr && cfg.lightRamp != nullptr);
    PS2_Assert(cfg.width > 0 && cfg.height > 0);

    // VRAM is laid out from here, over the boot splash's fixed spots: it draws nothing more.
    splash::Retire();

    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    detail::g_state.width      = cfg.width;
    detail::g_state.height     = cfg.height;
    detail::g_state.currentTex = nullptr;

    // Two framebuffers. 16-bit halves them - 1120 KB each down to 560 KB - which
    // is where most of the texture heap's headroom comes from, at the cost of
    // 5:5:5 color (see the dither below) and one bit of destination alpha, which
    // nothing reads: every blend here scales by *source* alpha.
    const int framePsm = cfg.framebuffer16Bit ? GS_PSM_16 : GS_PSM_32;

    framebuffer_t * const frames = detail::g_state.framebuffer;

    // Framebuffer[0]
    frames[0].width   = static_cast<unsigned int>(cfg.width);
    frames[0].height  = static_cast<unsigned int>(cfg.height);
    frames[0].mask    = 0;
    frames[0].psm     = static_cast<unsigned int>(framePsm);
    frames[0].address = static_cast<unsigned int>(graph_vram_allocate(cfg.width, cfg.height, framePsm, GRAPH_ALIGN_PAGE));

    // Framebuffer[1]
    frames[1]         = frames[0];
    frames[1].address = static_cast<unsigned int>(graph_vram_allocate(cfg.width, cfg.height, framePsm, GRAPH_ALIGN_PAGE));

    // Z-buffer for the 3D world; larger depth = closer (the projection maps the
    // near plane to 0xFFFF), hence GREATER_EQUAL. Depth is 16-bit either way -
    // what changes is which 16-bit format, because the GS requires the color and
    // depth buffers to share a page layout: PSMCT32/24 pair with Z32/Z24/Z16S,
    // PSMCT16 pairs with Z16, PSMCT16S with Z16S. Mismatch them and depth sorting
    // breaks while color looks fine, which is a confusing way to find out.
    const int zPsm = cfg.framebuffer16Bit ? GS_ZBUF_16 : GS_ZBUF_16S;

    zbuffer_t & zbuffer = detail::g_state.zbuffer;
    zbuffer.enable  = DRAW_ENABLE;
    zbuffer.method  = ZTEST_METHOD_GREATER_EQUAL;
    zbuffer.mask    = 0;
    zbuffer.zsm     = static_cast<unsigned int>(zPsm);
    zbuffer.address = static_cast<unsigned int>(graph_vram_allocate(cfg.width, cfg.height, zPsm, GRAPH_ALIGN_PAGE));

    // All four CLUTs live with the fixed allocations; the streamed texture heap
    // takes everything after them, rounded up to a page so its footprint math
    // stays page-aligned (the rest of the last CLUT's page is unused: the four
    // take 4 KB of its 8). graph_vram_allocate hands out increasing addresses, so
    // the heap starts past the last of them.
    const int clutVramAddr      = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    const int litClutVramAddr   = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    const int alphaRampClutAddr = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    const int lightRampClutAddr = graph_vram_allocate(tex::Clut::kImageWidth, tex::Clut::kImageHeight,
                                                      GS_PSM_32, GRAPH_ALIGN_BLOCK);
    PS2_Assert(lightRampClutAddr > alphaRampClutAddr && alphaRampClutAddr > litClutVramAddr &&
               litClutVramAddr > clutVramAddr);

    vram::Init((lightRampClutAddr + tex::Clut::kNumEntries + 2047) & ~2047);
    s_globalPaletteClut.vramAddr = vram::Address(clutVramAddr);
    s_litPaletteClut.vramAddr    = vram::Address(litClutVramAddr);
    s_alphaRampClut.vramAddr     = vram::Address(alphaRampClutAddr);
    s_lightRampClut.vramAddr     = vram::Address(lightRampClutAddr);

    // The addresses are all ClutAddress() needs; the entry buffers stay here.
    detail::g_state.globalPaletteClut = s_globalPaletteClut.vramAddr;
    detail::g_state.litPaletteClut    = s_litPaletteClut.vramAddr;
    detail::g_state.alphaRampClut     = s_alphaRampClut.vramAddr;
    detail::g_state.lightRampClut     = s_lightRampClut.vramAddr;

    // Display framebuffer 0 first, in the config's standard. These are graph_initialize's calls -
    // interlaced FIELD mode with the flicker filter, a black background, frame 0 on both read
    // circuits - with the mode passed in, where graph_initialize can only take the console's own,
    // and the picture where ps2_screen_x/y put it. The output comes on below, once frame 0 holds a
    // picture.
    graph_set_mode(GRAPH_MODE_INTERLACED, video::GraphMode(cfg.standard), GRAPH_MODE_FIELD, GRAPH_ENABLE);
    SetDisplayPosition();
    graph_set_bgcolor(0, 0, 0);
    graph_set_framebuffer_filtered(static_cast<int>(frames[0].address), cfg.width, framePsm, 0, 0);

    // The standard fixes the field rate the present clock counts in: 50 a second on PAL, 59.94
    // on NTSC.
    s_ticksPerField = (cfg.standard == video::Standard::Pal) ? (kPresentTicksPerSec / 50u)
                                                             : (((kPresentTicksPerSec * 1001u) + 30000u) / 60000u);

    s_texUploadPacket.Init(kTexUploadQwords);
    InitVsyncWait();

    // Program both drawing contexts: context 0 -> frame 0, context 1 -> frame 1.
    // The environment defaults texture wrapping to CLAMP; Quake's DrawTileClear
    // addresses texels in screen space and needs REPEAT.
    texwrap_t wrap;
    wrap.horizontal = WRAP_REPEAT;
    wrap.vertical   = WRAP_REPEAT;
    wrap.minu = wrap.maxu = 0;
    wrap.minv = wrap.maxv = 0;

    // On the upload packet, not the frame chain: gs::Init runs before mod::Init, and the
    // chain's halves live in the arena that reserves (see PS2_RefInit's ordering note).
    GifWriter & pkt = s_texUploadPacket.Begin();
    pkt.SetupEnvironment(Index(DrawContext::Ctx0), frames[0], zbuffer);
    pkt.TextureWrapping(Index(DrawContext::Ctx0), wrap);
    pkt.SetupEnvironment(Index(DrawContext::Ctx1), frames[1], zbuffer);
    pkt.TextureWrapping(Index(DrawContext::Ctx1), wrap);

    // DIMX is global rather than per-context and never changes, so it is set up
    // once here; only the DTHE enable is rewritten per frame (see BeginFrame).
    pkt.SetRegister(static_cast<u64>(GS_REG_DIMX), PackDitherMatrix(kDitherMatrix));

    // Frame 0 is on screen from here until the first present (the first frame is drawn into frame
    // 1), showing whatever VRAM holds there. The boot splash left the id logo there in exactly this
    // layout unless the config chose another one, so the logo stays up until the game's first
    // frame. In any other layout those texels would show as garbage: the frame starts black.
    if (!splash::IsShownIn(static_cast<int>(frames[0].address), cfg.width, cfg.height, framePsm, cfg.standard))
    {
        pkt.DisableTests(Index(DrawContext::Ctx0), zbuffer);
        pkt.Clear(Index(DrawContext::Ctx0), 0.0f, 0.0f,
                        static_cast<float>(cfg.width),
                        static_cast<float>(cfg.height),
                        0, 0, 0);
        pkt.EnableTests(Index(DrawContext::Ctx0), zbuffer);
    }
    pkt.Finish();

    s_texUploadPacket.SendNormal();
    GifPacket::WaitGifChannel();
    GifPacket::WaitFinish();

    graph_enable_output();

    // Build and upload the CLUTs. Only UpdateCluts changes them again.
    s_globalPaletteClut.BuildFromPalette(cfg.palette);
    s_litPaletteClut.BuildFromPalette(cfg.litPalette);
    s_alphaRampClut.BuildAlphaRamp();
    s_lightRampClut.BuildLightRamp(cfg.lightRamp);

    UploadCluts({ &s_globalPaletteClut, &s_litPaletteClut, &s_alphaRampClut, &s_lightRampClut });
    SeedClutBuffer();
}

void UpdateCluts(const u32 * const palette, const u32 * const litPalette, const u8 * const lightRamp)
{
    // As UploadTexture: nothing may be in flight over the other paths while this goes down
    // PATH3, and the caller finished the frame that was left drawing (rs::FinishFrameInFlight).
    // The GS reads the CLUTs as it draws, which is the other reason this sits between frames.
    cmdbuf::WaitIdle();

    s_globalPaletteClut.BuildFromPalette(palette);
    s_litPaletteClut.BuildFromPalette(litPalette);
    s_lightRampClut.BuildLightRamp(lightRamp);

    // The alpha ramp hasn't changed, but one send covers them all. The seed reloads the global
    // palette into the GS's CLUT buffer: the one it holds is whichever was bound last, and a bind
    // of the same address would keep it (see SeedClutBuffer).
    UploadCluts({ &s_globalPaletteClut, &s_litPaletteClut, &s_alphaRampClut, &s_lightRampClut });
    SeedClutBuffer();
}

// ------------------------------------------------------------------------------------------------
// GS register values
// ------------------------------------------------------------------------------------------------

u64 MakeTex0(const tex::Texture & texture, const bool lit)
{
    PS2_AssertMsg(texture.IsVramResident(), "MakeTex0 for a texture with no VRAM!");

    // Indexed formats sample through one of the fixed CLUTs, which the GS copies into its on-chip
    // CLUT buffer when a TEX0 write asks it to. CLUT_COMPARE_CBP0 makes that copy only when the
    // CLUT's address differs from CBP0's, the one already in the buffer. A plain CLUT_LOAD copied
    // the 1 KB palette again on every write, and a 3D batch resends TEX0 with every output window
    // the microprogram kicks, so a frame reloaded the same palette hundreds of times.
    //
    // Comparing the address alone is enough because only UpdateCluts rewrites a CLUT after Init,
    // reseeding CBP0 as Init does (see SeedClutBuffer), and every indexed TEX0 is built here. Everything else leaves the
    // CLUT fields zero, which is also what CPSM reads as: GS_PSM_32 is 0.
    const vram::Address clutAddr = ClutAddress(texture.format, lit);
    const bool palettized = (clutAddr != vram::Address::Invalid);

    const int psm = tex::GsPsm(texture.format);

    // The stride (TEX0's TBW) differs from the width for narrow 8-bit textures; the page-grid
    // footprint vram.cpp charges for already covers the rounding.
    const int stride = tex::TextureStridePixels(texture, psm);

    return GS_SET_TEX0(static_cast<u32>(texture.vramAddr) >> 6,
                       static_cast<u32>(stride) >> 6,
                       psm,
                       tex::Log2(static_cast<u32>(texture.width)),
                       tex::Log2(static_cast<u32>(texture.height)),
                       tex::GsComponents(texture.components),
                       tex::GsFunction(texture.function),
                       palettized ? (static_cast<int>(clutAddr) >> 6) : 0,
                       GS_PSM_32, // CPSM; only read for palettized PSMs (and == 0 anyway)
                       CLUT_STORAGE_MODE1, 0,
                       palettized ? CLUT_COMPARE_CBP0 : CLUT_NO_LOAD);
}

u64 MakeMipTbp1(const tex::Texture & texture)
{
    if (tex::MipLevels(texture) == 0)
    {
        return 0;
    }
    PS2_AssertMsg(texture.IsVramResident(), "MakeMipTbp1 for a texture with no VRAM!");

    // The same offsets and buffer widths UploadTexture wrote the levels with. A texture with
    // fewer than three levels leaves the unused ones pointing at its base; MXL keeps the GS off
    // them.
    const vram::MipLayout & layout = vram::MipLayoutFor(texture);
    const u32 base = static_cast<u32>(texture.vramAddr) >> 6;
    return GS_SET_MIPTBP1(base + layout.blockOffset[1], layout.strideUnits[1],
                          base + layout.blockOffset[2], layout.strideUnits[2],
                          base + layout.blockOffset[3], layout.strideUnits[3]);
}

void ReleaseTexture(const tex::Texture & texture)
{
    // Never leave the TEX0 dedupe pointing at a released texture: a rebind in
    // the same 2D section must go through EnsureTextureResident again, and the
    // cache may recycle the slot for a different image entirely.
    if (detail::g_state.currentTex == &texture)
    {
        detail::g_state.currentTex = nullptr;
    }

    if (!texture.IsVramResident())
    {
        return;
    }

    vram::Free(texture); // raises the reuse hazard
}

void DefragVramHeap()
{
    if (!vram::Defragment())
    {
        return;
    }

    // Every texture is non-resident now: the 2D dedupe would otherwise skip the
    // rebind of the current one and sample VRAM it no longer owns. The recycled
    // heap raises the reuse hazard inside vram::Defragment.
    detail::g_state.currentTex = nullptr;
}

// ------------------------------------------------------------------------------------------------
// Presentation
// ------------------------------------------------------------------------------------------------

void PresentFramebuffer(const DrawContext ctx)
{
    // DISPFB has to be rewritten inside the blanking interval or the change tears, and the wait
    // doubles as the frame's pacing. The probe wraps the sleep as it wrapped the spin, so the
    // frame log's "EE work = Frame - VSync" reads the same either way.
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::VSync);
        WaitVsync();
    }
    StampPresent();

    // The picture's place too, in the same blanking interval, when the video menu moved it.
    if (s_screenAdjustX != video::ScreenAdjustX() || s_screenAdjustY != video::ScreenAdjustY())
    {
        SetDisplayPosition();
    }

    const framebuffer_t & fb = detail::g_state.framebuffer[Index(ctx)];
    graph_set_framebuffer_filtered(static_cast<int>(fb.address),
                                   static_cast<int>(fb.width),
                                   static_cast<int>(fb.psm), 0, 0);
    StampDisplayWrite();
}

const PresentClock & GetPresentClock()
{
    return s_presentClock;
}

// ------------------------------------------------------------------------------------------------
// Readback
// ------------------------------------------------------------------------------------------------

// The sequence is ps2sdk's own (ee/debug/screenshot.c), which runs on hardware and in PCSX2; it is
// reimplemented here rather than linked because that one DMAs into a buffer with no alignment
// guarantee, one row per transfer. The Quake port's renderer has the same (quake-ps2, gs.cpp).
void DownloadFramebufferRows(const DrawContext ctx, const int y, const int rows, void * const dst)
{
    const framebuffer_t & fb = detail::g_state.framebuffer[Index(ctx)];
    const int psm   = static_cast<int>(fb.psm);
    const int width = static_cast<int>(fb.width);
    const u32 bytes = static_cast<u32>(width * rows * ((psm == GS_PSM_16) ? 2 : 4));

    PS2_Assert(y >= 0 && rows > 0 && (y + rows) <= static_cast<int>(fb.height));
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(dst) & 63u) == 0 && (bytes & 63u) == 0,
                  "Readback target must be whole, aligned cache lines!");

    // The request goes down PATH2, VIF1 DIRECT, rather than the GIF channel the uploads use: the
    // VIF1 FIFO is what turns around to carry the pixels back. MSKPATH3 holds PATH3 off until the
    // bus is the right way round again, and FLUSHA lets every path drain first. The EE waits on
    // the FINISH ahead of TRXDIR to know the GS has the request before it turns the bus.
    const u64 bitBltBuf = GS_SET_BITBLTBUF(fb.address >> 6, fb.width >> 6, psm, 0, 0, psm);
    const u64 trxPos    = GS_SET_TRXPOS(0, y, 0, 0, 0);
    const u64 trxReg    = GS_SET_TRXREG(width, rows);
    const u64 trxDir    = GS_SET_TRXDIR(1); // local -> host
    const u64 gifTag    = GIF_SET_TAG(5, 1, 0, 0, GIF_FLG_PACKED, 1);

    alignas(64) static u64 s_request[14];
    u32 * const vifCodes = static_cast<u32 *>(static_cast<void *>(s_request));
    vifCodes[0] = VIF_CODE(0, 0, VIF_CMD_NOP, 0);
    vifCodes[1] = VIF_CODE(0x8000, 0, VIF_CMD_MSKPATH3, 0);
    vifCodes[2] = VIF_CODE(0, 0, VIF_CMD_FLUSHA, 0);
    vifCodes[3] = VIF_CODE(6, 0, VIF_CMD_DIRECT, 0);
    s_request[2]  = gifTag;    s_request[3]  = GIF_REG_AD;
    s_request[4]  = bitBltBuf; s_request[5]  = GS_REG_BITBLTBUF;
    s_request[6]  = trxPos;    s_request[7]  = GS_REG_TRXPOS;
    s_request[8]  = trxReg;    s_request[9]  = GS_REG_TRXREG;
    s_request[10] = 0;         s_request[11] = GS_REG_FINISH;
    s_request[12] = trxDir;    s_request[13] = GS_REG_TRXDIR;

    // And the one that lets PATH3 back in afterwards.
    alignas(64) static u32 s_unmaskPath3[4];
    s_unmaskPath3[0] = VIF_CODE(0, 0, VIF_CMD_MSKPATH3, 0);
    s_unmaskPath3[1] = VIF_CODE(0, 0, VIF_CMD_NOP, 0);
    s_unmaskPath3[2] = VIF_CODE(0, 0, VIF_CMD_NOP, 0);
    s_unmaskPath3[3] = VIF_CODE(0, 0, VIF_CMD_NOP, 0);

    // VIF1_STAT.FDR: the VIF1 FIFO's direction, set for VIF1 -> memory. FQC is how full it is.
    constexpr u32 kVif1StatFdr   = 1u << 23;
    constexpr u32 kVif1StatFqc   = 0x1Fu << 24;
    constexpr u64 kCsrFinish     = GS_SET_CSR(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    constexpr u64 kImrFinishMask = 1u << 9; // FINISHMSK: the event is polled here, not taken

    const u64 prevImr = GsPutIMR(GsGetIMR() | kImrFinishMask);
    *GS_REG_CSR = kCsrFinish; // clear any earlier FINISH event

    dma_channel_send_normal(DMA_CHANNEL_VIF1, s_request, 7, 0, 0);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    while ((*GS_REG_CSR & kCsrFinish) == 0) {}
    while ((VIF1_STAT & kVif1StatFqc) != 0) {}

    // Turn the bus around and let the VIF1 channel pull the rows in. Nothing dirty may be left
    // over 'dst' to be written back on top of what arrives, and nothing cached may be read
    // instead of it afterwards.
    VIF1_STAT      = kVif1StatFdr;
    *GS_REG_BUSDIR = GS_SET_BUSDIR(1);

    SyncDCache(dst, static_cast<u8 *>(dst) + bytes);
    dma_channel_receive_normal(DMA_CHANNEL_VIF1, dst, static_cast<int>(bytes), 0, 0);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    InvalidDCache(dst, static_cast<u8 *>(dst) + bytes);

    VIF1_STAT      = 0;
    *GS_REG_BUSDIR = GS_SET_BUSDIR(0);

    GsPutIMR(prevImr);
    *GS_REG_CSR = kCsrFinish;

    dma_channel_send_normal(DMA_CHANNEL_VIF1, s_unmaskPath3, 1, 0, 0);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
}

// ------------------------------------------------------------------------------------------------
// Texture upload
// ------------------------------------------------------------------------------------------------

void UploadTexture(const tex::Texture & texture)
{
    PS2_Assert(texture.pixels != nullptr);
    PS2_AssertMsg(texture.IsVramResident(), "UploadTexture before VRAM was allocated for it!");

    const int psm    = tex::GsPsm(texture.format);
    const int stride = tex::TextureStridePixels(texture, psm);

    if (texture.dirtyPixels)
    {
        // The CPU just wrote these pixels; part of them may still sit in the data cache, and
        // SendChain only writes back the chain-tag buffer, not REF'd data - flush the range or
        // the GS reads stale texels. Built-ins are never dirty (the ELF loader wrote them).
        void * pixels = const_cast<void *>(texture.pixels);
        SyncDCache(pixels, static_cast<u8 *>(pixels) + tex::PixelBytes(texture));
        texture.dirtyPixels = false;
    }

    // Synchronous DMA upload; the chain references the pixels in EE RAM. TextureTransfer cannot
    // EnsureSpace up front - only draw_texture_transfer knows how many chain tags a given
    // texture needs - but GifWriter::Advance checks afterwards under asserts, so a texture that
    // outgrows this scratch packet says so instead of scribbling past it.
    GifWriter & pkt = s_texUploadPacket.Begin();
    const int mipLevels = tex::MipLevels(texture);
    if (mipLevels == 0)
    {
        pkt.TextureTransfer(texture.pixels, texture.width, texture.height, psm, texture.vramAddr, stride);
    }
    else
    {
        // One transfer per level, each to where the chain's layout puts it and at its own buffer
        // width - the same two things MIPTBP1 hands the GS for sampling it.
        const vram::MipLayout & layout = vram::MipLayoutFor(texture);
        const u8 * levelPixels = static_cast<const u8 *>(texture.pixels);
        for (int level = 0; level <= mipLevels; ++level)
        {
            const int levelWidth  = texture.width  >> level;
            const int levelHeight = texture.height >> level;
            const auto levelAddr  = vram::Address(static_cast<int>(texture.vramAddr) + (layout.blockOffset[level] * 64));

            pkt.TextureTransfer(levelPixels, levelWidth, levelHeight, psm, levelAddr, layout.strideUnits[level] * 64);
            levelPixels += levelWidth * levelHeight * tex::BytesPerTexel(texture.format);
        }
    }
    pkt.TextureFlush();

    // Nothing may reach the GS over PATH3 while a chain is still being fed to it over PATH1 and
    // PATH2: the two would interleave at the GIF, and an image transfer cut in half is the one
    // thing it does not put back together. Nothing is outstanding here today - every mid-frame
    // kick drains, and the frame ps2_gs_latency leaves drawing is retired at the next BeginFrame,
    // before any of this frame's binds - so this reads as free. It is the guard that keeps that
    // true: a kick left in flight anywhere upstream would otherwise surface as a corrupt texture
    // on 5% of frames, which is the kind of bug that takes a week.
    cmdbuf::WaitIdle();

    s_texUploadPacket.SendChain();
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::GsWait);
        GifPacket::WaitGifChannel();
    }

    vram::NoteTextureUpload(); // for the debug overlay's per-frame upload count
}

// ------------------------------------------------------------------------------------------------
// GIF emission
// ------------------------------------------------------------------------------------------------

void EmitClear(GifWriter & w, const DrawContext ctx, const u8 color[3], const bool dither)
{
    w.EnsureSpace(kClearQwords);

    draw_disable_blending(); // draw_clear must overwrite, never blend
    w.DisableTests(Index(ctx), detail::g_state.zbuffer);

    // Re-arm depth writes: a blended VU1 batch (ZMSK = 1) may have been this context's last word
    // on ZBUF two frames ago, and draw_disable_tests only touches TEST - without this the z=0
    // sprite would clear color but leave stale depth behind.
    w.SetRegister(ContextReg(GS_REG_ZBUF, ctx), MakeZBuf(false));

    // Dithering hides the banding a 5:5:5 framebuffer would otherwise show on smooth gradients.
    // Rewritten every frame (one qword) purely so the caller's knob can be flipped live to
    // compare; it does nothing to a 32-bit framebuffer.
    const bool dtheOn = (detail::g_state.framebuffer[0].psm == GS_PSM_16) && dither;
    w.SetRegister(static_cast<u64>(GS_REG_DTHE), GS_SET_DTHE(dtheOn ? 1 : 0));

    // The z=0 sprite with an ALLPASS z-test clears color and depth in one pass (0 = farthest).
    w.Clear(Index(ctx), 0.0f, 0.0f,
            static_cast<float>(Width()), static_cast<float>(Height()),
            static_cast<int>(color[0]), static_cast<int>(color[1]), static_cast<int>(color[2]));

    w.EnableTests(Index(ctx), detail::g_state.zbuffer); // restore the real z-test for the 3D world
}

void EmitBegin2D(GifWriter & w, const DrawContext ctx)
{
    w.EnsureSpace(kBegin2DQwords);

    w.DisableTests(Index(ctx), detail::g_state.zbuffer);
    w.SetRegister(ContextReg(GS_REG_ZBUF, ctx), MakeZBuf(false));
}

void EmitEnd2D(GifWriter & w, const DrawContext ctx)
{
    w.EnsureSpace(kEnd2DQwords);

    w.SetRegister(ContextReg(GS_REG_TEST, ctx), MakePixelTests());
}

void EmitFillRect(GifWriter & w, const DrawContext ctx, const int x, const int y,
                  const int width, const int height, const u8 r, const u8 g, const u8 b, const u8 a)
{
    w.EnsureSpace(kFillRectQwords);

    rect_t rect;
    rect.v0.x = static_cast<float>(x);
    rect.v0.y = static_cast<float>(y);
    rect.v0.z = 0u;
    rect.v1.x = static_cast<float>(x + width);
    rect.v1.y = static_cast<float>(y + height);
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
        w.RectFilled(Index(ctx), rect);
    }
    else
    {
        // Translucent (fade screen and friends). GS alpha is 0..0x80 = 0..1.
        draw_enable_blending();
        rect.color.a = static_cast<u8>(a >> 1);

        // The GS is slow on very large polygons; libdraw recommends strips for near-fullscreen
        // fills.
        if (width >= Width() / 2)
        {
            w.RectFilledStrips(Index(ctx), rect);
        }
        else
        {
            w.RectFilled(Index(ctx), rect);
        }
        draw_disable_blending();
    }
}

void EmitTextureBind(GifWriter & w, const DrawContext ctx, const Bind2D & bind)
{
    PS2_AssertMsg(bind.needsBind, "EmitTextureBind for a binding that did not need one!");
    const tex::Texture & bindTex = *bind.texture;

    w.EnsureSpace(kTextureBindQwords);

    // The same two registers, in the same order, that libdraw's draw_texture_sampling and
    // draw_texturebuffer wrote here before - each is one GIF tag plus one A+D pair, which is
    // exactly what SetRegister emits.
    w.SetRegister(ContextReg(GS_REG_TEX1, ctx), MakeTex1(bindTex));
    w.SetRegister(ContextReg(GS_REG_TEX0, ctx), MakeTex0(bindTex, tex::TakesIntensity(bindTex.type)));

    detail::g_state.currentTex = &bindTex;
}

void EmitTexturedRect(GifWriter & w, const DrawContext ctx, const int x, const int y,
                      const int width, const int height,
                      const int u0, const int v0, const int u1, const int v1,
                      const Bind2D & bind, const u8 brightness[3])
{
    PS2_AssertMsg(detail::g_state.currentTex != nullptr, "EmitTexturedRect with nothing bound!");

    w.EnsureSpace(kTexturedRectQwords);

    // The bind's origins are both zero unless a scrap atlas is bound, in which case they shift
    // the coordinates from the image's own space into its corner of the atlas.
    texrect_t rect;
    rect.v0.x = static_cast<float>(x);
    rect.v0.y = static_cast<float>(y);
    rect.v0.z = 0u;
    rect.t0.u = static_cast<float>(u0 + bind.originU);
    rect.t0.v = static_cast<float>(v0 + bind.originV);
    rect.v1.x = static_cast<float>(x + width);
    rect.v1.y = static_cast<float>(y + height);
    rect.v1.z = 0u;
    rect.t1.u = static_cast<float>(u1 + bind.originU);
    rect.t1.v = static_cast<float>(v1 + bind.originV);

    // Modulate: 0x80 = 1.0, so 'brightness' 128 leaves texels unchanged. Vertex alpha 0x80
    // likewise preserves texel alpha, which the alpha test then uses to cut out transparent
    // texels (e.g. the console font background).
    rect.color.r = brightness[0];
    rect.color.g = brightness[1];
    rect.color.b = brightness[2];
    rect.color.a = 0x80;
    rect.color.q = 1.0f;

    draw_disable_blending();
    w.RectTextured(Index(ctx), rect);
}

} // namespace ps2::gs
