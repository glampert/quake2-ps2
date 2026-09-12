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
 *  condition, not an error: the caller (gs::EnsureTextureResident) fences the GS,
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

#include <tamtypes.h>
#include <gs_psm.h>

namespace ps2::vram {
namespace {

constexpr int kVramTotalWords = 1024 * 1024; // 4 MB of GS VRAM, in 32-bit words.

// Debug knob: nonzero clamps the heap to this many words so eviction can be
// exercised without loading more textures than VRAM holds. Keep 0 normally.
constexpr int kDebugHeapLimitWords = 0;

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
//
// The pool is sized by Init() from the heap it is handed, since how much VRAM
// is left over depends on the framebuffer format (see gs::Init).
static Block * s_blockPool         = nullptr;
static int     s_blockPoolCapacity = 0;
static Block * s_unusedBlocks      = nullptr;
static Block * s_blockList         = nullptr; // null until Init()
static int     s_blockCount        = 0;       // blocks currently in s_blockList
static u32     s_frame             = 0;

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
// at one descriptor per GS page (see Init), which is the most the list can ever
// hold, so this is belt-and-braces rather than an expected path.
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
    for (int i = 0; i < s_blockPoolCapacity; ++i)
    {
        s_blockPool[i].owner = nullptr; // no stale Texture pointers left in the pool
        s_blockPool[i].next  = (i + 1 < s_blockPoolCapacity) ? &s_blockPool[i + 1] : nullptr;
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
               s_blockCount, s_blockPoolCapacity);

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

    // One descriptor per GS page bounds the list: every block spans at least a
    // whole page, since the heap base is page-aligned and TextureFootprintWords
    // is page-granular, so every split remainder is page-aligned too. The spare
    // few cost nothing and keep the bound from being exactly tight. Allocated
    // rather than fixed because how much VRAM is left for the heap depends on
    // the framebuffer format (see gs::Init); Init runs once and the renderer
    // has no teardown, so the pool lives for the length of the process.
    s_blockPoolCapacity = (s_heapTotalWords / 2048) + 8;
    s_blockPool = new Block[static_cast<size_t>(s_blockPoolCapacity)];

    ResetHeap();

    Com_Printf("GS texture heap: %d KB of VRAM (%d block descriptors).\n",
               s_heapTotalWords * 4 / 1024, s_blockPoolCapacity);
}

void BeginFrame()
{
    ++s_frame;
    s_uploadsThisFrame  = 0;
    s_oomSyncsThisFrame = 0;
}

void EndFrame()
{
    // Nothing for now.
}

int TextureFootprintWords(int width, int height, int psm)
{
    PS2_Assert(width > 0 && height > 0);

    // A texture occupies every GS page its pixel rectangle touches: pages tile
    // the *texture space* in fixed pixel dimensions, and the swizzled layout
    // scatters texels across the whole page grid. libgraph's graph_vram_size
    // counts linear width*height words instead, which undercounts textures with
    // non-page-multiple dimensions and would let the next allocation overlap.
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
    case GS_PSM_8:
        pageWidth  = 128;
        pageHeight = 64;
        break;
    default:
        PS2_AssertMsg(false, "Unsupported texture PSM!");
        return 0;
    }

    const int pagesX = (width  + pageWidth  - 1) / pageWidth;
    const int pagesY = (height + pageHeight - 1) / pageHeight;
    return pagesX * pagesY * 2048; // one GS page = 8 KB = 2048 words
}

int HeapTotalWords()
{
    return s_heapTotalWords;
}

Address TryAllocate(const tex::Texture & texture, int sizeWords, bool * outEvicted)
{
    PS2_AssertMsg(s_blockList != nullptr, "vram::Init not called!");
    PS2_AssertMsg(texture.vramAddr == tex::Texture::kNotResident, "Texture already resident!");
    PS2_Assert(sizeWords > 0 && outEvicted != nullptr);

    *outEvicted = false;

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
        *outEvicted = true;
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
    PS2_AssertMsg(texture.vramAddr != tex::Texture::kNotResident, "Touch on a non-resident texture!");

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
    PS2_AssertMsg(texture.vramAddr != tex::Texture::kNotResident, "BoundThisFrame on a non-resident texture!");

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
    if (texture.vramAddr == tex::Texture::kNotResident)
    {
        return;
    }

    Block * block = FindBlockFor(texture);
    if (block != nullptr) [[likely]]
    {
        texture.vramAddr = tex::Texture::kNotResident;
        block->owner     = nullptr;
        CoalesceFree(block);
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

    Com_DPrintf("VRAM: heap defragmented, %d resident textures evicted.\n", evicted);
    return evicted > 0;
}

void NoteTextureUpload()
{
    ++s_uploadsThisFrame;
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
