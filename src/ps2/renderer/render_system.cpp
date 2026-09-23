/* ================================================================================================
 * File: render_system.cpp
 * Brief: The render system and the frame it records. See render_system.h.
 *
 *  Frame structure: BeginFrame rewinds the command buffer and writes the colour+depth clear at
 *  the head of it. 2D and 3D then draw in any order into that same buffer, 2D accumulating in a
 *  deferred DIRECT section (always-pass z-test, so it lands on top) that is closed at each
 *  2D->3D boundary and once more by EndFrame. Nothing is sent until then: one chain, one kick.
 *  Ordering is the buffer's own plus the VIF FLUSH each section opens with; where the GS must
 *  have actually finished, FenceGs submits what is built and waits.
 *
 *  3D draws (modelled on the ps2sdk "draw/vu1" sample) each build one VIF1 source chain: the
 *  frame constants unpacked to fixed low VU addresses, then per chunk of up to
 *  vu1::kMaxVertsPerBatch vertices the batch header, GIF tags and vertices unpacked at the
 *  current double buffer, plus FLUSH + MSCAL to run the microprogram. XTOP flips on every MSCAL,
 *  so the VIF unpacks one chunk while the VU still transforms the previous one, and the chunks
 *  need no syncing between them: MSCAL stalls the VIF while a program runs, and each program's
 *  XGKICK stalls until the previous one drained.
 *
 *  The A+D block every batch opens with programs TEST, ALPHA and ZBUF as well as TEX0/TEX1, so a
 *  batch draws correctly whatever the surrounding 2D sections left behind.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/render_system.h"
#include "ps2/debug/profile.h"
#include "ps2/debug/pipeline_dump.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/texture.h"

#include <cstdint>
#include <optional>
#include <gif_tags.h>
#include <gs_gp.h>
#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_utils.h>
#include <packet2_vif.h>
#include <vif_registers.h> // VIF1_TOPS

namespace ps2::rs {
namespace {

// ------------------------------------------------------------------------------------------------
// Render System state/constants
// ------------------------------------------------------------------------------------------------

// Room every GIF block keeps back for the EOP terminator CloseGifBlock writes.
constexpr int kBlockTailQwords = 1;

// What a block must be able to take before it may be split - what the command buffer is asked to
// reserve when one opens. The clear knows its whole size; the 2D overlay does not (an empty HUD is
// a handful of qwords, a full console thousands), so it takes what is left of the half and GifData
// splits it, and this is only the floor one more primitive needs.
constexpr int kClearBlockQwords = gs::kClearQwords;
constexpr int k2DBlockMinQwords = 64 + kBlockTailQwords;

// What opening a DIRECT block costs on top of its payload: the CNT tag, whose own qword carries
// the two VIFcodes as well.
constexpr int kDirectOverheadQwords = 1;

// True only between Begin/EndFrame.
static bool s_frameStarted = false;

// The context this frame draws into, swapped by EndFrame. Every batch's registers and prim tag
// name it.
static gs::DrawContext s_drawCtx = gs::DrawContext::Ctx1;

// Whether a 2D section is accumulating. Distinct from a block being open: the clear opens a block
// of its own, and a 2D section can outlive a block across a split.
static bool s_in2D = false;

// The GIF block currently open - the frame clear, or the 2D overlay - as a cursor into the
// command buffer. Engaged only between OpenGifBlock and CloseGifBlock, which is also what says
// whether a block is open at all.
static std::optional<gs::GifWriter> s_gifBlock;

// High-water of what one block held, banked by CloseGifBlock since the writer is rebuilt per
// block.
static int s_gifBlockPeakQwords = 0;

// The framebuffer the chain in flight is drawing into - what DISPFB is pointed at once it has
// been fenced. Differs from s_drawCtx exactly when a frame is left drawing while the next is
// built, which is what "one frame of latency" means here. Empty until the first kick.
static std::optional<gs::DrawContext> s_inFlightCtx;

// Background colour for the frame clear. Distinctive dark blue, so an unwritten pixel is obvious.
static u8 s_clearColor[3] = { 0x20, 0x20, 0x38 };

// ------------------------------------------------------------------------------------------------
// The recorder: DMA tags, VIFcodes and the DIRECT blocks carrying raw GIF data. Internal - the
// world reaches the chain through the draws, the streams and the 2D primitives.
// ------------------------------------------------------------------------------------------------

// The half being recorded into. Inline, and cmdbuf caches it, so this costs one load.
Q_ALWAYS_INLINE packet2_t * Packet()
{
    return cmdbuf::Packet();
}

// Halts if the next emission would overrun the half; 'qwords' is a safe upper bound for it
// (DEBUG ONLY). The backstop, not the mechanism - a draw too large for one half is
// cmdbuf::Reserve's job. This catches a chunk emitter writing more than it declares.
Q_ALWAYS_INLINE void EnsureSpace([[maybe_unused]] const int qwords)
{
#if PS2_QUAKE_ASSERTS
    if (cmdbuf::QwordCount() + qwords > cmdbuf::QwordCapacity()) [[unlikely]]
    {
        Sys_Error("Command buffer overflow: %d qwords in use + %d needed exceeds the "
                  "%d a half can hold.", cmdbuf::QwordCount(), qwords, cmdbuf::QwordCapacity());
    }
#endif // PS2_QUAKE_ASSERTS
}

// FLUSH + MSCAL: waits for any previous run, then starts the microprogram at 'prog'.
Q_ALWAYS_INLINE void AddStartProgram(const vu1::ProgramAddr prog)
{
    packet2_utils_vu_add_start_program(Packet(), static_cast<u32>(prog));
}

// General form for the packed VIF formats, where transfer length and unpack length differ.
// 'srcQwords' is what the REF tag carries; 'numElements' is the VIFcode NUM field, the elements
// *written* to VU memory (one qword each for the V4 formats, 256 max).
//
// The transfer must hold exactly the payload the unpack consumes - a V4_8 element eats one source
// word, so numElements must be 4 * srcQwords. Spare words decode as VIFcodes, and a short transfer
// stalls the VIF waiting for payload that never comes.
//
// 'writeLen'/'cycleLen' are STCYCL's WL and CL. Equal, the elements land in consecutive qwords;
// WL < CL writes WL of them and then skips CL - WL qwords, which is how a lerp chunk's two streams
// interleave into one vertex (see AddLerpBatchChunk). NUM counts only what is written in that
// mode, so 'numElements' means the same either way.
void AddUnpackDataFmt(const u32 vuAddr, const void * data, const u32 srcQwords,
                      const u32 numElements, const enum UnpackMode format, const bool useTop,
                      const u32 writeLen = 1, const u32 cycleLen = 1)
{
    PS2_AssertMsg(numElements <= 256, "VIF unpacks are limited to 256 elements!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(data) & 15u) == 0,
                  "Unpack data must be 16-byte aligned!");
    PS2_AssertMsg(writeLen > 0 && writeLen <= cycleLen, "Filling write is not supported here!");

    packet2_t * const pkt = Packet();
    packet2_chain_ref(pkt, data, srcQwords, 0, 0, 0);
    packet2_vif_stcycl(pkt, writeLen, cycleLen, 0);
    packet2_vif_open_unpack(pkt, format, vuAddr, useTop, /*masked=*/0, /*usigned=*/1, 0);
    packet2_vif_close_unpack_manual(pkt, numElements);
}

