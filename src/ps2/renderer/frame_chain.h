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

#include <cstdint>

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

// Worst case a Kick() appends past whatever the caller has already written: the trailing FLUSH
// block and the END tag. Public because it is part of the capacity arithmetic - a caller
// reserving a sequence that ends in a kick (every draw does) has to count it, or the terminator
// comes out of the next caller's budget.
constexpr int kTerminatorQwords = 4;

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
// **This is the only thing that rewinds a half mid-frame, and that is what makes it the only
// thing that can invalidate a span.** Everything else only ever appends: a Kick submits what has
// been built since the last one and leaves the write cursor where it is, so a REF tag emitted at
// the top of the frame still points at live data at the bottom of it.
//
// The overflow path is the work the renderer used to do for every single batch: terminate what is
// built, kick it, wait for VU1 and the GS to consume it, and rewind. So it is always correct and
// never worse than the old behaviour - but it drains the pipeline, and a true return means every
// pointer into the chain and every piece of per-chain state the caller had set up (the frame
// constants, the current batch's GIF tags) is gone and has to be re-emitted before anything else
// is appended.
//
// Callers that must not be interrupted mid-structure should reserve their whole worst case up
// front rather than reserving piecemeal - and a caller whose data has to outlive its own draw
// (the MD2 shadow redrawing the model's stream) has to reserve that second draw up front too.
bool Reserve(int qwords);

// --------------------------------------------------------------------------------------------
// Payload storage
// --------------------------------------------------------------------------------------------

// Storage inside the chain: payload a REF tag emitted later points at, not part of the tag
// structure, so nothing reads it until something references it and every byte belongs to the
// caller. This is what stopped the gather buffers being file-level statics - a batch claims
// its worst case here, fills what it needs, gives the rest back, and the chunks its Flush
// emits reference the span in place exactly as they used to reference the static.
//
// **Alloc can never rewind, and that is the whole reason it is separate from Reserve.** A rewind
// invalidates every outstanding pointer and everything already built, so it may only happen where
// the caller knows nothing is live. Reserve is that point, and it covers the whole upcoming
// sequence - the payload, the tags that will reference it, and the kick that sends them. That is
// why a caller reserves more than it allocates (see CalcAllocCost and vu1.h's chain budget), and
// why the two cannot be collapsed into one call: they answer different questions.
//
// Allocating outside a reservation that covers it asserts, and overrunning the half Sys_Errors
// rather than corrupting the other one.
//
// **Lifetime: a block is good until the half is rewound**, which is BeginFrame in the ordinary
// case. Not until the next kick, and not until the draw that referenced it returns - which is the
// rule that replaced "draws are synchronous" for anything living in here.

// Qwords one allocation costs on top of its payload: the tag that carries the DMAC over the
// storage rather than through it. A source chain is a tag stream - the qword after a tag's
// payload is read as the next tag - so raw bytes cannot simply be left sitting in it.
constexpr int kAllocOverheadQwords = 1;

namespace detail {
// Whole-qword primitives behind the typed forms below, which are what callers should use.
// Public only because the templates are defined in this header. 'committable' is whether the
// block may later be cut back by Commit, which is what tells the two allocation patterns apart.
void * AllocQwords(int qwords, bool committable);
void CommitQwords(void * base, int usedQwords);

template<typename T>
constexpr int QwordsFor(const int count)
{
    const size_t bytes = static_cast<size_t>(count) * sizeof(T);
    return static_cast<int>((bytes + 15u) / 16u);
}

template<typename T>
T * TypedAlloc(const int count, const bool committable)
{
    // The chain hands out qword-aligned blocks and nothing more: the base is 64-byte aligned
    // and every allocation is a whole number of qwords, so a type wanting more alignment than
    // a qword is one the chain cannot place. Caught here rather than as a DMA fault later.
    static_assert(alignof(T) <= 16, "chain::Alloc cannot align this type");

    PS2_Assert(count > 0);
    void * const mem = AllocQwords(QwordsFor<T>(count), committable);

    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(mem) & (alignof(T) - 1u)) == 0,
                  "chain::Alloc handed back a block this type cannot use!");
    return static_cast<T *>(mem);
}
} // namespace detail

