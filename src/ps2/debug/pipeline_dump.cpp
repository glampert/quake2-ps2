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
#include <cstdio>
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

// Eight rows to a DumpPrintf: on a console each one is a log write, an open, append and close
// that costs ~23 ms over USB, and all of VU1's data memory is 1024 rows.
void DumpQwordRows(const volatile u32 * const words, const int firstQw, const int count)
{
    constexpr int kRowsPerWrite = 8;
    char rows[kRowsPerWrite * 48]; // a row is 45 chars

    int used = 0;
    for (int i = 0; i < count; ++i)
    {
        const volatile u32 * const w = words + (i * 4);
        used += std::snprintf(rows + used, sizeof(rows) - static_cast<size_t>(used),
                              "  %5d: %08x %08x %08x %08x\n", firstQw + i, w[0], w[1], w[2], w[3]);

        if (((i + 1) % kRowsPerWrite) == 0 || i == count - 1)
        {
            DumpPrintf("%s", rows);
            used = 0;
        }
    }
}

// Follows a GIF packet's tags through 'qwords' qwords and says how it ends. A tag that wants more
// data than follows it is a packet the GIF will sit on, holding its path open: on PATH2 that
// stalls VIF1 at its next FLUSH, and nothing else reaches the GS. In a DIRECT block several
// packets may follow one another (each ends at a tag with EOP set), and the block's last one has
// to end inside it. An XGKICK sends one packet: the GIF stops at its first EOP, and whatever
// follows in the window is leftovers, so 'firstPacketOnly' stops there too.
void DescribeGifPacket(const volatile u32 * const words, const u32 qwords, const bool firstPacketOnly,
                       char * const out, const size_t size)
{
    static const char * const kFlg[] = { "PACKED", "REGLIST", "IMAGE", "IMAGE" };

    u32 at = 0;
    int tags = 0;
    int packets = 0;
    bool open = false;
    while (at < qwords)
    {
        const volatile u32 * const t = words + (at * 4);
        const u32 nloop = Bits(t[0], 0, 15);
        const u32 eop   = Bits(t[0], 15, 1);
        const u32 flg   = Bits(t[1], 26, 2);
        const u32 nreg  = (Bits(t[1], 28, 4) != 0) ? Bits(t[1], 28, 4) : 16u;

        // PACKED: a qword per register; REGLIST: two registers to a qword; IMAGE: NLOOP qwords.
        const u32 data = (flg == 0) ? (nloop * nreg) : (flg == 1) ? ((nloop * nreg) + 1) / 2 : nloop;
        ++tags;

        if (at + 1 + data > qwords)
        {
            std::snprintf(out, size,
                          "tag %d at +%u (NLOOP %u %s NREG %u EOP %u) needs %u qwords, %u follow - "
                          "THE GIF WAITS FOR THE REST, HOLDING ITS PATH",
                          tags, at, nloop, kFlg[flg], nreg, eop, data, qwords - at - 1);
            return;
        }

        at += 1 + data;
        open = (eop == 0);
        packets += (eop != 0) ? 1 : 0;

        if (firstPacketOnly && eop != 0)
        {
            std::snprintf(out, size, "%d tags, ending with EOP after %u of the %u qwords", tags, at, qwords);
            return;
        }
    }

    if (open)
    {
        std::snprintf(out, size, "%d tags, %d packets closed; THE LAST ONE HAS NO EOP, so its path stays open",
                      tags, packets);
    }
    else
    {
        std::snprintf(out, size, "%d tags in %d packets, all ending with EOP", tags, packets);
    }
}

// What the last DumpChainTrail found: see LastChainTrailMscal.
static int s_lastTrailMscal = -1;

const char * DmaTagName(const u32 id)
{
    static const char * const kNames[] = { "REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END" };
    return kNames[id & 7u];
}

