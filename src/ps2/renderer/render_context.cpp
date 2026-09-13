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
 *  3D draws (modelled on the ps2sdk "draw/vu1" sample) build one VIF1 source chain each: frame
 *  constants - MVP plus the GS screen mapping - unpacked to fixed low VU addresses, then, per
 *  chunk of up to vu1::kMaxVertsPerBatch vertices, the batch (header, GIF tags, vertices) unpacked
 *  at the current double buffer plus FLUSH + MSCAL to run the microprogram, which transforms,
 *  clips and XGKICKs the triangles to the GS over PATH1. XTOP flips on every MSCAL, so the VIF
 *  unpacks one chunk into a buffer half while the VU still transforms the previous one. No extra
 *  syncs are needed between chunks: MSCAL stalls the VIF while a program runs, and each program's
 *  XGKICK stalls until the previous kick drained, which keeps a half's output area safe from the
 *  next-but-one program until the GS is done reading it.
 *
 *  The A+D block every batch opens with programs TEST, ALPHA and ZBUF as well as TEX0/TEX1, so a
 *  batch draws with the proper z-test, blend function and depth-write mask no matter what state
 *  the surrounding 2D sections (or an earlier blended batch) left behind.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/render_context.h"
#include "ps2/debug/profile.h"
#include "ps2/renderer/render_profile.h"
#include "ps2/renderer/texture.h"

#include <cstdint>
#include <gif_tags.h>
#include <gs_gp.h>

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

// True only between Begin/EndFrame.
static bool s_frameStarted = false;

// Whether a 2D section is accumulating. Distinct from a block being open: the clear opens a block
// of its own, and a 2D section can outlive a block across a split.
static bool s_in2D = false;

// The GIF block currently open - the clear at the top of the frame, or the 2D overlay - as a
// cursor into the command buffer's own memory. Engaged only between OpenGifBlock and
// CloseGifBlock, which is also what says whether a block is open.
static std::optional<gs::GifWriter> s_gifBlock;

// High-water of what one GIF block held, banked by CloseGifBlock since the writer itself is
// rebuilt per block. Shown as "Gif2DPk" in the draw-stats overlay.
static int s_gifBlockPeakQwords = 0;

// The framebuffer the chain in flight is drawing into - what DISPFB is pointed at once it has
// been fenced. Not the same as detail::g_drawCtx once a frame is left drawing while the next one is
// built: that is the whole of what "one frame of latency" means here. Empty until the first
// frame has been kicked, which is the one frame with nothing finished to show.
static std::optional<gs::DrawContext> s_inFlightCtx;

// Background colour for the frame clear. Distinctive dark blue, so an unwritten pixel is obvious;
// ref.cpp sets black for release builds.
static u8 s_clearColor[3] = { 0x20, 0x20, 0x38 };

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
    gs::EmitBegin2D(*s_gifBlock, detail::g_drawCtx);
}

// Empties the whole pipeline: submits the frame as far as it has been built and blocks until the
// GS has finished drawing every bit of it - and anything left over from the frame before, which
// under a deferred present may still be rasterising. The frame then carries on building where it
// left off: this is a stall, not a reset, and no pointer into the buffer moves.
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
// when there is nothing outstanding, which is how the deferred and immediate paths share it.
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
    // Nothing waiting to be shown: either EndFrame already presented this frame (the immediate
    // path, where this is then the no-op at the next BeginFrame) or none has been kicked yet. The
    // early out has to come before the vsync, not after - falling through would spend a whole
    // field here and a second one at the frame's real present, halving the frame rate.
    if (!s_inFlightCtx.has_value())
    {
        return;
    }

    cmdbuf::WaitIdle(); // the GS fence; marks its own GsWait
    gs::PresentFramebuffer(*s_inFlightCtx);

    s_inFlightCtx.reset(); // shown; EndFrame is what puts the next one up
}

} // namespace

