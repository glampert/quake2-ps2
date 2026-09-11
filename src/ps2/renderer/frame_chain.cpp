/* ================================================================================================
 * File: frame_chain.cpp
 * Brief: The frame's single DMA source chain. See frame_chain.h for the layout and for why the
 *        two halves live inside the world loader's lump scratch.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/frame_chain.h"
#include "ps2/renderer/model_load.h"
#include "ps2/renderer/render_profile.h"

#include <cstdint>
#include <dma.h>
#include <kernel.h> // FlushCache
#include <packet2_chain.h>
#include <packet2_utils.h>
#include <packet2_vif.h>

namespace ps2::chain {
namespace {

static bool s_initialized = false;

// The most recent *committable* allocation - what AllocMax handed out - and the NEXT tag that
// carries the DMAC over it, which Commit has to re-aim once the real size is known.
//
// An exact Alloc clears both, which is what makes Commit's check do double duty: it fires on a
// Commit of a block that was never committable, and on one something else has allocated on top
// of since - two gathers open at once, where the first one's commit would silently cut the
// second one's span away, with no assert firing on the pointer arithmetic alone and the
// corruption surfacing later as a mangled DMA tag.
static dma_tag_t * s_allocSkipTag = nullptr;
static void *      s_lastAlloc    = nullptr;

// Where the last Reserve said the chain may be built up to. Alloc must stay inside it: the
// contract is that a caller reserves its whole sequence up front, precisely so no allocation
// can drain, and until now that was a comment rather than something the code checked. Zero
// means no live reservation, which is what a rewind leaves behind - so an Alloc with no
// Reserve in front of it trips too.
static int s_reserveEnd = 0;

// Aims an allocation's skip tag at the first qword past its payload.
//
// Masked explicitly: the DMAC wants a physical address and packet2_chain_set_dma_tag stores
// whatever it is handed (dma_channel_send_chain masks the chain's start address, but nothing
// masks the addresses inside tags). The chain is in the normal cached segment today, so this
// changes nothing - it is here so a move to UCAB cannot quietly send the DMAC off the end of
// RAM instead.
Q_ALWAYS_INLINE void AimSkipTag(dma_tag_t * const tag, const qword_t * const target)
{
    tag->ADDR = static_cast<u64>(reinterpret_cast<std::uintptr_t>(target) & 0x0FFFFFFFu);
}

// True once a Kick has gone out that nothing has waited on yet. Kept rather than polling the
// DMAC: reading CHCR goes over the bus and interrupts the transfer in progress, which is the
// reason both reference implementations (ps2gl, ps2stuff) double-buffer instead of chasing it.
static bool s_kickInFlight = false;

// The two halves, alternating per frame. Both are packet2 headers over memory we do not own -
// packet2_create_from takes the base rather than allocating one - so neither is ever passed to
// packet2_free, which would try to free the loader's arena out from under it.
static packet2_t * s_packets[2] = {};
static int s_half = 0;

// High-water across both halves, and the per-frame counters the overlay reads. The 'last frame'
// copies exist because the debug overlay is drawn during the 2D pass, before EndFrame has run.
static u32 s_peakQwords = 0;
static int s_kicks = 0;
static int s_kicksLastFrame = 0;
static int s_emergencyDrains = 0;
static int s_emergencyDrainsLasFrame = 0;

// Worst case a Kick() appends past whatever the caller has already written: the trailing FLUSH
// block and the END tag. Reserve() has to keep this much in hand at all times, or an overflow
// would have nowhere to put the terminator it needs in order to drain.
constexpr int kTerminatorQwords = 4;

Q_ALWAYS_INLINE packet2_t * Current()
{
    PS2_AssertMsg(s_initialized, "chain::Init not called!");
    return s_packets[s_half];
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void Init()
{
    PS2_AssertMsg(!s_initialized, "chain::Init called twice!");

    const mod::ScratchBlock scratch = mod::WorldScratchBlock();
    PS2_AssertMsg(scratch.base != nullptr, "chain::Init before the world arena was reserved!");
    PS2_AssertMsg(scratch.sizeBytes >= 2u * kFrameChainBytes, "World scratch cannot hold both chain halves!");

    // 64-byte aligned because that is a cache line: the whole buffer is written by the EE and
    // read by the DMAC, and a half that started mid-line would share its first line with the
    // other half. ReserveWorldArena aligns the arena and kWorldHunkCapacity is a multiple of 64,
    // so the scratch base inherits it - assert rather than assume, since both are easy to change.
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(scratch.base) & 63u) == 0, "World scratch must be 64-byte aligned for the frame chain!");
    static_assert((kFrameChainBytes & 63u) == 0, "Chain halves must be a whole number of cache lines");

    u8 * const base = static_cast<u8 *>(scratch.base);

    for (int i = 0; i < 2; ++i)
    {
        // Through void*: the compiler cannot see that 'base' is 64-byte aligned, and
        // -Wcast-align refuses a straight u8* -> qword_t* on that basis. The alignment is
        // asserted above instead.
        void * const halfMem = base + (static_cast<size_t>(i) * kFrameChainBytes);
        qword_t * const half = static_cast<qword_t *>(halfMem);

        // Source-chain mode with tags transferred inline (tte=1), matching VifPacket: the VIFcode
        // for each transfer rides in the upper 64 bits of its own DMA tag.
        s_packets[i] = packet2_create_from(half, half, static_cast<u16>(kFrameChainQwords),
                                           P2_TYPE_NORMAL, P2_MODE_CHAIN, /*tte=*/1);
        PS2_AssertMsg(s_packets[i] != nullptr, "packet2_create_from failed!");
    }

    // Only the two packet2 headers are ours; the qword buffers belong to the world arena and are
    // already booked against MemTag::WorldMdl, so counting them here would double count them.
    ps2::heap::TagsAddMem(ps2::heap::MemTag::Renderer, 2u * sizeof(packet2_t));

    s_initialized = true;

    Com_DPrintf("Frame chain: 2 x %u KB inside the world lump scratch (%u KB), no heap of its own.\n",
                kFrameChainBytes / 1024u, scratch.sizeBytes / 1024u);
}

