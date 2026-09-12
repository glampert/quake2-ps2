#pragma once
/* ================================================================================================
 * File: render_packet.h
 * Brief: RenderPacket wraps the write cursor that the libdraw draw_* helpers thread through,
 *        so building GIF packets reads as method calls on the packet instead of free functions
 *        over a bare qword pointer. Thin wrappers only: blending state, DMA waits and frame
 *        pacing stay with the caller.
 *
 *  Two ways to be pointed at memory, and the difference is ownership rather than behaviour:
 *  Init() allocates a buffer of its own, which is what the synchronous texture-upload path
 *  still needs (it sends the packet itself), while Attach() borrows a DIRECT block opened
 *  inside the frame chain, so the clear and the 2D overlay build straight into the frame's
 *  one source chain and are submitted with it. Every draw_* wrapper below is the same either
 *  way - that is the whole point, since it is what let the 2D call sites stay untouched when
 *  the frame packets went away.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/heap.h"

#include <cstring> // memset
#include <dma.h>
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>
#include <gif_tags.h>

namespace ps2::gs {

class RenderPacket final
{
public:
    RenderPacket() = default;

    // Non-copyable: may own the underlying packet buffer, and is a cursor either way.
    RenderPacket(const RenderPacket &) = delete;
    RenderPacket & operator=(const RenderPacket &) = delete;

    // Slack allocated past m_maxQwords. The libdraw draw_* helpers only report how
    // much they wrote by returning the advanced cursor, so an emission that runs
    // past capacity can only be detected after the fact - this is the room that
    // makes "after the fact" still be inside our own allocation, so Advance() can
    // halt on it instead of it becoming a heap corruption someone debugs later.
    // Comfortably larger than any single emission: the biggest EnsureSpace request
    // in the renderer is 64 qwords, and draw_texture_transfer's whole chain fits
    // in the 128-qword upload packet.
    static constexpr int kGuardQwords = 256;

    // Allocates the packet buffer. Call once, and not from a static constructor -
    // the heap must already be up.
    void Init(int maxQwords)
    {
        PS2_AssertMsg(m_base == nullptr, "RenderPacket::Init called twice!");
        PS2_Assert(maxQwords > 0);

        // This used to be libpacket's packet_init(), which was the last thing in
        // the link still pulling in -lpacket - and it was only ever used as an
        // allocator: everything below writes through the cursor with libdraw's
        // draw_* helpers, never a packet_* call. Allocating directly drops the
        // dependency and puts the buffer behind the tagged allocator, so it shows
        // up in the memory overlay instead of needing a hand-written TagsAddMem.
        // Same shape packet_init produced: 64-byte (cache line) aligned, zeroed.
        const size_t sizeBytes = static_cast<size_t>(maxQwords + kGuardQwords) * sizeof(qword_t);

        m_base = static_cast<qword_t *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(64), sizeBytes, ps2::heap::MemTag::Renderer));
        std::memset(m_base, 0, sizeBytes);

        m_maxQwords = maxQwords;
        m_ptr       = m_base;
        m_owned     = true;
    }

    // Rewinds the write cursor to the start of the buffer, banking what the cycle
    // just about to be discarded reached.
    void Reset()
    {
        const int used = QwordCount();
        if (used > m_peakQwords) { m_peakQwords = used; }
        m_ptr = m_base;
    }

    // Points the wrappers at memory somebody else owns - the payload of a DIRECT block
    // opened in the frame chain - instead of a buffer of this packet's own. 'maxQwords' is
    // what may be written there before the block has to be closed and sent.
    //
    // Nothing is allocated and nothing is sent: Detach() hands the cursor back to whoever
    // opened the block, and that owner is what puts the data on the wire. Note there is no
    // kGuardQwords slack past the bound here - what follows an attached block is the rest of
    // the chain half, not our own spare room - which is why Advance()'s check is live rather
    // than debug-only.
    void Attach(qword_t * const cursor, const int maxQwords)
    {
        PS2_AssertMsg(!m_owned, "RenderPacket::Attach on a packet that owns its buffer!");
        PS2_AssertMsg(m_base == nullptr, "RenderPacket::Attach with a block already attached!");
        PS2_Assert(cursor != nullptr && maxQwords > 0);

        m_base      = cursor;
        m_ptr       = cursor;
        m_maxQwords = maxQwords;
    }

    // Gives the borrowed block back, banking what it held. The cursor the owner needs is
    // Cursor(), read before this.
    void Detach()
    {
        PS2_AssertMsg(!m_owned, "RenderPacket::Detach on a packet that owns its buffer!");
        PS2_AssertMsg(m_base != nullptr, "RenderPacket::Detach with no block attached!");

        const int used = QwordCount();
        if (used > m_peakQwords) { m_peakQwords = used; }

        m_base      = nullptr;
        m_ptr       = nullptr;
        m_maxQwords = 0;
    }

    // Where the cursor ended up - what an attached block's owner needs to know how far the
    // draw_* wrappers advanced it.
    qword_t * Cursor() const
    {
        return m_ptr;
    }

    // Qwords written since the last Reset().
    int QwordCount() const
    {
        return static_cast<int>(m_ptr - m_base);
    }

    int QwordCapacity() const
    {
        return m_maxQwords;
    }

    // The most qwords this packet has ever held. Reset() and Detach() bank it, and the
    // current cycle is folded in here so a packet that is filled but never Reset (the
    // texture-upload chain) still reports honestly. For an owned packet this is what the
    // capacity passed to Init() should be sized against; for the attached 2D block it is
    // how much of a chain half the overlay wants (the "Gif2DPk" row in the draw stats).
    int PeakQwords() const
    {
        const int used = QwordCount();
        return (used > m_peakQwords) ? used : m_peakQwords;
    }

    // Halt visibly if the next emission would overrun the buffer.
    // 'qwords' is a safe upper bound for what comes next (DEBUG ONLY).
    void EnsureSpace([[maybe_unused]] const int qwords) const
    {
#if PS2_QUAKE_ASSERTS
        if (QwordCount() + qwords > m_maxQwords) [[unlikely]]
        {
            Sys_Error("Render packet overflow: %d qwords in use + %d needed exceeds "
                      "the %d capacity. Raise the size passed to Init().",
                      QwordCount(), qwords, m_maxQwords);
        }
#endif // PS2_QUAKE_ASSERTS
    }

    // --------------------------------------------------------------------------------------------
    // libdraw wrappers; each appends to the packet and advances the cursor.
    // --------------------------------------------------------------------------------------------

    void SetupEnvironment(int context, framebuffer_t & frame, zbuffer_t & zbuffer)
    {
        Advance(draw_setup_environment(m_ptr, context, &frame, &zbuffer));
    }

    void TextureWrapping(int context, texwrap_t & wrap)
    {
        Advance(draw_texture_wrapping(m_ptr, context, &wrap));
    }

    // Pixel tests for subsequent draws. Disable switches the z-test to ALLPASS
    // (draw on top of everything; depth writes still happen) while keeping the
    // environment's alpha test; Enable restores the z-buffer's test method.
    void DisableTests(int context, zbuffer_t & zbuffer)
    {
        Advance(draw_disable_tests(m_ptr, context, &zbuffer));
    }

    void EnableTests(int context, zbuffer_t & zbuffer)
    {
        Advance(draw_enable_tests(m_ptr, context, &zbuffer));
    }

    void Clear(int context, float x, float y, float width, float height, int r, int g, int b)
    {
        Advance(draw_clear(m_ptr, context, x, y, width, height, r, g, b));
    }

    // Writes one GS register directly, as a GIF tag + A+D data pair. For the
    // rare register the draw_* helpers leave untouched (e.g. re-arming ZBUF's
    // write mask, which draw_enable/disable_tests never program).
    void SetRegister(u64 reg, u64 data)
    {
        EnsureSpace(2); // Unlike the draw_* helpers, this one knows its own size.

        PACK_GIFTAG(m_ptr, GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        ++m_ptr;
        PACK_GIFTAG(m_ptr, data, reg);
        ++m_ptr;
    }

    void RectFilled(int context, rect_t & rect)
    {
        Advance(draw_rect_filled(m_ptr, context, &rect));
    }

    void RectFilledStrips(int context, rect_t & rect)
    {
        Advance(draw_rect_filled_strips(m_ptr, context, &rect));
    }

    void RectTextured(int context, texrect_t & rect)
    {
        Advance(draw_rect_textured(m_ptr, context, &rect));
    }

    void TextureSampling(int context, lod_t & lod)
    {
        Advance(draw_texture_sampling(m_ptr, context, &lod));
    }

    void TextureBuffer(int context, texbuffer_t & texbuf, clutbuffer_t & clut)
    {
        Advance(draw_texturebuffer(m_ptr, context, &texbuf, &clut));
    }

    // Emits the DMA chain tags for a texture upload; the pixels are referenced
    // in place and must stay valid until the transfer completes. 'destWidth'
    // is the VRAM buffer width in pixels (the TBW stride the texture will be
    // sampled with) - usually just 'width', but 8-bit formats round it up to a
    // multiple of 128 and the 16x16 CLUT image uses the 64-pixel minimum.
    void TextureTransfer(const void * pixels, int width, int height, int psm,
                         vram::Address vramAddr, int destWidth)
    {
        Advance(draw_texture_transfer(m_ptr, const_cast<void *>(pixels),
                                      width, height, psm, static_cast<int>(vramAddr), destWidth));
    }

    void TextureFlush()
    {
        Advance(draw_texture_flush(m_ptr));
    }

    // Appends a FINISH event so draw_wait_finish() can tell when the GS is done.
    void Finish()
    {
        Advance(draw_finish(m_ptr));
    }

    // Ends the GIF packet without drawing anything: a PACKED tag with NLOOP = 0, so no data
    // follows it, and EOP set.
    //
    // The GIF stays bound to the path feeding it until it sees EOP, and most of the draw_*
    // helpers above emit their tags with EOP clear. A block that simply stopped after one of
    // those would leave PATH2 open, and the next microprogram's XGKICK - PATH1, in the same
    // chain a few qwords later - would wait for a packet nothing is going to finish. So every
    // DIRECT block in the frame chain is closed with one of these, whatever it ends on.
    void EndGifPacket()
    {
        EnsureSpace(1);

        PACK_GIFTAG(m_ptr, GIF_SET_TAG(0, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        Advance(m_ptr + 1);
    }

    // Wait until FINISH event occurs.
    static void WaitFinish()
    {
        draw_wait_finish();
    }

    // --------------------------------------------------------------------------------------------
    // DMA kick-off over the GIF channel. Fire and forget; waits stay with the caller.
    // --------------------------------------------------------------------------------------------

    // Sends the packet contents as one normal transfer.
    void SendNormal()
    {
        dma_channel_send_normal(DMA_CHANNEL_GIF, m_base, QwordCount(), 0, 0);
    }

    // Sends the packet as a source-chain transfer (the packet holds the chain tags).
    void SendChain()
    {
        dma_channel_send_chain(DMA_CHANNEL_GIF, m_base, QwordCount(), 0, 0);
    }

    // Waits until channel is usable based on coprocessor status.
    // NOTE: Assumes fast waits are enabled for the GIF DMA channel.
    static void Wait()
    {
        dma_wait_fast();
    }

private:
    // Takes the cursor a draw_* helper returned and halts if the emission went
    // past capacity. Every wrapper above goes through here, so a caller that
    // forgets EnsureSpace - or one whose upper bound turns out to be wrong - still
    // gets a named error rather than corrupting the heap. kGuardQwords is what
    // keeps the write that tripped this inside our own allocation.
    Q_ALWAYS_INLINE void Advance(qword_t * const newPtr)
    {
        m_ptr = newPtr;

        // Live in release, not just under asserts. An owned packet has kGuardQwords of its own
        // slack to land in, so the overrun is contained either way - but an attached one is a
        // window into the frame chain, and what it would run into is the next segment's DMA
        // tags. That fails somewhere else entirely, with nothing pointing back to here.
        if (QwordCount() > m_maxQwords) [[unlikely]]
        {
            Sys_Error("Render packet overflow: emission ran to %d qwords, past the %d capacity. "
                      "Raise the size passed to Init(), or the block budget in gs.cpp.",
                      QwordCount(), m_maxQwords);
        }
    }

    qword_t * m_base       = nullptr; // start of the buffer or block being written
    qword_t * m_ptr        = nullptr; // write cursor, advanced by every append
    int       m_maxQwords  = 0;       // capacity of m_base, for EnsureSpace
    int       m_peakQwords = 0;       // high-water of QwordCount, for sizing m_maxQwords
    bool      m_owned      = false;   // true once Init allocated m_base; Attach borrows instead
};

} // namespace ps2::gs