RenderContext   detail::g_context;
gs::DrawContext detail::g_drawCtx = gs::DrawContext::Ctx1; // which framebuffer this frame draws into

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
    gs::EmitFillRect(GifData(gs::kFillRectQwords), detail::g_drawCtx, x, y, width, height, r, g, b, a);
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
        gs::EmitTextureBind(GifData(gs::kTextureBindQwords), detail::g_drawCtx, bind);
    }

    gs::EmitTexturedRect(GifData(gs::kTexturedRectQwords), detail::g_drawCtx,
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

void SetClearColor(const u8 r, const u8 g, const u8 b)
{
    s_clearColor[0] = r;
    s_clearColor[1] = g;
    s_clearColor[2] = b;
}

void BeginFrame(const bool dither)
{
    PS2_AssertMsg(!s_frameStarted, "BeginFrame: frame already started!");
    s_frameStarted = true;

    // Safe here rather than a frame late: PS2_BeginFrame runs debug::FrameLogCapture, which reads
    // the finished frame's counters, before it calls this.
    Ctx().Stats() = {};

    // Retires and shows the previous frame when EndFrame left it drawing. Already done - by
    // EndFrame itself - when it presented immediately, and this is then the no-op that lets the
    // two paths share everything below.
    PresentFrameInFlight();

    cmdbuf::BeginFrame();

    // The clear is the first thing in the frame - a DIRECT block of GIF data at the head of it -
    // so the VU1 3D world that follows in the same buffer cannot land on an uncleared
    // framebuffer whatever the two GIF paths do.
    gs::GifWriter & clear = OpenGifBlock(kClearBlockQwords);
    gs::EmitClear(clear, detail::g_drawCtx, s_clearColor, dither);
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

void EndFrame(const bool deferPresent)
{
    PS2_AssertMsg(s_frameStarted, "EndFrame without BeginFrame!");
    s_frameStarted = false;

    // Close whatever 2D accumulated since the last flush (the HUD/console overlay in the common
    // case) so it lands on top before the buffer is displayed.
    Ctx().FlushPending2D();

    // Everything the frame told the GS to do has been sitting in the command buffer since
    // BeginFrame - the clear, every VU1 batch, every 2D block - and this is where all of it goes
    // out, in one kick, with one FlushCache(0).
    s_inFlightCtx = detail::g_drawCtx;
    cmdbuf::Kick();

    // 'deferPresent' is only about who waits for that kick. Clear, this frame is fenced and
    // shown before EndFrame returns. Set, it is left drawing and PresentFrameInFlight at the next
    // BeginFrame picks it up - so the GS rasterises it across the engine's own frame work instead
    // of the EE standing at the fence.
    //
    // Clearing it mid-run costs one frame: the one left drawing is fenced by the Kick above and
    // then never shown, because the line above it has already claimed s_inFlightCtx. That is a
    // duplicated field on screen, not a corrupt one, and it is not worth code to avoid.
    if (!deferPresent)
    {
        PresentFrameInFlight();
    }

    // Rolls the command buffer's high-water and latches its counters for the overlay.
    cmdbuf::EndFrame();

    detail::g_drawCtx = gs::Other(detail::g_drawCtx); // draw into the other buffer next frame
}

int Gif2DPeakQwords()
{
    // Folds in the block still open, so a mid-frame reader (the overlay is drawn during the 2D
    // pass) reports honestly rather than only what previous blocks held.
    const int used = s_gifBlock.has_value() ? s_gifBlock->QwordCount() : 0;
    return (used > s_gifBlockPeakQwords) ? used : s_gifBlockPeakQwords;
}

// ------------------------------------------------------------------------------------------------
// 3D draws
// ------------------------------------------------------------------------------------------------

namespace {

// The frame's lights, as the microprogram reads them. SetDynamicLights builds this once a frame
// and every draw's opening copies it into the buffer - see BeginDrawChain.
static vu1::LightConstants s_lightConstants;

// Which blend equation the batch's ALPHA register gets. The flags select
// alternative equations, they are not switches to combine - each one brings the
// prim's ABE bit and the depth-write mask with it - so this also asserts that.
//
// DynamicLights over Modulate is the lit lightmap pass: the modulate scales the
// framebuffer by the luxel intensity as usual, and the D term adds the lit
// program's computed colour on top. Cs is exactly that colour, because the atlas
// texel is an alpha-ramp CLUT entry whose RGB is pinned at the modulate identity
// (Ct * Cv >> 7 == Cv) and As is untouched, still the luxel intensity.
inline gs::BlendMode BlendModeFor(DrawFlags flags)
{
    const int blendModes = static_cast<int>(HasDrawFlag(flags, DrawFlags::Blended))
                         + static_cast<int>(HasDrawFlag(flags, DrawFlags::Additive))
                         + static_cast<int>(HasDrawFlag(flags, DrawFlags::Modulate));
    PS2_AssertMsg(blendModes <= 1,
                  "Pick one blend mode - Blended, Additive and Modulate are exclusive!");

    if (HasDrawFlag(flags, DrawFlags::Additive))
    {
        return gs::BlendMode::Additive;
    }
    if (HasDrawFlag(flags, DrawFlags::Modulate))
    {
        return HasDrawFlag(flags, DrawFlags::DynamicLights) ? gs::BlendMode::ModulateAdd
                                                            : gs::BlendMode::Modulate;
    }
    return gs::BlendMode::Blend; // what an opaque batch writes too; ABE is off for it
}

// The GS z conversion for a batch, as the (offset, scale) pair the microprogram
// applies to NDC z. The unhacked pair is the mapping described on vu1::kGsDepthScale;
// a hacked one squeezes NDC z into [1 - 2s, 1] before it, i.e.
//
//     Z = 16 * kGsDepthScale * (1 + (s * ndcZ + (1 - s)))
//       = 16 * (kGsDepthScale * (2 - s) + ndcZ * kGsDepthScale * s)
//
// leaving the nearest s of the z-buffer to the batch and costing the
// microprogram nothing - it multiplies and adds these either way.
inline void DepthRangeFor(DrawFlags flags, float * outScale, float * outOffset)
{
    const float s = HasDrawFlag(flags, DrawFlags::DepthHack) ? vu1::kDepthHackScale : 1.0f;
    *outScale  = vu1::kGsDepthScale * s;
    *outOffset = vu1::kGsDepthScale * (2.0f - s);
}

// Emits the 6 qwords of state every batch opens with: the GIF tag announcing
// five A+D register writes, then TEST, TEX1, TEX0, ALPHA and ZBUF for this
// context. Shared by the triangle and particle paths, which differ only in the
// seventh qword - the drawing tag - that each appends afterwards.
//
// Returns whether the batch blends, since the drawing tag needs it for the
// prim's ABE bit and it is decided here.
bool AddBatchStateBlock(RenderContext & ctx, const tex::Texture & texture,
                        gs::DrawContext drawCtx, DrawFlags flags)
{
    const gs::BlendMode blendMode = BlendModeFor(flags);

    // A blend mode was asked for (any of the three flags), as opposed to the equation every
    // batch writes: that is what turns the ABE bit on and masks depth writes.
    const bool blended = HasDrawFlag(flags, DrawFlags::Blended)
                      || HasDrawFlag(flags, DrawFlags::Additive)
                      || HasDrawFlag(flags, DrawFlags::Modulate);

    // Five A+D register writes: pixel tests, the texture bind, the blend
    // function and the depth-write mask for this context...
    ctx.AddQword(GIF_SET_TAG(5, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    ctx.AddQword(gs::MakePixelTests(), gs::ContextReg(GS_REG_TEST, drawCtx));
    ctx.AddQword(gs::MakeTex1(texture), gs::ContextReg(GS_REG_TEX1, drawCtx));
    ctx.AddQword(gs::MakeTex0(texture, tex::TakesIntensity(texture.type)),
                 gs::ContextReg(GS_REG_TEX0, drawCtx));
    ctx.AddQword(gs::MakeAlphaBlend(blendMode), gs::ContextReg(GS_REG_ALPHA, drawCtx));
    ctx.AddQword(gs::MakeZBuf(blended || HasDrawFlag(flags, DrawFlags::NoDepthWrite)),
                 gs::ContextReg(GS_REG_ZBUF, drawCtx));

    return blended;
}

// Emits the batch's 7 GIF tag qwords into an open inline unpack: the A+D
// state block and the drawing tag for 'vertCount' vertices. Blended batches
// turn the prim's ABE bit on and mask depth writes; NoDepthWrite masks them
// without the ABE bit; untextured ones clear the TME bit (the texture
// registers are still written, just not sampled).
void AddBatchGifTags(RenderContext & ctx, const tex::Texture & texture, gs::DrawContext drawCtx,
                     int vertCount, DrawFlags flags, bool packedRgbaOut = false)
{
    const bool blended = AddBatchStateBlock(ctx, texture, drawCtx, flags);
    const int  tme     = HasDrawFlag(flags, DrawFlags::Untextured) ? 0 : 1;
    const int  abe     = blended ? 1 : 0;

    // ...then the drawing tag: gouraud triangle list, STQ mapping, with the
    // per-vertex registers of vu1::kVertexRegList.
    //
    // Built with the gif_tags.h macros, not packet2_utils.h's VU_GS_PRIM /
    // VU_GS_GIFTAG: those do not parenthesize their parameters, so an
    // argument that is an expression silently mis-assembles. Passing
    // 'blended ? 1 : 0' for ABE expanded to '(blended ? 1 : 0 << 6)', which
    // parses as 'blended ? 1 : (0 << 6)' and drops the bit at position 0 -
    // inside the PRIM field, where PRIM_TRIANGLE (3) already has that bit
    // set. Nothing warned and the primitive still drew, just never blended.
    const u64 prim = GIF_SET_PRIM(PRIM_TRIANGLE, 1, tme, 0, abe, 0, 0, gs::Index(drawCtx), 0);
    // The programs that *compute* their color emit PACKED RGBAQ; the ones that
    // receive it already packed emit an A+D write. The register list has to
    // follow whichever this batch will run, so the caller says which.
    const bool packedRgba = packedRgbaOut || HasDrawFlag(flags, DrawFlags::DynamicLights);
    ctx.AddQword(GIF_SET_TAG(vertCount, 1, 1, prim, GIF_FLG_PACKED, 3),
                 packedRgba ? vu1::kLitVertexRegList : vu1::kVertexRegList);
}

// Builds the draw's transform and light blocks into the buffer and unpacks them to the
// fixed low VU addresses. Both are buffer payload rather than statics - see vu1::FrameConstants.
//
// kDrawSetupQwords is exactly what this appends, and ReserveChunk has already reserved it.
void BeginDrawChain(RenderContext & ctx, const math::Mat4 & mvp, DrawFlags flags)
{
    // Every chunk of a draw shares one flags value, so the batch's depth range
    // is a property of the whole chain and rides with the other constants.
    float depthScale, depthOffset;
    DepthRangeFor(flags, &depthScale, &depthOffset);

    constexpr int kFrameConstantsQwords = sizeof(vu1::FrameConstants) / 16;
    constexpr int kLightConstantsQwords = sizeof(vu1::LightConstants) / 16;
    // Each block costs what cmdbuf::CalcAllocCost says - its payload plus the skip tag - and the
    // REF tag that sends it, and the FLUSH below is the one qword on top.
    static_assert(kDrawSetupQwords == 1 + cmdbuf::CalcAllocCost<vu1::FrameConstants>(1)
                                        + cmdbuf::CalcAllocCost<vu1::LightConstants>(1) + 2,
                  "kDrawSetupQwords must match what BeginDrawChain appends");

    // Both unpacks below write absolute VU addresses, which the double buffer does not
    // protect, and the previous draw's last chunk is very likely still running: wait for it.
    // See kDrawSetupQwords.
    ctx.AddFlush();

    vu1::FrameConstants * const constants = cmdbuf::Alloc<vu1::FrameConstants>(1);

    constants->mvp        = mvp;
    constants->gsScale    = { 2048.0f, 2048.0f, depthScale, 0.0f };
    constants->gsOffset   = { 2048.0f + static_cast<float>(gs::Width())  * 0.5f,
                              2048.0f + static_cast<float>(gs::Height()) * 0.5f,
                              depthOffset, 0.0f };
    constants->clipScale  = vu1::kClipScale;
    constants->colorClamp = vu1::kColorClamp;

    ctx.AddUnpackData(vu1::kFrameConstantsAddr, constants, kFrameConstantsQwords, false);

    // s_lightConstants stays the source of truth - SetDynamicLights builds it once a frame -
    // and the buffer gets a copy, for the same lifetime reason as the transform block.
    vu1::LightConstants * const lights = cmdbuf::Alloc<vu1::LightConstants>(1);
    *lights = s_lightConstants;

    ctx.AddUnpackData(vu1::kLightBlockAddr, lights, kLightConstantsQwords, false);
}

// Makes room in the buffer for one chunk and (re)opens the draw's chain when it
// has to - the whole of the bookkeeping the three draw paths share.
//
// The constants block is reserved with every chunk rather than once, because a
// reservation that overflows drains the buffer and rewinds it: the constants go
// with it, and the chunk that follows would otherwise transform against whatever
// the previous draw happened to leave in VU memory. 'firstChunk' opens it for
// the same reason at the top of a call, where nothing has emitted it yet.
void ReserveChunk(RenderContext & ctx, const int chunkQwords, const math::Mat4 & mvp,
                  const DrawFlags flags, const bool firstChunk)
{
    if (cmdbuf::Reserve(kDrawSetupQwords + chunkQwords) || firstChunk)
    {
        BeginDrawChain(ctx, mvp, flags);
    }
}

// Emits one chunk into the buffer: batch header and GIF tags unpacked inline
// to the current double buffer, the vertex data referenced in place, and the
// MSCAL that runs the microprogram over it.
void AddBatchChunk(RenderContext & ctx, const tex::Texture & texture, gs::DrawContext drawCtx,
                   const vu1::DrawVertex * verts, int vertCount, DrawFlags flags)
{
    PS2_Assert(vertCount > 0 && vertCount <= vu1::kMaxVertsPerBatch && (vertCount % 3) == 0);
    ctx.EnsureSpace(kChunkChainQwords);

    ctx.OpenInlineUnpack(vu1::kBatchHeaderAddr, true);
    {
        ctx.AddU32(0);
        ctx.AddU32(0);
        ctx.AddU32(0);
        ctx.AddU32(static_cast<u32>(vertCount));

        AddBatchGifTags(ctx, texture, drawCtx, vertCount, flags);
    }
    ctx.CloseInlineUnpack();

    ctx.AddUnpackData(vu1::kVertexDataAddr, verts, static_cast<u32>(vertCount * 2), true);

    ctx.AddStartProgram(vu1::ProgramAddress(HasDrawFlag(flags, DrawFlags::DynamicLights)
                                            ? vu1::Program::Lit : vu1::Program::Textured));
}

// The lerped equivalent: header (count + the two lerp scale vectors) and GIF
// tags inline, then the two vertex streams, then the MSCAL. The byte-position
// DMA must be whole source qwords, so an odd count transfers one pad vertex
// the VU never reads (the fixed region has room: odd counts are < the even maximum).
void AddLerpBatchChunk(RenderContext & ctx, const tex::Texture & texture, gs::DrawContext drawCtx,
                       const math::Vec3 & frontv, const math::Vec3 & backv,
                       const math::Vec4 & shadeLight, float stScaleS, float stScaleT,
                       const vu1::LerpPosChunk & posChunk, const vu1::LerpDrawAttrib * attribs,
                       int vertCount,
                       FaceCull faceCull, DrawFlags flags)
{
    PS2_Assert(vertCount > 0 && vertCount <= vu1::kMaxLerpVertsPerBatch && (vertCount % 3) == 0);
    ctx.EnsureSpace(kLerpChunkChainQwords);

    ctx.OpenInlineUnpack(vu1::kLerpBatchHeaderAddr, true);
    {
        ctx.AddU32(static_cast<u32>(faceCull)); // backface cull mode in .x
        // The skin's size over its power-of-two TEX0 extent, which the
        // microprogram multiplies onto every vertex's ST. Here rather than on
        // the EE because the VU has the multiply slot free and the EE does not:
        // it is two mul.s per vertex saved out of an expansion loop that is the
        // single largest marker in the frame.
        ctx.AddFloat(stScaleS); // .y
        ctx.AddFloat(stScaleT); // .z
        ctx.AddU32(static_cast<u32>(vertCount));

        ctx.AddFloat(frontv.x);
        ctx.AddFloat(frontv.y);
        ctx.AddFloat(frontv.z);
        ctx.AddFloat(0.0f); // .w rides through the lerp; keep it finite

        ctx.AddFloat(backv.x);
        ctx.AddFloat(backv.y);
        ctx.AddFloat(backv.z);
        ctx.AddFloat(0.0f);

        // The entity's light, which the microprogram multiplies by each vertex's
        // shade term to get its color. On the EE this was a 162-entry table
        // rebuilt per entity per frame; here it is four floats per batch. The
        // shade arrives quantized (shade * 128), so .xyz carry the light already
        // divided by 128 - see VertexShadeLight.
        ctx.AddFloat(shadeLight.x);
        ctx.AddFloat(shadeLight.y);
        ctx.AddFloat(shadeLight.z);
        ctx.AddFloat(shadeLight.w); // vertex alpha, GS units

        AddBatchGifTags(ctx, texture, drawCtx, vertCount, flags, /*packedRgbaOut=*/true);
    }
    ctx.CloseInlineUnpack();

    // The keyframe bytes: V4_8 elements, one source word and two destination
    // qwords per vertex, padded to an even vertex count so the transfer is
    // whole qwords (every word the DMA carries must be unpack payload).
    const int srcVerts = vertCount + (vertCount & 1);
    ctx.AddUnpackDataFmt(vu1::kLerpPositionsAddr, posChunk.pos,
                         static_cast<u32>(srcVerts / 2), // qwords: 8 bytes per vertex
                         static_cast<u32>(srcVerts * 2), // elements: 2 per vertex
                         P2_UNPACK_V4_8, true);

    // Referenced in the model hunk rather than in the buffer: this is the stream the
    // EE no longer gathers at all. One REF tag either way - the DMAC does not care
    // which side of the bus the qwords came from, and nothing rewrites a model.
    ctx.AddUnpackData(vu1::kLerpAttribsAddr, attribs, static_cast<u32>(vertCount), true);

    ctx.AddStartProgram(vu1::ProgramAddress(vu1::Program::Lerped));
}

// Emits one particle chunk: the header, the batch constants and the GIF tags
// unpacked inline, the particles referenced in place, and the MSCAL.
//
// 'clipOffset' is the corner offset already transformed to clip space; the UVs
// are in the GS 12.4 fixed point the PACKED UV descriptor wants.
void AddParticleChunk(RenderContext & ctx, const tex::Texture & texture, gs::DrawContext drawCtx,
                      const math::Vec4 & clipOffset, u32 uvMaxU, u32 uvMaxV,
                      const vu1::ParticleVertex * particles, int count, DrawFlags flags)
{
    PS2_Assert(count > 0 && count <= vu1::kMaxParticlesPerBatch);
    ctx.EnsureSpace(kParticleChunkQwords);

    ctx.OpenInlineUnpack(vu1::kPrtBatchHeaderAddr, true);
    {
        ctx.AddU32(0);
        ctx.AddU32(0);
        ctx.AddU32(0);
        ctx.AddU32(static_cast<u32>(count));

        // The corner offset, with the distance blow-up rate riding in the .w the
        // offset itself has no use for (it is a direction, so its w is zero).
        ctx.AddFloat(clipOffset.x);
        ctx.AddFloat(clipOffset.y);
        ctx.AddFloat(clipOffset.z);
        ctx.AddFloat(vu1::kParticleBlowUpRate);

        // The two corner UVs. PACKED UV takes U in word 0 and V in word 1; the
        // upper half of the qword is not part of the descriptor.
        ctx.AddU32(0);
        ctx.AddU32(0);
        ctx.AddU32(0);
        ctx.AddU32(0);

        ctx.AddU32(uvMaxU);
        ctx.AddU32(uvMaxV);
        ctx.AddU32(0);
        ctx.AddU32(0);

        const bool blended = AddBatchStateBlock(ctx, texture, drawCtx, flags);
        const int  abe     = blended ? 1 : 0; // Hoisted: see the note in AddBatchGifTags.

        // The drawing tag: one sprite per particle, five registers each - the
        // A+D that sets its colour, then a UV/XYZ2 pair per corner. FST selects
        // UV over ST: a screen-aligned sprite needs no perspective correction.
        const u64 prim = GIF_SET_PRIM(PRIM_SPRITE, 0, 1, 0, abe, 0, 1, gs::Index(drawCtx), 0);
        ctx.AddQword(GIF_SET_TAG(count, 1, 1, prim, GIF_FLG_PACKED, 5), vu1::kParticleRegList);
    }
    ctx.CloseInlineUnpack();

    ctx.AddUnpackData(vu1::kPrtDataAddr, particles, static_cast<u32>(count), true);

    ctx.AddStartProgram(vu1::ProgramAddress(vu1::Program::Particles));
}

} // namespace

void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const vu1::DrawVertex * verts, int vertCount, DrawFlags flags)
{
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(verts) & 15u) == 0, "Vertex data must be 16-byte aligned!");

    RenderContext & ctx = Ctx();

    // Send any 2D accumulated before this 3D burst so it draws underneath (and
    // its textures are consumed before our uploads can evict them). A no-op once
    // the section is already flushed - only the first 3D draw after 2D pays it.
    ctx.FlushPending2D();

    EnsureTextureResident(texture);

    const gs::DrawContext drawCtx = CurrentDrawContext();

    // One chunk per VU run; the double buffer overlaps each chunk's unpack
    // with the previous chunk's transform.
    for (int firstVert = 0; firstVert < vertCount; firstVert += vu1::kMaxVertsPerBatch)
    {
        ReserveChunk(ctx, kChunkChainQwords, mvp, flags, /*firstChunk=*/firstVert == 0);

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < vu1::kMaxVertsPerBatch) ? remaining : vu1::kMaxVertsPerBatch;
        AddBatchChunk(ctx, texture, drawCtx, verts + firstVert, chunkVerts, flags);
    }
}