void BeginFrame()
{
    PS2_AssertMsg(s_initialized, "chain::Init not called!");

    // Conservative: s_kickInFlight is one flag for the channel rather than one per half, so this
    // waits even when the outstanding chain is the half we are *not* about to overwrite. That
    // costs nothing while submission is serial - the frame is drained before EndFrame returns
    // anyway - and it is the thing that has to become per-half, keyed off the GS fence, for one
    // frame of GS latency to buy anything. Until then, correctness before cleverness.
    WaitIdle();

    s_half ^= 1;
    packet2_reset(Current(), /*clear_mem=*/0);
    s_allocSkipTag = nullptr;
    s_lastAlloc    = nullptr;
    s_reserveEnd   = 0;

    s_kicks = 0;
    s_emergencyDrains = 0;
}

void EndFrame()
{
    PS2_AssertMsg(s_initialized, "chain::Init not called!");

    const u32 used = packet2_get_qw_count(Current());
    if (used > s_peakQwords)
    {
        s_peakQwords = used;
    }

    s_kicksLastFrame = s_kicks;
    s_emergencyDrainsLasFrame = s_emergencyDrains;
}

// ------------------------------------------------------------------------------------------------
// Building
// ------------------------------------------------------------------------------------------------

vu1::VifPacket Packet()
{
    return vu1::VifPacket{ Current(), QwordCapacity() };
}

int QwordCount()
{
    return static_cast<int>(packet2_get_qw_count(Current()));
}

int QwordCapacity()
{
    return static_cast<int>(kFrameChainQwords) - kTerminatorQwords;
}