// References 'data' in place (REF tag) and unpacks it to VU memory at 'vuAddr' (a qword address,
// relative to the current double buffer when 'useTop'). 16-byte aligned, valid until the frame's
// kick, at most 256 qwords.
Q_ALWAYS_INLINE void AddUnpackData(const u32 vuAddr, const void * data, const u32 qwords, const bool useTop)
{
    AddUnpackDataFmt(vuAddr, data, qwords, qwords, P2_UNPACK_V4_32, useTop);
}

// A VIF FLUSH of its own: stalls VIF1 until the running microprogram has ended and its XGKICKs
// have reached the GS. One qword, the CNT tag carrying FLUSH + NOP with a zero QWC.
//
// Needed in front of anything writing VU memory at an *absolute* address, which the double buffer
// does not protect. Per-chunk unpacks do not need it - the MSCAL after each carries one anyway.
void AddFlush()
{
    packet2_t * const pkt = Packet();
    packet2_chain_open_cnt(pkt, 0, 0, 0);
    packet2_vif_flush(pkt, 0);
    packet2_vif_nop(pkt, 0);
    packet2_chain_close_tag(pkt);
}

// Small unpacks built directly into the chain: open, append qwords, close.
Q_ALWAYS_INLINE void OpenInlineUnpack(const u32 vuAddr, const bool useTop)
{
    packet2_utils_vu_open_unpack(Packet(), vuAddr, useTop);
}

Q_ALWAYS_INLINE void CloseInlineUnpack()
{
    packet2_utils_vu_close_unpack(Packet());
}

Q_ALWAYS_INLINE void AddQword(const u64 lo, const u64 hi)
{
    packet2_add_2x_s64(Packet(), static_cast<s64>(lo), static_cast<s64>(hi));
}

Q_ALWAYS_INLINE void AddFloat(const float value)
{
    packet2_add_float(Packet(), value);
}

Q_ALWAYS_INLINE void AddU32(const u32 value)
{
    packet2_add_u32(Packet(), value);
}

// Opens a DIRECT transfer: everything written until CloseDirect reaches the GIF verbatim as GIF
// tags and register data - the frame clear and the 2D overlay.
//
// The leading FLUSH keeps a block opened after a batch from interleaving with PATH1 at the GIF,
// and costs nothing when no VU work is outstanding, which is why it is unconditional. It and
// DIRECT ride the CNT tag's own qword (tte=1), so the opening is one qword and the payload starts
// on the next - which is what makes CloseDirect's count come out right.
void OpenDirect()
{
    packet2_t * const pkt = Packet();
    packet2_chain_open_cnt(pkt, 0, 0, 0);
    packet2_vif_flush(pkt, 0);
    packet2_vif_open_direct(pkt, 0);
}

// Patches the DIRECT VIFcode's qword count and the CNT tag's QWC from where the cursor ended up.
void CloseDirect()
{
    packet2_t * const pkt = Packet();
    const vif_code_t * const code = pkt->vif_code_opened_at;
    PS2_AssertMsg(code != nullptr, "CloseDirect with no DIRECT block open!");

    // The payload starts at the qword boundary just past the VIFcode's own word.
    const std::uintptr_t payload = reinterpret_cast<std::uintptr_t>(code) + sizeof(u32);
    const u32 qwords = static_cast<u32>(
        (reinterpret_cast<std::uintptr_t>(pkt->next) - payload) >> 4);

    // An empty DIRECT is not a no-op: the count is a 16-bit immediate and zero means 65536
    // qwords, so the VIF would swallow the rest of the chain as GIF data.
    PS2_AssertMsg(qwords > 0 && qwords <= 0xFFFFu,
                  "CloseDirect on an empty or oversized block - a DIRECT carries 1..65535 qwords!");

    packet2_vif_close_direct_manual(pkt, qwords);
    packet2_chain_close_tag(pkt);
}

// The raw write cursor inside an open DIRECT block, and the way to hand back where a writer left
// it - for payload built by something that takes a qword_t * of its own (a GifWriter).
qword_t * DirectCursor()
{
    PS2_AssertMsg(Packet()->vif_code_opened_at != nullptr,
                  "DirectCursor with no DIRECT block open!");
    return Packet()->next;
}

void SetDirectCursor(qword_t * const cursor)
{
    packet2_t * const pkt = Packet();
    PS2_AssertMsg(pkt->vif_code_opened_at != nullptr,
                  "SetDirectCursor with no DIRECT block open!");
    PS2_AssertMsg(cursor >= pkt->next && (cursor - pkt->base) <= cmdbuf::QwordCapacity(),
                  "SetDirectCursor past the end of the chain half!");
    pkt->next = cursor;
}

// ------------------------------------------------------------------------------------------------
// GIF sections
// ------------------------------------------------------------------------------------------------

// Opens a DIRECT block and points a GIF writer at its payload.
//
// 'minQwords' is what the caller must be able to write before the block is closed and another
// opened. Reserving it may drain and rewind, which is safe at every call site for one reason: a
// block only ever opens where no span into the buffer is live.
//
// The block's capacity is everything left in the half rather than what was reserved, because the
// 2D overlay's real size is not knowable up front. Reserve is the floor, GifData the ceiling.
gs::GifWriter & OpenGifBlock(const int minQwords)
{
    PS2_AssertMsg(!s_gifBlock.has_value(), "A GIF block is already open!");

    cmdbuf::Reserve(minQwords + kDirectOverheadQwords);

    OpenDirect();

    const int capacity = cmdbuf::QwordCapacity() - cmdbuf::QwordCount();
    PS2_Assert(capacity >= minQwords);

    return s_gifBlock.emplace(DirectCursor(), capacity);
}

