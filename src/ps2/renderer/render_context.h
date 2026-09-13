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
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/vu1.h"

#include <cstdint>
#include <optional>
#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_utils.h>
#include <packet2_vif.h>

namespace ps2::rc {

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
    // 256-instruction limit). 'destInstr' is a VU micro memory address in 64-bit instruction
    // units.
    void AddMicroProgram(const u32 destInstr, const vu1::VUCode code)
    {
        packet2_vif_add_micro_program(Packet(), destInstr, code.start, code.end);
    }

    // FLUSH + MSCAL: waits for any previous run, then starts the microprogram at 'progInstr'
    // (64-bit instruction units; 0 = start of micro memory).
    void AddStartProgram(const u32 progInstr)
    {
        packet2_utils_vu_add_start_program(Packet(), progInstr);
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
} // namespace detail

// The frame's recorder.
Q_ALWAYS_INLINE RenderContext & Ctx()
{
    return detail::g_context;
}

// --------------------------------------------------------------------------------------------
// Frame lifecycle
// --------------------------------------------------------------------------------------------

// Reads the cvars the frame is steered by. Call once, after gs::Init and cmdbuf::Init.
void Init();

// Opens the frame: shows the previous one if it was left drawing, rewinds the command buffer
// and writes the screen clear at the head of it. 2D and 3D may then be drawn in any order, and
// both record into the same buffer.
void BeginFrame();

// Closes the frame: flushes any pending 2D, submits the command buffer, and - unless
// ps2_gs_latency leaves it drawing for the next BeginFrame to show - waits for the GS and flips.
void EndFrame();

// The GS drawing context (0 or 1) being rendered into this frame. Every context-indexed register
// a caller programs itself, and the prim CTXT bit, must match it.
int CurrentDrawContext();

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

} // namespace ps2::rc
