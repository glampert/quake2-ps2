#pragma once
/* ================================================================================================
 * File: scrap_atlas.h
 * Brief: Shared atlases ("scraps", after ref_gl) that the small 2D pics are packed
 *        into, so a 24x24 HUD icon stops costing a whole GS page of VRAM.
 *
 *  A texture occupies every GS page its pixels touch - 128x64 texels for PSMT8 -
 *  so any pic up to 64x64 costs the full 8 KB regardless of its real size. baseq2
 *  has 89 such pics: 51 KB of texels spread over 712 KB of VRAM, and since Pics
 *  are exempt from end-of-level eviction that VRAM is held for the whole session.
 *  Packed into 256x256 atlases the same images cost 64 KB apiece, and a HUD frame
 *  binds one texture instead of fifteen.
 *
 *  Packing is monotonic: images are never individually freed, matching the pics'
 *  own lifetime. The texture cache tags a packed image with the atlas it landed
 *  in and where (Texture::atlas / atlasX / atlasY); gs::ResolveBind2D turns that
 *  into the atlas to bind and the texel shift to add, so nothing above the
 *  renderer knows the difference.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <tamtypes.h>
#include <cstring>

namespace ps2::tex { struct Texture; }

namespace ps2 {

// ------------------------------------------------------------------------------------------------
// SkylinePacker - ref_gl's LM_AllocBlock
// ------------------------------------------------------------------------------------------------

// Packs rectangles into a Width x Height image by tracking the filled height of
// each column and dropping each new block onto the lowest shelf that fits.
// Allocation is monotonic: blocks are never individually freed, only wiped
// wholesale by Reset. Used by both the lightmap atlases and the 2D scrap atlas.
template<int Width, int Height>
struct SkylinePacker final
{
    static_assert(Width > 0 && Height > 0, "Skyline packer needs a positive extent!");

    // Filled height of each column.
    int columns[static_cast<unsigned>(Width)] = {};

    void Reset() { std::memset(columns, 0, sizeof(columns)); }

    // Finds the lowest shelf 'w' columns wide that leaves room for 'h' rows and
    // raises those columns over it. False when the image has no room left.
    bool Alloc(const int w, const int h, int * outX, int * outY)
    {
        PS2_Assert(w > 0 && h > 0 && outX != nullptr && outY != nullptr);

        if (w > Width || h > Height)
        {
            return false; // cannot fit at any offset
        }

        int best  = Height;
        int bestX = 0;

        // <= because a block ending flush with the last column still fits;
        // ref_gl's '<' here quietly wastes its rightmost columns.
        for (int i = 0; i <= Width - w; ++i)
        {
            int best2 = 0;
            int j     = 0;

            for (; j < w; ++j)
            {
                if (columns[i + j] >= best)
                {
                    break; // already taller than the best shelf found so far
                }
                if (columns[i + j] > best2)
                {
                    best2 = columns[i + j];
                }
            }

            if (j == w) // every column cleared: a valid spot, and lower than the last
            {
                bestX = i;
                best  = best2;
            }
        }

        if (best + h > Height)
        {
            return false; // would overflow the bottom
        }

        for (int i = 0; i < w; ++i)
        {
            columns[bestX + i] = best + h;
        }

        *outX = bestX;
        *outY = best;
        return true;
    }
};

} // namespace ps2

namespace ps2::scrap {

// Atlas dimensions, in texels. Power-of-two so the packed images sample through
// a clean TEX0 TW/TH, and 256x256 PSMT8 is exactly 8 GS pages (64 KB).
constexpr int kScrapWidth  = 256;
constexpr int kScrapHeight = 256;

// Largest image accepted, per ref_gl. Above this the page-granular waste stops
// being worth an atlas: a 128x128 PSMT8 pic already fills its pages exactly.
constexpr int kMaxImageSize = 64;

// Atlases available. ref_gl ships one; baseq2's 89 qualifying pics are ~51 KB of
// texels, which one 64 KB atlas cannot hold at realistic packing efficiency, so
// two. Allocated on demand - a session that never leaves the menu pays for one.
constexpr int kMaxScraps = 2;

// True when an image of this size and format belongs in a scrap. The caller
// still has to check the image class (Pic) and that it is not a built-in.
inline bool IsPackable(const int width, const int height)
{
    return width > 0 && height > 0 && width <= kMaxImageSize && height <= kMaxImageSize;
}

// Copies the 8-bit image into a scrap and reports which one and where. False
// when every scrap is full, in which case the caller keeps the image standalone.
// Marks the atlas dirty, so the next bind re-uploads it.
bool TryPack(const u8 * pic8, int width, int height,
             const tex::Texture ** outAtlas, int * outX, int * outY);

// Logs how full the scraps are, for the level-load summary.
void DumpUsage();

} // namespace ps2::scrap