void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight,
                         const vu1::LerpPosChunk * posChunks, const vu1::LerpDrawAttrib * attribs,
                         int vertCount, FaceCull faceCull, DrawFlags flags)
{
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawLerpedTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(posChunks) & 15u) == 0, "Position chunks must be 16-byte aligned!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(attribs) & 15u) == 0, "Attribute stream must be 16-byte aligned!");

    RenderContext & ctx = Ctx();
    ctx.FlushPending2D();

    EnsureTextureResident(texture);

    // A property of the texture, so it is resolved here rather than threaded
    // down from every caller; StScaleFor is pure arithmetic on its dimensions.
    float stScaleS, stScaleT;
    tex::StScaleFor(texture, &stScaleS, &stScaleT);

    const gs::DrawContext drawCtx = CurrentDrawContext();

    // Chunking as in DrawTriangles. The positions are already grouped this way -
    // one LerpPosChunk is one VU run - and the attributes are simply sliced at the
    // same boundary, which works because the caller gathered the positions from the
    // attribute array in order. Only a final odd chunk pads its position transfer
    // (see AddLerpBatchChunk).
    for (int firstVert = 0, c = 0; firstVert < vertCount; firstVert += vu1::kMaxLerpVertsPerBatch, ++c)
    {
        ReserveChunk(ctx, kLerpChunkChainQwords, mvp, flags, /*firstChunk=*/firstVert == 0);

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < vu1::kMaxLerpVertsPerBatch) ? remaining : vu1::kMaxLerpVertsPerBatch;

        AddLerpBatchChunk(ctx, texture, drawCtx, frontv, backv, shadeLight, stScaleS, stScaleT,
                          posChunks[c], attribs + firstVert, chunkVerts, faceCull, flags);
    }
}

