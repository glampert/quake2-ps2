#pragma once
/* ================================================================================================
 * File: render_context.h
 * Brief: The renderer's command recorder: builds the frame's VIF1 DMA source chain in the
 *        command buffer - microprogram uploads, unpacks into VU memory, program kicks, and the
 *        DIRECT blocks that carry raw GIF data to the GS.
 *
 *        One instance per frame, reached through Ctx(). Owns no memory: the chain belongs to
 *        ps2::cmdbuf, which owns the rewind, the terminator and the kick.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/vu1.h"

#include <cstdint>
#include <optional>
#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_utils.h>
#include <packet2_vif.h>

namespace ps2::tex { struct Texture; }

namespace ps2::rc {

// ------------------------------------------------------------------------------------------------
// Draw state
// ------------------------------------------------------------------------------------------------

// Optional batch draw flags (OR-able). Blended batches combine with the
// framebuffer through the A+D block's source-alpha blend and mask their
// depth writes - the z-test still reads, so they sort against opaque
// geometry but never occlude. Untextured batches draw pure gouraud colour
// (the texture argument is still bound, just not sampled).
//
// Additive replaces Blended's equation with Cs * As + Cd - the source colour
// added to the framebuffer instead of interpolated with it - and implies
// everything else Blended does, including the depth-write mask. Note the alpha
// still scales the contribution, so a fully opaque (As = 0x80) additive batch
// is exactly OpenGL's glBlendFunc(GL_ONE, GL_ONE) while a fading one needs no
// second blend mode. Additive results saturate rather than wrap: gs::Init
// leaves the GS COLCLAMP register enabled.
//
// Modulate is the third equation, Cd * As: it scales what is already in the
// framebuffer by the batch's alpha and contributes no colour of its own. This
// is how the lightmap pass darkens the diffuse pass under it. The GS blend unit
// multiplies by a scalar alpha and never by a second colour, so this is as close
// to OpenGL's glBlendFunc(GL_ZERO, GL_SRC_COLOR) as the hardware gets - what it
// modulates by is the source's *alpha*, not its RGB. The colour half of a luxel
// therefore cannot come through here at all; it reaches the screen through the
// diffuse pass's vertex colour instead (see lm::AtlasColors).
//
// The three are mutually exclusive; passing more than one asserts. Each implies
// the ABE bit and the depth-write mask.
//
// DepthHack is orthogonal to all of the above and touches no GS register: it
// compresses the batch's depth into the slice of the z-buffer nearest the
// camera, so the view weapon can never poke through a wall it is visually in
// front of (Quake 2's RF_DEPTHHACK, ref_gl's glDepthRange(0, 0.3)). It applies
// where OpenGL's depth range does - to the window coordinate, *after* the clip
// judgement - by scaling the microprogram's NDC-to-GS z conversion. Folding an
// equivalent remap into the caller's projection instead would run it before the
// judgement, which is not the same thing: clipw tests |z| against |w|, so a
// remapped z stops being rejected once w goes negative and the geometry behind
// the camera reaches the GS mirrored through the origin.
//
// NoDepthWrite is likewise orthogonal: it masks the batch's depth writes on
// their own, without the blend equation and ABE bit the three modes above drag
// along with theirs. The z-test still reads, so the batch sorts against what is
// already in the buffer but leaves nothing behind for later ones to sort
// against - what an opaque primitive standing in for infinity wants. The
// skybox is the caller: it draws at a finite 2300 units so the world can
// occlude it, and must not occlude anything drawn after it out there in return.
//
// DynamicLights run the lit microprogram: the batch's vertex colour is computed
// from the frame's dynamic point lights (see SetDynamicLights) instead of taken
// from the input vertices, and Modulate batches add it on top of what they
// modulate. Only meaningful for geometry submitted in world space.
enum class DrawFlags : u32
{
    None          = 0,
    Blended       = 1 << 0,
    Untextured    = 1 << 1,
    Additive      = 1 << 2,
    Modulate      = 1 << 3,
    DepthHack     = 1 << 4,
    NoDepthWrite  = 1 << 5,
    DynamicLights = 1 << 6,
};

constexpr DrawFlags operator|(DrawFlags a, DrawFlags b)
{
    return static_cast<DrawFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

constexpr bool HasDrawFlag(DrawFlags flags, DrawFlags test)
{
    return (static_cast<u32>(flags) & static_cast<u32>(test)) != 0;
}

// Backface culling mode for DrawLerpedTriangles: which sign of a triangle's
// screen-space signed area gets rejected on the VU. Which of the two is
// facing away depends on the winding and the projection's Y orientation.
enum class FaceCull : u32
{
    None     = 0,
    Negative = 1,
    Positive = 2,
};

class RenderContext final
{
public:
    RenderContext() = default;

    // Non-copyable: there is one recorder for the frame, reached through Ctx().
    RenderContext(const RenderContext &) = delete;
    RenderContext & operator=(const RenderContext &) = delete;

    // Qwords written into the command buffer's current half, and what may be written there.
    int QwordCount() const { return cmdbuf::QwordCount(); }
    int QwordCapacity() const { return cmdbuf::QwordCapacity(); }

    // Halts if the next emission would overrun the half. 'qwords' is a safe upper bound for
    // what comes next (DEBUG ONLY).
    //
    // The backstop, not the mechanism: a draw too large for one half is cmdbuf::Reserve's job,
    // and it rewinds rather than failing. What is left for this to catch is a chunk emitter
    // writing more than the footprint constant it declares.
    void EnsureSpace([[maybe_unused]] const int qwords) const
    {
#if PS2_QUAKE_ASSERTS
        if (QwordCount() + qwords > QwordCapacity()) [[unlikely]]
        {
            Sys_Error("Command buffer overflow: %d qwords in use + %d needed exceeds the "
                      "%d a half can hold.", QwordCount(), qwords, QwordCapacity());
        }
#endif // PS2_QUAKE_ASSERTS
    }

    // --------------------------------------------------------------------------------------------
    // VU1 microprogram and data transfers; each appends DMA tags/VIF codes and advances.
    // --------------------------------------------------------------------------------------------

    // References a microprogram into the chain as MPG transfers (chunked to the VIF's
    // 256-instruction limit).
    void AddMicroProgram(const vu1::ProgramAddr dest, const vu1::VUCode code)
    {
        packet2_vif_add_micro_program(Packet(), static_cast<u32>(dest), code.start, code.end);
    }

    // FLUSH + MSCAL: waits for any previous run, then starts the microprogram at 'prog'.
    void AddStartProgram(const vu1::ProgramAddr prog)
    {
        packet2_utils_vu_add_start_program(Packet(), static_cast<u32>(prog));
    }

    // Programs the VIF1 BASE/OFFSET registers that split VU data memory into the two halves
    // XTOP alternates between. Both in qwords.
    void AddDoubleBufferSettings(const u32 baseQw, const u32 offsetQw)
    {
        packet2_utils_vu_add_double_buffer(Packet(), static_cast<u16>(baseQw),
                                          static_cast<u16>(offsetQw));
    }

    // References 'data' in place (REF tag) and unpacks it to VU data memory at 'vuAddr' (qword
    // address; relative to the current double buffer when 'useTop'). The data must be 16-byte
    // aligned and stay valid until the frame's kick. At most 256 qwords per unpack.
    void AddUnpackData(const u32 vuAddr, const void * data, const u32 qwords, const bool useTop)
    {
        AddUnpackDataFmt(vuAddr, data, qwords, qwords, P2_UNPACK_V4_32, useTop);
    }

    // General form for the packed VIF formats, where the transfer length and the unpack length
    // differ. 'srcQwords' is what the REF tag carries; 'numElements' is the VIFcode NUM field -
    // the elements *written* to VU memory (one destination qword each for the V4 formats; 256
    // max). The transfer must hold exactly the payload the unpack consumes: a V4_8 element eats
    // one source word, so numElements must be 4 * srcQwords. Spare words would be decoded as
    // VIFcodes, and a short transfer stalls the VIF waiting for payload that never comes.
    void AddUnpackDataFmt(const u32 vuAddr, const void * data, const u32 srcQwords,
                          const u32 numElements, const enum UnpackMode format, const bool useTop)
    {
        PS2_AssertMsg(numElements <= 256, "VIF unpacks are limited to 256 elements!");
        PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(data) & 15u) == 0,
                      "Unpack data must be 16-byte aligned!");

        packet2_t * const pkt = Packet();
        packet2_chain_ref(pkt, data, srcQwords, 0, 0, 0);
        packet2_vif_stcycl(pkt, 1, 1, 0);
        packet2_vif_open_unpack(pkt, format, vuAddr, useTop, /*masked=*/0, /*usigned=*/1, 0);
        packet2_vif_close_unpack_manual(pkt, numElements);
    }

    // A VIF FLUSH of its own: stalls VIF1 until the running microprogram has ended and its
    // XGKICKs have reached the GS. One qword - the CNT tag carries the FLUSH and a NOP in its
    // two VIFcode slots, and its own QWC is zero.
    //
    // Needed in front of anything that writes VU data memory at an *absolute* address, which
    // the double buffer does not protect. The per-chunk unpacks do not need it, and the MSCAL
    // after each carries a FLUSH anyway.
    void AddFlush()
    {
        packet2_t * const pkt = Packet();
        packet2_chain_open_cnt(pkt, 0, 0, 0);
        packet2_vif_flush(pkt, 0);
        packet2_vif_nop(pkt, 0);
        packet2_chain_close_tag(pkt);
    }

    // Small unpacks built directly into the chain: open, append qwords, close.
    void OpenInlineUnpack(const u32 vuAddr, const bool useTop)
    {
        packet2_utils_vu_open_unpack(Packet(), vuAddr, useTop);
    }

    void CloseInlineUnpack()
    {
        packet2_utils_vu_close_unpack(Packet());
    }

    void AddQword(const u64 lo, const u64 hi)
    {
        packet2_add_2x_s64(Packet(), static_cast<s64>(lo), static_cast<s64>(hi));
    }

    void AddFloat(const float value) { packet2_add_float(Packet(), value); }
    void AddU32(const u32 value) { packet2_add_u32(Packet(), value); }

    // --------------------------------------------------------------------------------------------
    // DIRECT blocks: GIF data carried through VIF1 to the GIF over PATH2
    // --------------------------------------------------------------------------------------------

    // What opening a DIRECT block costs on top of its payload: the CNT tag, whose own qword
    // also carries the two VIFcodes below. Part of the chain budget arithmetic.
    static constexpr int kDirectOverheadQwords = 1;

    // Opens a DIRECT transfer: everything written until CloseDirect goes to the GIF verbatim as
    // GIF tags and register data - the frame clear and the 2D overlay.
    //
    // The leading FLUSH stalls VIF1 until the last microprogram has ended and its XGKICKs have
    // reached the GS, so a block opened after a batch cannot interleave with PATH1 at the GIF.
    // It costs nothing when no VU work is outstanding, which is why it is unconditional.
    //
    // FLUSH and DIRECT are the two VIFcodes riding the CNT tag's own qword (tte=1), so the
    // opening is one qword and the payload starts on the next - which is what makes
    // CloseDirect's qword count come out right.
    void OpenDirect()
    {
        packet2_t * const pkt = Packet();
        packet2_chain_open_cnt(pkt, 0, 0, 0);
        packet2_vif_flush(pkt, 0);
        packet2_vif_open_direct(pkt, 0);
    }

    // Patches the DIRECT VIFcode's qword count and the CNT tag's QWC from where the cursor
    // ended up.
    void CloseDirect()
    {
        packet2_t * const pkt = Packet();
        const vif_code_t * const code = pkt->vif_code_opened_at;
        PS2_AssertMsg(code != nullptr, "CloseDirect with no DIRECT block open!");

        // The payload starts at the qword boundary just past the VIFcode's own word.
        const std::uintptr_t payload = reinterpret_cast<std::uintptr_t>(code) + sizeof(u32);
        const u32 qwords = static_cast<u32>(
            (reinterpret_cast<std::uintptr_t>(pkt->next) - payload) >> 4);

        // An empty DIRECT is not a harmless no-op: the count is a 16-bit immediate and zero
        // means 65536 qwords, so the VIF would swallow the rest of the chain as GIF data.
        PS2_AssertMsg(qwords > 0 && qwords <= 0xFFFFu,
                      "CloseDirect on an empty or oversized block - a DIRECT carries 1..65535 qwords!");

        packet2_vif_close_direct_manual(pkt, qwords);
        packet2_chain_close_tag(pkt);
    }

    // The raw write cursor inside an open DIRECT block, and the way to hand back where a writer
    // left it - for payload built by something that takes a qword_t * of its own (a GifWriter).
    qword_t * DirectCursor() const
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
        PS2_AssertMsg(cursor >= pkt->next && (cursor - pkt->base) <= QwordCapacity(),
                      "SetDirectCursor past the end of the chain half!");
        pkt->next = cursor;
    }

    // --------------------------------------------------------------------------------------------
    // GIF sections
    // --------------------------------------------------------------------------------------------

    // Room for 'qwords' of GIF data in an open section, handing back the writer to put it in.
    // Splits the section when the current block runs out, which is invisible to the caller: the
    // state a section programmed lives in the GS's registers, not in the block.
    //
    // **The writer is only good until the next call.** A split replaces it, and so does anything
    // that fences the GS (a texture upload), so take it again after either rather than holding it.
    gs::GifWriter & GifData(int qwords);

    // Closes the open 2D section, so what follows draws under it. Called at every 2D->3D
    // boundary and at EndFrame; a no-op when nothing has accumulated.
    void FlushPending2D();

    // --------------------------------------------------------------------------------------------
    // 2D primitives. Each opens the 2D section on demand - callers just draw, no bracket - and
    // the section is closed automatically before the next 3D draw, so 2D always lands on top.
    // --------------------------------------------------------------------------------------------

    // A solid rectangle. Alpha below 255 blends with the framebuffer.
    void FillRect(int x, int y, int width, int height, u8 r, u8 g, u8 b, u8 a);

    // A textured rectangle sampling 'texture' over texel range [u0,v0]..[u1,v1], made resident
    // first if it is not already. 'brightness' modulates the texel colour per RGB channel:
    // 128 leaves it unchanged. Texels with alpha 0 are cut out by the alpha test.
    void DrawTexturedRect(const tex::Texture & texture, int x, int y, int width, int height,
                          int u0, int v0, int u1, int v1, const u8 brightness[3]);

