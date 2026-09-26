#pragma once
/* ================================================================================================
 * File: vram.h
 * Brief: GS VRAM texture heap: tracks which textures are resident in the VRAM left
 *        over after the framebuffers and z-buffer, handing out space on demand and
 *        evicting the least-recently-bound textures when full. Pure bookkeeping -
 *        the DMA uploads and GS synchronisation stay with rs::EnsureTextureResident.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::tex { struct Texture; }

namespace ps2::vram {

// 32-bits type to represent VRAM addresses.
enum struct Address : int
{
    Invalid = -1,
};

// Takes ownership of GS VRAM from 'heapBaseWords' (a word address) up to the 4 MB
// end. Call once, from gs::Init(), after the framebuffer/z-buffer allocations.
void Init(int heapBaseWords);

// Advances the LRU clock and resets the per-frame counters (see GetStats).
// Call once per frame, from rs::BeginFrame().
void BeginFrame();

// VRAM words the texture occupies from its base: up to and including the last GS block
// any of its texels lands in (a block is 64 words). Always a whole number of blocks,
// since that is the unit TEX0's base pointer addresses. libgraph's graph_vram_size
// undercounts here - see the .cpp for why.
int TextureFootprintWords(const tex::Texture & texture);

// Where a mipmapped texture's levels sit inside its block of VRAM, laid out once per
// size: level 0 at the base, and each level after it at the lowest block where it misses
// every level already placed - which for most sizes is space level 0's pages leave
// unused, so the levels cost few blocks or none. What the upload writes each level to
// and what MIPTBP1 tells the GS, so the two cannot disagree.
constexpr int kMipChainLevels = 4; // level 0 plus tex::kMaxMipLevels

struct MipLayout
{
    u16 blockOffset[kMipChainLevels]; // from the texture's base, in 64-word GS blocks
    u8  strideUnits[kMipChainLevels]; // the level's buffer width (TBW), in 64-texel units
    u16 extentBlocks;                 // blocks the whole chain covers from the base
};

// The layout for a mipmapped texture (tex::MipLevels > 0: a power-of-two PSMT8 wall).
const MipLayout & MipLayoutFor(const tex::Texture & texture);

// Size of the heap handed to Init(), in words. The largest request that can
// ever be serviced, since Defragment() can always remake the heap as one block.
int HeapTotalWords();

// Allocates 'sizeWords' for 'texture', evicting least-recently-bound textures as
// needed - never ones bound this frame, whose draws may still be in flight.
// Evicted textures get vramAddr = kNotResident and self-heal on their next bind.
// Returns the block's word address, or Address::Invalid when the request cannot
// be met without touching a texture bound this frame. An eviction raises the
// reuse hazard below, which the caller must resolve before writing over the
// VRAM it handed out.
//
// Failure is not fatal and not the end of the road: the caller fences the GS -
// sending the frame's chain so far and waiting for it, which is what the this-frame
// protection guards against - then calls UnpinAll and retries, then Defragment and
// retries. See rs::EnsureTextureResident.
Address TryAllocate(const tex::Texture & texture, int sizeWords);

// Drops the this-frame eviction protection from every resident texture, making
// the whole heap evictable again. Only legal once the GS has been fenced - the pins
// exist because a block bound this frame is still sitting in the frame's chain
// unsent, or queued at the GS, and nothing else re-establishes that guarantee. Relative LRU
// order is preserved, so the coldest textures stay the preferred victims.
void UnpinAll();

// Marks the (resident) texture as bound this frame, protecting it from eviction
// until the next frame.
void Touch(const tex::Texture & texture);

// True when the (resident) texture was already bound this frame - its draws may
// still be queued in the unsent part of the frame's chain, so overwriting its VRAM (dynamic
// texture re-upload) must sync the GS first.
bool BoundThisFrame(const tex::Texture & texture);

// Returns the texture's block to the heap and marks it non-resident (no-op when
// not resident). The freed range may be handed out without an eviction, so the
// caller must treat it like evicted VRAM: sync the GS before writing over it.
void Free(const tex::Texture & texture);

// Compacts the heap the cheap way: evicts every resident texture and remakes it
// as one contiguous free range. Nothing is lost - as with any eviction the pixels
// stay in EE RAM and re-upload on the next bind, which repacks the live set from
// the heap base with no holes, and only for the textures actually bound again.
// Returns true when anything was evicted; the caller must then sync the GS before
// the recycled VRAM is written, exactly like Allocate's *outEvicted.
//
// Meant for level changes: freeing the previous level's textures leaves holes
// between the surviving textures, and the new level's first frame binds its whole
// working set at once - every block pinned, nothing evictable - so an allocation
// can fail there with plenty of free VRAM left, just not contiguous.
bool Defragment();

// Records one texture DMA upload for the per-frame counter. Called by
// gs::UploadTexture each time it transfers a texture's pixels into VRAM
// (a first upload or a dirty re-upload); reset each frame by BeginFrame().
void NoteTextureUpload();

// Records one mid-frame GS drain forced by a failed allocation, for the same
// per-frame counters. A nonzero count means the frame's working set outgrew the
// heap and the renderer traded pipelining for it.
void NoteOomSync();

// --------------------------------------------------------------------------------------------
// Reuse hazard
// --------------------------------------------------------------------------------------------

// Whether VRAM has been handed out again, or given back, since this was last cleared - by an
// eviction inside TryAllocate, by Free, or by Defragment. While it is set, draws already built
// (or still rasterising) may reference a range something else now owns, so anything about to
// write over that VRAM must make the GS idle first and clear this.
//
// Sticky on purpose: a block freed early in a frame can be handed out later in it without a
// second eviction, and the hazard that created is the same one.
bool HasReuseHazard();
void ClearReuseHazard();

// Prints the whole block list and the current stats: which textures hold VRAM,
// how much each takes and how recently each was bound. For diagnosing a failed
// allocation from the EE log - blocks marked [pinned] were bound this frame, and
// a large free total next to a small largest-free-block means fragmentation.
void DumpAllBlocks();

// Live snapshot of the texture heap, for the ref.cpp debug overlay.
struct Stats
{
    int freeWords;         // uncommitted VRAM: total size of the free blocks
    int totalWords;        // heap size handed to Init()
    int residentTextures;  // textures currently holding a block
    int uploadsThisFrame;  // texture DMA uploads since the last BeginFrame()
    int oomSyncsThisFrame; // allocation failures that forced a GS drain
};

// Computes the current stats (cheap; walks the block list).
Stats GetStats();

} // namespace ps2::vram