bool Reserve(const int qwords)
{
    PS2_Assert(qwords >= 0);

    const int capacity = QwordCapacity();
    if (QwordCount() + qwords <= capacity) [[likely]]
    {
        s_reserveEnd = QwordCount() + qwords;
        return false;
    }

    // A request that does not fit an *empty* chain will never fit, so draining first would just
    // add a pipeline stall to the error. Say so before doing anything else - silently truncating
    // it would send the DMAC through a half-written tag, which fails with no message attached.
    if (qwords > capacity) [[unlikely]]
    {
        Sys_Error("Frame chain: a single %d qword reservation does not fit the %d qwords a half "
                  "can hold. Raise chain::kFrameChainBytes (and kWorldScratchCapacity with it).",
                  qwords, capacity);
    }

    // Everything built so far still has to reach the GS, so send it and wait - exactly the work
    // the per-batch path used to do for every batch - then hand the caller an empty chain.
    Drain();

    s_reserveEnd = QwordCount() + qwords;
    ++s_emergencyDrains;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Payload storage
// ------------------------------------------------------------------------------------------------

void * detail::AllocQwords(const int qwords, const bool committable)
{
    PS2_Assert(qwords > 0);

    packet2_t * const pkt = Current();

    // The reservation is what guarantees this cannot need to drain. Debug only - the capacity
    // check below is the one that has to be live - but it is the check that catches the real
    // mistake, which is reserving for the payload and forgetting the tags that follow it.
    PS2_AssertMsg(QwordCount() + qwords + kAllocOverheadQwords <= s_reserveEnd,
                  "chain::Alloc outside a reservation that covers it!");

    PS2_AssertMsg(!packet2_is_dma_tag_opened(pkt) && !packet2_is_vif_code_opened(pkt),
                  "chain::Alloc inside an open tag - payload cannot land mid-structure!");

    // Live in release for the same reason VifPacket::EnsureSpace is: the overrun would run off
    // the end of this half and into the other one, and the failure would surface a frame or two
    // later as corruption with nothing to connect it back to here.
    if (QwordCount() + qwords + kAllocOverheadQwords > QwordCapacity()) [[unlikely]]
    {
        Sys_Error("Frame chain: a %d qword allocation does not fit the %d qwords left in the "
                  "half. Alloc never drains - the caller has to Reserve its worst case first.",
                  qwords, QwordCapacity() - QwordCount());
    }

    // The chain is a tag stream: the qword after a tag's payload is read as the next tag, so
    // raw storage cannot simply be left sitting in it - the DMAC would walk into the gathered
    // vertices and hand them to VIF1 as VIFcodes. Front the allocation with a NEXT tag whose
    // QWC is zero: transfer nothing, and continue at ADDR - which points past the payload.
    // Through void*: -Wcast-align will not take qword_t* -> dma_tag_t* directly, and the
    // cursor is qword aligned by construction (packet2 asserts it on every tag it adds).
    void * const skipMem = pkt->next;
    dma_tag_t * const skip = static_cast<dma_tag_t *>(skipMem);
    packet2_chain_add_dma_tag(pkt, 0, 0, P2_DMA_TAG_NEXT, 0, nullptr, 0);

    // TTE is on, so the tag's upper 64 bits reach VIF1 as two VIFcodes whatever the tag id is.
    // They have to be NOPs, and they are also what pads the tag out to the whole qword.
    packet2_vif_nop(pkt, 0);
    packet2_vif_nop(pkt, 0);

    qword_t * const mem = pkt->next;
    pkt->next = mem + qwords;
    AimSkipTag(skip, pkt->next);

    // Only a committable block is worth remembering: an exact one is already the size it will
    // stay, and forgetting it here is what makes a stray Commit on it assert.
    s_allocSkipTag = committable ? skip : nullptr;
    s_lastAlloc    = committable ? static_cast<void *>(mem) : nullptr;
    return mem;
}

void detail::CommitQwords(void * const base, const int usedQwords)
{
    PS2_Assert(usedQwords >= 0);

    packet2_t * const pkt = Current();
    qword_t * const mem = static_cast<qword_t *>(base);

    // Catches both ways this goes wrong, and they are the two invariants the whole scheme rests
    // on: something appended to the chain since the Alloc (so cutting back would eat into it),
    // or the chain was rewound underneath the allocation (so the pointer is stale and the gather
    // wrote into memory that has since been handed to somebody else).
    PS2_AssertMsg(mem >= pkt->base && (mem + usedQwords) <= pkt->next,
                  "chain::Commit on a stale allocation - the chain moved underneath it!");
    PS2_AssertMsg(base == s_lastAlloc,
                  "chain::Commit on a block that was not the last AllocMax - committing an "
                  "exact Alloc, or two gathers open at once?");

    pkt->next = mem + usedQwords;
    AimSkipTag(s_allocSkipTag, pkt->next);

    s_allocSkipTag = nullptr;
    s_lastAlloc    = nullptr;
}

// ------------------------------------------------------------------------------------------------
// Submission
// ------------------------------------------------------------------------------------------------

void Kick()
{
    packet2_t * const pkt = Current();
    if (packet2_get_qw_count(pkt) == 0)
    {
        return; // nothing built
    }

    // Trailing FLUSH: stalls VIF1 until the last microprogram ends and its XGKICKs drain to the
    // GS, so waiting on this chain's DMA covers the VU work too.
    packet2_chain_open_cnt(pkt, 0, 0, 0);
    packet2_vif_flush(pkt, 0);
    packet2_vif_nop(pkt, 0); // pad the CNT block to a whole qword
    packet2_chain_close_tag(pkt);
    packet2_utils_vu_add_end_tag(pkt);

    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::DmaSend);
        {
            // Writes the chain back to memory so the DMAC reads what the EE just built. Whole-cache
            // rather than a targeted SyncDCache on purpose: half a megabyte is 8192 cache lines
            // against the 128 that exist in the whole 8 KB D-cache, so per-line sync work would
            // cost more than flushing everything. It also cannot be skipped - packet2's send path
            // passes qwc = 0 into dma_channel_send_chain, so the SDK's implicit SyncDCache is a
            // no-op here and this is the only thing making the chain coherent.
            PS2_PROFILE_SCOPED_EVENT(prof_evt::DmaFlush);
            FlushCache(0);
        }

        dma_channel_send_packet2(pkt, DMA_CHANNEL_VIF1, /*flush_cache=*/0);
        s_kickInFlight = true;
        ++s_kicks;
    }
}