private:
    // The half being recorded into. Inline and cached by cmdbuf, so this costs what naming a
    // member would.
    static Q_ALWAYS_INLINE packet2_t * Packet() { return cmdbuf::Packet(); }
};

namespace detail {
// The one recorder. Exposed so Ctx() can be inline, which will matter once the recorder carries
// state the emitters read per call; written only by render_context.cpp.
extern RenderContext g_context;

// The context this frame draws into, swapped by EndFrame. Exposed for the same reason: every
// batch asks for it. Written only by render_context.cpp.
extern gs::DrawContext g_drawCtx;
} // namespace detail

// The frame's recorder.
Q_ALWAYS_INLINE RenderContext & Ctx()
{
    return detail::g_context;
}

// --------------------------------------------------------------------------------------------
// Frame lifecycle
// --------------------------------------------------------------------------------------------

// Opens the frame: shows the previous one if it was left drawing, rewinds the command buffer
// and writes the screen clear at the head of it. 2D and 3D may then be drawn in any order, and
// both record into the same buffer.
//
// 'dither' enables the GS's ordered dither, which hides the banding a 16-bit framebuffer shows on
// smooth gradients and does nothing to a 32-bit one. Passed per frame so it can be flipped live.
void BeginFrame(bool dither);

// Closes the frame: flushes any pending 2D and submits the command buffer.
//
// 'deferPresent' leaves the frame drawing for the next BeginFrame to show, so the GS rasterises it
// while the EE builds the frame after - one frame of latency bought for the fence the EE would
// otherwise stand at. Clear it and this waits for the GS and flips before returning.
void EndFrame(bool deferPresent);

