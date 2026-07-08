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

#include "ps2/qcommon.h"

#include <dma.h>
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>
#include <packet.h>

namespace ps2::gs {

class RenderPacket final
{
public:
    RenderPacket() = default;

    // Non-copyable: owns the underlying packet buffer.
    RenderPacket(const RenderPacket &) = delete;
    RenderPacket & operator=(const RenderPacket &) = delete;

    // Allocates the packet buffer. Call once, and not from a static constructor -
    // the heap must already be up.
    void Init(int maxQwords)
    {
        PS2_AssertMsg(m_packet == nullptr, "RenderPacket::Init called twice!");
        m_packet = packet_init(maxQwords, PACKET_NORMAL);
        PS2_AssertMsg(m_packet != nullptr, "packet_init failed!");
        m_ptr = m_packet->data;
    }

    // Rewinds the write cursor to the start of the buffer.
    void Reset()
    {
        m_ptr = m_packet->data;
    }

    // Qwords written since the last Reset().
    int QwordCount() const
    {
        return static_cast<int>(m_ptr - m_packet->data);
    }

    // Halt visibly if the next emission could overrun the buffer (silent overflow
    // corrupts the heap). 'qwords' is a safe upper bound for what comes next.
    void EnsureSpace(int qwords) const
    {
        PS2_AssertMsg(QwordCount() + qwords <= static_cast<int>(m_packet->qwords),
                      "Render packet overflow! Bump the packet size.");
    }

    // --------------------------------------------------------------------------------------------
    // libdraw wrappers; each appends to the packet and advances the cursor.
    // --------------------------------------------------------------------------------------------

    void SetupEnvironment(int context, framebuffer_t & frame, zbuffer_t & zbuffer)
    {
        m_ptr = draw_setup_environment(m_ptr, context, &frame, &zbuffer);
    }

    void TextureWrapping(int context, texwrap_t & wrap)
    {
        m_ptr = draw_texture_wrapping(m_ptr, context, &wrap);
    }

    void Clear(int context, float x, float y, float width, float height, int r, int g, int b)
    {
        m_ptr = draw_clear(m_ptr, context, x, y, width, height, r, g, b);
    }

    void RectFilled(int context, rect_t & rect)
    {
        m_ptr = draw_rect_filled(m_ptr, context, &rect);
    }

    void RectFilledStrips(int context, rect_t & rect)
    {
        m_ptr = draw_rect_filled_strips(m_ptr, context, &rect);
    }

    void RectTextured(int context, texrect_t & rect)
    {
        m_ptr = draw_rect_textured(m_ptr, context, &rect);
    }

    void TextureSampling(int context, lod_t & lod)
    {
        m_ptr = draw_texture_sampling(m_ptr, context, &lod);
    }

    void TextureBuffer(int context, texbuffer_t & texbuf, clutbuffer_t & clut)
    {
        m_ptr = draw_texturebuffer(m_ptr, context, &texbuf, &clut);
    }

    // Emits the DMA chain tags for a texture upload; the pixels are referenced
    // in place and must stay valid until the transfer completes.
    void TextureTransfer(const void * pixels, int width, int height, int psm, int vramAddr)
    {
        m_ptr = draw_texture_transfer(m_ptr, const_cast<void *>(pixels),
                                      width, height, psm, vramAddr, width);
    }

    void TextureFlush()
    {
        m_ptr = draw_texture_flush(m_ptr);
    }

    // Appends a FINISH event so draw_wait_finish() can tell when the GS is done.
    void Finish()
    {
        m_ptr = draw_finish(m_ptr);
    }

    // --------------------------------------------------------------------------------------------
    // DMA kick-off over the GIF channel. Fire and forget; waits stay with the caller.
    // --------------------------------------------------------------------------------------------

    // Sends the packet contents as one normal transfer.
    void SendNormal()
    {
        dma_channel_send_normal(DMA_CHANNEL_GIF, m_packet->data, QwordCount(), 0, 0);
    }

    // Sends the packet as a source-chain transfer (the packet holds the chain tags).
    void SendChain()
    {
        dma_channel_send_chain(DMA_CHANNEL_GIF, m_packet->data, QwordCount(), 0, 0);
    }

private:
    packet_t * m_packet = nullptr; // ps2sdk packet; owns the qword buffer
    qword_t *  m_ptr    = nullptr; // write cursor, advanced by every append
};

} // namespace ps2::gs