// One VIFcode, as text. Returns how many of the words after it are its data rather than VIFcodes
// (STMASK takes one, STROW and STCOL four), so the caller doesn't decode those as commands.
int DecodeVifCode(const u32 code, char * const out, const size_t size)
{
    const u32 cmd = Bits(code, 24, 7); // bit 31 is the interrupt request
    const u32 num = Bits(code, 16, 8);
    const u32 imm = Bits(code, 0, 16);

    if ((cmd & 0x60u) == 0x60u)
    {
        static const int kBits[] = { 32, 16, 8, 5 };
        std::snprintf(out, size, "UNPACK V%u-%d num %u addr %u%s%s%s",
                      Bits(cmd, 2, 2) + 1, kBits[Bits(cmd, 0, 2)], num, Bits(imm, 0, 10),
                      Bits(imm, 15, 1) ? " +TOPS" : "", Bits(imm, 14, 1) ? " unsigned" : "",
                      Bits(cmd, 4, 1) ? " masked" : "");
        return 0;
    }

    switch (cmd)
    {
    case 0x00: std::snprintf(out, size, "NOP"); break;
    case 0x01: std::snprintf(out, size, "STCYCL cl %u wl %u", Bits(imm, 0, 8), Bits(imm, 8, 8)); break;
    case 0x02: std::snprintf(out, size, "OFFSET %u", Bits(imm, 0, 10)); break;
    case 0x03: std::snprintf(out, size, "BASE %u", Bits(imm, 0, 10)); break;
    case 0x04: std::snprintf(out, size, "ITOP %u", Bits(imm, 0, 10)); break;
    case 0x05: std::snprintf(out, size, "STMOD %u", Bits(imm, 0, 2)); break;
    case 0x06: std::snprintf(out, size, "MSKPATH3 %u", Bits(imm, 15, 1)); break;
    case 0x07: std::snprintf(out, size, "MARK %04x", imm); break;
    case 0x10: std::snprintf(out, size, "FLUSHE"); break;
    case 0x11: std::snprintf(out, size, "FLUSH"); break;
    case 0x13: std::snprintf(out, size, "FLUSHA"); break;
    case 0x14: std::snprintf(out, size, "MSCAL %u", imm); break;
    case 0x15: std::snprintf(out, size, "MSCALF %u", imm); break;
    case 0x17: std::snprintf(out, size, "MSCNT"); break;
    case 0x20: std::snprintf(out, size, "STMASK"); return 1;
    case 0x30: std::snprintf(out, size, "STROW"); return 4;
    case 0x31: std::snprintf(out, size, "STCOL"); return 4;
    case 0x4A: std::snprintf(out, size, "MPG num %u addr %u", num, imm); break;
    case 0x50: std::snprintf(out, size, "DIRECT %u", (imm != 0) ? imm : 65536u); break;
    case 0x51: std::snprintf(out, size, "DIRECTHL %u", (imm != 0) ? imm : 65536u); break;
    default:   std::snprintf(out, size, "?? %08x", code); break;
    }
    return 0;
}

// One tag of the trail: the tag, its two VIFcodes and, for a DIRECT whose payload follows the
// tag, how the GIF packet in it ends.
void DumpChainTag(const volatile u32 * const chainWords, const int chainQwords, const int qw, const bool atTadr)
{
    const volatile u32 * const t = chainWords + (qw * 4);
    const u32 qwc = Bits(t[0], 0, 16);
    const u32 id  = Bits(t[0], 28, 3);

    char addr[24] = "";
    if (id == 0 || id == 2 || id == 3 || id == 4)
    {
        std::snprintf(addr, sizeof(addr), " addr %08x", t[1] & 0x7FFFFFFFu);
    }

    char code0[64];
    char code1[64];
    const int dataWords = DecodeVifCode(t[2], code0, sizeof(code0));
    if (dataWords > 0)
    {
        std::snprintf(code1, sizeof(code1), "(data %08x)", t[3]);
    }
    else
    {
        DecodeVifCode(t[3], code1, sizeof(code1));
    }

    // Only the second VIFcode can open a DIRECT here: the payload is what follows the tag.
    char gif[200] = "";
    const u32 cmd1 = Bits(t[3], 24, 7);
    const bool payloadFollows = (id == 1 || id == 2 || id == 7);
    if (dataWords == 0 && (cmd1 == 0x50 || cmd1 == 0x51) && payloadFollows && qw + 1 < chainQwords)
    {
        const u32 imm = Bits(t[3], 0, 16);
        u32 direct = (imm != 0) ? imm : 65536u;
        const u32 room = static_cast<u32>(chainQwords - qw - 1);
        direct = (direct < room) ? direct : room;

        char packet[160];
        DescribeGifPacket(chainWords + ((qw + 1) * 4), direct, /*firstPacketOnly=*/false, packet, sizeof(packet));
        std::snprintf(gif, sizeof(gif), "\n             GIF: %s", packet);
    }

    DumpPrintf("  %5d: %-4s qwc %5u%s | %s | %s%s%s\n", qw, DmaTagName(id), qwc, addr, code0, code1,
               atTadr ? "   <- D1_TADR" : "", gif);
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
    // Clamped, since the addresses come from registers read off a pipeline that has gone wrong.
    const int first = (addrQw > 0) ? ((addrQw < kVu1DataMemoryQwords) ? addrQw : kVu1DataMemoryQwords) : 0;
    const int last  = (first + count < kVu1DataMemoryQwords) ? (first + count) : kVu1DataMemoryQwords;

    DumpPrintf("VU1 data memory, %s (qwords %d..%d):\n", what, first, last - 1);

    const volatile u32 * const mem = reinterpret_cast<const volatile u32 *>(kVu1DataMemory);
    DumpQwordRows(mem + (first * 4), first, last - first);
}

Q_COLD_FUNC void DumpVu1GifPacket(const char * const what, const int addrQw, const int maxQwords)
{
    if (addrQw < 0 || addrQw >= kVu1DataMemoryQwords)
    {
        DumpPrintf("VU1 GIF packet, %s: qword %d is outside VU1 data memory.\n", what, addrQw);
        return;
    }

    const int room = kVu1DataMemoryQwords - addrQw;
    const u32 qwords = static_cast<u32>((maxQwords < room) ? maxQwords : room);

    char packet[160];
    const volatile u32 * const mem = reinterpret_cast<const volatile u32 *>(kVu1DataMemory);
    DescribeGifPacket(mem + (addrQw * 4), qwords, /*firstPacketOnly=*/true, packet, sizeof(packet));
    DumpPrintf("VU1 GIF packet, %s (qword %d, %u qwords): %s\n", what, addrQw, qwords, packet);
}

