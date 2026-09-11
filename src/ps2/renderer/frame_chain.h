#pragma once
/* ================================================================================================
 * File: frame_chain.h
 * Brief: The frame's single DMA source chain: one double-buffered block that holds everything
 *        the GS is told to do this frame - DMA tags, VIF codes, GIF tags, the clear, the 2D
 *        overlay and every vertex - written straight into it by the gather loops.
 *
 *  Replaces the per-module gather statics and the per-batch kick. A batch opens an inline
 *  unpack, hands the gather a pointer at the write cursor, fills it and closes the tag, so
 *  vertex payload rides in the chain as CNT-tag data rather than being built in a static and
 *  referenced out of it by a REF tag.
 *
 *  Layout of one half:
 *
 *      FrameChain half (kFrameChainBytes)
 *      |-- DIRECT block  -- the frame clear
 *      |-- REF unpack    -- FrameConstants + LightConstants (once, not per batch)
 *      |-- CNT unpack    -- batch header + 7 GIF tag qwords     \  one chunk,
 *      |-- CNT unpack    -- 96 x DrawVertex, written in place   |  <= 96 verts
 *      |-- MSCAL         -- run the microprogram                /
 *      |-- ... ~130 more chunks ...
 *      |-- DIRECT block  -- the 2D/HUD overlay
 *      `-- FLUSH + END
 *
 *  Where the memory comes from: both halves live inside the world loader's lump scratch
 *  (mod::WorldScratchBlock), which is claimed only while a .bsp is being parsed and is dead
 *  for the whole of gameplay. No rendering happens during a load and no load happens during a
 *  frame, so the region serves two owners that never overlap in time - and the renderer
 *  allocates nothing of its own. DrainBeforeWorldLoad is the interlock between the two.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vif_packet.h"

namespace ps2::chain {

// Bytes in each of the two halves. Both live in the world loader's lump scratch, so this is
// bounded by kWorldScratchCapacity / 2 - model_load.cpp static_asserts the pair against each
// other, and raising this means raising that.
//
// 512 KB covers roughly the median frame (about 540 KB of chain for a 4000-triangle view), so
// a typical frame kicks twice rather than the 50+ times the per-batch submission used to. The
// number to steer by is PeakBytes() against the capacity, plus EmergencyDrainsLastFrame(): the
// overflow path is correct but it costs a full pipeline drain, so it should be rare.
constexpr u32 kFrameChainBytes  = 512u * 1024u;
constexpr u32 kFrameChainQwords = kFrameChainBytes / 16u;

// Points the two halves at the loader scratch and opens the first one. Call once at renderer
// init, after mod::Init() - the arena the halves live in is reserved from there.
void Init();

// Swaps halves and rewinds the write cursor. Nothing written before this survives.
void BeginFrame();

// Rolls the high-water marks and latches the per-frame counters the overlay reads. Does not
// kick: terminating and submitting the frame's chain is the caller's call (see gs::EndFrame).
void EndFrame();

// The chain being built. Valid between BeginFrame and EndFrame.
vu1::VifPacket Packet();

// Qwords written into the current half so far.
int QwordCount();

// Qwords a caller may write into a half. Not kFrameChainQwords: the chain always holds back
// room for the terminator Kick() appends, because a half with no room left for its own END tag
// could not be drained. Reserve() measures against exactly this, and it is the capacity to hand
// any helper that range-checks its own writes.
int QwordCapacity();

// Makes room for 'qwords' more, and says whether it had to empty the chain to do it.
//
// The overflow path is the same work the renderer used to do for every single batch: terminate
// what is built, kick it, wait for VU1 and the GS to consume it, and rewind. So it is always
// correct and never worse than the old behaviour - but it drains the pipeline, and a true
// return means every piece of per-chain state the caller had set up (the frame constants, the
// current batch's GIF tags) is gone and has to be re-emitted before anything else is appended.
//
// Callers that must not be interrupted mid-structure should reserve their whole worst case up
// front rather than reserving piecemeal.
bool Reserve(int qwords);

// Terminates the chain with a trailing FLUSH so a DMA wait covers the VU runs and their
// XGKICKs, writes the data cache back, and kicks it at VIF1. Does nothing on an empty chain.
void Kick();

// Blocks until the chain the last Kick() sent has been fully consumed.
void WaitIdle();

// Kick + WaitIdle + rewind, for a caller that needs the GS to have caught up before it changes
// something the queued draws depend on - an upload into evicted VRAM, a lightmap atlas rewrite,
// a CLUT refresh. Returns false if there was nothing to drain.
bool Drain();

// The interlock that lets the halves live in the loader's lump scratch: waits for anything in
// flight and abandons whatever is half-built, because the memory underneath is about to become
// the .bsp lump staging buffer. Called from LoadBrushModel before it claims the scratch.
void DrainBeforeWorldLoad();

// --------------------------------------------------------------------------------------------
// Debug counters
// --------------------------------------------------------------------------------------------

// Most bytes either half has ever held, against kFrameChainBytes. The two together are what
// says whether the capacity is right.
u32 PeakBytes();

// Chains kicked, and overflow drains taken, during the frame just finished. One kick and zero
// emergency drains is the good case; the drain firing every frame means the capacity is too small.
int KicksLastFrame();
int EmergencyDrainsLastFrame();

} // namespace ps2::chain
