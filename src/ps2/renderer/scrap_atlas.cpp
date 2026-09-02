/* ================================================================================================
 * File: scrap_atlas.cpp
 * Brief: Shared atlases for the small 2D pics. See scrap_atlas.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/scrap_atlas.h"
#include "ps2/renderer/texture.h"
#include "ps2/system/heap.h"

#include <cstdio>
#include <cstring>

namespace ps2::scrap {
namespace {

constexpr int kScrapTexels = kScrapWidth * kScrapHeight;
constexpr int kScrapBytes  = kScrapTexels * static_cast<int>(sizeof(u8)); // PixelFormat::Palette8

// The transparent palette index. Gaps the packer leaves are filled with it so
// they resolve to alpha 0 in the global CLUT and the alpha test discards them -
// a packed image whose UVs stray a texel outside its block reads as nothing at
// all rather than as its neighbour. Same reason ref_gl clears its scrap to 255.
constexpr u8 kTransparentIndex = 255;

struct Scrap final
{
    tex::Texture texture = {};
    u8 *         pixels  = nullptr;
    SkylinePacker<kScrapWidth, kScrapHeight> packer = {};
};

static Scrap s_scraps[kMaxScraps];
static int   s_scrapCount = 0;

// Brings the next scrap online. The atlases live outside the texture cache: they
// are never looked up by name, and their lifetime is the whole session rather
// than a registration sequence (Pics are never evicted either, so nothing would
// be reclaimed by tying them to one).
bool NextScrap()
{
    if (s_scrapCount == kMaxScraps)
    {
        return false;
    }

    const int index = s_scrapCount++;
    Scrap &   scrap = s_scraps[index];

    scrap.pixels = static_cast<u8 *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(16), kScrapBytes, ps2::heap::MemTag::TexImage));
    std::memset(scrap.pixels, kTransparentIndex, kScrapBytes);
    scrap.packer.Reset();

    tex::Texture & texture = scrap.texture;
    std::snprintf(texture.name, sizeof(texture.name), "*scrap%d", index);

    texture.regSequence  = 0;
    texture.pixels       = scrap.pixels;
    texture.width        = kScrapWidth;
    texture.height       = kScrapHeight;
    texture.srcWidth     = kScrapWidth;  // never resampled: already a power of two
    texture.srcHeight    = kScrapHeight;
    texture.dirtyPixels  = true; // CPU-written; the first upload has to flush the dcache
    texture.type         = tex::ImageType::Pic;
    texture.flags        = tex::TexFlags::None;
    texture.format       = tex::PixelFormat::Palette8;
    // RGBA so the texture function keeps the palette's alpha: that is what cuts
    // out both the packing gaps and the images' own transparent texels.
    texture.components   = tex::TexComponents::RGBA;
    texture.function     = tex::TexFunction::Modulate;
    // Nearest, like the standalone pics - and doubly necessary here, since
    // bilinear taps at a packed image's edge would bleed in its neighbour.
    texture.magFilter    = tex::TexFilter::Nearest;
    texture.minFilter    = tex::TexFilter::Nearest;
    texture.textureChain = nullptr;
    texture.atlas        = nullptr; // an atlas is not itself packed into one
    texture.atlasX       = 0;
    texture.atlasY       = 0;
    texture.vramAddr     = tex::Texture::kNotResident;

    return true;
}

// Copies the image in row by row at (x, y).
void BlitInto(Scrap & scrap, const u8 * pic8, int width, int height, int x, int y)
{
    for (int row = 0; row < height; ++row)
    {
        std::memcpy(scrap.pixels + ((y + row) * kScrapWidth) + x,
                    pic8 + (row * width),
                    static_cast<size_t>(width));
    }
    scrap.texture.MarkPixelsDirty();
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

bool TryPack(const u8 * pic8, const int width, const int height,
             const tex::Texture ** outAtlas, int * outX, int * outY)
{
    PS2_Assert(pic8 != nullptr && outAtlas != nullptr && outX != nullptr && outY != nullptr);
    PS2_Assert(IsPackable(width, height));

    for (int i = 0; i < s_scrapCount; ++i)
    {
        Scrap & scrap = s_scraps[i];
        if (scrap.packer.Alloc(width, height, outX, outY))
        {
            BlitInto(scrap, pic8, width, height, *outX, *outY);
            *outAtlas = &scrap.texture;
            return true;
        }
    }

    // Every open scrap is full. Opening another is cheap when one is left;
    // otherwise the caller keeps the image standalone, which still works - it
    // just costs a page.
    if (!NextScrap())
    {
        return false;
    }

    Scrap & scrap = s_scraps[s_scrapCount - 1];
    if (!scrap.packer.Alloc(width, height, outX, outY))
    {
        return false; // cannot happen for a packable size in an empty scrap
    }

    BlitInto(scrap, pic8, width, height, *outX, *outY);
    *outAtlas = &scrap.texture;
    return true;
}

void DumpUsage()
{
    if (s_scrapCount == 0)
    {
        return;
    }

    static const cvar_t * s_developer = Cvar_Get("developer", "0", 0);
    if (s_developer->value == 0.0f)
    {
        return;
    }

    for (int i = 0; i < s_scrapCount; ++i)
    {
        // The packer's skyline is the filled height per column, so summing it
        // gives the texels committed - counting a shelf's unused tail as used,
        // which is what the atlas can no longer hand out anyway.
        int usedTexels = 0;
        for (const int column : s_scraps[i].packer.columns)
        {
            usedTexels += column;
        }

        Com_DPrintf("Scrap %d: %dx%d, %d%% full.\n", i, kScrapWidth, kScrapHeight,
                    (usedTexels * 100) / kScrapTexels);
    }
}

} // namespace ps2::scrap