void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const vu1::ParticleVertex * particles,
                   int count, DrawFlags flags)
{
    PS2_AssertMsg(count > 0, "DrawParticles wants at least one particle!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(particles) & 15u) == 0, "Particle data must be 16-byte aligned!");

    RenderContext & ctx = Ctx();
    ctx.FlushPending2D();

    EnsureTextureResident(texture);

    // The corner offset transforms once for the whole call, as a direction
    // (w = 0). Because it is orthogonal to the view axis its clip z and w both
    // come out zero, which is what lets the microprogram reuse the centre's
    // depth and 1/w for both corners - see particles.vcl.
    const math::Vec4 clipOffset = math::Transform(
        math::Vec4{ quadOffset.x, quadOffset.y, quadOffset.z, 0.0f }, mvp);

    // Corner UVs in the GS 12.4 fixed point, spanning the whole image.
    // Particle images are power-of-two, so no ST rescale applies here.
    const u32 uvMaxU = static_cast<u32>(texture.width)  << 4;
    const u32 uvMaxV = static_cast<u32>(texture.height) << 4;

    const gs::DrawContext drawCtx = CurrentDrawContext();

    for (int first = 0; first < count; first += vu1::kMaxParticlesPerBatch)
    {
        ReserveChunk(ctx, kParticleChunkQwords, mvp, flags, /*firstChunk=*/first == 0);

        const int remaining  = count - first;
        const int chunkCount = (remaining < vu1::kMaxParticlesPerBatch) ? remaining : vu1::kMaxParticlesPerBatch;
        AddParticleChunk(ctx, texture, drawCtx, clipOffset, uvMaxU, uvMaxV,
                         particles + first, chunkCount, flags);
    }
}

void SetDynamicLights(const vu1::DynamicLight * lights, const int count)
{
    // Zeroed slots cost the microprogram nothing to evaluate: colour 0 and
    // -(colour/r^2) 0 make the whole term max(0 * d + 0, 0) = 0, so there is no
    // branch and no separate "how many lights" path.
    s_lightConstants = {};
    s_lightConstants.clamp = { 255.0f, 255.0f, 255.0f, vu1::kLitVertexAlpha };

    const int used = (count < vu1::kMaxDynamicLights) ? count : vu1::kMaxDynamicLights;
    PS2_Assert(used >= 0 && (used == 0 || lights != nullptr));

    // Transposed: one light per SIMD lane rather than one axis per lane, which
    // is what lets the microprogram do all four at once.
    float px[vu1::kMaxDynamicLights] = {};
    float py[vu1::kMaxDynamicLights] = {};
    float pz[vu1::kMaxDynamicLights] = {};

    for (int i = 0; i < used; ++i)
    {
        const vu1::DynamicLight & l = lights[i];

        // A zero or negative radius has no inside, and would divide by zero
        // below; leave the slot dark.
        if (l.radius <= 0.0f)
        {
            continue;
        }

        px[i] = l.origin.x;
        py[i] = l.origin.y;
        pz[i] = l.origin.z;

        // Pre-scaled to the GS 0-255 range and pre-divided by the radius
        // squared. Doing both here is what reduces the VU's attenuation to a
        // single multiply-add - the microprogram never divides and never takes
        // a square root.
        const float scale = 255.0f;
        const float invR2 = 1.0f / (l.radius * l.radius);

        s_lightConstants.color[i] = { l.color.x * scale, l.color.y * scale, l.color.z * scale, 0.0f };
        s_lightConstants.negColorDivR2[i] = { -s_lightConstants.color[i].x * invR2,
                                              -s_lightConstants.color[i].y * invR2,
                                              -s_lightConstants.color[i].z * invR2, 0.0f };
    }

    s_lightConstants.posX = { px[0], px[1], px[2], px[3] };
    s_lightConstants.posY = { py[0], py[1], py[2], py[3] };
    s_lightConstants.posZ = { pz[0], pz[1], pz[2], pz[3] };
}

// ------------------------------------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------------------------------------

vu1::ParticleVertex * RenderContext::BeginParticles(const int count)
{
    PS2_AssertMsg(count > 0, "BeginParticles with nothing to draw!");
    PS2_AssertMsg(m_particles == nullptr, "BeginParticles without an EndParticles!");

    // Taking the buffer is the 2D->3D boundary: a pending 2D section holds an open DMA tag and an
    // allocation cannot land inside one. The reservation covers the chunks the draw appends on top
    // as well, because those must not drain the buffer out from under the span they reference.
    FlushPending2D();
    cmdbuf::Reserve(cmdbuf::CalcAllocCost<vu1::ParticleVertex>(count) + DrawParticlesChainCost(count));

    m_particles     = cmdbuf::Alloc<vu1::ParticleVertex>(count);
    m_particleCount = count;
    return m_particles;
}

void RenderContext::EndParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                                 const math::Vec3 & quadOffset, const DrawFlags flags)
{
    PS2_AssertMsg(m_particles != nullptr, "EndParticles without a BeginParticles!");

    ++m_stats.drawBatches;
    m_stats.particles += m_particleCount;

    DrawParticles(mvp, texture, quadOffset, m_particles, m_particleCount, flags);

    m_particles     = nullptr;
    m_particleCount = 0;
}

} // namespace ps2::rc