// Closes the open block, handing back the cursor the emitters advanced. Does not submit - on a
// split the next block simply follows this one in the same chain.
void CloseGifBlock()
{
    PS2_AssertMsg(s_gifBlock.has_value(), "No GIF block open!");

    s_gifBlock->EndGifPacket();

    SetDirectCursor(s_gifBlock->Cursor());
    CloseDirect();

    const int used = s_gifBlock->QwordCount();
    if (used > s_gifBlockPeakQwords) { s_gifBlockPeakQwords = used; }

    s_gifBlock.reset();
}

// GifData's cold half: the open block cannot take 'qwords', so close it and open another. Out of
// line so the emitters pay only the compare. Invisible to what is being drawn - a section's state
// lives in the GS's registers, not in the block, so the new one needs no re-arming.
gs::GifWriter & SplitGifBlock(const int qwords)
{
    CloseGifBlock();
    return OpenGifBlock(qwords + kBlockTailQwords);
}

// Room for 'qwords' of GIF data in the open section, handing back the writer to put it in.
//
// **The writer is only good until the next call.** A split replaces it, and so does anything that
// fences the GS, so take it again after either rather than holding it.
Q_ALWAYS_INLINE gs::GifWriter & GifData(const int qwords)
{
    PS2_AssertMsg(s_gifBlock.has_value(), "GIF emission with no block open!");

    // Plus the tail, so the EOP terminator can never be what overruns the block.
    if (s_gifBlock->QwordCount() + qwords + kBlockTailQwords <= s_gifBlock->QwordCapacity()) [[likely]]
    {
        return *s_gifBlock;
    }
    return SplitGifBlock(qwords);
}

// Opens the 2D section on demand: the first primitive after a flush lands here.
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

