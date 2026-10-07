/* ================================================================================================
 * File: pipeline_dump.cpp
 * Brief: Prints the state of everything between the EE and the framebuffer. See pipeline_dump.h.
 *
 *  Bit layouts are the EE User's Manual's. Every register is printed raw as well as decoded, so a
 *  decode that drifts from the hardware still leaves the number it was read from.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/debug/pipeline_dump.h"
#include "ps2/debug/log_file.h" // DumpPrintf

#include <cstdint>
#include <tamtypes.h>
#include <vif_registers.h>
#include <ee_regs.h>
#include <gs_privileged.h>

namespace ps2::debug {
namespace {

// VU1 data memory, seen from the EE. The SDK's VU1_MEM1_START / VU1_MICROMEM1_START disagree with
// the EE memory map on which of these two is which, so the map is used and the names say what
// they hold: micro memory first, then data memory.
constexpr u32 kVu1DataMemory = 0x1100C000;

Q_ALWAYS_INLINE u32 Reg(const u32 address)
{
    return *reinterpret_cast<volatile u32 *>(address);
}

Q_ALWAYS_INLINE u32 Bits(const u32 value, const int first, const int count)
{
    return (value >> first) & ((1u << count) - 1u);
}

void DumpVif1Dma()
{
    const u32 chcr = Reg(A_EE_D1_CHCR);

    DumpPrintf("  D1_CHCR  %08x  STR=%u (%s)  MOD=%u (%s)  TTE=%u  TIE=%u  ASP=%u\n",
               chcr, Bits(chcr, 8, 1), Bits(chcr, 8, 1) ? "still running" : "finished",
               Bits(chcr, 2, 2),
               Bits(chcr, 2, 2) == 0 ? "normal" : Bits(chcr, 2, 2) == 1 ? "chain" : "interleave",
               Bits(chcr, 6, 1), Bits(chcr, 7, 1), Bits(chcr, 4, 2));
    DumpPrintf("  D1_MADR  %08x   D1_QWC %5u   D1_TADR %08x\n",
               Reg(A_EE_D1_MADR), Reg(A_EE_D1_QWC), Reg(A_EE_D1_TADR));
}

void DumpVif1()
{
    const u32 stat = Reg(A_EE_VIF1_STAT);
    const u32 code = Reg(A_EE_VIF1_CODE);

    static const char * const kVps[] = { "idle", "waiting for data", "decoding VIFcode", "transferring" };

    DumpPrintf("  VIF1_STAT %08x  VPS=%s  FQC=%u qw\n",
               stat, kVps[Bits(stat, 0, 2)], Bits(stat, 24, 5));
    DumpPrintf("            VEW=%u%s  VGW=%u%s  ER0=%u  ER1=%u  VSS=%u VFS=%u VIS=%u  DBF=%u\n",
               Bits(stat, 2, 1), Bits(stat, 2, 1) ? " <- WAITING ON A VU1 MICROPROGRAM" : "",
               Bits(stat, 3, 1), Bits(stat, 3, 1) ? " <- WAITING ON THE GIF" : "",
               Bits(stat, 12, 1), Bits(stat, 13, 1),
               Bits(stat, 8, 1), Bits(stat, 9, 1), Bits(stat, 10, 1), Bits(stat, 7, 1));

    // The VIFcode it stopped on. CMD is what matters: 0x14 is MSCAL, 0x17 MSCNT, 0x11 FLUSH,
    // 0x10 FLUSHE, 0x13 FLUSHA, 0x50 DIRECT, 0x4A MPG, 0x6x-0x7x UNPACK.
    DumpPrintf("  VIF1_CODE %08x  CMD=%02x  NUM=%u  IMM=%04x   VIF1_NUM=%u\n",
               code, Bits(code, 24, 8), Bits(code, 16, 8), Bits(code, 0, 16), Reg(A_EE_VIF1_NUM));
    DumpPrintf("  VIF1_ERR  %08x  VIF1_MARK %08x  BASE=%u OFST=%u TOPS=%u TOP=%u\n",
               Reg(A_EE_VIF1_ERR), Reg(A_EE_VIF1_MARK), Reg(A_EE_VIF1_BASE),
               Reg(A_EE_VIF1_OFST), Reg(A_EE_VIF1_TOPS), Reg(A_EE_VIF1_TOP));
}

// The GIF's own DMA channel, which is what feeds PATH3 - the texture uploads here. VIF1's FLUSH
// waits on PATH3 as well as on the microprograms, so a stalled channel 2 deadlocks channel 1
// without either of them looking wrong on its own.
void DumpGifDma()
{
    const u32 chcr = Reg(A_EE_D2_CHCR);

    DumpPrintf("  D2_CHCR  %08x  STR=%u (%s)  MOD=%u  TTE=%u   D2_MADR %08x  D2_QWC %5u  D2_TADR %08x\n",
               chcr, Bits(chcr, 8, 1), Bits(chcr, 8, 1) ? "still running" : "finished",
               Bits(chcr, 2, 2), Bits(chcr, 6, 1),
               Reg(A_EE_D2_MADR), Reg(A_EE_D2_QWC), Reg(A_EE_D2_TADR));
}

void DumpGif()
{
    const u32 stat = Reg(A_EE_GIF_STAT);
    const u32 path = Bits(stat, 10, 2);

    static const char * const kPath[] = { "idle", "PATH1 (VU1 XGKICK)", "PATH2 (VIF1 DIRECT)", "PATH3 (GIF DMA)" };

    DumpPrintf("  GIF_STAT %08x  APATH=%s  OPH=%u%s  FQC=%u qw\n",
               stat, kPath[path], Bits(stat, 9, 1),
               Bits(stat, 9, 1) ? " (a path is open - a packet has not reached EOP)" : "",
               Bits(stat, 24, 5));
    DumpPrintf("           P1Q=%u P2Q=%u P3Q=%u  IP3=%u  PSE=%u\n",
               Bits(stat, 8, 1), Bits(stat, 7, 1), Bits(stat, 6, 1),
               Bits(stat, 5, 1), Bits(stat, 3, 1));

    // The tag the GIF is chewing on. A wrong NLOOP shows up here as a count that will not run out.
    // The EE manual has these readable only while the GIF is paused (GIF_CTRL.PSE), and
    // PCSX2 doesn't care, so an unpaused read could look fine in the emulator and be junk on
    // a console. Nothing is going to move the GIF again after this, but it is resumed anyway.
    *R_EE_GIF_CTRL = 1u << 3; // PSE
    const u32 tag0 = Reg(A_EE_GIF_TAG0);
    DumpPrintf("  GIF_TAG  %08x %08x %08x %08x   NLOOP=%u EOP=%u  GIF_CNT %08x\n",
               tag0, Reg(A_EE_GIF_TAG1), Reg(A_EE_GIF_TAG2), Reg(A_EE_GIF_TAG3),
               Bits(tag0, 0, 15), Bits(tag0, 15, 1), Reg(A_EE_GIF_CNT));
    DumpPrintf("  GIF_P3TAG %08x  NLOOP=%u EOP=%u  GIF_P3CNT %08x (PATH3's interrupted image)\n",
               Reg(A_EE_GIF_P3TAG), Bits(Reg(A_EE_GIF_P3TAG), 0, 15),
               Bits(Reg(A_EE_GIF_P3TAG), 15, 1), Reg(A_EE_GIF_P3CNT));
    *R_EE_GIF_CTRL = 0;
}

// VU1's run state. The EE has no register of its own for it: it is VU0's control register 29
// (VPU-STAT), read in macro mode. VBS1 is the bit to trust; VGW1 (VU1 stalled in an XGKICK,
// waiting for the GIF) is the VU User's Manual's bit 12, unverified here.
void DumpVu1()
{
    u32 vpuStat;
    asm volatile("cfc2 %0, $29" : "=r"(vpuStat));

    DumpPrintf("  VPU_STAT %08x  VU1: VBS1=%u%s  VDS1=%u VTS1=%u VFS1=%u  VGW1=%u%s\n",
               vpuStat, Bits(vpuStat, 8, 1),
               Bits(vpuStat, 8, 1) ? " <- A MICROPROGRAM IS STILL RUNNING" : " (idle)",
               Bits(vpuStat, 9, 1), Bits(vpuStat, 10, 1), Bits(vpuStat, 11, 1),
               Bits(vpuStat, 12, 1), Bits(vpuStat, 12, 1) ? " <- STALLED IN AN XGKICK" : "");
}

void DumpQwordRows(const volatile u32 * const words, const int firstQw, const int count)
{
    for (int i = 0; i < count; ++i)
    {
        const volatile u32 * const w = words + (i * 4);
        DumpPrintf("  %5d: %08x %08x %08x %08x\n", firstQw + i, w[0], w[1], w[2], w[3]);
    }
}

} // namespace

Q_COLD_FUNC void DumpPipelineState(const char * const why)
{
    DumpPrintf("=== RENDER PIPELINE STALLED: %s ===\n", why);
    DumpVif1Dma();
    DumpVif1();
    DumpVu1();
    DumpGifDma();
    DumpGif();
    DumpPrintf("  GS_CSR   %08x  FINISH=%u  VSINT=%u\n",
               static_cast<u32>(*GS_REG_CSR), Bits(static_cast<u32>(*GS_REG_CSR), 1, 1),
               Bits(static_cast<u32>(*GS_REG_CSR), 3, 1));
    DumpPrintf("=== end of pipeline registers ===\n");
}

Q_COLD_FUNC void DumpVu1DataMemory(const char * const what, const int addrQw, const int count)
{
    DumpPrintf("VU1 data memory, %s (qwords %d..%d):\n", what, addrQw, addrQw + count - 1);

    const volatile u32 * const mem = reinterpret_cast<const volatile u32 *>(kVu1DataMemory);
    DumpQwordRows(mem + (addrQw * 4), addrQw, count);
}

Q_COLD_FUNC void DumpQwords(const char * const what, const void * const qwords, const int firstQw, const int count)
{
    DumpPrintf("%s (qwords %d..%d at %08x):\n", what, firstQw, firstQw + count - 1,
               static_cast<u32>(reinterpret_cast<std::uintptr_t>(qwords)));
    DumpQwordRows(static_cast<const volatile u32 *>(qwords), firstQw, count);
}

} // namespace ps2::debug
#endif // PS2_QUAKE_DEBUG