// Qwords an allocation of 'count' objects consumes: the payload rounded up to whole qwords,
// plus its tag. This is the figure to hand Reserve, together with whatever the caller will
// append after it.
template<typename T>
constexpr int CalcAllocCost(const int count)
{
    return detail::QwordsFor<T>(count) + kAllocOverheadQwords;
}

// Room for exactly 'count' objects of T, for a caller that knows the size before it writes
// anything - the particle list, the per-draw constant blocks. There is nothing to give back,
// so there is no Commit to forget, and Commit on one of these asserts.
//
// Rounds up to whole qwords, so a type smaller than a qword (the MD2 keyframe pairs) may leave
// a pad element at the end - transferred, never read.
template<typename T>
T * Alloc(const int count)
{
    return detail::TypedAlloc<T>(count, /*committable=*/false);
}

// Room for up to 'count' objects, for a gather that only knows its real size when it finishes -
// the triangle batches. **Must be followed by Commit**, which cuts the block back to what was
// written and hands the rest of the chain back; until then the write cursor sits above the
// whole worst case and nothing else may allocate.
template<typename T>
T * AllocMax(const int count)
{
    return detail::TypedAlloc<T>(count, /*committable=*/true);
}

// Cuts the most recent AllocMax back to 'usedCount', in the units it was made in.
//
// 'base' must still be the top of the chain - nothing may have been appended since, because the
// point of this is to move the write cursor back down to where the data actually ends - and it
// must have come from AllocMax rather than Alloc. Both are asserted.
template<typename T>
void Commit(T * const base, const int usedCount)
{
    PS2_Assert(usedCount >= 0);
    detail::CommitQwords(base, detail::QwordsFor<T>(usedCount));
}

// --------------------------------------------------------------------------------------------
// Submission
// --------------------------------------------------------------------------------------------

// Submits everything built since the last Kick() as a chain of its own: terminates that segment
// with a trailing FLUSH so a DMA wait covers the VU runs and their XGKICKs, writes the data cache
// back, and kicks it at VIF1. Does nothing when nothing new has been built.
//
// Segment-at-a-time rather than whole-buffer, because the write cursor never goes back: a half
// holds one frame's worth of chain built front to back, and each kick sends the slice the last
// one did not. The terminator is written into the chain at the cursor and the next segment starts
// after it, which is what kTerminatorQwords costs.
void Kick();

// Blocks until the chain the last Kick() sent has been fully consumed.
void WaitIdle();

// Kick + WaitIdle, for a caller that needs the GS to have caught up before it changes something
// the queued draws depend on - an upload into evicted VRAM, a lightmap atlas rewrite, a CLUT
// refresh. Returns false if there was nothing to drain.
//
// Does **not** rewind: the pipeline empties, but everything built stays where it is and every
// pointer into it stays good. That is what lets a draw's vertex data outlive its own submission,
// which the MD2 shadow's redraw of the model's stream needs. The chain is rewound at BeginFrame,
// by Reserve's overflow path, and by DrainBeforeWorldLoad - nowhere else.
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

// Bytes the frame just finished built, counting what an overflow rewind threw away. Against
// kFrameChainBytes this is the number that says whether a frame fits a half - PeakBytes() only
// ever reports what one half held at once, which is the same thing until the day it overflows.
u32 BytesLastFrame();

// Chains kicked, and overflow drains taken, during the frame just finished. One kick and zero
// emergency drains is the good case; the drain firing every frame means the capacity is too small.
int KicksLastFrame();
int EmergencyDrainsLastFrame();

} // namespace ps2::chain
