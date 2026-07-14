/* ================================================================================================
 * File: vram.cpp
 * Brief: GS VRAM texture heap. See vram.h.
 *
 *  The heap is tracked as a small, address-ordered array of blocks, each either
 *  free or owned by one texture (modelled on gsKit's TexManager block list).
 *  Allocation is first-fit, splitting off the free remainder; when nothing fits,
 *  the least-recently-bound texture is evicted and its block coalesced with free
 *  neighbours until the request can be satisfied. Eviction is "two-level"
 *  (ps2gl's trick): it only marks the victim non-resident - the victim's pixels
 *  stay in EE RAM and re-upload transparently the next time it is bound.
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

// One entry per contiguous VRAM range. Worst case alternates owned/free blocks,
// so this comfortably covers the texture cache's 32 slots.
constexpr int kMaxBlocks = 96;

// Debug knob: nonzero clamps the heap to this many words so eviction can be
// exercised without loading more textures than VRAM holds. Keep 0 normally.
constexpr int kDebugHeapLimitWords = 0;

struct Block
{
    int                  addrWords;      // absolute GS VRAM word address
    int                  sizeWords;
    const tex::Texture * owner;          // nullptr = free block
    u32                  lastBoundFrame; // LRU stamp; valid while owned
};

static Block s_blocks[kMaxBlocks];
static int   s_blockCount = 0;
static u32   s_frame      = 0;

void InsertBlockAt(int index)
{
    PS2_AssertMsg(s_blockCount < kMaxBlocks, "Out of VRAM block descriptors!");
    for (int i = s_blockCount; i > index; --i)
    {
        s_blocks[i] = s_blocks[i - 1];
    }
    ++s_blockCount;
}

void RemoveBlockAt(int index)
{
    for (int i = index; i < s_blockCount - 1; ++i)
    {
        s_blocks[i] = s_blocks[i + 1];
    }
    --s_blockCount;
}

// Merges the free block at 'index' with free neighbours, keeping the invariant
// that no two adjacent blocks are both free.
void CoalesceFreeAt(int index)
{
    PS2_Assert(s_blocks[index].owner == nullptr);

    if (index + 1 < s_blockCount && s_blocks[index + 1].owner == nullptr)
    {
        s_blocks[index].sizeWords += s_blocks[index + 1].sizeWords;
        RemoveBlockAt(index + 1);
    }
    if (index > 0 && s_blocks[index - 1].owner == nullptr)
    {
        s_blocks[index - 1].sizeWords += s_blocks[index].sizeWords;
        RemoveBlockAt(index);
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init(int heapBaseWords)
{
    PS2_AssertMsg(s_blockCount == 0, "vram::Init called twice!");
    PS2_Assert(heapBaseWords > 0 && heapBaseWords < kVramTotalWords);

    int heapEndWords = kVramTotalWords;
    if constexpr (kDebugHeapLimitWords != 0)
    {
        heapEndWords = heapBaseWords + kDebugHeapLimitWords;
        PS2_Assert(heapEndWords <= kVramTotalWords);
    }

    s_blocks[0] = { heapBaseWords, heapEndWords - heapBaseWords, nullptr, 0 };
    s_blockCount = 1;

    Com_Printf("GS texture heap: %d KB of VRAM.\n", s_blocks[0].sizeWords * 4 / 1024);
}

void BeginFrame()
{
    ++s_frame;
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

int Allocate(const tex::Texture & texture, int sizeWords, bool * outEvicted)
{
    PS2_AssertMsg(s_blockCount > 0, "vram::Init not called!");
    PS2_AssertMsg(texture.vramAddr == tex::Texture::kNotResident, "Texture already resident!");
    PS2_Assert(sizeWords > 0 && outEvicted != nullptr);

    *outEvicted = false;

    for (;;)
    {
        // First fit among the free blocks.
        for (int i = 0; i < s_blockCount; ++i)
        {
            if (s_blocks[i].owner != nullptr || s_blocks[i].sizeWords < sizeWords)
            {
                continue;
            }

            if (s_blocks[i].sizeWords > sizeWords)
            {
                // Split off the free remainder.
                InsertBlockAt(i + 1);
                s_blocks[i + 1] = { s_blocks[i].addrWords + sizeWords,
                                    s_blocks[i].sizeWords - sizeWords,
                                    nullptr, 0 };
                s_blocks[i].sizeWords = sizeWords;
            }

            s_blocks[i].owner = &texture;
            s_blocks[i].lastBoundFrame = s_frame;
            return s_blocks[i].addrWords;
        }

        // Nothing fits: evict the least-recently-bound texture and retry.
        // Textures bound this frame are off-limits - their draws may still be
        // queued in the frame packet or in flight on the GS.
        int victim = -1;
        for (int i = 0; i < s_blockCount; ++i)
        {
            if (s_blocks[i].owner == nullptr || s_blocks[i].lastBoundFrame == s_frame)
            {
                continue;
            }
            if (victim < 0 || s_blocks[i].lastBoundFrame < s_blocks[victim].lastBoundFrame)
            {
                victim = i;
            }
        }
        PS2_AssertMsg(victim >= 0, "GS texture heap too small for this frame's working set!");

        Com_DPrintf("VRAM: evicting '%s' (%d KB)\n",
                    s_blocks[victim].owner->name, s_blocks[victim].sizeWords * 4 / 1024);

        s_blocks[victim].owner->vramAddr = tex::Texture::kNotResident;
        s_blocks[victim].owner = nullptr;
        *outEvicted = true;
        CoalesceFreeAt(victim);
    }
}

void Touch(const tex::Texture & texture)
{
    PS2_AssertMsg(texture.vramAddr != tex::Texture::kNotResident, "Touch on a non-resident texture!");

    for (int i = 0; i < s_blockCount; ++i)
    {
        if (s_blocks[i].owner == &texture)
        {
            s_blocks[i].lastBoundFrame = s_frame;
            return;
        }
    }

    PS2_AssertMsg(false, "Resident texture has no VRAM block!");
}

} // namespace ps2::vram
