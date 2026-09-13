/* ================================================================================================
 * File: render_context.cpp
 * Brief: The renderer's command recorder and the frame it records. See render_context.h.
 *
 *  Frame structure: BeginFrame opens the command buffer and writes the colour+depth clear at the
 *  head of it. 2D and 3D then draw in any order, both recording into that same buffer. 2D
 *  primitives accumulate in a deferred DIRECT section (always-pass z-test, so it lands on top);
 *  the first primitive after a flush opens one lazily. The section is closed at each 2D->3D
 *  boundary and once more by EndFrame.
 *
 *  Nothing is sent until EndFrame: the whole frame is one chain and one kick. Ordering is the
 *  buffer's own order plus the VIF FLUSH each section opens with, and where a frame needs the GS
 *  to have actually *finished* - an upload about to land on VRAM queued draws still sample -
 *  FenceGs submits what has been built and waits for it.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/render_context.h"
#include "ps2/debug/profile.h"
#include "ps2/renderer/render_profile.h"
#include "ps2/renderer/texture.h"

namespace ps2::rc {
namespace {

// Room every GIF block keeps back for its own tail: the EOP terminator CloseGifBlock always
// writes.
constexpr int kBlockTailQwords = 1;

// What a GIF block must be able to take before it may be split, which is what the command buffer
// is asked to reserve when one opens.
//
// The clear knows its whole size up front. The 2D overlay does not - an empty HUD is a handful of
// qwords and a full console is thousands - so it takes whatever is left of the half and GifData
// splits the block when that runs out. For it this is only the floor one more primitive needs.
constexpr int kClearBlockQwords = gs::kClearQwords;
constexpr int k2DBlockMinQwords = 64 + kBlockTailQwords;

// The GIF block currently open - the clear at the top of the frame, or the 2D overlay - as a
// cursor into the command buffer's own memory. Engaged only between OpenGifBlock and
// CloseGifBlock, which is also what says whether a block is open.
static std::optional<gs::GifWriter> s_gifBlock;

// High-water of what one GIF block held, banked by CloseGifBlock since the writer itself is
// rebuilt per block. Shown as "Gif2DPk" in the draw-stats overlay.
static int s_gifBlockPeakQwords = 0;

// Whether a 2D section is accumulating. Distinct from a block being open: the clear opens a block
// of its own, and a 2D section can outlive a block across a split.
static bool s_in2D = false;

static bool s_frameStarted = false;

// Which framebuffer/context we render into this frame.
static int s_drawCtx = 1;

// The framebuffer the chain in flight is drawing into - what DISPFB is pointed at once it has
// been fenced. Not the same as s_drawCtx once a frame is left drawing while the next one is
// built: that is the whole of what "one frame of latency" means here. -1 until the first frame
// has been kicked, which is the one frame with nothing finished to show.
static int s_inFlightCtx = -1;

// ps2_gs_latency: leave the frame's chain drawing at EndFrame and show it at the *next* one, so
// the GS rasterises frame N while the EE builds N+1, instead of the EE standing at the fence
// waiting for it. Costs one frame of input lag, which is why it is a cvar and not a constant -
// sampled each EndFrame so it can be flipped live and judged on hardware.
static const cvar_t * s_gsLatency = nullptr;

// ps2_fb_dither: sampled every frame so the 5:5:5 banding it hides can be compared on the spot.
static const cvar_t * s_enableDither = nullptr;

// Opens a DIRECT block and points a GIF writer at its payload.
//
// 'minQwords' is what the caller must be able to write before the block can be closed and another
// opened; the command buffer reserves it, which may drain and rewind. That is safe at every call
// site and for the same reason: a GIF block only ever opens where no span into the buffer is live
// - the top of the frame, and a 2D section, which by construction has no 3D gather in flight.
//
// The block's capacity is not what was reserved but everything left in the half, because the 2D
// overlay's real size is not knowable up front. Reserve is the floor; GifData watches the ceiling.
gs::GifWriter & OpenGifBlock(const int minQwords)
{
    PS2_AssertMsg(!s_gifBlock.has_value(), "A GIF block is already open!");

    cmdbuf::Reserve(minQwords + RenderContext::kDirectOverheadQwords);

    RenderContext & ctx = Ctx();
    ctx.OpenDirect();

    const int capacity = cmdbuf::QwordCapacity() - cmdbuf::QwordCount();
    PS2_Assert(capacity >= minQwords);

    return s_gifBlock.emplace(ctx.DirectCursor(), capacity);
}

// Closes the open block, handing the command buffer back the cursor the emitters advanced. Does
// not submit: for a 2D split the next block simply follows this one in the same chain.
void CloseGifBlock()
{
    PS2_AssertMsg(s_gifBlock.has_value(), "No GIF block open!");

    s_gifBlock->EndGifPacket();

    RenderContext & ctx = Ctx();
    ctx.SetDirectCursor(s_gifBlock->Cursor());
    ctx.CloseDirect();

    const int used = s_gifBlock->QwordCount();
    if (used > s_gifBlockPeakQwords) { s_gifBlockPeakQwords = used; }

    s_gifBlock.reset();
}

// Opens the 2D section on demand: the first 2D primitive after a flush (or after BeginFrame)
// lands here. Cheap no-op once it is already open.
void Ensure2D()
{
    PS2_AssertMsg(s_frameStarted, "2D draw outside Begin/EndFrame!");
    if (s_in2D)
    {
        return;
    }
    s_in2D = true;

    // A VU1 batch writes TEX0 for this context on every chunk, so whatever was bound for 2D
    // before is not bound now.
    gs::Invalidate2DBinding();

    OpenGifBlock(k2DBlockMinQwords);
    gs::EmitBegin2D(*s_gifBlock, s_drawCtx);
}

// Empties the whole pipeline: submits the frame as far as it has been built and blocks until the
// GS has finished drawing every bit of it - and anything left over from the frame before, which
// under ps2_gs_latency may still be rasterising. The frame then carries on building where it left
// off: this is a stall, not a reset, and no pointer into the buffer moves.
//
// Its cost to the buffer is the terminator the kick writes, which every draw already reserves -
// see cmdbuf::kTerminatorQwords. It must not reserve anything itself: this fires in the middle of
// a draw, with the gather it is about to reference already in the buffer, and a reservation that
// overflowed would rewind that away.
void FenceGs()
{
    const bool reopen = s_gifBlock.has_value();
    if (reopen)
    {
        CloseGifBlock();
    }

    cmdbuf::Drain(); // marks its own DmaSend/DmaFlush/GsWait

    if (reopen)
    {
        // As a split: the section's GS state is in the registers, which a drain does not touch.
        OpenGifBlock(k2DBlockMinQwords);
    }

    vram::ClearReuseHazard();
}

// Finds 'sizeWords' of VRAM for the texture, escalating when the heap is full.
//
// The normal path evicts the least-recently-bound textures, but never ones bound this frame:
// their draws are still in the buffer, or queued at the GS. When that leaves nothing to take, the
// pins are the only thing in the way - and the sole reason they exist is work that has not been
// drawn yet. Fencing the GS retires that work, after which dropping the pins is legitimate and
// the whole heap is fair game again. The frame still renders correctly; it just spends its one
// kick early and pays a second one at EndFrame.
//
// The last rung repacks the heap into one free block, so it can only come up short for a texture
// larger than the entire heap - which the caller rejects before ever getting here.
vram::Address AllocateVramFor(const tex::Texture & texture, const int sizeWords)
{
    vram::Address addr = vram::TryAllocate(texture, sizeWords);

    if (addr == vram::Address::Invalid)
    {
        Com_DPrintf("VRAM: heap full mid-frame for '%s' (%d KB), fencing the GS to unpin.\n",
                    texture.name, sizeWords * 4 / 1024);

        FenceGs();
        gs::Invalidate2DBinding(); // the 2D dedupe must not survive an eviction
        vram::UnpinAll();
        vram::NoteOomSync();

        addr = vram::TryAllocate(texture, sizeWords);
    }

    if (addr == vram::Address::Invalid)
    {
        // Enough free words, just not contiguous. The GS is already idle from the rung above, so
        // the wholesale evict-and-repack is safe here.
        vram::Defragment();
        gs::Invalidate2DBinding();

        addr = vram::TryAllocate(texture, sizeWords);
    }

    if (addr == vram::Address::Invalid) [[unlikely]]
    {
        vram::DumpAllBlocks();
        Sys_Error("GS VRAM allocation failed for '%s' (%d KB) even after draining and defragmenting!",
                  texture.name, sizeWords * 4 / 1024);
    }

    return addr;
}

// Waits for the chain in flight to have been drawn, then puts its framebuffer on screen. A no-op
// when there is nothing outstanding, which is how the two ps2_gs_latency paths share it.
//
// **Why this is the top of a frame and not the bottom of the previous one.** There are two
// framebuffers, so the one the GS may be drawing into and the one being scanned out have to be
// the two different ones - which means the flip to the frame just finished has to happen before
// anything of the next frame reaches the GS, not after. Doing it here, ahead of the clear, leaves
// the whole of the frame's build with the display parked on the previous image and the other
// buffer free: a mid-frame kick - an overflow rewind, a texture fence - then lands somewhere
// nobody is looking.
void PresentFrameInFlight()
{
    // Nothing waiting to be shown: either EndFrame already presented this frame (ps2_gs_latency
    // off, where this is then the no-op at the next BeginFrame) or none has been kicked yet. The
    // early out has to come before the vsync, not after - falling through would spend a whole
    // field here and a second one at the frame's real present, halving the frame rate.
    if (s_inFlightCtx < 0)
    {
        return;
    }

    cmdbuf::WaitIdle(); // the GS fence; marks its own GsWait
    gs::PresentFramebuffer(s_inFlightCtx);

    s_inFlightCtx = -1; // shown; EndFrame is what puts the next one up
}

} // namespace

RenderContext detail::g_context;

// ------------------------------------------------------------------------------------------------
// GIF sections
// ------------------------------------------------------------------------------------------------

gs::GifWriter & RenderContext::GifData(const int qwords)
{
    PS2_AssertMsg(s_gifBlock.has_value(), "GIF emission with no block open!");

    // Plus the block's tail: keeping room for it here is what lets the EOP terminator never be
    // the thing that overruns the block.
    if (s_gifBlock->QwordCount() + qwords + kBlockTailQwords <= s_gifBlock->QwordCapacity()) [[likely]]
    {
        return *s_gifBlock;
    }

    // The split is invisible to what is being drawn: the state the section programmed lives in
    // the GS's registers, not in the block, so the new one needs no re-arming.
    CloseGifBlock();
    return OpenGifBlock(qwords + kBlockTailQwords);
}

void RenderContext::FlushPending2D()
{
    if (!s_in2D)
    {
        return; // nothing accumulated since the last flush
    }
    s_in2D = false;

    CloseGifBlock();

    // Closed, not sent. The block stays where it is and goes out with the frame: 3D that follows
    // lands after it because it is later in the buffer, and cannot overtake it at the GIF because
    // every block opens with a VIF FLUSH.
    //
    // The one job this does not do is making the GS idle, so the VRAM reuse hazard is not cleared
    // here - a texture bound by the 2D just closed may still be sampled by draws nothing has
    // sent. FenceGs is what clears it, and vram::TryAllocate refusing to evict anything bound
    // this frame is what keeps that rare.
}

// ------------------------------------------------------------------------------------------------
// 2D primitives
// ------------------------------------------------------------------------------------------------

void RenderContext::FillRect(const int x, const int y, const int width, const int height,
                             const u8 r, const u8 g, const u8 b, const u8 a)
{
    Ensure2D();
    gs::EmitFillRect(GifData(gs::kFillRectQwords), s_drawCtx, x, y, width, height, r, g, b, a);
}

void RenderContext::DrawTexturedRect(const tex::Texture & texture, const int x, const int y,
                                     const int width, const int height,
                                     const int u0, const int v0, const int u1, const int v1,
                                     const u8 brightness[3])
{
    Ensure2D();

    const gs::Bind2D bind = gs::ResolveBind2D(texture);
    if (bind.needsBind)
    {
        // Residency before the writer is taken: it can fence the GS, which closes and reopens
        // the section, and the dedupe above is what keeps this off the per-glyph path.
        EnsureTextureResident(*bind.texture);
        gs::EmitTextureBind(GifData(gs::kTextureBindQwords), s_drawCtx, bind);
    }

    gs::EmitTexturedRect(GifData(gs::kTexturedRectQwords), s_drawCtx,
                         x, y, width, height, u0, v0, u1, v1, bind, brightness);
}

// ------------------------------------------------------------------------------------------------
// Texture residency
// ------------------------------------------------------------------------------------------------

void EnsureTextureResident(const tex::Texture & texture)
{
    PS2_Assert(texture.type != tex::ImageType::Null && texture.pixels != nullptr);

    // A scrapped image has no VRAM of its own and its pixels are a window into the atlas:
    // residency is the atlas's, and binding it here would upload the whole atlas under the wrong
    // name and sample from the wrong corner. Only the 2D path can produce one, and
    // gs::ResolveBind2D resolves the atlas before this is called.
    PS2_AssertMsg(texture.atlas == nullptr,
                  "EnsureTextureResident on a scrapped image - bind its atlas!");

    if (texture.vramAddr != tex::Texture::kNotResident)
    {
        if (!texture.dirtyPixels)
        {
            vram::Touch(texture); // protect from eviction until the next frame
            return;
        }

        // Dynamic texture with rewritten pixels: re-upload over its own block. Draws queued
        // earlier this frame would sample the new texels instead of the ones they were issued
        // with - fence the GS first. (Its block is still owned, so the reuse hazard does not
        // apply here.)
        const bool boundThisFrame = vram::BoundThisFrame(texture);
        vram::Touch(texture);
        if (boundThisFrame)
        {
            FenceGs();
        }
    }
    else
    {
        const int psm = tex::GsPsm(texture.format);
        const int sizeWords = vram::TextureFootprintWords(texture.width, texture.height, psm);

        // Nothing below can service a texture bigger than the whole heap, and trying would evict
        // the entire working set first and then report it as a working-set problem. Say what is
        // actually wrong instead.
        if (sizeWords > vram::HeapTotalWords()) [[unlikely]]
        {
            Sys_Error("Texture '%s' (%dx%d) needs %d KB of GS VRAM, but the whole texture heap is only %d KB!",
                      texture.name, texture.width, texture.height,
                      sizeWords * 4 / 1024, vram::HeapTotalWords() * 4 / 1024);
        }

        const vram::Address addr = AllocateVramFor(texture, sizeWords);

        // Queued or in-flight draws may still sample VRAM the allocation just recycled; the
        // upload below would pull it out from under them.
        if (vram::HasReuseHazard())
        {
            FenceGs();
        }

        texture.vramAddr = addr;

        Com_DPrintf("VRAM: uploaded '%s' (%dx%d, %d KB)\n", texture.name,
                    texture.width, texture.height, sizeWords * 4 / 1024);
    }

    gs::UploadTexture(texture);
}

// ------------------------------------------------------------------------------------------------
// Frame lifecycle
// ------------------------------------------------------------------------------------------------

void Init()
{
    s_gsLatency    = Cvar_Get("ps2_gs_latency", "1", CVAR_ARCHIVE);
    s_enableDither = Cvar_Get("ps2_fb_dither", "0", CVAR_ARCHIVE); // the skybox looks worse with it on
    s_drawCtx      = 1;
}

void BeginFrame()
{
    PS2_AssertMsg(!s_frameStarted, "BeginFrame: frame already started!");
    s_frameStarted = true;

    // Retires and shows the previous frame when ps2_gs_latency left it drawing. Already done - by
    // EndFrame itself - when the cvar is off, and this is then the no-op that lets the two paths
    // share everything below.
    PresentFrameInFlight();

    cmdbuf::BeginFrame();

    // The clear is the first thing in the frame - a DIRECT block of GIF data at the head of it -
    // so the VU1 3D world that follows in the same buffer cannot land on an uncleared
    // framebuffer whatever the two GIF paths do.
    gs::GifWriter & clear = OpenGifBlock(kClearBlockQwords);
    gs::EmitClear(clear, s_drawCtx, s_enableDither->value != 0.0f);
    CloseGifBlock();

    // Nothing is sent here. The clear sits at the head of the buffer and goes out with the rest
    // of the frame at EndFrame - which is the whole point of the stage, and is safe precisely
    // because the buffer is ordered: the VU1 world that follows cannot reach the GS first.
    //
    // The two lines below still need the GS to be idle, and it still is: whether the previous
    // frame was fenced at its own EndFrame or left drawing until PresentFrameInFlight above, it
    // has been fenced by the time the clear is built.
    PS2_AssertMsg(!cmdbuf::KickInFlight(), "BeginFrame with a frame still drawing!");
    vram::ClearReuseHazard();
    vram::BeginFrame();
}

void EndFrame()
{
    PS2_AssertMsg(s_frameStarted, "EndFrame without BeginFrame!");
    s_frameStarted = false;

    // Close whatever 2D accumulated since the last flush (the HUD/console overlay in the common
    // case) so it lands on top before the buffer is displayed.
    Ctx().FlushPending2D();

    // Everything the frame told the GS to do has been sitting in the command buffer since
    // BeginFrame - the clear, every VU1 batch, every 2D block - and this is where all of it goes
    // out, in one kick, with one FlushCache(0).
    s_inFlightCtx = s_drawCtx;
    cmdbuf::Kick();

    // ps2_gs_latency is only about who waits for that kick. Off, this frame is fenced and shown
    // before EndFrame returns. On, it is left drawing and PresentFrameInFlight at the next
    // BeginFrame picks it up - so the GS rasterises it across the engine's own frame work instead
    // of the EE standing at the fence.
    //
    // Turning it off mid-run costs one frame: the one left drawing is fenced by the Kick above
    // and then never shown, because the line above it has already claimed s_inFlightCtx. That is
    // a duplicated field on screen, not a corrupt one, and it is not worth code to avoid.
    if (s_gsLatency->value == 0.0f)
    {
        PresentFrameInFlight();
    }

    // Rolls the command buffer's high-water and latches its counters for the overlay.
    cmdbuf::EndFrame();

    s_drawCtx ^= 1; // draw into the other buffer next frame
}

int CurrentDrawContext()
{
    return s_drawCtx;
}

int Gif2DPeakQwords()
{
    // Folds in the block still open, so a mid-frame reader (the overlay is drawn during the 2D
    // pass) reports honestly rather than only what previous blocks held.
    const int used = s_gifBlock.has_value() ? s_gifBlock->QwordCount() : 0;
    return (used > s_gifBlockPeakQwords) ? used : s_gifBlockPeakQwords;
}

} // namespace ps2::rc
