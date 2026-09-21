#pragma once
/* ================================================================================================
 * File: pipeline_dump.h
 * Brief: Prints the state of everything between the EE and the framebuffer, for when a frame
 *        stops finishing.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#if PS2_QUAKE_DEBUG
namespace ps2::debug {

// Dumps the VIF1 DMA channel, VIF1, the GIF and the GS, raw and decoded. 'why' names what was
// being waited for. Cold: only a wait that has already given up should call this.
//
// The three questions it answers, in order of how often they are the answer:
//  - Is VIF1 still waiting on a microprogram (STAT.VEW)? Then a VU1 program is not ending.
//  - Is the GIF still holding a path open (STAT.APATH, OPH)? Then a GS packet never reached EOP.
//  - Has the DMA even finished (D1_CHCR.STR)? Then the chain itself is stuck.
void DumpPipelineState(const char * why) Q_COLD_FUNC;

// Qwords of VU1 data memory, as hex. For reading back what a microprogram left behind - a GS
// packet the GIF choked on, or a clipper's scratch. 'addrQw' and 'count' are in qwords.
void DumpVu1DataMemory(const char * what, int addrQw, int count) Q_COLD_FUNC;

} // namespace ps2::debug
#endif // PS2_QUAKE_DEBUG
