#pragma once
/* ================================================================================================
 * File: cmd_buffer.h
 * Brief: The frame's single DMA source chain: one double-buffered block that holds everything
 *        the GS is told to do this frame - DMA tags, VIF codes, GIF tags, the clear, the 2D
 *        overlay and every vertex - written straight into it by the gather loops.
 *
 *  Layout of one half:
 *
 *      Command buffer half (kHalfBytes)
 *      |-- DIRECT block  -- the frame clear
 *      |-- FLUSH + REF   -- FrameConstants, then the per-draw block: LightConstants, or
 *      |                    LerpConstants for an MD2 draw (once per draw, not per chunk)
 *      |-- CNT unpack    -- batch header + parameters + 7 GIF tag qwords   \  one chunk,
 *      |-- REF unpack    -- <= 90 DrawVertex, gathered into this buffer    |  one VU run
 *      |                    or referenced where the loader baked them      |
 *      |-- FLUSH + MSCAL -- run the microprogram                           /
 *      |-- ... more chunks, then more draws ...
 *      |-- DIRECT block  -- the 2D/HUD overlay
 *      `-- FLUSH + FINISH + END -- the terminator, appended by the one kick at rs::EndFrame
 *
 *  An MD2 chunk carries <= 60 vertices in two REF unpacks rather than one - the keyframe bytes,
 *  gathered here, and the attributes, straight from the model hunk - which the VIF interleaves
 *  as it writes them (rs::AddLerpBatchChunk). A particle chunk is one REF of billboards. A 3D
 *  draw's GS packets are never in this buffer: the microprogram builds them in VU memory.
 *
 *  Both halves live inside the world loader's lump scratch (mod::WorldScratchBlock), which is
 *  claimed only while a .bsp is parsed and is dead for the whole of gameplay: no rendering
 *  during a load, no load during a frame, so the region serves two owners that never overlap
 *  and the renderer allocates nothing of its own. DrainBeforeWorldLoad is the interlock.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/profile.h"

#include <cstdint>
#include <packet2.h>

namespace ps2::cmdbuf {

// Bytes in each of the two halves, bounded by kWorldScratchCapacity / 2 (model_load.cpp
// static_asserts the pair, so raising this means raising that).
//
// A frame builds 420 KB of chain on average over the perf demos and 681 KB at p95, so this holds
// most frames whole and the rest take one overflow rewind - about 1.5 kicks a frame. The numbers
// to steer by are BytesLastFrame(), which counts what a rewind threw away and so says whether a
// frame *fits*, and EmergencyDrainsLastFrame().
constexpr u32 kHalfBytes  = 512u * 1024u;
constexpr u32 kHalfQwords = kHalfBytes / 16u;

// Worst case a Kick() appends past what the caller wrote: the trailing FLUSH and the GS fence it
// carries (1 qword of tag and VIFcodes, 2 of DIRECT payload) plus the END tag. Public because a
// caller reserving a sequence that ends in a kick has to count it, or the terminator comes out of
// the next caller's budget.
constexpr int kTerminatorQwords = 4;

// Points the two halves at the loader scratch and opens the first one. Call once at renderer
// init, after mod::Init() - the arena the halves live in is reserved from there.
void Init(void * memory, u32 memorySizeBytes);

// Swaps halves and rewinds the write cursor. Nothing written before this survives.
void BeginFrame();

// Rolls the high-water marks and latches the per-frame counters the overlay reads. Does not
// kick: terminating and submitting the frame's chain is the caller's call (see rs::EndFrame).
void EndFrame();

namespace detail {
// The half being built, swapped by BeginFrame. Exposed so the accessors below can be inline -
// they are on the hot path of every emission - and written only by cmd_buffer.cpp.
extern packet2_t * g_packet;
} // namespace detail

// The half being built. Valid between BeginFrame and EndFrame.
//
// No "Init was called" assert here: this is on the hot path of every emission, where the check
// would expand hundreds of times a frame. Reserve() carries it instead, and every draw goes
// through that before it emits anything.
Q_ALWAYS_INLINE packet2_t * Packet()
{
    return detail::g_packet;
}

// Qwords written into the current half so far.
Q_ALWAYS_INLINE int QwordCount()
{
    return static_cast<int>(packet2_get_qw_count(Packet()));
}

// Qwords a caller may write into a half. Not kHalfQwords: the buffer always holds back room for
// the terminator Kick() appends, because a half with no room left for its own END tag could not
// be drained. Reserve() measures against exactly this, and it is the capacity to hand any helper
// that range-checks its own writes.
constexpr int QwordCapacity()
{
    return static_cast<int>(kHalfQwords) - kTerminatorQwords;
}

// Makes room for 'qwords' more, and says whether it had to empty the chain to do it.
//
// **This is the only thing that rewinds a half mid-frame, and that is what makes it the only
// thing that can invalidate a span.** Everything else only ever appends: a Kick submits what has
// been built since the last one and leaves the write cursor where it is, so a REF tag emitted at
// the top of the frame still points at live data at the bottom of it.
//
// The overflow path terminates what is built, kicks it, waits for VU1 and the GS, and rewinds. A
// true return therefore means every pointer into the chain and every piece of per-chain state the
// caller set up (the frame constants, the current batch's GIF tags) is gone and must be re-emitted
// before anything else is appended.
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
// caller. A gather claims its worst case here, fills what it needs and gives the rest back.
//
// **Alloc can never rewind, and that is why it is separate from Reserve.** A rewind invalidates
// every outstanding pointer, so it may only happen where the caller knows nothing is live -
// Reserve is that point, and it covers the whole upcoming sequence: the payload, the tags that
// will reference it, and the kick that sends them. Hence a caller reserving more than it
// allocates (see CalcAllocCost and rs's chain budget).
//
// Allocating outside a reservation that covers it asserts; overrunning the half Sys_Errors rather
// than corrupting the other one.
//
// **Lifetime: a block is good until the half is rewound**, normally BeginFrame. Not until the next
// kick, and not until the draw that referenced it returns.

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
    static_assert(alignof(T) <= 16, "cmdbuf::Alloc cannot align this type");

    PS2_Assert(count > 0);
    void * const mem = AllocQwords(QwordsFor<T>(count), committable);

    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(mem) & (alignof(T) - 1u)) == 0,
                  "cmdbuf::Alloc handed back a block this type cannot use!");
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

// Room for exactly 'count' objects, for a caller that knows the size before it writes anything.
// Nothing to give back, so Commit on one of these asserts.
//
// Rounds up to whole qwords, so a type smaller than a qword may leave a pad element at the end -
// transferred, never read.
template<typename T>
T * Alloc(const int count)
{
    return detail::TypedAlloc<T>(count, /*committable=*/false);
}