// The GS drawing context being rendered into this frame. Every context-indexed register a caller
// programs itself, and the prim CTXT bit, must match it.
Q_ALWAYS_INLINE gs::DrawContext CurrentDrawContext() { return detail::g_drawCtx; }

// Background colour the frame clear fills with.
void SetClearColor(u8 r, u8 g, u8 b);

// Makes the texture's pixels resident in GS VRAM, uploading them on a miss and evicting the
// least-recently-bound textures when the heap is full. Already-resident textures only have their
// LRU stamp refreshed, unless their pixels were marked dirty, which re-uploads in place.
//
// May fence the GS - submitting the frame so far and waiting for it - when an upload would land
// on VRAM that queued draws still sample. That closes and reopens any open GIF section, so a
// GifWriter taken before this call must not be used after it.
void EnsureTextureResident(const tex::Texture & texture);

// The most qwords one GIF block has held. Shown as "Gif2DPk" in the draw-stats overlay; what it
// measures against is the command buffer half it has to fit inside (cmdbuf::kHalfBytes).
int Gif2DPeakQwords();

// ------------------------------------------------------------------------------------------------
// Chain budget
//
// What a draw costs the command buffer besides its vertex data, so a caller whose vertex data is
// *itself* in the buffer can reserve the pair together.
//
// It has to reserve the pair. The chunk loop reserves as it goes, and cmdbuf::Reserve rewinds when
// it comes up short - which would pull the buffer out from under the very span the chunks being
// emitted reference. Reserving the whole draw up front means that reservation can never fire half
// way through one.
//
// TRANSITIONAL: public only until the vertex streams take reservation over and no caller reserves
// anything, at which point this becomes internal to render_context.cpp.
// ------------------------------------------------------------------------------------------------