// Empties the pipeline: submits the frame as far as it is built and blocks until the GS has drawn
// all of it, plus anything left from the frame before. A stall, not a reset - the frame carries on
// where it left off and no pointer into the buffer moves.
//
// It must not reserve anything: this fires mid-draw with the gather it is about to reference
// already in the buffer. The terminator its kick writes is budgeted by every draw instead
// (cmdbuf::kTerminatorQwords).
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
// Eviction skips textures bound this frame, whose draws are still in the buffer or queued at the
// GS. When that leaves nothing to take, those pins are the only thing in the way - so fencing the
// GS retires the work they exist for and makes dropping them legitimate. The frame still renders
// correctly, it just spends its one kick early and pays a second at EndFrame.
//
// The last rung repacks the heap into one free block, so it can only come up short for a texture
// larger than the whole heap, which the caller rejects before getting here.
vram::Address AllocateVramFor(const tex::Texture & texture, const int sizeWords)
{
    vram::Address addr = vram::TryAllocate(texture, sizeWords);

    if (addr == vram::Address::Invalid)
    {
#if PS2_QUAKE_DEBUG
        Com_DPrintf("VRAM: heap full mid-frame for '%s' (%d KB), fencing the GS to unpin.\n",
                    texture.name, sizeWords * 4 / 1024);
#endif // PS2_QUAKE_DEBUG

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
// with nothing outstanding, which is how the deferred and immediate paths share it.
//
// **This belongs at the top of a frame, not the bottom of the previous one.** With two
// framebuffers, the one being drawn into and the one being scanned out must be the two different
// ones, so the flip has to happen before any of the next frame reaches the GS. Here, ahead of the
// clear, the whole build runs with the display parked on the previous image - so a mid-frame kick
// from an overflow rewind or a texture fence lands somewhere nobody is looking.
void PresentFrameInFlight()
{
    // Nothing to show: EndFrame presented immediately, or nothing has been kicked yet. The early
    // out must come before the vsync - falling through would spend a field here and another at the
    // frame's real present, halving the frame rate.
    if (!s_inFlightCtx.has_value())
    {
        return;
    }

    cmdbuf::WaitIdle(); // the GS fence; marks its own GsWait
    gs::PresentFramebuffer(*s_inFlightCtx);

    s_inFlightCtx.reset(); // shown; EndFrame is what puts the next one up
}

} // namespace

DrawStats detail::g_drawStats = {};

// ------------------------------------------------------------------------------------------------
// 2D primitives
// ------------------------------------------------------------------------------------------------

void FlushPending2D()
{
    if (!s_in2D)
    {
        return; // nothing accumulated since the last flush
    }
    s_in2D = false;

    CloseGifBlock();

    // Closed, not sent: the block goes out with the frame. 3D that follows lands after it in the
    // buffer and cannot overtake it at the GIF, because every block opens with a VIF FLUSH.
    //
    // This does not make the GS idle, so the VRAM reuse hazard stays set - a texture bound by the
    // section just closed may still be sampled by draws nothing has sent. FenceGs clears it.
}

void FillRect(const int x, const int y, const int width, const int height,
              const u8 r, const u8 g, const u8 b, const u8 a)
{
    Ensure2D();
    gs::EmitFillRect(GifData(gs::kFillRectQwords), s_drawCtx, x, y, width, height, r, g, b, a);
}

void DrawTexturedRect(const tex::Texture & texture, const int x, const int y, const int width, const int height,
                      const int u0, const int v0, const int u1, const int v1, const u8 brightness[3])
{
    Ensure2D();

    const gs::Bind2D bind = gs::ResolveBind2D(texture);
    if (bind.needsBind)
    {
        // Residency before the writer is taken: it can fence the GS, which reopens the section.
        // The dedupe above is what keeps this off the per-glyph path.
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

    // A scrapped image has no VRAM of its own - its pixels are a window into the atlas, and
    // residency is the atlas's. gs::ResolveBind2D resolves it before this is called.
    PS2_AssertMsg(texture.atlas == nullptr, "EnsureTextureResident on a scrapped image - bind its atlas!");

    if (texture.IsVramResident())
    {
        if (!texture.dirtyPixels)
        {
            vram::Touch(texture); // protect from eviction until the next frame
            return;
        }

        // Dynamic texture with rewritten pixels: re-upload over its own block. Draws queued
        // earlier this frame would sample the new texels, so fence the GS first. (The block is
        // still owned, so the reuse hazard does not apply.)
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

        // Queued draws may still sample VRAM the allocation just recycled.
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
// Initialization / frame lifecycle
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_DEBUG

// Added to the pipeline dump when a frame stops finishing. The registers say the pipeline is
// stuck; this says what the microprograms were building when it happened.
//
// The GIF tag block at the head of an output window is what a stalled PATH1 transfer is about. A
// drawing tag whose NLOOP does not match the vertices actually written, or that never got its
// EOP, is a packet the GIF can never finish - and the VU is long gone by then, so the only place
// left to read it is the memory it wrote.
Q_COLD_FUNC void DumpVuWorkInProgress()
{
    // Which half XTOP handed the microprograms this run.
    const int tops = static_cast<int>(VIF1_TOPS);

    debug::DumpVu1DataMemory("batch header, params and GIF tags", tops + vu1::kBatchHeaderAddr, 9);
    debug::DumpVu1DataMemory("output window A", tops + vu1::kOutputWindowAAddr, vu1::kNumGifTagQwords);
    debug::DumpVu1DataMemory("output window B", tops + vu1::kOutputWindowBAddr, vu1::kNumGifTagQwords);
}

#endif // PS2_QUAKE_DEBUG

void Init(const gs::Config & gsConfig, void * memory, const u32 memorySizeBytes)
{
    gs::Init(gsConfig);

    // NOTE: Must happen after mod::Init since the chain halves live in the arena it reserves.
    cmdbuf::Init(memory, memorySizeBytes);

    // NOTE: Must happen after cmdbuf::Init since the microprogram upload goes out on the cmdbuf chain.
    vu1::Init();

#if PS2_QUAKE_DEBUG
    cmdbuf::SetHangReportHook(&DumpVuWorkInProgress);
#endif
}

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
    GetStats() = {};

    // Retires and shows the previous frame when EndFrame left it drawing; a no-op when EndFrame
    // presented it immediately, which is what lets the two paths share everything below.
    PresentFrameInFlight();

    cmdbuf::BeginFrame();

    // The clear is the first thing in the frame, so the VU1 world that follows in the same buffer
    // cannot land on an uncleared framebuffer whatever the two GIF paths do.
    gs::GifWriter & clear = OpenGifBlock(kClearBlockQwords);
    gs::EmitClear(clear, s_drawCtx, s_clearColor, dither);
    CloseGifBlock();

    // The two lines below need the GS idle, and it is: whether the previous frame was fenced at
    // its own EndFrame or left drawing until PresentFrameInFlight above, it has been fenced by
    // the time the clear is built.
    PS2_AssertMsg(!cmdbuf::KickInFlight(), "BeginFrame with a frame still drawing!");
    vram::ClearReuseHazard();
    vram::BeginFrame();
}

void EndFrame(const bool deferPresent)
{
    PS2_AssertMsg(s_frameStarted, "EndFrame without BeginFrame!");
    s_frameStarted = false;

    // Close whatever 2D accumulated since the last flush, so it lands on top.
    FlushPending2D();

    // Everything since BeginFrame - the clear, every VU1 batch, every 2D block - goes out here,
    // in one kick, with one FlushCache(0).
    s_inFlightCtx = s_drawCtx;
    cmdbuf::Kick();

    // 'deferPresent' only decides who waits for that kick: clear, this frame is fenced and shown
    // before returning; set, the next BeginFrame picks it up, so the GS rasterises it across the
    // engine's own frame work. Clearing it mid-run duplicates one field on screen and is not
    // worth code to avoid.
    if (!deferPresent)
    {
        PresentFrameInFlight();
    }

    // Rolls the command buffer's high-water and latches its counters for the overlay.
    cmdbuf::EndFrame();

    s_drawCtx = gs::NextDrawContext(s_drawCtx); // draw into the other buffer next frame
}

void KickAndWait()
{
    cmdbuf::Drain();
}

int Gif2DPeakQwords()
{
    // Folds in the block still open, so a mid-frame reader (the overlay draws during the 2D pass)
    // reports honestly rather than only what previous blocks held.
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

// The frame's turbulent surface animation (rs::SetWarpAnimation). The phase goes up with the
// frame constants every draw chain; the scroll is applied per batch, by the chunk emitter, since
// only surfaces flagged SURF_FLOWING take it.
static float s_warpPhaseTurns   = 0.0f;
static float s_warpScrollTexels = 0.0f;

// Which blend equation the batch's ALPHA register gets. The three flags pick alternatives rather
// than combine, which this asserts.
//
// DynamicLights over Modulate is the lit lightmap pass: the modulate scales the framebuffer by the
// luxel intensity and the D term adds the computed light colour on top. Cs is exactly that colour,
// because the atlas texel is an alpha-ramp CLUT entry whose RGB sits at the modulate identity
// (Ct * Cv >> 7 == Cv) with As still the luxel intensity.
inline gs::BlendMode BlendModeFor(DrawFlags flags)
{
    const int blendModes = static_cast<int>(HasDrawFlag(flags, DrawFlags::Blended))
                         + static_cast<int>(HasDrawFlag(flags, DrawFlags::Additive))
                         + static_cast<int>(HasDrawFlag(flags, DrawFlags::Modulate));
    PS2_AssertMsg(blendModes <= 1, "Pick one blend mode - Blended, Additive and Modulate are exclusive!");

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

// The GS z conversion for a batch, as the (offset, scale) pair the microprogram applies to NDC z.
// The unhacked pair is the mapping on vu1::kGsDepthScale; a hacked one squeezes NDC z into
// [1 - 2s, 1] first, leaving the batch the nearest s of the z-buffer:
//
//     Z = 16 * kGsDepthScale * (1 + (s * ndcZ + (1 - s)))
//       = 16 * (kGsDepthScale * (2 - s) + ndcZ * kGsDepthScale * s)
//
// It costs the microprogram nothing - it multiplies and adds these either way. Note this applies
// where OpenGL's depth range does, to the window coordinate *after* the clip judgement. Folding an
// equivalent remap into the caller's projection would run it before: clipw tests |z| against |w|,
// so a remapped z stops being rejected once w goes negative and geometry behind the camera reaches
// the GS mirrored through the origin.
inline void DepthRangeFor(DrawFlags flags, float * outScale, float * outOffset)
{
    const float s = HasDrawFlag(flags, DrawFlags::DepthHack) ? vu1::kDepthHackScale : 1.0f;
    *outScale  = vu1::kGsDepthScale * s;
    *outOffset = vu1::kGsDepthScale * (2.0f - s);
}

// The 6 qwords of state every batch opens with: a GIF tag announcing five A+D register writes,
// then TEST, TEX1, TEX0, ALPHA and ZBUF for this context. The triangle and particle paths share
// it and differ only in the drawing tag each appends after.
//
// Returns whether the batch blends, which is decided here and which that tag needs for ABE.
bool AddBatchStateBlock(const tex::Texture & texture, gs::DrawContext drawCtx, DrawFlags flags)
{
    const gs::BlendMode blendMode = BlendModeFor(flags);

    // A blend mode was asked for, as opposed to the equation every batch writes: that is what
    // turns the ABE bit on and masks depth writes.
    const bool blended = HasDrawFlag(flags, DrawFlags::Blended)
                      || HasDrawFlag(flags, DrawFlags::Additive)
                      || HasDrawFlag(flags, DrawFlags::Modulate);

    // Pixel tests, the texture bind, the blend function and the depth-write mask...
    AddQword(GIF_SET_TAG(5, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    AddQword(gs::MakePixelTests(), gs::ContextReg(GS_REG_TEST, drawCtx));
    AddQword(gs::MakeTex1(texture), gs::ContextReg(GS_REG_TEX1, drawCtx));
    AddQword(gs::MakeTex0(texture, tex::TakesIntensity(texture.type)),
                 gs::ContextReg(GS_REG_TEX0, drawCtx));
    AddQword(gs::MakeAlphaBlend(blendMode), gs::ContextReg(GS_REG_ALPHA, drawCtx));
    AddQword(gs::MakeZBuf(blended || HasDrawFlag(flags, DrawFlags::NoDepthWrite)),
                 gs::ContextReg(GS_REG_ZBUF, drawCtx));

    return blended;
}

// The batch's 7 GIF tag qwords into an open inline unpack: the A+D state block plus the drawing
// tag, whose NLOOP the microprogram fills in.
void AddBatchGifTags(const tex::Texture & texture, gs::DrawContext drawCtx,
                     DrawFlags flags, bool packedRgbaOut = false)
{
    const bool blended = AddBatchStateBlock(texture, drawCtx, flags);
    const int  tme     = HasDrawFlag(flags, DrawFlags::Untextured) ? 0 : 1;
    const int  abe     = blended ? 1 : 0;

    // ...then the drawing tag: gouraud triangle list, STQ mapping, per-vertex registers from
    // vu1::kVertexRegList.
    //
    // **gif_tags.h's macros, never packet2_utils.h's VU_GS_PRIM / VU_GS_GIFTAG**: those do not
    // parenthesize their parameters, so any argument that is an expression silently
    // mis-assembles. 'abe' hoisted into a local for the same reason.
    const u64 prim = GIF_SET_PRIM(PRIM_TRIANGLE, 1, tme, 0, abe, 0, 0, gs::Index(drawCtx), 0);

    // Programs that *compute* their colour emit PACKED RGBAQ; ones receiving it already packed
    // emit an A+D write. The register list follows whichever this batch runs, so the caller says.
    const bool packedRgba = packedRgbaOut || HasDrawFlag(flags, DrawFlags::DynamicLights);

    // NLOOP starts at 0 and the microprogram patches it with what it actually wrote - the count
    // is not knowable here, since a clipping program's output depends on the geometry. Zero is
    // the safe placeholder rather than the vertex count: a patch that somehow never happened
    // then draws nothing, where a real-looking count would send the GIF reading past what was
    // written and into the next window's tag block. Both callers patch; particles do not come
    // through here, and their count *is* known up front.
    AddQword(GIF_SET_TAG(0, 1, 1, prim, GIF_FLG_PACKED, 3),
             packedRgba ? vu1::kLitVertexRegList : vu1::kVertexRegList);
}

// Builds the draw's transform and per-draw blocks into the buffer and unpacks them to their fixed
// VU addresses. kDrawSetupQwords is at least what this appends; ReserveChunk has reserved it.
//
// The per-draw block is the frame's dynamic lights, or 'lerp' for a lerp draw, which reads its
// constants from the same address and never reads the lights (see vu1::kLerpBlockAddr).
void BeginDrawChain(const math::Mat4 & mvp, DrawFlags flags, const vu1::LerpConstants * lerp)
{
    // One flags value per draw, so the depth range is a property of the whole chain.
    float depthScale, depthOffset;
    DepthRangeFor(flags, &depthScale, &depthOffset);

    constexpr int kFrameConstantsQwords = sizeof(vu1::FrameConstants) / 16;
    constexpr int kLightConstantsQwords = sizeof(vu1::LightConstants) / 16;
    constexpr int kLerpConstantsQwords  = sizeof(vu1::LerpConstants) / 16;
    // Each block costs what cmdbuf::CalcAllocCost says - its payload plus the skip tag - and the
    // REF tag that sends it, and the FLUSH below is the one qword on top. Sized for the lights,
    // the larger of the two per-draw blocks.
    static_assert(kDrawSetupQwords == 1 + cmdbuf::CalcAllocCost<vu1::FrameConstants>(1)
                                        + cmdbuf::CalcAllocCost<vu1::LightConstants>(1) + 2,
                  "kDrawSetupQwords must match what BeginDrawChain appends");
    static_assert(cmdbuf::CalcAllocCost<vu1::LerpConstants>(1) <= cmdbuf::CalcAllocCost<vu1::LightConstants>(1),
                  "The lerp block must cost no more than the light block it stands in for");

    // Both unpacks below write absolute VU addresses, which the double buffer does not
    // protect, and the previous draw's last chunk is very likely still running: wait for it.
    // See kDrawSetupQwords.
    AddFlush();

    vu1::FrameConstants * const constants = cmdbuf::Alloc<vu1::FrameConstants>(1);

    constants->mvp        = mvp;
    // .w is the clipper's plane shrink, and is read by nothing else - the screen mapping uses
    // .xyz only. See vu1::kVuClipShrink.
    constants->gsScale    = { 2048.0f, 2048.0f, depthScale, 1.0f - vu1::kVuClipShrink };
    // .w is the clipper's distance scale, read by nothing else - the screen mapping
    // uses .xyz only. See vu1::kVuClipDistScale.
    constants->gsOffset   = { 2048.0f + static_cast<float>(gs::Width())  * 0.5f,
                              2048.0f + static_cast<float>(gs::Height()) * 0.5f,
                              depthOffset, vu1::kVuClipDistScale };
    // .xyz is the constant guard band scale; .w is the turbulent animation phase, which only
    // a warped batch reads. See the note on vu1::kClipScale.
    constants->clipScale   = vu1::kClipScale;
    constants->clipScale.w = s_warpPhaseTurns;
    constants->colorClamp  = vu1::kColorClamp;

    AddUnpackData(vu1::kFrameConstantsAddr, constants, kFrameConstantsQwords, false);

    if (lerp != nullptr)
    {
        // A copy in the buffer, like everything else here: the caller's is a local.
        vu1::LerpConstants * const block = cmdbuf::Alloc<vu1::LerpConstants>(1);
        *block = *lerp;

        AddUnpackData(vu1::kLerpBlockAddr, block, kLerpConstantsQwords, false);
    }
    else
    {
        // s_lightConstants is the source of truth; the buffer gets a copy, for the same lifetime
        // reason as the transform block.
        vu1::LightConstants * const lights = cmdbuf::Alloc<vu1::LightConstants>(1);
        *lights = s_lightConstants;

        AddUnpackData(vu1::kLightBlockAddr, lights, kLightConstantsQwords, false);
    }
}

// Makes room for one chunk and (re)opens the draw's chain when it has to - the bookkeeping all
// three draw paths share.
//
// The constants are reserved with every chunk rather than once because a reservation that
// overflows rewinds the buffer, taking them with it; the next chunk would then transform against
// whatever the previous draw left in VU memory. 'firstChunk' emits them at the top of a call.
// 'lerp' is a lerp draw's per-draw block, and null for everything else; see BeginDrawChain.
void ReserveChunk(const int chunkQwords, const math::Mat4 & mvp, const DrawFlags flags,
                  const bool firstChunk, const vu1::LerpConstants * lerp = nullptr)
{
    if (cmdbuf::Reserve(kDrawSetupQwords + chunkQwords) || firstChunk)
    {
        BeginDrawChain(mvp, flags, lerp);
    }
}

// One chunk: batch header and GIF tags unpacked inline to the current double buffer, the vertex
// data referenced in place, and the MSCAL that runs the microprogram over it.
void AddBatchChunk(const tex::Texture & texture, gs::DrawContext drawCtx,
                   const vu1::DrawVertex * verts, int vertCount, DrawFlags flags)
{
    PS2_Assert(vertCount > 0 && vertCount <= vu1::kMaxVertsPerBatch && (vertCount % 3) == 0);
    EnsureSpace(kChunkChainQwords);

    const bool lit = HasDrawFlag(flags, DrawFlags::DynamicLights);

    // A warped batch is this same layout with the warp flag set, and reads its parameters from
    // the qword after the header; see below.
    const bool warped = HasDrawFlag(flags, DrawFlags::Warped);

    OpenInlineUnpack(vu1::kBatchHeaderAddr, true);
    {
        // Header: what the microprogram should do per vertex. The mode fields are separate lanes
        // so it can test each against zero directly.
        AddU32(static_cast<u32>(lit ? vu1::BatchColorMode::Computed
                                    : vu1::BatchColorMode::PackedU32));
        AddU32(static_cast<u32>(warped ? vu1::BatchWarp::On : vu1::BatchWarp::Off));
        AddU32(static_cast<u32>(vu1::BatchVertexFormat::DrawVertex));
        AddU32(static_cast<u32>(vertCount));

        // Parameters for whatever that asked for. The warp wants the texel-to-image divide -
        // taken from the texture's size on disk, for the same reason ref_gl's hardcoded 64
        // works (see DrawAnimatedWaterPolys) - and the frame's SURF_FLOWING drift.
        if (warped)
        {
            AddFloat(1.0f / static_cast<float>(texture.srcWidth));
            AddFloat(1.0f / static_cast<float>(texture.srcHeight));
            AddFloat(HasDrawFlag(flags, DrawFlags::WarpFlowing) ? s_warpScrollTexels : 0.0f);
            AddFloat(0.0f);
        }
        else
        {
            AddU32(0);
            AddU32(0);
            AddU32(0);
            AddU32(0);
        }

        AddBatchGifTags(texture, drawCtx, flags);
    }
    CloseInlineUnpack();

    AddUnpackData(vu1::kVertexDataAddr, verts, static_cast<u32>(vertCount * 2), true);

    AddStartProgram(vu1::ProgramAddress(vu1::Program::Textured));
}

// The lerped equivalent, for the same microprogram: the same head as a world batch, then the two
// vertex streams interleaved into the input as they unpack, then the MSCAL. Everything
// per-entity - the pose scales, the light, the cull sign - went up once with the draw
// (vu1::LerpConstants), so the header carries only what the batch is. The byte-position DMA must
// be whole source qwords, so an odd count transfers one pad vertex the VU never reads.
void AddLerpBatchChunk(const tex::Texture & texture, gs::DrawContext drawCtx,
                       const vu1::LerpPosChunk & posChunk, const vu1::LerpDrawAttrib * attribs,
                       int vertCount, DrawFlags flags)
{
    PS2_Assert(vertCount > 0 && vertCount <= vu1::kMaxLerpVertsPerBatch && (vertCount % 3) == 0);
    EnsureSpace(kLerpChunkChainQwords);

    OpenInlineUnpack(vu1::kBatchHeaderAddr, true);
    {
        // The colour is computed, from the shade term; no warp; keyframe vertices.
        AddU32(static_cast<u32>(vu1::BatchColorMode::Computed));
        AddU32(static_cast<u32>(vu1::BatchWarp::Off));
        AddU32(static_cast<u32>(vu1::BatchVertexFormat::Keyframes));
        AddU32(static_cast<u32>(vertCount));

        // No per-batch parameters. The qword is still sent, so the tags land where a world
        // batch's do and one open inline unpack covers the lot.
        AddU32(0);
        AddU32(0);
        AddU32(0);
        AddU32(0);

        AddBatchGifTags(texture, drawCtx, flags, /*packedRgbaOut=*/true);
    }
    CloseInlineUnpack();

    // The keyframe bytes: V4_8 elements, one source word and so one destination qword each, cur
    // then old, padded to an even count so the transfer is whole qwords. Two written, one skipped:
    // the skipped qword is where the attribute lands below. The pad vertex, when there is one,
    // lands in the slot after the last vertex, which a short chunk never reads.
    const int srcVerts = vertCount + (vertCount & 1);
    AddUnpackDataFmt(vu1::kVertexDataAddr + vu1::kLerpCurOffset, posChunk.pos,
                     static_cast<u32>(srcVerts / 2), // qwords: 8 bytes per vertex
                     static_cast<u32>(srcVerts * 2), // elements: 2 per vertex
                     P2_UNPACK_V4_8, true,
                     /*writeLen=*/2, /*cycleLen=*/vu1::kLerpVertexQwords);

    // Referenced in the model hunk rather than copied into the buffer: one REF tag either way,
    // and nothing rewrites a model mid-frame. One written, two skipped - the positions' slots.
    AddUnpackDataFmt(vu1::kVertexDataAddr + vu1::kLerpAttribOffset, attribs,
                     static_cast<u32>(vertCount), static_cast<u32>(vertCount),
                     P2_UNPACK_V4_32, true,
                     /*writeLen=*/1, /*cycleLen=*/vu1::kLerpVertexQwords);

    AddStartProgram(vu1::ProgramAddress(vu1::Program::Textured));
}

// One particle chunk: header, batch constants and GIF tags unpacked inline, the particles
// referenced in place, then the MSCAL. 'clipOffset' is the corner offset already in clip space;
// the UVs are in the GS 12.4 fixed point the PACKED UV descriptor wants.
void AddParticleChunk(const tex::Texture & texture, gs::DrawContext drawCtx,
                      const math::Vec4 & clipOffset, u32 uvMaxU, u32 uvMaxV,
                      const vu1::ParticleVertex * particles, int count, DrawFlags flags)
{
    PS2_Assert(count > 0 && count <= vu1::kMaxParticlesPerBatch);
    EnsureSpace(kParticleChunkQwords);

    OpenInlineUnpack(vu1::kPrtBatchHeaderAddr, true);
    {
        AddU32(0);
        AddU32(0);
        AddU32(0);
        AddU32(static_cast<u32>(count));

        // The corner offset, with the distance blow-up rate riding in its unused .w.
        AddFloat(clipOffset.x);
        AddFloat(clipOffset.y);
        AddFloat(clipOffset.z);
        AddFloat(vu1::kParticleBlowUpRate);

        // The two corner UVs. PACKED UV takes U in word 0 and V in word 1.
        AddU32(0);
        AddU32(0);
        AddU32(0);
        AddU32(0);

        AddU32(uvMaxU);
        AddU32(uvMaxV);
        AddU32(0);
        AddU32(0);

        const bool blended = AddBatchStateBlock(texture, drawCtx, flags);
        const int  abe     = blended ? 1 : 0; // Hoisted: see the note in AddBatchGifTags.

        // One sprite per particle, five registers each: the A+D that sets its colour, then a
        // UV/XYZ2 pair per corner. FST selects UV over ST - a screen-aligned sprite needs no
        // perspective correction.
        const u64 prim = GIF_SET_PRIM(PRIM_SPRITE, 0, 1, 0, abe, 0, 1, gs::Index(drawCtx), 0);
        AddQword(GIF_SET_TAG(count, 1, 1, prim, GIF_FLG_PACKED, 5), vu1::kParticleRegList);
    }
    CloseInlineUnpack();

    AddUnpackData(vu1::kPrtDataAddr, particles, static_cast<u32>(count), true);

    AddStartProgram(vu1::ProgramAddress(vu1::Program::Particles));
}

} // namespace

void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const vu1::DrawVertex * verts, int vertCount, DrawFlags flags)
{
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(verts) & 15u) == 0, "Vertex data must be 16-byte aligned!");

    // Send any 2D accumulated before this 3D burst so it draws underneath, and its textures are
    // consumed before our uploads can evict them. Only the first 3D draw after 2D pays it.
    FlushPending2D();

    EnsureTextureResident(texture);

    // One chunk per VU run; the double buffer overlaps each unpack with the previous transform.
    for (int firstVert = 0; firstVert < vertCount; firstVert += vu1::kMaxVertsPerBatch)
    {
        ReserveChunk(kChunkChainQwords, mvp, flags, /*firstChunk=*/firstVert == 0);

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < vu1::kMaxVertsPerBatch) ? remaining : vu1::kMaxVertsPerBatch;
        AddBatchChunk(texture, s_drawCtx, verts + firstVert, chunkVerts, flags);
    }
}

void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight, const vu1::LerpPosChunk * posChunks,
                         const vu1::LerpDrawAttrib * attribs, int vertCount,
                         FaceCull faceCull, DrawFlags flags)
{
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawLerpedTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(posChunks) & 15u) == 0, "Position chunks must be 16-byte aligned!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(attribs) & 15u) == 0, "Attribute stream must be 16-byte aligned!");

    FlushPending2D();

    EnsureTextureResident(texture);

    // A property of the texture, so resolved here rather than threaded down from every caller.
    float stScaleS, stScaleT;
    tex::StScaleFor(texture, &stScaleS, &stScaleT);

    // The draw's per-entity block, which BeginDrawChain sends up in place of the lights. The ST
    // scale is the skin's size over its power-of-two TEX0 extent, multiplied onto every vertex by
    // the microprogram: the VU has the multiply slot free and the EE does not. The light arrives
    // pre-divided by 128, to meet the quantized shade term - see VertexShadeLight.
    const vu1::LerpConstants lerp = {
        { frontv.x, frontv.y, frontv.z, 0.0f },
        { backv.x,  backv.y,  backv.z,  0.0f },
        shadeLight,
        { CullSignFor(faceCull), stScaleS, stScaleT, 1.0f }
    };

    // Chunking as in DrawTriangles. The positions are already grouped one LerpPosChunk per VU
    // run, and the attributes slice at the same boundary because the caller gathered them in
    // order. Only a final odd chunk pads its position transfer.
    for (int firstVert = 0, c = 0; firstVert < vertCount; firstVert += vu1::kMaxLerpVertsPerBatch, ++c)
    {
        ReserveChunk(kLerpChunkChainQwords, mvp, flags, /*firstChunk=*/firstVert == 0, &lerp);

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < vu1::kMaxLerpVertsPerBatch) ? remaining : vu1::kMaxLerpVertsPerBatch;

        AddLerpBatchChunk(texture, s_drawCtx, posChunks[c], attribs + firstVert, chunkVerts, flags);
    }
}

