#pragma once
/* ================================================================================================
 * File: render_packet.h
 * Brief: RenderPacket wraps a ps2sdk DMA packet together with the write cursor that the
 *        libdraw draw_* helpers thread through, so building GIF packets reads as method
 *        calls on the packet instead of free functions over a bare qword pointer. Thin
 *        wrappers only: blending state, DMA waits and frame pacing stay with the caller.
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

    // Non-copyable: owns the underlying packet buffer.
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
    }

    // Rewinds the write cursor to the start of the buffer, banking what the cycle
    // just about to be discarded reached.
    void Reset()
    {
        const int used = QwordCount();
        if (used > m_peakQwords) { m_peakQwords = used; }
        m_ptr = m_base;
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

    // The most qwords this packet has ever held. Reset() banks it, and the current
    // cycle is folded in here so a packet that is filled but never Reset (the clear
    // and texture-upload chains) still reports honestly. This is what the capacity
    // passed to Init() should be sized against - see kPacketQwords in gs.cpp.
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

#if PS2_QUAKE_ASSERTS
        if (QwordCount() > m_maxQwords) [[unlikely]]
        {
            Sys_Error("Render packet overflow: emission ran to %d qwords, past the %d "
                      "capacity (into the %d qword guard). Raise the size passed to Init().",
                      QwordCount(), m_maxQwords, kGuardQwords);
        }
#endif // PS2_QUAKE_ASSERTS
    }

    qword_t * m_base       = nullptr; // owns the qword buffer
    qword_t * m_ptr        = nullptr; // write cursor, advanced by every append
    int       m_maxQwords  = 0;       // capacity of m_base, for EnsureSpace
    int       m_peakQwords = 0;       // high-water of QwordCount, for sizing m_maxQwords
};

} // namespace ps2::gs