// Chain qwords one chunk of each draw appends. The world path: the header/GIF-tag inline unpack
// (1 tag qword plus 8 of payload), the vertex REF unpack (1 - its VIFcodes ride in the tag's upper
// half) and the FLUSH + MSCAL (1). 11 in practice, declared with room to spare; over-declaring
// only reserves slightly more of the buffer than a chunk needs. The lerped path adds a second REF
// unpack and a longer header (15 in practice), the particle path an 11-qword header (14).
constexpr int kChunkChainQwords      = 16;
constexpr int kLerpChunkChainQwords  = 22;
constexpr int kParticleChunkQwords   = 22;

// Chain qwords a draw's opening costs: the transform block and the dynamic-light block, both
// built in the buffer rather than referenced out of a static. 8 and 12 qwords of payload, each
// fronted by the skip tag cmdbuf::Alloc needs and followed by the REF tag that sends it, plus
// the VIF FLUSH in front of the pair.
//
// That FLUSH is what stops the two unpacks landing on VU memory a microprogram is still
// reading. Both go to *absolute* addresses - the constants sit below the double buffer and the
// light block above it - so unlike a chunk's data they get no protection from the buffer swap,
// and since draws stopped being kicked and waited on one at a time the previous draw's last
// chunk is routinely still running when the next draw's setup arrives.
constexpr int kDrawSetupQwords = 1 + (8 + 2) + (12 + 2);

