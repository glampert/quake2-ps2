#pragma once
/* ================================================================================================
 * File: vif_packet.h
 * Brief: VifPacket wraps a ps2sdk packet2 DMA source chain aimed at VIF1, the path used to
 *        feed VU1: microprogram upload (MPG), data unpacks into VU memory and program kicks
 *        (MSCAL). Sibling of RenderPacket, which drives the GIF/PATH3 2D path. Thin wrappers
 *        only: what goes into VU memory stays with the caller.
 *
 *  Non-owning, and deliberately so. The chain being built belongs to ps2::chain
 *  (frame_chain.h), which owns the memory, the rewind, the terminator and the kick - a
 *  VifPacket is two words of stack naming a packet2_t and how much of it the caller may
 *  write, so a draw call makes one, builds with it and throws it away. That is why there is
 *  no Send(), no Wait() and no Reset(): submission is a property of the frame, not of a packet.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <cstdint>
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
    // 'maxQwords' is what the caller may write, which is not the buffer's size: the chain holds
    // back room for the terminator it must always be able to append, so pass chain::Capacity().
    VifPacket(packet2_t * const packet, const int maxQwords)
        : m_packet{ packet }
        , m_maxQwords{ maxQwords }
    {
        PS2_AssertMsg(packet != nullptr && maxQwords > 0, "VifPacket needs a packet to build into!");
    }

    // Qwords written into the underlying packet since it was last rewound.
    int QwordCount() const
    {
        return static_cast<int>(packet2_get_qw_count(m_packet));
    }

    int QwordCapacity() const
    {
        return m_maxQwords;
    }

    // Halt visibly if the next emission would overrun the buffer.
    // 'qwords' is a safe upper bound for what comes next (DEBUG ONLY).
    //
    // This is the backstop, not the mechanism: a draw too large for one chain is
    // chain::Reserve's job, and it drains rather than failing. What is left for
    // this to catch is a chunk emitter writing more than the footprint constant
    // it declares - a code bug, but one whose symptom is DMA tags written over
    // the other chain half, which then fails somewhere unrelated.
    void EnsureSpace(const int qwords) const
    {
#if PS2_QUAKE_ASSERTS
        if (QwordCount() + qwords > m_maxQwords) [[unlikely]]
        {
            Sys_Error("VIF packet overflow: %d qwords in use + %d needed exceeds the "
                      "%d a chain half can hold.", QwordCount(), qwords, m_maxQwords);
        }
#endif // PS2_QUAKE_ASSERTS
    }

    // --------------------------------------------------------------------------------------------
    // Chain building; each appends DMA tags/VIF codes and advances the cursor.
    // --------------------------------------------------------------------------------------------

    // References a VU microprogram into the chain as MPG transfers (chunked to
    // the 256-instruction VIF limit). 'destInstr' is the VU micro memory
    // address in 64-bit instruction units.
    void AddMicroProgram(const u32 destInstr, const VUCode code)
    {
        packet2_vif_add_micro_program(m_packet, destInstr, code.start, code.end);
    }

    // FLUSH + MSCAL: waits for any previous run, then starts the microprogram at
    // 'progInstr' (64-bit instruction units; 0 = start of micro memory).
    void AddStartProgram(const u32 progInstr)
    {
        packet2_utils_vu_add_start_program(m_packet, progInstr);
    }

    // Programs the VIF1 BASE/OFFSET registers that split VU data memory into the
    // two halves the XTOP double buffering alternates between. Both in qwords.
    void AddDoubleBufferSettings(const u32 baseQw, const u32 offsetQw)
    {
        packet2_utils_vu_add_double_buffer(m_packet,
                                           static_cast<u16>(baseQw),
                                           static_cast<u16>(offsetQw));
    }

    // References 'data' in place (REF tag) and unpacks it to VU data memory at
    // 'vuAddr' (qword address; relative to the current double buffer when
    // 'useTop'). The data must be 16-byte aligned and stay untouched until the
    // transfer completes. At most 256 qwords per unpack.
    void AddUnpackData(const u32 vuAddr, const void * data, const u32 qwords, const bool useTop)
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
    void AddUnpackDataFmt(const u32 vuAddr, const void * data, const u32 srcQwords, const u32 numElements,
                          const enum UnpackMode format, const bool useTop)
    {
        PS2_AssertMsg(numElements <= 256, "VIF unpacks are limited to 256 elements!");
        PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(data) & 15u) == 0, "Unpack data must be 16-byte aligned!");

        packet2_chain_ref(m_packet, data, srcQwords, 0, 0, 0);
        packet2_vif_stcycl(m_packet, 1, 1, 0);
        packet2_vif_open_unpack(m_packet, format, vuAddr, useTop, /*masked=*/0, /*usigned=*/1, 0);
        packet2_vif_close_unpack_manual(m_packet, numElements);
    }

    // Small unpacks built directly into the chain: open, append qwords, close.
    void OpenInlineUnpack(const u32 vuAddr, const bool useTop)
    {
        packet2_utils_vu_open_unpack(m_packet, vuAddr, useTop);
    }

    void CloseInlineUnpack()
    {
        packet2_utils_vu_close_unpack(m_packet);
    }

    void AddQword(const u64 lo, const u64 hi)
    {
        packet2_add_2x_s64(m_packet, static_cast<s64>(lo), static_cast<s64>(hi));
    }

    void AddFloat(const float value)
    {
        packet2_add_float(m_packet, value);
    }

    void AddU32(const u32 value)
    {
        packet2_add_u32(m_packet, value);
    }

private:
    packet2_t * m_packet;    // the chain being built; not owned.
    const int   m_maxQwords; // what the owner says may be written, for EnsureSpace().
};

} // namespace ps2::vu1