void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const vu1::ParticleVertex * particles,
                   int count, DrawFlags flags)
{
    PS2_AssertMsg(count > 0, "DrawParticles wants at least one particle!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(particles) & 15u) == 0, "Particle data must be 16-byte aligned!");

    FlushPending2D();

    EnsureTextureResident(texture);

    // Transformed once for the whole call, as a direction (w = 0). Being orthogonal to the view
    // axis, its clip z and w both come out zero, which is what lets the microprogram reuse the
    // centre's depth and 1/w for both corners - see particles.vcl.
    const math::Vec4 clipOffset = math::Transform(
        math::Vec4{ quadOffset.x, quadOffset.y, quadOffset.z, 0.0f }, mvp);

    // Corner UVs in GS 12.4 fixed point, spanning the whole image - particle images are
    // power-of-two, so no ST rescale applies.
    const u32 uvMaxU = static_cast<u32>(texture.width)  << 4;
    const u32 uvMaxV = static_cast<u32>(texture.height) << 4;

    for (int first = 0; first < count; first += vu1::kMaxParticlesPerBatch)
    {
        ReserveChunk(kParticleChunkQwords, mvp, flags, /*firstChunk=*/first == 0);

        const int remaining  = count - first;
        const int chunkCount = (remaining < vu1::kMaxParticlesPerBatch) ? remaining : vu1::kMaxParticlesPerBatch;
        AddParticleChunk(texture, s_drawCtx, clipOffset, uvMaxU, uvMaxV,
                         particles + first, chunkCount, flags);
    }
}

void SetDynamicLights(const vu1::DynamicLight * lights, const int count)
{
    // Zeroed slots evaluate to max(0 * d + 0, 0) = 0, so unused lights need no branch and no
    // separate "how many" path.
    s_lightConstants = {};
    s_lightConstants.clamp = { 255.0f, 255.0f, 255.0f, vu1::kLitVertexAlpha };

    const int used = (count < vu1::kMaxDynamicLights) ? count : vu1::kMaxDynamicLights;
    PS2_Assert(used >= 0 && (used == 0 || lights != nullptr));

    // Transposed: one light per SIMD lane rather than one axis per lane, so the microprogram
    // does all four at once.
    float px[vu1::kMaxDynamicLights] = {};
    float py[vu1::kMaxDynamicLights] = {};
    float pz[vu1::kMaxDynamicLights] = {};

    for (int i = 0; i < used; ++i)
    {
        const vu1::DynamicLight & l = lights[i];

        // No inside, and would divide by zero below; leave the slot dark.
        if (l.radius <= 0.0f)
        {
            continue;
        }

        px[i] = l.origin.x;
        py[i] = l.origin.y;
        pz[i] = l.origin.z;

        // Pre-scaled to the GS 0-255 range and pre-divided by the radius squared, which reduces
        // the VU's attenuation to one multiply-add with no divide or square root.
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

void SetWarpAnimation(const float phaseTurns, const float scrollTexels)
{
    s_warpPhaseTurns   = phaseTurns;
    s_warpScrollTexels = scrollTexels;
}

// ------------------------------------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------------------------------------

namespace {
// The span Begin claimed, until Submit sends it.
static vu1::ParticleVertex * s_particles     = nullptr;
static int                   s_particleCount = 0;
} // namespace

template<>
vu1::ParticleVertex * Begin<vu1::ParticleVertex *>(const int particleCount)
{
    PS2_AssertMsg(particleCount > 0, "rs::Begin<ParticleVertex *> with nothing to draw!");
    PS2_AssertMsg(s_particles == nullptr, "rs::Begin<ParticleVertex *> without an rs::Submit!");

    // Taking the buffer is the 2D->3D boundary: a pending 2D section holds an open DMA tag and an
    // allocation cannot land inside one. The reservation covers the chunks the draw appends on
    // top, which must not drain the buffer out from under the span they reference.
    FlushPending2D();
    cmdbuf::Reserve(cmdbuf::CalcAllocCost<vu1::ParticleVertex>(particleCount) + DrawParticlesChainCost(particleCount));

    s_particles     = cmdbuf::Alloc<vu1::ParticleVertex>(particleCount);
    s_particleCount = particleCount;
    return s_particles;
}

void Submit(vu1::ParticleVertex * __restrict & particles, const math::Mat4 & mvp, const tex::Texture & texture,
            const math::Vec3 & quadOffset, const DrawFlags flags)
{
    PS2_AssertMsg(s_particles != nullptr, "rs::Submit of particles without an rs::Begin!");
    PS2_AssertMsg(particles == s_particles, "rs::Submit of particles given a pointer rs::Begin did not return!");

    DrawStats & stats = GetStats();
    ++stats.drawBatches;
    stats.particles += s_particleCount;

    DrawParticles(mvp, texture, quadOffset, s_particles, s_particleCount, flags);

    s_particles     = nullptr;
    s_particleCount = 0;

    particles = nullptr;
}

// ------------------------------------------------------------------------------------------------
// VU1 bring-up
// ------------------------------------------------------------------------------------------------

void AddVUMicroProgram(const vu1::ProgramAddr dest, const vu1::VUCode code)
{
    packet2_vif_add_micro_program(Packet(), static_cast<u32>(dest), code.start, code.end);
}

void AddVUDoubleBufferSettings(const u32 baseQw, const u32 offsetQw)
{
    packet2_utils_vu_add_double_buffer(Packet(), static_cast<u16>(baseQw), static_cast<u16>(offsetQw));
}

void AddVUDataUpload(const u32 vuAddrQw, const void * const data, const u32 qwords)
{
    // useTop false: an absolute VU address, not one of the double-buffer halves. Safe without a
    // FLUSH because the only caller is vu1::Init, where nothing is running yet.
    AddUnpackData(vuAddrQw, data, qwords, false);
}

} // namespace ps2::rs