constexpr int ChunkCount(const int items, const int perChunk)
{
    return (items + perChunk - 1) / perChunk;
}

// Chain qwords the three draws need for 'count' vertices / particles, not counting the data
// itself.
//
// The peak the chunk loop *demands*, which is one setup block more than the draw ever
// appends: every chunk reserves the setup alongside itself, because a reservation that
// overflowed would rewind the setup with everything else and the next chunk has to be able to
// re-emit it. So the last chunk asks for room the draw will not end up using, and reserving
// only what is written would let that final ask rewind the buffer mid-draw.
//
// Plus the terminator. A draw no longer ends in a kick, but it can still contain one: the GS
// fence EnsureTextureResident takes when an upload is about to land on VRAM that queued
// draws may still sample has to send the buffer now that nothing else will until EndFrame, and
// a kick writes its FLUSH + END into the buffer at the cursor. Budgeted here rather than
// reserved where it fires, because reserving there is exactly what must not happen - a
// reservation that overflowed would rewind the buffer out from under the span the draw is
// about to reference.
constexpr int DrawTrianglesChainCost(const int vertCount)
{
    return cmdbuf::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(vertCount, vu1::kMaxVertsPerBatch) * kChunkChainQwords);
}

constexpr int DrawLerpedTrianglesChainCost(const int vertCount)
{
    return cmdbuf::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(vertCount, vu1::kMaxLerpVertsPerBatch) * kLerpChunkChainQwords);
}

