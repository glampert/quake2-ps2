#pragma once
/* ================================================================================================
 * File: gif_writer.h
 * Brief: Bounds-checked write cursor for GIF packets, wrapping the libdraw draw_* emitters.
 *        Non-owning: the memory belongs to whoever opened the block being written.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vram.h"

#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <draw_sampling.h>
#include <gif_tags.h>

namespace ps2::gs {

// A cursor into GIF packet memory somebody else owns - a DIRECT block inside the frame's
// command buffer, or a packet built for a one-off DMA. Two pointers and a bound, so it is
// made, used and assigned over rather than attached and detached.
//
// Every emitter advances the cursor and halts if the emission ran past the bound. The check
// has to be after the fact: the libdraw helpers report how much they wrote only by returning
// the advanced cursor, so there is nothing to test up front.
class GifWriter final
{
public:
    // 'maxQwords' is what may be written at 'base'. There is no default state: a writer always
    // refers to real memory, and re-pointing one means constructing another over the new block
    // (m_maxQwords is const, so assignment is not available).
    GifWriter(qword_t * const base, const int maxQwords)
        : m_base{ base }
        , m_ptr{ base }
        , m_maxQwords{ maxQwords }
    {
        PS2_AssertMsg(base != nullptr && maxQwords > 0, "GifWriter needs memory to write into!");
    }

    // Where the cursor ended up - what the block's owner needs to close it.
    qword_t * Cursor() const { return m_ptr; }

    int QwordCount() const { return static_cast<int>(m_ptr - m_base); }
    int QwordCapacity() const { return m_maxQwords; }

    // Halts if the next emission would overrun. 'qwords' is an upper bound for what comes next.
    // DEBUG ONLY, as is the per-emission check in Advance: both compile out of a release build,
    // which is assumed to have been run through every level under asserts first. An overflow in
    // release is undefined behaviour.
    void EnsureSpace([[maybe_unused]] const int qwords) const
    {
#if PS2_QUAKE_ASSERTS
        if (QwordCount() + qwords > m_maxQwords) [[unlikely]]
        {
            Sys_Error("GIF packet overflow: %d qwords in use + %d needed exceeds the %d capacity.",
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

    // Switches the z-test to ALLPASS - draw on top of everything, depth writes still happen -
    // keeping the environment's alpha test.
    void DisableTests(int context, zbuffer_t & zbuffer)
    {
        Advance(draw_disable_tests(m_ptr, context, &zbuffer));
    }

    // Restores the z-buffer's own test method.
    void EnableTests(int context, zbuffer_t & zbuffer)
    {
        Advance(draw_enable_tests(m_ptr, context, &zbuffer));
    }

    void Clear(int context, float x, float y, float width, float height, int r, int g, int b)
    {
        Advance(draw_clear(m_ptr, context, x, y, width, height, r, g, b));
    }

    // One GS register as a GIF tag + A+D data pair, for the registers the draw_* helpers
    // leave untouched (e.g. re-arming ZBUF's write mask, which the test helpers never program).
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

    // DMA chain tags for a texture upload; the pixels are referenced in place and must stay
    // valid until the transfer completes. 'destWidth' is the VRAM buffer width in pixels (the
    // TBW stride the texture will be sampled with) - usually just 'width', but 8-bit formats
    // round it up to a multiple of 128 and the 16x16 CLUT image uses the 64-pixel minimum.
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

    // A FINISH event, so a GS-idle wait has something to test.
    void Finish()
    {
        Advance(draw_finish(m_ptr));
    }

    // Ends the packet without drawing: a PACKED tag with NLOOP = 0 and EOP set.
    //
    // Required at the end of every block. The GIF stays bound to the path feeding it until it
    // sees EOP and most of the draw_* helpers emit their tags with EOP clear, so a block that
    // simply stopped would leave its path open and the next XGKICK would wait on a packet
    // nothing is going to finish.
    void EndGifPacket()
    {
        EnsureSpace(1);

        PACK_GIFTAG(m_ptr, GIF_SET_TAG(0, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        Advance(m_ptr + 1);
    }

private:
    // Takes the cursor a draw_* helper returned and, under asserts, halts if the emission went
    // past capacity - so a caller whose upper bound was wrong gets a named error rather than
    // silent corruption of whatever follows the block.
    Q_ALWAYS_INLINE void Advance(qword_t * const newPtr)
    {
        m_ptr = newPtr;

#if PS2_QUAKE_ASSERTS
        if (QwordCount() > m_maxQwords) [[unlikely]]
        {
            Sys_Error("GIF packet overflow: emission ran to %d qwords, past the %d capacity.",
                      QwordCount(), m_maxQwords);
        }
#endif // PS2_QUAKE_ASSERTS
    }

    qword_t * m_base;            // start of the block being written
    qword_t * m_ptr;             // write cursor, advanced by every append
    const int m_maxQwords;       // what may be written at m_base
};

} // namespace ps2::gs