Q_COLD_FUNC void DumpQwords(const char * const what, const void * const qwords, const int firstQw, const int count)
{
    DumpPrintf("%s (qwords %d..%d at %08x):\n", what, firstQw, firstQw + count - 1,
               static_cast<u32>(reinterpret_cast<std::uintptr_t>(qwords)));
    DumpQwordRows(static_cast<const volatile u32 *>(qwords), firstQw, count);
}

Q_COLD_FUNC void DumpChainTrail(const void * const chain, const int chainQwords, const int stopQw, const int maxTags)
{
    constexpr int kMaxTrail = 64;
    const int keep = (maxTags > 0 && maxTags < kMaxTrail) ? maxTags : kMaxTrail;
    int trail[kMaxTrail];

    const volatile u32 * const words = static_cast<const volatile u32 *>(chain);
    const s64 basePhys = static_cast<s64>(reinterpret_cast<std::uintptr_t>(chain) & 0x0FFFFFFFu);

    // From the first qword, tag to tag. Every step must move forward, which also bounds the walk:
    // a NEXT aimed backwards, or a tag the renderer never writes, ends it with the reason.
    const char * stopped = nullptr;
    int walked = 0;
    int qw = 0;
    int lastMscalQw = -1;
    s_lastTrailMscal = -1;
    for (;;)
    {
        if (qw > stopQw)
        {
            stopped = "went past D1_TADR: it is not on a tag boundary of this walk";
            break;
        }

        trail[walked % keep] = qw;
        ++walked;

        // The last MSCAL anywhere on the way, not only in the tags shown: it names the
        // microprogram that ran last, whose packet the VU1 dump decodes. STMASK/STROW/STCOL
        // consume the word after them, so it is data, not a VIFcode.
        const volatile u32 * const t = words + (qw * 4);
        const u32 cmd0 = Bits(t[2], 24, 7);
        const u32 cmd1 = Bits(t[3], 24, 7);
        if (cmd0 == 0x14 || cmd0 == 0x15)
        {
            s_lastTrailMscal = static_cast<int>(Bits(t[2], 0, 16));
            lastMscalQw = qw;
        }
        if ((cmd1 == 0x14 || cmd1 == 0x15) && cmd0 != 0x20 && cmd0 != 0x30 && cmd0 != 0x31)
        {
            s_lastTrailMscal = static_cast<int>(Bits(t[3], 0, 16));
            lastMscalQw = qw;
        }

        if (qw == stopQw)
        {
            break;
        }

        const u32 id = Bits(t[0], 28, 3);
        s64 next = -1;
        switch (id)
        {
        case 1: // CNT
        case 7: // END: the next kick's segment, if any, starts after it
            next = qw + 1 + static_cast<s64>(Bits(t[0], 0, 16));
            break;
        case 2: // NEXT: the renderer's skip over raw storage
            next = ((static_cast<s64>(t[1] & 0x7FFFFFFFu)) - basePhys) / 16;
            break;
        case 3: // REF
        case 4: // REFS
            next = qw + 1;
            break;
        default:
            stopped = "stopped at a tag the renderer never writes";
            break;
        }

        if (stopped != nullptr)
        {
            break;
        }
        if (next <= qw || next >= chainQwords)
        {
            stopped = "stopped: the next tag would be outside the chain or behind this one";
            break;
        }
        qw = static_cast<int>(next);
    }

    const int shown = (walked < keep) ? walked : keep;
    DumpPrintf("Chain trail, the last %d of %d tags walked from qword 0 to D1_TADR (qword %d)%s%s:\n",
               shown, walked, stopQw, (stopped != nullptr) ? ". The walk " : "",
               (stopped != nullptr) ? stopped : "");

    for (int i = walked - shown; i < walked; ++i)
    {
        const int at = trail[i % keep];
        DumpChainTag(words, chainQwords, at, at == stopQw);
    }

    if (s_lastTrailMscal >= 0)
    {
        DumpPrintf("Last MSCAL on the way to D1_TADR: %d, in the tag at qword %d.\n", s_lastTrailMscal, lastMscalQw);
    }
    else
    {
        DumpPrintf("No MSCAL on the way to D1_TADR: the last microprogram ran from an earlier chain.\n");
    }
}

int LastChainTrailMscal()
{
    return s_lastTrailMscal;
}

u32 Vu1DataWord(const int addrQw, const int lane)
{
    if (addrQw < 0 || addrQw >= kVu1DataMemoryQwords || lane < 0 || lane > 3)
    {
        return 0;
    }
    const volatile u32 * const mem = reinterpret_cast<const volatile u32 *>(kVu1DataMemory);
    return mem[(addrQw * 4) + lane];
}

} // namespace ps2::debug
#endif // PS2_QUAKE_DEBUG
