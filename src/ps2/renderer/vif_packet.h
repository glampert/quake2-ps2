#pragma once
/* ================================================================================================
 * File: vif_packet.h
 * Brief: VifPacket wraps a ps2sdk packet2 DMA source chain aimed at VIF1, the path used to
 *        feed VU1: microprogram upload (MPG), data unpacks into VU memory and program kicks
 *        (MSCAL). Sibling of RenderPacket, which drives the GIF/PATH3 2D path. Thin wrappers
 *        only: what goes into VU memory and when to wait stays with the caller.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <cstdint>
#include <dma.h>
#include <kernel.h> // FlushCache
#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_utils.h>
#include <packet2_vif.h>

namespace ps2::vu1 {

struct VUCode
{
    u32* start;
    u32* end;
};

class VifPacket final
{
public:
    VifPacket() = default;

    // Non-copyable: owns the underlying packet buffer.
    VifPacket(const VifPacket &) = delete;
    VifPacket & operator=(const VifPacket &) = delete;

    // Allocates the packet buffer (source-chain mode, tags transferred inline).
    // Call once, and not from a static constructor - the heap must already be up.
    void Init(int maxQwords)
    {
        PS2_AssertMsg(m_packet == nullptr, "VifPacket::Init called twice!");
        m_packet = packet2_create(static_cast<u16>(maxQwords),
                                  P2_TYPE_NORMAL, P2_MODE_CHAIN, /*tte=*/1);
        PS2_AssertMsg(m_packet != nullptr, "packet2_create failed!");
        m_maxQwords = maxQwords;

        // packet2_create mallocs the header struct plus the qword buffer; neither goes
        // through the tagged allocators, so account for them here. Never freed.
        ps2::heap::TagsAddMem(ps2::heap::MemTag::Renderer, sizeof(packet2_t) + (static_cast<size_t>(maxQwords) * sizeof(qword_t)));
    }

    // Rewinds the write cursor to the start of the buffer.
    void Reset()
    {
        packet2_reset(m_packet, /*clear_mem=*/0);
    }

    // Qwords written since the last Reset().
    int QwordCount() const
    {
        return static_cast<int>(packet2_get_qw_count(m_packet));
    }

    // Halt visibly if the next emission would overrun the buffer. 'qwords' is a
    // safe upper bound for what comes next.
    //
    // Sys_Error rather than PS2_Assert, for the same reason as
    // RenderPacket::EnsureSpace: an assert compiles out of the release build, and
    // a silent overflow writes DMA tags over whatever came next in the heap, which
    // then fails somewhere unrelated. Unlike RenderPacket this has no guard region
    // to fall back on - packet2 owns its buffer - so the pre-check is the only
    // line of defence, which is exactly why it has to be live in release. Every
    // emitter in vu1.cpp calls this before appending a chunk.
    void EnsureSpace(int qwords) const
    {
        if (QwordCount() + qwords > m_maxQwords) [[unlikely]]
        {
            Sys_Error("VIF packet overflow: %d qwords in use + %d needed exceeds the "
                      "%d capacity. Raise the size passed to Init().",
                      QwordCount(), qwords, m_maxQwords);
        }
    }

    // --------------------------------------------------------------------------------------------
    // Chain building; each appends DMA tags/VIF codes and advances the cursor.
    // --------------------------------------------------------------------------------------------

    // References a VU microprogram into the chain as MPG transfers (chunked to
    // the 256-instruction VIF limit). 'destInstr' is the VU micro memory
    // address in 64-bit instruction units.
    void AddMicroProgram(u32 destInstr, VUCode code)
    {
        packet2_vif_add_micro_program(m_packet, destInstr, code.start, code.end);
    }

    // Programs the VIF1 BASE/OFFSET registers that split VU data memory into the
    // two halves the XTOP double buffering alternates between. Both in qwords.
    void AddDoubleBufferSettings(u32 baseQw, u32 offsetQw)
    {
        packet2_utils_vu_add_double_buffer(m_packet,
                                           static_cast<u16>(baseQw),
                                           static_cast<u16>(offsetQw));
    }

    // References 'data' in place (REF tag) and unpacks it to VU data memory at
    // 'vuAddr' (qword address; relative to the current double buffer when
    // 'useTop'). The data must be 16-byte aligned and stay untouched until the
    // transfer completes. At most 256 qwords per unpack.
    void AddUnpackData(u32 vuAddr, const void * data, u32 qwords, bool useTop)
    {
        AddUnpackDataFmt(vuAddr, data, qwords, qwords, P2_UNPACK_V4_32, useTop);
    }

    // General form of AddUnpackData for the packed VIF formats, where the DMA
    // transfer length and the unpack length differ. 'srcQwords' is what the
    // REF tag carries; 'numElements' is the VIFcode NUM field - the elements
    // *written* to VU memory (one destination qword each for the V4 formats;
    // 256 max). The transfer must hold exactly the payload the unpack
    // consumes: a V4_8 element eats one source word, so numElements must be
    // 4 * srcQwords - any spare words in the transfer would be decoded as
    // VIFcodes (with data-dependent chaos as the result), and the VIF stalls
    // waiting for payload that never comes if the transfer runs short.
    void AddUnpackDataFmt(u32 vuAddr, const void * data, u32 srcQwords, u32 numElements,
                          enum UnpackMode format, bool useTop)
    {
        PS2_AssertMsg(numElements <= 256, "VIF unpacks are limited to 256 elements!");
        PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(data) & 15u) == 0, "Unpack data must be 16-byte aligned!");

        packet2_chain_ref(m_packet, data, srcQwords, 0, 0, 0);
        packet2_vif_stcycl(m_packet, 1, 1, 0);
        packet2_vif_open_unpack(m_packet, format, vuAddr, useTop, /*masked=*/0, /*usigned=*/1, 0);
        packet2_vif_close_unpack_manual(m_packet, numElements);
    }

    // Small unpacks built directly into the chain: open, append qwords, close.
    void OpenInlineUnpack(u32 vuAddr, bool useTop)
    {
        packet2_utils_vu_open_unpack(m_packet, vuAddr, useTop);
    }

    void CloseInlineUnpack()
    {
        packet2_utils_vu_close_unpack(m_packet);
    }

    void AddQword(u64 lo, u64 hi)
    {
        packet2_add_2x_s64(m_packet, static_cast<s64>(lo), static_cast<s64>(hi));
    }

    void AddFloat(float value)
    {
        packet2_add_float(m_packet, value);
    }

    void AddU32(u32 value)
    {
        packet2_add_u32(m_packet, value);
    }

    // FLUSH + MSCAL: waits for any previous run, then starts the microprogram at
    // 'progInstr' (64-bit instruction units; 0 = start of micro memory).
    void AddStartProgram(u32 progInstr)
    {
        packet2_utils_vu_add_start_program(m_packet, progInstr);
    }

    // Trailing FLUSH: stalls VIF1 until the microprogram ends and its XGKICKs
    // drain to the GS, so a DMA wait on this chain covers the VU work too.
    void AddFlush()
    {
        packet2_chain_open_cnt(m_packet, 0, 0, 0);
        packet2_vif_flush(m_packet, 0);
        packet2_vif_nop(m_packet, 0); // pad the CNT block to a whole qword
        packet2_chain_close_tag(m_packet);
    }

    // Terminates the chain. Every chain must end with this before Send().
    void AddEndTag()
    {
        packet2_utils_vu_add_end_tag(m_packet);
    }

    // --------------------------------------------------------------------------------------------
    // DMA kick-off over the VIF1 channel.
    // --------------------------------------------------------------------------------------------

    // Writes the chain - and everything it REFs - back to memory so the DMAC
    // reads what the EE just built. This is FlushCache(0), a kernel syscall
    // that writes back the *whole* data cache, so besides its own cost it
    // also evicts the working set the next batch is about to gather from.
    // Split out from the kick so the two can be measured apart; ps2sdk's
    // flush_cache=1 does exactly this, in this order.
    static void FlushDataCache()
    {
        FlushCache(0);
    }

    // Kicks the chain without touching the cache. Only safe once the data is
    // already coherent - pair with FlushDataCache() or a targeted SyncDCache.
    void Kick()
    {
        dma_channel_send_packet2(m_packet, DMA_CHANNEL_VIF1, /*flush_cache=*/0);
    }

    // Fire and forget (flushes the data cache first); pair with Wait().
    void Send()
    {
        FlushDataCache();
        Kick();
    }

    // Blocks until the chain (including any trailing FLUSH) has been consumed.
    static void Wait()
    {
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    }

private:
    packet2_t * m_packet    = nullptr; // ps2sdk packet2; owns the qword buffer
    int         m_maxQwords = 0;       // allocated size, for EnsureSpace()
};

} // namespace ps2::vu1