void WaitIdle()
{
    if (!s_kickInFlight)
    {
        return;
    }

    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::GsWait);
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    }

    s_kickInFlight = false;
}

bool Drain()
{
    packet2_t * const pkt = Current();
    const bool hadWork = (packet2_get_qw_count(pkt) != 0) || s_kickInFlight;

    Kick();
    WaitIdle();

    // Roll the high-water before rewinding, or a frame that drained mid-way would only ever
    // report the size of its final segment.
    const u32 used = static_cast<u32>(packet2_get_qw_count(pkt));
    if (used > s_peakQwords)
    {
        s_peakQwords = used;
    }

    packet2_reset(pkt, /*clear_mem=*/0);
    s_allocSkipTag = nullptr;
    s_lastAlloc    = nullptr;
    s_reserveEnd   = 0;
    return hadWork;
}

void DrainBeforeWorldLoad()
{
    if (!s_initialized)
    {
        return; // a load before the renderer is up cannot be racing anything
    }
    Drain();
}

// ------------------------------------------------------------------------------------------------
// Debug overlay counters
// ------------------------------------------------------------------------------------------------

u32 PeakBytes()
{
    return s_peakQwords * 16u;
}

int KicksLastFrame()
{
    return s_kicksLastFrame;
}

int EmergencyDrainsLastFrame()
{
    return s_emergencyDrainsLasFrame;
}

} // namespace ps2::chain
