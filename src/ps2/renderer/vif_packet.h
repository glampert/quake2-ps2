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
#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_utils.h>
#include <packet2_vif.h>

namespace ps2::vu1 {

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
        m_packet = packet2_create(static_cast<std::uint16_t>(maxQwords),
                                  P2_TYPE_NORMAL, P2_MODE_CHAIN, /*tte=*/1);
        PS2_AssertMsg(m_packet != nullptr, "packet2_create failed!");
        m_maxQwords = maxQwords;
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

    // Halt visibly if the next emission could overrun the buffer (silent overflow
    // corrupts the heap). 'qwords' is a safe upper bound for what comes next.
    void EnsureSpace(int qwords) const
    {
        PS2_AssertMsg(QwordCount() + qwords <= m_maxQwords,
                      "VIF packet overflow! Bump the packet size.");
    }

    // --------------------------------------------------------------------------------------------
    // Chain building; each appends DMA tags/VIF codes and advances the cursor.
    // --------------------------------------------------------------------------------------------

    // References a VU microprogram into the chain as MPG transfers (chunked to
    // the 256-instruction VIF limit). 'destInstr' is the VU micro memory
    // address in 64-bit instruction units. u32 is the SDK's type for the code
    // words (note: NOT std::uint32_t, which is unsigned long on this ABI).
    void AddMicroProgram(int destInstr, u32 * codeStart, u32 * codeEnd)
    {
        packet2_vif_add_micro_program(m_packet, static_cast<u32>(destInstr),
                                      codeStart, codeEnd);
    }

    // Programs the VIF1 BASE/OFFSET registers that split VU data memory into the
    // two halves the XTOP double buffering alternates between. Both in qwords.
    void AddDoubleBufferSettings(int baseQw, int offsetQw)
    {
        packet2_utils_vu_add_double_buffer(m_packet, static_cast<std::uint16_t>(baseQw),
                                           static_cast<std::uint16_t>(offsetQw));
    }

    // References 'data' in place (REF tag) and unpacks it to VU data memory at
    // 'vuAddr' (qword address; relative to the current double buffer when
    // 'useTop'). The data must be 16-byte aligned and stay untouched until the
    // transfer completes. At most 256 qwords per unpack.
    void AddUnpackData(int vuAddr, const void * data, int qwords, bool useTop)
    {
        PS2_AssertMsg(qwords <= 256, "VIF unpacks are limited to 256 qwords!");
        PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(data) & 15u) == 0, "Unpack data must be 16-byte aligned!");

        packet2_chain_ref(m_packet, data, static_cast<std::uint32_t>(qwords), 0, 0, 0);
        packet2_vif_stcycl(m_packet, 1, 1, 0);
        packet2_vif_open_unpack(m_packet, P2_UNPACK_V4_32, static_cast<std::uint32_t>(vuAddr),
                                useTop, /*masked=*/0, /*usigned=*/1, 0);
        packet2_vif_close_unpack_manual(m_packet, static_cast<std::uint32_t>(qwords));
    }

    // Small unpacks built directly into the chain: open, append qwords, close.
    void OpenInlineUnpack(int vuAddr, bool useTop)
    {
        packet2_utils_vu_open_unpack(m_packet, static_cast<std::uint32_t>(vuAddr), useTop);
    }

    void CloseInlineUnpack()
    {
        packet2_utils_vu_close_unpack(m_packet);
    }

    void AddQword(std::uint64_t lo, std::uint64_t hi)
    {
        packet2_add_2x_s64(m_packet, static_cast<std::int64_t>(lo), static_cast<std::int64_t>(hi));
    }

    void AddFloat(float value)
    {
        packet2_add_float(m_packet, value);
    }

    void AddU32(std::uint32_t value)
    {
        packet2_add_u32(m_packet, value);
    }

    // FLUSH + MSCAL: waits for any previous run, then starts the microprogram at
    // 'progInstr' (64-bit instruction units; 0 = start of micro memory).
    void AddStartProgram(int progInstr)
    {
        packet2_utils_vu_add_start_program(m_packet, static_cast<std::uint32_t>(progInstr));
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

    // Fire and forget (flushes the data cache first); pair with Wait().
    void Send()
    {
        dma_channel_send_packet2(m_packet, DMA_CHANNEL_VIF1, /*flush_cache=*/1);
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