// Room for up to 'count' objects, for a gather that only knows its real size when it finishes.
// **Must be followed by Commit**, which cuts the block back to what was written; until then the
// write cursor sits above the whole worst case and nothing else may allocate.
template<typename T>
T * AllocMax(const int count)
{
    return detail::TypedAlloc<T>(count, /*committable=*/true);
}

// Cuts the most recent AllocMax back to 'usedCount', in the units it was made in.
//
// 'base' must still be the top of the chain - this moves the write cursor back down to where the
// data ends, so nothing may have been appended since - and must have come from AllocMax. Both
// are asserted.
template<typename T>
void Commit(T * const base, const int usedCount)
{
    PS2_Assert(usedCount >= 0);
    detail::CommitQwords(base, detail::QwordsFor<T>(usedCount));
}

// --------------------------------------------------------------------------------------------
// Submission
// --------------------------------------------------------------------------------------------

// Submits everything built since the last Kick() as a chain of its own: terminates the segment
// with a trailing FLUSH so a DMA wait covers the VU runs and their XGKICKs, arms the GS fence
// behind it, writes the data cache back and kicks it at VIF1. Does nothing when nothing is new.
//
// **Fire and forget**, which is what lets rs::EndFrame leave a frame drawing while the EE builds
// the next. It does wait for an *earlier* kick nothing has fenced - one chain at a time on the
// channel, one frame at a time at the GS.
//
// Segment-at-a-time rather than whole-buffer because the write cursor never goes back: each kick
// sends the slice the last one did not, with the terminator written at the cursor between them.
void Kick();

// Blocks until the chain the last Kick() sent has been drawn: the DMA transfer complete, the
// microprograms finished and their XGKICKs delivered (the terminator's FLUSH), and the GS done
// rasterising all of it (the FINISH the terminator arms). Returns immediately when nothing is
// outstanding.
//
// The frame fence. The GS raises a single CSR bit and so carries only one at a time, which is all
// one frame of latency needs: Kick waits any earlier chain before submitting and rs retires the
// previous frame before kicking the next, so the bit is armed and consumed in strict alternation.
// (Two frames of latency would need ps2gl's SIGNAL + INTC_GS handler, which can carry a frame id -
// a semaphore and an interrupt where this is a load, plus an IMR re-arm quirk.)
void WaitIdle();

// True while a Kick has gone out that nothing has fenced - the GS is still drawing an earlier
// frame, so anything about to overwrite what it reads (VRAM a draw samples, a CLUT, a texture's
// pixels, the half it was built in) must WaitIdle() first.
bool KickInFlight();

#if PS2_QUAKE_DEBUG
// Installed by the renderer, called when a wait gives up, just after the pipeline registers are
// dumped. Those say *that* the pipeline is stuck; only the renderer can say what its
// microprograms were building when it happened, so it adds that here.
using HangReportFn = void (*)();
void SetHangReportHook(HangReportFn hook);
#endif // PS2_QUAKE_DEBUG

// Kick + WaitIdle, for a caller that needs what is built so far to have reached the GS before it
// changes something those draws depend on - an upload into evicted VRAM, a lightmap atlas
// rewrite, a CLUT refresh. Returns false if there was nothing to drain. The wait covers the GS as
// well as the transfer, so on return every side effect the frame asked for has landed.
//
// Does **not** rewind: the pipeline empties, but everything built stays where it is and every
// pointer into it stays good - which is what lets a draw's vertex data outlive its submission.
// The chain is rewound at BeginFrame, by Reserve's overflow path and by DrainBeforeWorldLoad,
// nowhere else.
bool Drain();

// The interlock that lets the halves live in the loader's lump scratch: waits for anything in
// flight - the GS included, since a frame left drawing is still reading out of a half - and
// abandons whatever is half-built, because the memory underneath is about to become the .bsp lump
// staging buffer. Called from LoadBrushModel before it claims the scratch.
void DrainBeforeWorldLoad();

// --------------------------------------------------------------------------------------------
// Debug counters
// --------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE
// Most bytes either half has ever held, against kHalfBytes. The two together are what
// says whether the capacity is right.
u32 PeakBytes();

// Bytes the frame just finished built, counting what an overflow rewind threw away. Against
// kHalfBytes this is the number that says whether a frame fits a half - PeakBytes() only
// ever reports what one half held at once, which is the same thing until the day it overflows.
u32 BytesLastFrame();

// Chains kicked, and overflow drains taken, during the frame just finished. One kick and zero
// emergency drains is the good case; the drain firing every frame means the capacity is too small.
int KicksLastFrame();
int EmergencyDrainsLastFrame();
#endif // PS2_QUAKE_PROFILE

} // namespace ps2::cmdbuf
