#pragma once
/* ================================================================================================
 * File: pipeline_dump.h
 * Brief: Prints the state of everything between the EE and the framebuffer, for when a frame
 *        stops finishing. Everything goes to stdout and to the log file (DumpPrintf), since a
 *        console run has only the log.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include <tamtypes.h>

#if PS2_QUAKE_DEBUG
namespace ps2::debug {

// Dumps the VIF1 DMA channel, VIF1, VU1's run state, the GIF and the GS, raw and decoded. 'why'
// names what was being waited for. Cold: only a wait that has already given up should call this.
//
// The three questions it answers, in order of how often they are the answer:
//  - Is VIF1 still waiting on a microprogram (STAT.VEW)? Then a VU1 program is not ending, and
//    VPU_STAT says whether it is still running or stuck handing a packet to the GIF.
//  - Is the GIF still holding a path open (STAT.APATH, OPH)? Then a GS packet never reached EOP.
//  - Has the DMA even finished (D1_CHCR.STR)? Then the chain itself is stuck.
void DumpPipelineState(const char * why) Q_COLD_FUNC;

// VU1 data memory is 16 KB.
constexpr int kVu1DataMemoryQwords = 1024;

// Qwords of VU1 data memory, as hex. For reading back what a microprogram left behind - a GS
// packet the GIF choked on, or a clipper's scratch. 'addrQw' and 'count' are in qwords.
void DumpVu1DataMemory(const char * what, int addrQw, int count) Q_COLD_FUNC;

// Follows the GIF tags of the packet an XGKICK at 'addrQw' would send, through at most
// 'maxQwords' of VU1 data memory, and says whether it ends there with EOP: for the GS packets the
// microprograms build.
void DumpVu1GifPacket(const char * what, int addrQw, int maxQwords) Q_COLD_FUNC;

// The last 'maxTags' (up to 64) DMA tags of a source chain, from its first qword up to the tag at
// 'stopQw' (where D1_TADR stopped), decoded: tag id, QWC, ADDR and the two VIFcodes each tag
// carries with TTE on - UNPACK targets, MSCAL addresses, FLUSHes and DIRECT sizes. A DIRECT's
// payload is followed through its GIF tags too, since a PATH2 packet that never reaches EOP is
// one of the ways a frame hangs. The walk follows CNT, NEXT, REF and END, across kicks.
void DumpChainTrail(const void * chain, int chainQwords, int stopQw, int maxTags) Q_COLD_FUNC;

// The MSCAL address of the last microprogram the most recent DumpChainTrail walked past, or -1
// if there was none before D1_TADR (the last program then ran from an earlier chain).
int LastChainTrailMscal();

// One 32-bit lane of a VU1 data memory qword, for reading a batch header in a dump.
u32 Vu1DataWord(int addrQw, int lane);

// Qwords of EE memory, as hex. 'qwords' points at qword number 'firstQw' of whatever buffer it
// is in, so the rows carry their place in that buffer - the frame chain around where the DMAC
// stopped.
void DumpQwords(const char * what, const void * qwords, int firstQw, int count) Q_COLD_FUNC;

} // namespace ps2::debug
#endif // PS2_QUAKE_DEBUG
