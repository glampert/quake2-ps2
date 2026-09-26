/* ================================================================================================
 * File: vram.cpp
 * Brief: GS VRAM texture heap. See vram.h.
 *
 *  The heap is tracked as an address-ordered linked list of blocks, each either
 *  free or owned by one texture (modelled on gsKit's TexManager block list).
 *  Allocation is first-fit, splitting off the free remainder; when nothing fits,
 *  the least-recently-bound texture is evicted and its block coalesced with free
 *  neighbours until the request can be satisfied. Eviction is "two-level"
 *  (ps2gl's trick): it only marks the victim non-resident - the victim's pixels
 *  stay in EE RAM and re-upload transparently the next time it is bound.
 *
 *  Textures bound this frame are pinned, because their draws are still sitting in
 *  the frame's chain unsent - or already queued at the GS - and a request that would
 *  have to evict one fails rather than corrupting them. That failure is a normal
 *  condition, not an error: the caller (rs::EnsureTextureResident) fences the GS,
 *  which sends the chain and waits for it, then calls UnpinAll and retries, then
 *  Defragment and retries, trading the whole frame's pipelining for the space. Since Defragment
 *  remakes the heap as one free block, the retry can only fail for a texture
 *  larger than the entire heap, which the caller rejects up front.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vram.h"
#include "ps2/renderer/texture.h"

#include <cstring> // memset
#include <tamtypes.h>
#include <gs_psm.h>

namespace ps2::vram {
namespace {

constexpr int kVramTotalWords = 1024 * 1024; // 4 MB of GS VRAM, in 32-bit words.

// Debug knob: nonzero clamps the heap to this many words so eviction can be
// exercised without loading more textures than VRAM holds. Keep 0 normally.
constexpr int kDebugHeapLimitWords = 0;

// GS VRAM granularity. A block is the unit a texture's base pointer (TEX0's TBP,
// BITBLTBUF's DBP) addresses, and a page is 32 of them, arranged as the format's own
// grid of blocks.
constexpr int kBlockWords = 64;
constexpr int kPageBlocks = 32;
constexpr int kPageWords  = kBlockWords * kPageBlocks; // 8 KB

// Block descriptors in the pool, which bounds how many blocks the list can hold. Every
// resident texture can leave at most one free block before it, so this covers 255
// resident textures - the most any capture has seen is 129. Past it, TryAllocate hands
// out a free block whole rather than splitting it (see NewBlock).
constexpr int kBlockPoolCapacity = 512;

// Where each 16x16-texel block of a PSMT8 page sits within it: the 128x64-texel page is
// 8 blocks across and 4 down, numbered in this order. It is PSMCT32's order too, over
// its 8x8-texel blocks - PCSX2's GS tables have _blockTable8 == _blockTable32. Every row
// and every column increases, which is what lets Psmt8ExtentBlocks read a rectangle's
// highest block straight off its far corner.
constexpr u8 kPsmt8BlockOrder[4][8] =
{
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
};

// Blocks a PSMT8 image of 'width' x 'height' texels occupies from its base pointer, laid
// out at 'stridePixels' (its TBW, a multiple of 128): one past the highest block any of
// its texels lands in.
//
// The GS finds a texel's block as base + page * 32 + kPsmt8BlockOrder[y][x], a plain
// add, so a base need only be block-aligned and an image at any base covers exactly
// [base, base + this). Pages number row-major across the stride. The highest one the
// image touches is its far corner's, and within that page the highest block is the
// corner's own entry, since the order increases along both axes. Blocks inside the range
// that the image does not touch - the right half of a page under a 64-wide image - go
// unused, which is the price of a contiguous allocation.
int Psmt8ExtentBlocks(const int width, const int height, const int stridePixels)
{
    PS2_Assert(width > 0 && height > 0 && stridePixels >= width && (stridePixels % 128) == 0);

    const int lastBlockX = (width  - 1) / 16; // the far corner's block column and row
    const int lastBlockY = (height - 1) / 16;

    const int pagesPerRow = stridePixels / 128;
    const int lastPage    = ((lastBlockY / 4) * pagesPerRow) + (lastBlockX / 8);

    return (lastPage * kPageBlocks) + kPsmt8BlockOrder[lastBlockY % 4][lastBlockX % 8] + 1;
}

// Mip layouts, one per power-of-two size from 8 to 1024 on each side (log2 3..10), laid
// out the first time a texture of that size asks. Walls come in about twenty sizes.
static_assert(kMipChainLevels == tex::kMaxMipLevels + 1, "A layout holds level 0 and every mip level");

constexpr int kMinMipLayoutLog2 = 3;
constexpr int kMipLayoutSizes   = 8;

struct MipLayoutSlot
{
    MipLayout layout;
    bool      laidOut;
};
static MipLayoutSlot s_mipLayouts[kMipLayoutSizes][kMipLayoutSizes];

// Blocks the layout search can place into: a 1024x1024 chain covers 5440 of them.
constexpr int kMaxLayoutBlocks = 8192;

// Calls 'visit' with every block a PSMT8 image of 'width' x 'height' at 'stridePixels' covers
// from base 0, stopping early when it returns false. The same addressing Psmt8ExtentBlocks
// reads the highest of.
template<typename Visit>
bool ForEachPsmt8Block(const int width, const int height, const int stridePixels, Visit && visit)
{
    const int blocksX     = (width  + 15) / 16;
    const int blocksY     = (height + 15) / 16;
    const int pagesPerRow = stridePixels / 128;

    for (int by = 0; by < blocksY; ++by)
    {
        for (int bx = 0; bx < blocksX; ++bx)
        {
            const int page = ((by / 4) * pagesPerRow) + (bx / 8);
            if (!visit((page * kPageBlocks) + kPsmt8BlockOrder[by % 4][bx % 8]))
            {
                return false;
            }
        }
    }
    return true;
}

// First fit over blocks: level 0 at the base, then each level at the lowest offset where every
// block it covers is still free. A level's buffer width is its own width, or the 128 a PSMT8
// buffer cannot go below (TBW must be even), so a small level still lays out 128 wide - which is
// exactly what lets it tuck into the part of a page a narrower level 0 leaves unused.
MipLayout LayOutMipChain(const int width, const int height, const int mipLevels)
{
    static u32 s_used[kMaxLayoutBlocks / 32];
    std::memset(s_used, 0, sizeof(s_used));

    const auto isUsed = [](const int block) { return (s_used[block >> 5] & (1u << (block & 31))) != 0; };

    MipLayout layout = {};
    int extent = 0;

    for (int level = 0; level <= mipLevels; ++level)
    {
        const int levelWidth  = width  >> level;
        const int levelHeight = height >> level;
        const int stride      = (levelWidth > 128) ? levelWidth : 128;

        int offset = 0;
        for (;;)
        {
            const bool fits = ForEachPsmt8Block(levelWidth, levelHeight, stride, [&](const int block) {
                PS2_Assert(offset + block < kMaxLayoutBlocks);
                return !isUsed(offset + block);
            });
            if (fits)
            {
                break;
            }
            ++offset;
        }

        ForEachPsmt8Block(levelWidth, levelHeight, stride, [&](const int block) {
            s_used[(offset + block) >> 5] |= 1u << ((offset + block) & 31);
            return true;
        });

        const int levelExtent = offset + Psmt8ExtentBlocks(levelWidth, levelHeight, stride);
        extent = (levelExtent > extent) ? levelExtent : extent;

        layout.blockOffset[level] = static_cast<u16>(offset);
        layout.strideUnits[level] = static_cast<u8>(stride / 64);
    }

    layout.extentBlocks = static_cast<u16>(extent);
    return layout;
}

// A size's first request. The level count follows from the size, so one layout serves every
// texture of it. Out of line: the search is the rare half of MipLayoutFor, and inlined it would
// cost every lookup its register saves.
[[gnu::noinline]] Q_COLD_FUNC void LayOutSlot(MipLayoutSlot & slot, const tex::Texture & texture)
{
    PS2_AssertMsg(tex::MipLevels(texture) > 0 && tex::GsPsm(texture.format) == GS_PSM_8,
                  "Mip layouts are for mipmapped PSMT8 textures!");

    slot.layout  = LayOutMipChain(texture.width, texture.height, tex::MipLevels(texture));
    slot.laidOut = true;
}

struct Block
{
    Address              addrWords;      // absolute GS VRAM word address
    int                  sizeWords;
    const tex::Texture * owner;          // nullptr = free block
    u32                  lastBoundFrame; // LRU stamp; valid while owned
    Block *              next;           // neighbours by address; null at the heap ends
    Block *              prev;
};

// Block descriptors are handed out from s_unusedBlocks - the pool's free-list,
// threaded through 'next' - and linked into s_blockList, which stays sorted by
// address, so a block's list neighbours are always its VRAM neighbours. That is
// what makes splitting and coalescing a pointer fixup instead of an array shift,
// and it keeps Block pointers stable for as long as the block lives.
static Block   s_blockPool[kBlockPoolCapacity];
static Block * s_unusedBlocks      = nullptr;
static Block * s_blockList         = nullptr; // null until Init()
static int     s_blockCount        = 0;       // blocks currently in s_blockList
static u32     s_frame             = 0;

// Set when VRAM was recycled or given back; see HasReuseHazard in vram.h.
static bool    s_reuseHazard       = false;

// The heap extent Init() took over (Defragment() remakes the list from it) and
// the debug-overlay stats for this frame.
static int s_heapBaseWords     = 0;
static int s_heapTotalWords    = 0;
static int s_uploadsThisFrame  = 0;
static int s_oomSyncsThisFrame = 0;

// Takes a descriptor from the pool and fills it in as a free block, or returns
// null when the pool is empty. The caller links it into the heap list
// (LinkAfter); ResetHeap makes the list head.
//
// Running dry is not an error: the only caller that can hit it is the split in
// TryAllocate, which just hands out the whole block instead. The pool is sized
// well past any working set seen (see kBlockPoolCapacity), so this is a fallback
// rather than an expected path.
Block * NewBlock(Address addrWords, int sizeWords)
{
    if (s_unusedBlocks == nullptr)
    {
        return nullptr;
    }

    Block * block  = s_unusedBlocks;
    s_unusedBlocks = block->next;

    block->addrWords      = addrWords;
    block->sizeWords      = sizeWords;
    block->owner          = nullptr;
    block->lastBoundFrame = 0;
    block->next           = nullptr;
    block->prev           = nullptr;

    ++s_blockCount;
    return block;
}

// Unlinks the block from the heap list and returns its descriptor to the pool.
// The caller's pointer dangles afterwards.
void DeleteBlock(Block * block)
{
    if (block->prev != nullptr)
    {
        block->prev->next = block->next;
    }
    else
    {
        s_blockList = block->next; // it was the list head
    }

    if (block->next != nullptr)
    {
        block->next->prev = block->prev;
    }

    block->owner   = nullptr;
    block->prev    = nullptr;
    block->next    = s_unusedBlocks;
    s_unusedBlocks = block;

    --s_blockCount;
}

// Links a block into the heap list right after 'after'.
void LinkAfter(Block * after, Block * block)
{
    block->prev = after;
    block->next = after->next;

    if (after->next != nullptr)
    {
        after->next->prev = block;
    }
    after->next = block;
}

// Drops the whole list and remakes the heap as one free block spanning it all.
// Callers deal with the owners first - this only rebuilds the bookkeeping.
void ResetHeap()
{
    for (int i = 0; i < kBlockPoolCapacity; ++i)
    {
        s_blockPool[i].owner = nullptr; // no stale Texture pointers left in the pool
        s_blockPool[i].next  = (i + 1 < kBlockPoolCapacity) ? &s_blockPool[i + 1] : nullptr;
    }

    s_unusedBlocks = &s_blockPool[0];
    s_blockCount   = 0;
    s_blockList    = NewBlock(Address(s_heapBaseWords), s_heapTotalWords);
    PS2_AssertMsg(s_blockList != nullptr, "VRAM block pool is empty!"); // pool was just refilled
}

// Merges the free block with its free neighbours, keeping the invariant that no
// two adjacent blocks are both free. 'block' itself may be merged away into its
// predecessor, so the caller must not touch it afterwards.
void CoalesceFree(Block * block)
{
    PS2_Assert(block->owner == nullptr);

    Block * next = block->next;
    if (next != nullptr && next->owner == nullptr)
    {
        block->sizeWords += next->sizeWords;
        DeleteBlock(next);
    }

    Block * prev = block->prev;
    if (prev != nullptr && prev->owner == nullptr)
    {
        prev->sizeWords += block->sizeWords;
        DeleteBlock(block);
    }
}

// The block a texture owns, or null when it holds none.
Block * FindBlockFor(const tex::Texture & texture)
{
    for (Block * block = s_blockList; block != nullptr; block = block->next)
    {
        if (block->owner == &texture)
        {
            return block;
        }
    }
    return nullptr;
}

// Enum names for the debug dump below.
const char * ImageTypeName(tex::ImageType type)
{
    switch (type)
    {
    case tex::ImageType::Null   : return "null";
    case tex::ImageType::Pic    : return "pic";
    case tex::ImageType::Skin   : return "skin";
    case tex::ImageType::Sprite : return "sprite";
    case tex::ImageType::Wall   : return "wall";
    case tex::ImageType::Sky    : return "sky";
    }
    return "???"; // Unreachable; keeps GCC's -Wreturn-type happy.
}

const char * PixelFormatName(tex::PixelFormat format)
{
    switch (format)
    {
    case tex::PixelFormat::RGBA32   : return "rgba32";
    case tex::PixelFormat::RGB16    : return "rgb16";
    case tex::PixelFormat::Palette8 : return "pal8";
    case tex::PixelFormat::Alpha8   : return "alpha8";
    }
    return "???"; // Unreachable; keeps GCC's -Wreturn-type happy.
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

// Prints the whole block list plus the current Stats to stdout, so a failed
// allocation can be diagnosed from the EE emulog: which textures were holding
// VRAM, how much each took and how recently they were bound. Blocks marked
// [pinned] were bound this frame and so cannot be evicted; when every used
// block is pinned the frame's working set simply does not fit. A large free
// total next to a small largest-free-block means fragmentation instead.
void DumpAllBlocks()
{
    Com_Printf("---- GS VRAM texture heap dump (frame %u) ----\n", s_frame);
    Com_Printf("idx  addrWords  sizeWords  sizeKB  lastBound  texture\n");

    int index            = 0;
    int usedBlocks       = 0;
    int pinnedBlocks     = 0;
    int largestFreeWords = 0;

    for (const Block * block = s_blockList; block != nullptr; block = block->next, ++index)
    {
        const int addrWords = static_cast<int>(block->addrWords);
        const int sizeKb    = block->sizeWords * 4 / 1024;

        if (block->owner == nullptr)
        {
            if (block->sizeWords > largestFreeWords)
            {
                largestFreeWords = block->sizeWords;
            }

            Com_Printf("%3d  %9d  %9d  %6d  %9s  <free>\n",
                       index, addrWords, block->sizeWords, sizeKb, "-");
            continue;
        }

        ++usedBlocks;

        const bool pinned = (block->lastBoundFrame == s_frame);
        if (pinned)
        {
            ++pinnedBlocks;
        }

        const tex::Texture & texture = *block->owner;
        Com_Printf("%3d  %9d  %9d  %6d  %9u  %s (%dx%d, %s, %s)%s%s\n",
                   index, addrWords, block->sizeWords, sizeKb, block->lastBoundFrame,
                   texture.name, texture.width, texture.height,
                   PixelFormatName(texture.format), ImageTypeName(texture.type),
                   pinned ? " [pinned]" : "", texture.dirtyPixels ? " [dirty]" : "");
    }

    const Stats stats = GetStats();

    Com_Printf("Blocks   : %d used (%d pinned this frame), %d free, %d of %d descriptors\n",
               usedBlocks,
               pinnedBlocks,
               s_blockCount - usedBlocks,
               s_blockCount, kBlockPoolCapacity);

    Com_Printf("Heap     : %d KB total, %d KB used, %d KB free (largest free block %d KB)\n",
               stats.totalWords * 4 / 1024,
               (stats.totalWords - stats.freeWords) * 4 / 1024,
               stats.freeWords * 4 / 1024,
               largestFreeWords * 4 / 1024);

    Com_Printf("Textures : %d resident, %d uploads this frame, %d GS drains this frame\n",
               stats.residentTextures,
               stats.uploadsThisFrame,
               stats.oomSyncsThisFrame);
}

void Init(int heapBaseWords)
{
    PS2_AssertMsg(s_blockList == nullptr, "vram::Init called twice!");
    PS2_Assert(heapBaseWords > 0 && heapBaseWords < kVramTotalWords);

    int heapEndWords = kVramTotalWords;
    if constexpr (kDebugHeapLimitWords != 0)
    {
        heapEndWords = heapBaseWords + kDebugHeapLimitWords;
        PS2_Assert(heapEndWords <= kVramTotalWords);
    }

    s_heapBaseWords  = heapBaseWords;
    s_heapTotalWords = heapEndWords - heapBaseWords;

    ResetHeap();

    Com_Printf("GS texture heap: %d KB of VRAM (%d block descriptors).\n",
               s_heapTotalWords * 4 / 1024, kBlockPoolCapacity);
}

void BeginFrame()
{
    ++s_frame;
    s_uploadsThisFrame  = 0;
    s_oomSyncsThisFrame = 0;
}

const MipLayout & MipLayoutFor(const tex::Texture & texture)
{
    // Every 3D batch of a mipmapped wall comes through here, so the hit is a table read. Mipmapped
    // textures are powers of two (LoadFromFile resamples them to one), which makes Log2 exact.
    PS2_Assert((texture.width & (texture.width - 1)) == 0 && (texture.height & (texture.height - 1)) == 0);

    const int log2W = tex::Log2(static_cast<u32>(texture.width));
    const int log2H = tex::Log2(static_cast<u32>(texture.height));
    PS2_Assert(log2W >= kMinMipLayoutLog2 && log2W < kMinMipLayoutLog2 + kMipLayoutSizes &&
               log2H >= kMinMipLayoutLog2 && log2H < kMinMipLayoutLog2 + kMipLayoutSizes);

    MipLayoutSlot & slot = s_mipLayouts[log2W - kMinMipLayoutLog2][log2H - kMinMipLayoutLog2];
    if (!slot.laidOut) [[unlikely]]
    {
        LayOutSlot(slot, texture);
    }
    return slot.layout;
}

int TextureFootprintWords(const tex::Texture & texture)
{
    PS2_Assert(texture.width > 0 && texture.height > 0);

    const int psm = tex::GsPsm(texture.format);
    if (tex::MipLevels(texture) > 0)
    {
        return MipLayoutFor(texture).extentBlocks * kBlockWords;
    }
    if (psm == GS_PSM_8)
    {
        return Psmt8ExtentBlocks(texture.width, texture.height,
                                 tex::TextureStridePixels(texture, psm)) * kBlockWords;
    }

    // The direct-colour formats keep whole pages: nothing the game ships uses them (only the debug
    // checkerboards and .tga replacements do), so their block orders are not worth carrying.
    //
    // A texture occupies every GS page its pixel rectangle touches: pages tile the *texture
    // space* in fixed pixel dimensions, and the swizzled layout scatters texels across the whole
    // page grid. libgraph's graph_vram_size counts linear width*height words instead, which
    // undercounts textures with non-page-multiple dimensions and would let the next allocation
    // overlap.
    int pageWidth, pageHeight;
    switch (psm)
    {
    case GS_PSM_32:
        pageWidth  = 64;
        pageHeight = 32;
        break;
    case GS_PSM_16:
    case GS_PSM_16S:
        pageWidth  = 64;
        pageHeight = 64;
        break;
    default:
        PS2_AssertMsg(false, "Unsupported texture PSM!");
        return 0;
    }

    const int pagesX = (texture.width  + pageWidth  - 1) / pageWidth;
    const int pagesY = (texture.height + pageHeight - 1) / pageHeight;
    return pagesX * pagesY * kPageWords;
}

int HeapTotalWords()
{
    return s_heapTotalWords;
}

Address TryAllocate(const tex::Texture & texture, int sizeWords)
{
    PS2_AssertMsg(s_blockList != nullptr, "vram::Init not called!");
    PS2_AssertMsg(!texture.IsVramResident(), "Texture already resident!");
    PS2_Assert(sizeWords > 0);

    for (;;)
    {
        // First fit among the free blocks.
        for (Block * block = s_blockList; block != nullptr; block = block->next)
        {
            if (block->owner != nullptr || block->sizeWords < sizeWords)
            {
                continue;
            }

            if (block->sizeWords > sizeWords)
            {
                // Split off the free remainder. With no descriptor left to
                // describe it, hand out the oversized block whole rather than
                // failing - the surplus comes back when the block is freed and
                // coalesced.
                Block * remainder = NewBlock(Address(static_cast<int>(block->addrWords) + sizeWords),
                                             block->sizeWords - sizeWords);
                if (remainder != nullptr)
                {
                    LinkAfter(block, remainder);
                    block->sizeWords = sizeWords;
                }
            }

            block->owner          = &texture;
            block->lastBoundFrame = s_frame;
            return block->addrWords;
        }

        // Nothing fits: evict the least-recently-bound texture and retry.
        // Textures bound this frame are off-limits - their draws may still be
        // queued in the frame's chain or in flight on the GS.
        Block * victim = nullptr;
        for (Block * block = s_blockList; block != nullptr; block = block->next)
        {
            if (block->owner == nullptr || block->lastBoundFrame == s_frame)
            {
                continue;
            }
            if (victim == nullptr || block->lastBoundFrame < victim->lastBoundFrame)
            {
                victim = block;
            }
        }

        if (victim == nullptr)
        {
            // Everything left is pinned. Not fatal: the caller drains the GS -
            // which is the only reason the pins exist - and retries. Anything
            // evicted on the way here stays evicted, which is fine; those
            // textures re-upload on their next bind.
            return Address::Invalid;
        }

        Com_DPrintf("VRAM: evicting '%s' (%d KB)\n", victim->owner->name, victim->sizeWords * 4 / 1024);

        victim->owner->vramAddr = tex::Texture::kNotResident;
        victim->owner = nullptr;
        s_reuseHazard = true;
        CoalesceFree(victim);
    }
}

void UnpinAll()
{
    PS2_AssertMsg(s_blockList != nullptr, "vram::Init not called!");

    // Clamp this frame's stamps back one frame rather than zeroing them: the
    // blocks stay ordered by how recently they were bound, so a texture the
    // frame has not touched in a while is still the first to go.
    if (s_frame == 0)
    {
        return; // no frame has started, so nothing can be pinned
    }

    for (Block * block = s_blockList; block != nullptr; block = block->next)
    {
        if (block->owner != nullptr && block->lastBoundFrame == s_frame)
        {
            block->lastBoundFrame = s_frame - 1;
        }
    }
}

void Touch(const tex::Texture & texture)
{
    PS2_AssertMsg(texture.IsVramResident(), "Touch on a non-resident texture!");

    Block * block = FindBlockFor(texture);
    if (block != nullptr) [[likely]]
    {
        block->lastBoundFrame = s_frame;
        return;
    }

    Sys_Error("Resident texture has no VRAM block!");
}

bool BoundThisFrame(const tex::Texture & texture)
{
    PS2_AssertMsg(texture.IsVramResident(), "BoundThisFrame on a non-resident texture!");

    const Block * block = FindBlockFor(texture);
    if (block != nullptr) [[likely]]
    {
        return block->lastBoundFrame == s_frame;
    }

    Sys_Error("Resident texture has no VRAM block!");
    return false;
}

void Free(const tex::Texture & texture)
{
    if (!texture.IsVramResident())
    {
        return;
    }

    Block * block = FindBlockFor(texture);
    if (block != nullptr) [[likely]]
    {
        texture.vramAddr = tex::Texture::kNotResident;
        block->owner     = nullptr;
        CoalesceFree(block);

        // Queued or in-flight draws may still sample the range just freed, so the next upload
        // that lands there has to make the GS idle first - same as an eviction.
        s_reuseHazard = true;
        return;
    }

    Sys_Error("Resident texture has no VRAM block!");
}

bool Defragment()
{
    PS2_AssertMsg(s_blockList != nullptr, "vram::Init not called!");

    if (s_blockList->next == nullptr)
    {
        return false; // a single block is unfragmented by definition
    }

    int evicted = 0;
    for (Block * block = s_blockList; block != nullptr; block = block->next)
    {
        if (block->owner != nullptr)
        {
            block->owner->vramAddr = tex::Texture::kNotResident;
            ++evicted;
        }
    }

    ResetHeap();

    // Everything that was resident now is not, and its VRAM is about to be handed out again.
    s_reuseHazard |= (evicted > 0);

    Com_DPrintf("VRAM: heap defragmented, %d resident textures evicted.\n", evicted);
    return evicted > 0;
}

void NoteTextureUpload()
{
    ++s_uploadsThisFrame;
}

bool HasReuseHazard()
{
    return s_reuseHazard;
}

void ClearReuseHazard()
{
    s_reuseHazard = false;
}

void NoteOomSync()
{
    ++s_oomSyncsThisFrame;
}

Stats GetStats()
{
    Stats stats = {};
    stats.totalWords        = s_heapTotalWords;
    stats.uploadsThisFrame  = s_uploadsThisFrame;
    stats.oomSyncsThisFrame = s_oomSyncsThisFrame;

    for (const Block * block = s_blockList; block != nullptr; block = block->next)
    {
        if (block->owner == nullptr)
        {
            stats.freeWords += block->sizeWords;
        }
        else
        {
            ++stats.residentTextures;
        }
    }

    return stats;
}

} // namespace ps2::vram