constexpr int DrawParticlesChainCost(const int count)
{
    return cmdbuf::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(count, vu1::kMaxParticlesPerBatch) * kParticleChunkQwords);
}

// ------------------------------------------------------------------------------------------------
// Draws
//
// **None is synchronous.** Each appends to the frame's command buffer and returns; nothing is sent
// until EndFrame kicks it. So the vertex data has to stay valid for the rest of the frame, not for
// the duration of the call - which is why it is a span of the buffer itself (cmdbuf::Alloc) rather
// than a gather static.
//
// A caller must have reserved the matching *ChainCost on top of that span, so that nothing here
// can rewind the buffer out from under it. A kick is harmless (it leaves the buffer where it is);
// a rewind would leave the REF tags pointing at memory the next gather is about to write.
// ------------------------------------------------------------------------------------------------

// Draws a batch of triangles (3 verts each, triangle list) with the given transform and texture
// (made resident on demand). Any whole-triangle count works: draws beyond vu1::kMaxVertsPerBatch
// are split into chunks submitted back to back, overlapping each chunk's upload with the previous
// one's transform.
void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const vu1::DrawVertex * verts, int vertCount,
                   DrawFlags flags = DrawFlags::None);

// Draws 'vertCount' keyframe-lerped vertices: 'posChunks' is the gathered position stream, one
// vu1::LerpPosChunk per VU run, and 'attribs' is a contiguous run of vertCount per-vertex
// attributes the chunks slice in the same order.
//
// The two come from different places on purpose. Positions are an indexed gather and have to be
// built, so they live in the command buffer like every other gather; attributes are the model's
// own baked array in draw order, so they are referenced where they lie and never copied. Both must
// stay valid until the frame's kick - the buffer does by construction, the model hunk because
// nothing unloads a model mid-frame - and 'attribs' must be qword aligned, which mod::AliasVertex
// is.
void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight,
                         const vu1::LerpPosChunk * posChunks, const vu1::LerpDrawAttrib * attribs,
                         int vertCount,
                         FaceCull faceCull = FaceCull::None,
                         DrawFlags flags = DrawFlags::None);

// Draws camera-facing particle billboards as GS sprites, expanded entirely on VU1 - the caller
// transforms nothing.
//
// 'quadOffset' is the world-space vector from a particle's anchor corner to its opposite corner:
// the camera's (up + right), pre-scaled by whatever blow-up the caller wants (ref_gl uses 1.5). It
// must be orthogonal to the view axis, which is what makes every corner share the centre's depth
// and lets the billboard draw as a single axis-aligned sprite; it is transformed once per call as
// a direction.
//
// The billboard also grows with distance the way ref_gl's particles do. The texture is sampled
// corner to corner through UV (no perspective correction, which a screen-aligned sprite does not
// need).
void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const vu1::ParticleVertex * particles,
                   int count, DrawFlags flags = DrawFlags::Blended);

// Sets the frame's lights, shared by every batch drawn with DrawFlags::DynamicLights until the
// next call. Fewer than vu1::kMaxDynamicLights is fine - unused slots are zeroed and contribute
// nothing. Pass count 0 to turn the lighting off without clearing the flag.
//
// Colours are pre-scaled here into the GS 0-255 range and pre-divided by the radius squared, which
// is what lets the microprogram attenuate with a single multiply-add and no divide or square root.
void SetDynamicLights(const vu1::DynamicLight * lights, int count);

} // namespace ps2::rc
