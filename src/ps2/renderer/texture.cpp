/* ================================================================================================
 * File: texture.cpp
 * Brief: Texture objects and the texture cache. See texture.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/hash_map.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/image_load.h"
#include "ps2/renderer/scrap_atlas.h" // small Pics share an atlas instead of a GS page each
#include "ps2/renderer/gs.h" // gs::ReleaseTexture / gs::DefragVramHeap (end-of-level eviction)
#include "ps2/builtin/builtin.h"
#include "ps2/small_pool.h"
#include "ps2/hash.h" // HashStr64 / kFnvPrime (shared with the model cache)
#include "ps2/fs/async_io.h"       // the background loader
#include "ps2/system/heap.h"
#include "ps2/debug/profile.h"     // ReadCycles
#include "ps2/renderer/profile.h"  // FrameLogNoteReady

#include <cstdio>
#include <cstring>

namespace ps2::tex {
namespace {

// ------------------------------------------------------------------------------------------------
// Global texture settings
// ------------------------------------------------------------------------------------------------

// Set by tex::SetSkyDownsample(); consumed by LoadFromFile for Sky images.
static bool s_skyDownsample = false;

// Set by tex::SetWallMipmaps(); consumed by LoadFromFile for WAL walls.
static bool s_wallMipmaps = false;

// ref_gl's 'intensity', as Init was given it: the brightening a lit true-colour image
// takes in its own texels. Latched, so it is the same for every image of a run.
static float s_intensityScale = 1.0f;

// ------------------------------------------------------------------------------------------------
// Local helpers
// ------------------------------------------------------------------------------------------------

// Cache lookup key: the name hash continued with the image type as one extra
// FNV-1a byte, so the same file may be cached independently per ImageType.
Q_ALWAYS_INLINE u64 LookupKey(const char * fullname, ImageType type)
{
    u64 hash = HashStr64(fullname);
    hash ^= static_cast<u8>(type);
    hash *= kFnvPrime;
    return hash;
}

// Expands a game image name into the full path key used by the cache. Pics
// resolve the same way ref_gl's Draw_FindPic did: bare names live under
// "pics/" as .pcx files, a leading path separator means 'name' is already the
// full path. Every other type (skins, walls, sky, sprites) always arrives as
// a full path with extension and is used verbatim.
void NormalizeName(const char * name, ImageType type, char (&out)[MAX_QPATH])
{
    if (type != ImageType::Pic)
    {
        std::snprintf(out, MAX_QPATH, "%s", name);
    }
    else if (name[0] != '/' && name[0] != '\\')
    {
        std::snprintf(out, MAX_QPATH, "pics/%s.pcx", name);
    }
    else
    {
        std::snprintf(out, MAX_QPATH, "%s", name + 1);
    }
}

// True when any texel indexes palette entry 255 - the transparent color, alpha
// 0 in the global CLUT. Those images sample with RGBA components so the alpha
// test cuts the transparent texels out.
bool HasTransparentTexels(const u8 * pic8, int texelCount)
{
    for (int i = 0; i < texelCount; ++i)
    {
        if (pic8[i] == 255)
        {
            return true;
        }
    }
    return false;
}

// Multiplies an RGBA32 image's colour channels in place, clamping each at full
// rather than wrapping. The alpha is left alone - it is coverage, not light.
// This is ref_gl's intensitytable applied directly to the texels, for the images
// that cannot reach it through a CLUT.
void ScaleTexelsForIntensity(u8 * rgba, int texelCount, float scale)
{
    if (scale <= 1.0f)
    {
        return;
    }

    // TODO: Precompute and cache ramp values?
    u8 ramp[256];
    for (int i = 0; i < ArrayLength(ramp); ++i)
    {
        const float scaled = static_cast<float>(i) * scale;
        ramp[i] = static_cast<u8>((scaled >= 255.0f) ? 255.0f : scaled);
    }

    for (int i = 0; i < texelCount; ++i, rgba += 4)
    {
        rgba[0] = ramp[rgba[0]];
        rgba[1] = ramp[rgba[1]];
        rgba[2] = ramp[rgba[2]];
    }
}

// Halves an 8-bit indexed image in both dimensions by point sampling, into a
// fresh allocation - a quarter of the VRAM for a sky face.
//
// Point sampling, not averaging: these are palette indices, and the mean of
// two indices is an unrelated colour. Averaging would have to go through the
// palette and back, and coming back needs an inverse-palette lookup this
// renderer has no table for. ref_gl's gl_skymip took the same shortcut by
// leaning on GL_MipMap, which averages the *unpalettized* image; here the
// image never leaves index space, so dropping every other row and column is
// what is left. On a sky - low frequency by nature - it is hard to tell apart.
//
// Frees 'pic8' and returns the replacement, or leaves it alone and returns it
// unchanged when the image is too small to halve.
u8 * DownsampleIndexed2x(u8 * pic8, int * width, int * height)
{
    const int srcW = *width;
    const int srcH = *height;
    if (srcW < 2 || srcH < 2)
    {
        return pic8;
    }

    const int dstW = srcW / 2;
    const int dstH = srcH / 2;

    u8 * const scaled = static_cast<u8 *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(16), static_cast<size_t>(dstW * dstH), ps2::heap::MemTag::TexImage));

    for (int y = 0; y < dstH; ++y)
    {
        const u8 * const srcRow = pic8   + (y * 2 * srcW);
        u8 * const       dstRow = scaled + (y * dstW);
        for (int x = 0; x < dstW; ++x)
        {
            dstRow[x] = srcRow[x * 2];
        }
    }

    ps2::heap::Free(pic8, static_cast<size_t>(srcW * srcH), ps2::heap::MemTag::TexImage);

    *width  = dstW;
    *height = dstH;
    return scaled;
}

// Stretches an image up to the next power of two in both dimensions, into a
// fresh allocation, for the textures that tile: the GS spreads normalized ST
// over the TEX0 extent - the image size rounded UP to a power of two - so a
// 240x128 wall would wrap every 256 texels, over 16 columns of whatever else
// happens to be in VRAM, with each tile stretched 6.7% besides. Filling the
// extent is what makes the wrap land on the image's own edge again, and the
// world's texture coordinates keep dividing by the size the image had on disk
// (Texture::srcWidth), so a tile still spans the world units it used to.
//
// Point sampling, for the same reason DownsampleIndexed2x above uses it: an
// 8-bit image is palette indices, and the weighted mean of two indices is an
// unrelated colour. Upscaling only ever duplicates rows and columns - no texel
// is dropped - so an image's transparent texels (index 255) all survive, and
// the duplication is invisible under bilinear sampling at PS2 resolutions.
//
// 'mipLevels' levels follow level 0 in 'pixels' (see MipChainBytes), and each
// is stretched to its own half of the one before, so the chain comes out as the
// power-of-two image's mip chain.
//
// Frees 'pixels' and returns the replacement, or leaves it alone and returns
// it unchanged when both dimensions are already powers of two.
u8 * ResampleToPowerOfTwo(u8 * pixels, int * width, int * height, const int bytesPerTexel, const int mipLevels)
{
    const int srcW = *width;
    const int srcH = *height;
    const int dstW = 1 << tex::Log2(static_cast<u32>(srcW));
    const int dstH = 1 << tex::Log2(static_cast<u32>(srcH));

    if (srcW == dstW && srcH == dstH)
    {
        return pixels;
    }

    u8 * const scaled = static_cast<u8 *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(16),
                                static_cast<size_t>(MipChainBytes(dstW, dstH, mipLevels, bytesPerTexel)),
                                ps2::heap::MemTag::TexImage));

    const u8 * srcLevel = pixels;
    u8 *       dstLevel = scaled;
    for (int level = 0; level <= mipLevels; ++level)
    {
        const int levelSrcW = srcW >> level;
        const int levelSrcH = srcH >> level;
        const int levelDstW = dstW >> level;
        const int levelDstH = dstH >> level;

        // 16.16 fixed-point steps through the source image. The destination never
        // shrinks, so both steps are <= 1.0 and the accumulators stay in bounds.
        const u32 stepS = (static_cast<u32>(levelSrcW) << 16) / static_cast<u32>(levelDstW);
        const u32 stepT = (static_cast<u32>(levelSrcH) << 16) / static_cast<u32>(levelDstH);

        const int srcPitch = levelSrcW * bytesPerTexel;
        const int dstPitch = levelDstW * bytesPerTexel;

        u32 accT = 0;
        for (int y = 0; y < levelDstH; ++y, accT += stepT)
        {
            const u8 * const srcRow = srcLevel + (static_cast<int>(accT >> 16) * srcPitch);
            u8 * const       dstRow = dstLevel + (y * dstPitch);

            if (levelSrcW == levelDstW) // Only the height grew; rows copy whole.
            {
                std::memcpy(dstRow, srcRow, static_cast<size_t>(srcPitch));
                continue;
            }

            u32 accS = 0;
            for (int x = 0; x < levelDstW; ++x, accS += stepS)
            {
                const u8 * const srcTexel = srcRow + (static_cast<int>(accS >> 16) * bytesPerTexel);
                u8 * const       dstTexel = dstRow + (x * bytesPerTexel);
                for (int b = 0; b < bytesPerTexel; ++b)
                {
                    dstTexel[b] = srcTexel[b];
                }
            }
        }

        srcLevel += levelSrcH * srcPitch;
        dstLevel += levelDstH * dstPitch;
    }

    ps2::heap::Free(pixels, static_cast<size_t>(MipChainBytes(srcW, srcH, mipLevels, bytesPerTexel)),
                    ps2::heap::MemTag::TexImage);

    *width  = dstW;
    *height = dstH;
    return scaled;
}

// A WAL image's pixel buffer: level 0 and the first 'mipLevels' of the file's mip levels
// after it, packed as MipChainBytes lays them out, each at the size the file has it.
u8 * CopyWalLevels(const WalFile & wal, const int mipLevels)
{
    PS2_Assert(mipLevels >= 0 && mipLevels < wal.numLevels);

    u8 * const chain = static_cast<u8 *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(16),
                                static_cast<size_t>(MipChainBytes(wal.width, wal.height, mipLevels, 1)),
                                ps2::heap::MemTag::TexImage));

    u8 * dst = chain;
    for (int level = 0; level <= mipLevels; ++level)
    {
        const int levelBytes = (wal.width >> level) * (wal.height >> level);
        std::memcpy(dst, wal.levels[level], static_cast<size_t>(levelBytes));
        dst += levelBytes;
    }
    return chain;
}

// Checkerboards for the DebugTexture() variants, RGB16. Variant 0 (pink) is
// the classic missing-image stand-in; the others give test scenes several
// distinct textures to exercise VRAM streaming.
constexpr int kCheckerDim     = 32;
constexpr int kCheckerSquares = 4;

const u16 * MakeCheckerPattern(int variant)
{
    if (variant < 0 || variant >= kNumDebugTextures)
    {
        variant = 0;
    }

    constexpr auto Rgb16 = [](u32 r, u32 g, u32 b) -> u16
    {
        return static_cast<u16>((1u << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    };

    // One bright color per variant, checkered against black.
    constexpr u16 variantColors[kNumDebugTextures] = {
        Rgb16(255, 100, 255), // pink
#if PS2_QUAKE_DEBUG
        Rgb16(255,  60,  60), // red
        Rgb16( 60, 255,  60), // green
        Rgb16( 80,  80, 255), // blue
        Rgb16(255, 255,  60), // yellow
        Rgb16( 60, 255, 255), // cyan
#endif // PS2_QUAKE_DEBUG
    };
    const u16 colors[2] = { variantColors[variant], Rgb16(0, 0, 0) };

    alignas(16) static u16 s_buffers[kNumDebugTextures][kCheckerDim * kCheckerDim];
    u16 * buffer = s_buffers[variant];

    constexpr int squareSize = kCheckerDim / kCheckerSquares;
    for (int y = 0; y < kCheckerDim; ++y)
    {
        for (int x = 0; x < kCheckerDim; ++x)
        {
            const int colorIndex = ((y / squareSize) + (x / squareSize)) % 2;
            buffer[x + (y * kCheckerDim)] = colors[colorIndex];
        }
    }

    return buffer;
}

// The particle image, generated rather than loaded - Quake 2 never shipped one
// as a file, ref_gl builds its own in R_InitParticleTexture.
//
// It is Alpha8: one coverage byte per texel, sampled through the shared
// alpha-ramp CLUT, which supplies the GS modulate identity (128) as the colour
// and the byte itself as the alpha. So a particle's colour rides entirely on
// its vertex colour and the image contributes only its shape. The ramp maps
// coverage 255 to alpha 128 (= 1.0 on the GS), so a fully opaque particle
// blends at exactly 1x rather than the ~2x an 0xFF alpha would give; coverage 0
// maps to alpha 0, and those texels never reach the blender at all - the
// batch's alpha test drops them.
//
// The dimension is a power of two, so no ST rescale is needed
// (unlike the model skins - see StScaleFor).
constexpr int kParticleDim = 32;

// Coverage falling off smoothly from the centre to nothing at the edge, drawn
// as a full sprite. The falloff is 1 - d^2 over the radius, squared again,
// which keeps a bright core and a long thin tail instead of the linear ramp's
// visible disc edge.
const u8 * MakeParticlePattern()
{
    constexpr float kCentre = (kParticleDim - 1) * 0.5f;
    constexpr float kRadius = kParticleDim * 0.5f;

    alignas(16) static u8 s_buffer[kParticleDim * kParticleDim];
    for (int y = 0; y < kParticleDim; ++y)
    {
        for (int x = 0; x < kParticleDim; ++x)
        {
            const float dx = (static_cast<float>(x) - kCentre) / kRadius;
            const float dy = (static_cast<float>(y) - kCentre) / kRadius;

            float falloff = 1.0f - ((dx * dx) + (dy * dy));
            falloff = (falloff <= 0.0f) ? 0.0f : (falloff * falloff);

            const u32 coverage = static_cast<u32>(falloff * 255.0f);
            s_buffer[x + (y * kParticleDim)] = static_cast<u8>((coverage > 255u) ? 255u : coverage);
        }
    }
    return s_buffer;
}

// ------------------------------------------------------------------------------------------------
// TextureCache
// ------------------------------------------------------------------------------------------------

// Owns the texture pool and the name lookup. Internal singleton (s_cache);
// the module API below is the public face.
class TextureCache final
{
public:
    void Init();
    const Texture * Find(const char * name, const ImageType type, bool async);
    const Texture & DebugTexture(int variant) const;
    const Texture & ParticleTexture() const;

    void BeginRegistration();
    void EndRegistration();

    // Background loads: see tex::PumpLoads / tex::DrainLoads.
    int  PumpLoads(unsigned budgetCycles);
    void DrainLoads();

    void SetTouchOnly(const bool enable) { m_touchOnly = enable; }
    bool IsTouchOnly() const { return m_touchOnly; }

    // Frees the level assets not stamped this cycle; see tex::FreeUnregistered. Returns how many.
    int FreeUnregistered(ImageType onlyType, bool early);

    // Stamp a texture as used this registration cycle (see tex::TouchTexture).
    void MarkReferenced(const Texture & texture)
    {
        const_cast<Texture &>(texture).regSequence = m_regSequence;
    }

private:
    // A new slot under 'name', in the lookup, with no pixels yet: Register fills one at once,
    // a background load when its file lands.
    Texture & AllocSlot(const char * name, ImageType type, TexFlags flags);
    void FillTexture(Texture & texture, const void * pixels, int width, int height,
                     PixelFormat format, TexComponents components, TexFlags flags);
    Texture & Register(const char * name, const void * pixels, int width, int height,
                       PixelFormat format, TexComponents components,
                       ImageType type, TexFlags flags);

    // From decoded texels to a texture, the steps the synchronous and the background path share:
    // 'into' is the pending slot to fill, or null to allocate one under 'fullname'.
    Texture & CommitIndexed(Texture * into, const char * fullname, ImageType type, u8 * pic8,
                            int width, int height, int mipLevels, bool transparent);
    Texture & CommitRgba(Texture * into, const char * fullname, ImageType type, u8 * pic32,
                         int width, int height, bool hasAlpha);

    const Texture * LoadFromFile(const char * fullname, ImageType type, bool async);
    void Unload(u16 slot);

    // A read in flight for a Loading slot.
    struct PendingLoad
    {
        bool          active;
        bool          tga;
        u16           slot;
        aio::Ticket   ticket;
        fs_location_t loc;
        u8 *          staging;
        u32           stagingBytes;
        int           length;
    };
    enum class AsyncStart { Missing, Queued, Declined };

    AsyncStart StartAsyncLoad(const char * fullname, ImageType type, bool tga, const Texture ** out);
    void FinishPending(PendingLoad & pending);

    static constexpr int kMaxPendingLoads = 16;
    PendingLoad m_pending[kMaxPendingLoads];
    int         m_numPending = 0;

    // Between BeginRegistration and EndRegistration: a level load, under the plaque, where every
    // load stays synchronous.
    bool m_registering = false;

    // Worst case for a full level plus UI (walls, skins, sprites, sky and
    // pics). ref_gl's MAX_GLTEXTURES was 1024, but that is a PC-era bound and
    // each idle slot still costs .bss here - the cap lives in texture.h now
    // (view.cpp sizes its texture chain to it too).
    using TexturePool = SmallPool<Texture, kMaxTextures>;

    TexturePool m_texturePool;
    const Texture * m_debugTextures[kNumDebugTextures] = {};
    const Texture * m_particleTexture = nullptr;

    // Level load/change cycle counter; textures stamped with an older value
    // are the ones EndRegistration() frees. See tex::BeginRegistration().
    // Starts at 1, not 0, so a freshly zeroed Texture slot (regSequence 0) never
    // looks like it was registered this cycle. Init() applies that, rather than a
    // default member initializer here: this cache is a file-level static, and one
    // non-zero word in it is enough to move the whole ~90 KB object out of .bss
    // and into .data, where the zeros cost ELF file size for nothing.
    u32 m_regSequence = 0;

    // Lookup: FNV-1a hash of the full path + image type -> pool slot of the texture.
    HashMap<kMaxTextures> m_lookup;

    // See tex::SetTouchOnly.
    bool m_touchOnly = false;

#if PS2_QUAKE_ASSERTS
    // What the early sweeps dropped this cycle, by lookup key. A texture freed there and then
    // loaded again before EndRegistration was one the new level uses after all - a name the touch
    // pass missed - and costs a second read from disk. Reported as it happens and counted, so
    // the MapCycle log shows whether the touch pass is complete. Bounded: a cycle drops a few
    // hundred at most; past the cap it stops recording rather than growing.
    static constexpr int kMaxEarlyFrees = 1024;
    u64 m_earlyFreedKeys[kMaxEarlyFrees] = {};
    int m_earlyFreedCount = 0;
    int m_reloadedCount   = 0;
#endif // PS2_QUAKE_ASSERTS
};

const Texture * TextureCache::Find(const char * name, const ImageType type, const bool async)
{
    PS2_Assert(name != nullptr && *name != '\0');
    PS2_Assert(type != ImageType::Null);

    char fullname[MAX_QPATH];
    NormalizeName(name, type, fullname);

    const u64 key  = LookupKey(fullname, type);
    const u16 slot = m_lookup.Find(key);

    if (slot == m_lookup.kInvalidValue)
    {
        if (m_touchOnly)
        {
            return nullptr; // Stamping only; the registration proper loads it.
        }

#if PS2_QUAKE_ASSERTS
        for (int i = 0; i < m_earlyFreedCount; ++i)
        {
            if (m_earlyFreedKeys[i] == key)
            {
                Com_Printf("WARNING: texture '%s' freed before load and then loaded again.\n", fullname);
                ++m_reloadedCount;
                break;
            }
        }
#endif // PS2_QUAKE_ASSERTS

        return LoadFromFile(fullname, type, async);
    }

    Texture & texture = m_texturePool.Slot(slot);

    // 64-bit FNV-1a collisions are vanishingly rare, but a miss here would
    // silently draw the wrong image - verify the actual name and type.
    PS2_AssertMsg(std::strcmp(texture.name, fullname) == 0 && texture.type == type,
                  "Texture lookup hash collision!");

    texture.regSequence = m_regSequence; // still in use this cycle
    return &texture;
}

const Texture * TextureCache::LoadFromFile(const char * fullname, const ImageType type, const bool async)
{
    const char * extension = std::strrchr(fullname, '.');
    if (extension == nullptr) [[unlikely]]
    {
        Com_DPrintf("WARNING: Image '%s' has no file extension!\n", fullname);
        return nullptr;
    }

    const bool pcx = (std::strcmp(extension, ".pcx") == 0);
    const bool wal = (std::strcmp(extension, ".wal") == 0);
    const bool tga = (std::strcmp(extension, ".tga") == 0);
    if (!pcx && !wal && !tga) [[unlikely]]
    {
        Com_DPrintf("WARNING: Unsupported image format '%s'!\n", fullname);
        return nullptr;
    }

    // A skin, sprite frame or pic first wanted while a level runs loads in the background: the
    // read was 41 ms from the HDD and over 90 from a USB stick, in the frame that asked. Walls
    // and sky faces only load with a level, under the plaque, and stay synchronous; so does
    // everything during registration, and a by-name 2D draw (async false), which menus rely on.
    if (async && !wal && !m_registering && !m_touchOnly &&
        (type == ImageType::Skin || type == ImageType::Sprite || type == ImageType::Pic) && aio::Enabled())
    {
        const Texture * pending = nullptr;
        switch (StartAsyncLoad(fullname, type, tga, &pending))
        {
        case AsyncStart::Missing:  return nullptr;
        case AsyncStart::Queued:   return pending;
        case AsyncStart::Declined: break; // no slot, memory or ticket: synchronously, below
        }
    }

    int width  = 0;
    int height = 0;

    if (tga)
    {
        u8 * pic32 = nullptr;
        bool hasAlpha = false;
        if (!LoadTga(fullname, &pic32, &width, &height, &hasAlpha))
        {
            return nullptr;
        }
        return &CommitRgba(nullptr, fullname, type, pic32, width, height, hasAlpha);
    }

    // PCX and WAL are both 8-bit palette indices, kept that way: they sample through the
    // global-palette CLUT uploaded at init, at a quarter of the RGBA32 footprint in RAM and VRAM.
    u8 * pic8        = nullptr;
    int  mipLevels   = 0; // levels 'pixels' carries after level 0 (WAL walls only)
    bool transparent = false;
    if (pcx)
    {
        u8 * fileData = nullptr;
        const int fileLen = FS_LoadFile(fullname, reinterpret_cast<void **>(&fileData));
        if (fileData == nullptr)
        {
            Com_DPrintf("WARNING: Can't load PCX file '%s'\n", fullname);
            return nullptr;
        }
        const bool decoded = DecodePcx(fullname, fileData, fileLen, &pic8, &width, &height, &transparent);
        FS_FreeFile(fileData);
        if (!decoded)
        {
            return nullptr;
        }
    }
    else
    {
        WalFile walFile;
        if (!LoadWal(fullname, &walFile))
        {
            return nullptr;
        }

        // A wall's mip levels are the file's own. They go up to the power-of-two extent
        // below along with level 0, so that is the size the count follows; a file whose
        // chain comes up short of it loads without any.
        if (type == ImageType::Wall && s_wallMipmaps)
        {
            const int wanted = MipLevelsFor(1 << tex::Log2(static_cast<u32>(walFile.width)),
                                            1 << tex::Log2(static_cast<u32>(walFile.height)));
            mipLevels = (walFile.numLevels - 1 >= wanted) ? wanted : 0;
        }

        width  = walFile.width;
        height = walFile.height;
        pic8   = CopyWalLevels(walFile, mipLevels);
        FreeWal(walFile);
        transparent = HasTransparentTexels(pic8, width * height);
    }

    if (type == ImageType::Sky && s_skyDownsample)
    {
        pic8 = DownsampleIndexed2x(pic8, &width, &height);
    }

    // World textures are the ones that tile, so they are the ones that have to be a power of two;
    // everything else samples within [0, 1] and is served by a coordinate scale instead. Keep the
    // size the file had: the BSP's texture coordinates normalize against it, not against what
    // was uploaded.
    const int srcWidth  = width;
    const int srcHeight = height;
    if (type == ImageType::Wall)
    {
        pic8 = ResampleToPowerOfTwo(pic8, &width, &height, BytesPerTexel(PixelFormat::Palette8), mipLevels);
    }

    Texture & texture = CommitIndexed(nullptr, fullname, type, pic8, width, height, mipLevels, transparent);
    texture.srcWidth  = static_cast<s16>(srcWidth);
    texture.srcHeight = static_cast<s16>(srcHeight);

    // The flag and the size are all that record how many levels there are.
    PS2_Assert(MipLevels(texture) == mipLevels);
    return &texture;
}

Texture & TextureCache::CommitIndexed(Texture * const into, const char * fullname, const ImageType type, u8 * pic8,
                                      const int width, const int height, const int mipLevels, const bool transparent)
{
    // Sky faces always sample as RGB, whatever they contain. The 3D path's alpha test cuts texels
    // whose alpha is zero (gs::MakePixelTests), and palette entry 255 is exactly that - so a sky
    // face that happened to use index 255 would be punched through to the clear colour instead of
    // drawing. No stock env/ face does, but ref_gl's sky upload path skips its transparency
    // handling for the same reason, and the sky is the one texture with nothing behind it to
    // show through to.
    const TexComponents components = (type != ImageType::Sky && transparent) ? TexComponents::RGBA
                                                                              : TexComponents::RGB;

    // Small HUD/menu images go into a shared scrap rather than each holding a GS page of their
    // own. Only Pics: world textures and sprites tile or are large enough that the page waste is
    // negligible, and a tiling texture could not use an atlas anyway. Built-ins never reach here,
    // which is what keeps "backtile" out - it is exactly 64x64 but Draw_TileClear addresses it
    // in screen space and needs WRAP_REPEAT.
    if (type == ImageType::Pic && scrap::IsPackable(width, height))
    {
        const Texture * atlas = nullptr;
        int atlasX = 0;
        int atlasY = 0;

        if (scrap::TryPack(pic8, width, height, &atlas, &atlasX, &atlasY))
        {
            // The texels live in the atlas now; the decoded copy is dead. FillTexture still wants
            // a non-null 'pixels', and every assert that checks it should keep passing, so point
            // it at the atlas - Unload knows not to free it.
            ps2::heap::Free(pic8, static_cast<size_t>(width * height), ps2::heap::MemTag::TexImage);

            Texture & packed = (into != nullptr) ? *into : AllocSlot(fullname, type, TexFlags::None);
            FillTexture(packed, atlas->pixels, width, height, PixelFormat::Palette8, components, TexFlags::None);
            packed.atlas       = atlas;
            packed.atlasX      = static_cast<s16>(atlasX);
            packed.atlasY      = static_cast<s16>(atlasY);
            packed.dirtyPixels = false; // the atlas carries the dirty flag, not the view
            return packed;
        }
    }

    Texture & texture = (into != nullptr) ? *into : AllocSlot(fullname, type, TexFlags::None);
    FillTexture(texture, pic8, width, height, PixelFormat::Palette8, components,
                (mipLevels > 0) ? TexFlags::Mipmapped : TexFlags::None);
    return texture;
}

Texture & TextureCache::CommitRgba(Texture * const into, const char * fullname, const ImageType type, u8 * pic32,
                                   const int width, const int height, const bool hasAlpha)
{
    // True-colour images have no CLUT to carry the intensity for them, so a lit one takes the
    // scale in its own texels here. Palettized images - which is everything the retail game
    // ships - sample a pre-brightened CLUT instead, built once by gs::Init.
    if (TakesIntensity(type))
    {
        ScaleTexelsForIntensity(pic32, width * height, s_intensityScale);
    }

    Texture & texture = (into != nullptr) ? *into : AllocSlot(fullname, type, TexFlags::None);
    FillTexture(texture, pic32, width, height, PixelFormat::RGBA32,
                hasAlpha ? TexComponents::RGBA : TexComponents::RGB, TexFlags::None);
    return texture;
}

// ------------------------------------------------------------------------------------------------
// Background loads
// ------------------------------------------------------------------------------------------------

TextureCache::AsyncStart TextureCache::StartAsyncLoad(const char * fullname, const ImageType type, const bool tga,
                                                      const Texture ** const out)
{
    PendingLoad * entry = nullptr;
    for (PendingLoad & p : m_pending)
    {
        if (!p.active)
        {
            entry = &p;
            break;
        }
    }
    if (entry == nullptr)
    {
        return AsyncStart::Declined;
    }

    fs_location_t loc;
    const int length = FS_LocateFile(fullname, &loc);
    if (length < 0)
    {
        return AsyncStart::Missing; // the synchronous answer, as before
    }
    if (length == 0 || loc.fd < 0)
    {
        FS_ReleaseLocation(&loc);
        return AsyncStart::Declined;
    }

    const u32 stagingBytes = aio::RoundUp64(static_cast<u32>(length));
    u8 * const staging = static_cast<u8 *>(ps2::heap::TryAllocAligned(ps2::heap::MemAlign(64), stagingBytes, ps2::heap::MemTag::Aio));
    if (staging == nullptr)
    {
        FS_ReleaseLocation(&loc);
        return AsyncStart::Declined;
    }

    aio::Request request;
    request.kind      = (loc.kind == FS_LOC_PKZ_DEFLATED) ? aio::Kind::InflateWhole : aio::Kind::Read;
    request.prio      = aio::Priority::Asset;
    request.fd        = loc.fd;
    request.offset    = loc.offset;
    request.bytes     = loc.bytes;
    request.dest      = staging;
    request.destBytes = stagingBytes;
    request.rawBytes  = loc.rawBytes;

    const aio::Ticket ticket = aio::Submit(request);
    if (!ticket.IsValid())
    {
        ps2::heap::Free(staging, stagingBytes, ps2::heap::MemTag::Aio);
        FS_ReleaseLocation(&loc);
        return AsyncStart::Declined;
    }

    Texture & texture = AllocSlot(fullname, type, TexFlags::Loading);

    entry->active       = true;
    entry->tga          = tga;
    entry->slot         = static_cast<u16>(m_texturePool.IndexOf(texture));
    entry->ticket       = ticket;
    entry->loc          = loc;
    entry->staging      = staging;
    entry->stagingBytes = stagingBytes;
    entry->length       = length;
    ++m_numPending;

    *out = &texture;
    return AsyncStart::Queued;
}

void TextureCache::FinishPending(PendingLoad & pending)
{
    Texture & texture = m_texturePool.Slot(pending.slot);
    PS2_Assert(HasFlag(texture.flags, TexFlags::Loading));

    const aio::State state  = aio::Poll(pending.ticket);
    const int        result = aio::Result(pending.ticket);

    bool ok = (state == aio::State::Done && result == pending.length);
    if (ok)
    {
        int width  = 0;
        int height = 0;
        if (pending.tga)
        {
            u8 * pic32 = nullptr;
            bool hasAlpha = false;
            ok = DecodeTga(texture.name, pending.staging, pending.length, &pic32, &width, &height, &hasAlpha);
            if (ok)
            {
                CommitRgba(&texture, texture.name, texture.type, pic32, width, height, hasAlpha);
            }
        }
        else
        {
            u8 * pic8 = nullptr;
            bool transparent = false;
            ok = DecodePcx(texture.name, pending.staging, pending.length, &pic8, &width, &height, &transparent);
            if (ok)
            {
                CommitIndexed(&texture, texture.name, texture.type, pic8, width, height, 0, transparent);
            }
        }
    }

    if (!ok)
    {
        // Never drawn from here on; the slot stays so repeated Finds don't retry.
        texture.flags = TexFlags::Failed;
        Com_Printf("WARNING: '%s': background load failed (%d); not drawn.\n", texture.name, result);
    }

    aio::Release(pending.ticket);
    ps2::heap::Free(pending.staging, pending.stagingBytes, ps2::heap::MemTag::Aio);
    FS_ReleaseLocation(&pending.loc);
    debug::FrameLogNoteReady(texture.name);

    pending.active  = false;
    pending.staging = nullptr;
    --m_numPending;
}

int TextureCache::PumpLoads(const unsigned budgetCycles)
{
    if (m_numPending == 0)
    {
        return 0;
    }

    const debug::CpuCycles start = debug::ReadCycles();
    int finished = 0;
    for (PendingLoad & p : m_pending)
    {
        if (!p.active)
        {
            continue;
        }
        const aio::State state = aio::Poll(p.ticket);
        if (state != aio::State::Done && state != aio::State::Failed)
        {
            continue;
        }
        FinishPending(p);
        ++finished;
        if (debug::ReadCycles() - start > budgetCycles)
        {
            break;
        }
    }
    return finished;
}

void TextureCache::DrainLoads()
{
    for (PendingLoad & p : m_pending)
    {
        if (p.active)
        {
            aio::Wait(p.ticket);
            FinishPending(p);
        }
    }
}

void TextureCache::Unload(u16 slot)
{
    Texture & texture = m_texturePool.Slot(slot);
    PS2_Assert(!HasFlag(texture.flags, TexFlags::Builtin));
    PS2_AssertMsg(!HasFlag(texture.flags, TexFlags::Loading), "Unloading a texture the background loader still owns!");

    gs::ReleaseTexture(texture); // return its GS VRAM to the heap (no-op when not resident)

    // A scrapped image owns neither VRAM nor its pixels - 'pixels' points into
    // the shared atlas, and freeing it here would take the atlas with it (at the
    // wrong size, no less). Scraps are monotonic and never reclaim a slot, so
    // there is nothing to give back. Pics are exempt from eviction anyway, which
    // makes this unreachable today; it is here so it stays correct if that
    // policy ever changes.
    if (texture.atlas == nullptr && texture.pixels != nullptr) // a Failed load has none
    {
        ps2::heap::Free(const_cast<void *>(texture.pixels), static_cast<size_t>(PixelBytes(texture)),
                        ps2::heap::MemTag::TexImage);
    }

    m_texturePool.Free(slot); // resets the slot; its type reads Null again
}

void TextureCache::BeginRegistration()
{
    // Nothing of the loader's may be in flight while this cycle's sweeps free slots.
    DrainLoads();
    ++m_regSequence;
    m_registering = true;
}

int TextureCache::FreeUnregistered(const ImageType onlyType, const bool early)
{
    // Free the level assets this registration cycle no longer references.
    // Pics are exempt like in ref_gl - the client caches pointers to them
    // across levels and they are small; built-ins are permanent.
    const int freedCount = static_cast<int>(m_lookup.RemoveIf([this, onlyType, early](u64 key, u16 slot) {
        const Texture & texture = m_texturePool.Slot(slot);
        if (HasFlag(texture.flags, TexFlags::Builtin) ||
            texture.type == ImageType::Pic ||
            texture.regSequence == m_regSequence ||
            (onlyType != ImageType::Null && texture.type != onlyType))
        {
            return false;
        }

#if PS2_QUAKE_ASSERTS
        if (early && m_earlyFreedCount < kMaxEarlyFrees)
        {
            m_earlyFreedKeys[m_earlyFreedCount++] = key;
        }
#else // PS2_QUAKE_ASSERTS
        (void)key;
        (void)early;
#endif // PS2_QUAKE_ASSERTS

        Com_DPrintf("Freeing unused texture '%s'\n", texture.name);
        Unload(slot);
        return true;
    }));

    if (freedCount > 0)
    {
        Com_DPrintf("Texture cache: freed %d unused textures%s.\n", freedCount, early ? " before load" : "");
        gs::DefragVramHeap();
    }
    return freedCount;
}

void TextureCache::EndRegistration()
{
    PS2_AssertMsg(!m_touchOnly, "Registration ended with touch-only mode still on!");

    FreeUnregistered(ImageType::Null, /*early=*/false);

#if PS2_QUAKE_ASSERTS
    if (m_reloadedCount > 0)
    {
        Com_Printf("WARNING: %d texture(s) freed before load were loaded again.\n", m_reloadedCount);
    }
    m_earlyFreedCount = 0;
    m_reloadedCount   = 0;
#endif // PS2_QUAKE_ASSERTS

    scrap::DumpUsage();
    m_registering = false;
}

const Texture & TextureCache::DebugTexture(int variant) const
{
    if (variant < 0 || variant >= kNumDebugTextures)
    {
        variant = 0;
    }
    return *m_debugTextures[variant];
}

const Texture & TextureCache::ParticleTexture() const
{
    return *m_particleTexture;
}

Texture & TextureCache::AllocSlot(const char * name, const ImageType type, const TexFlags flags)
{
    const u16 slot = m_texturePool.Alloc();
    if (slot == TexturePool::kInvalidIndex) [[unlikely]]
    {
        Sys_Error("Out of texture cache slots for '%s'! Bump tex::kMaxTextures (%u).",
                  name, kMaxTextures);
    }

    // Pics and sprites keep crisp texels (and their transparency cutouts
    // fringe-free); skins, walls and sky get smoothed by bilinear sampling.
    // The GS filters the post-CLUT colors, so Palette8 works with Linear too.
    const TexFilter filter = (type == ImageType::Pic || type == ImageType::Sprite)
                             ? TexFilter::Nearest : TexFilter::Linear;

    Texture & texture = m_texturePool.Slot(slot);
    std::snprintf(texture.name, sizeof(texture.name), "%s", name);

    texture.regSequence  = m_regSequence;
    texture.pixels       = nullptr; // FillTexture's
    texture.width        = 0;
    texture.height       = 0;
    texture.srcWidth     = 0;
    texture.srcHeight    = 0;
    texture.type         = type;
    texture.flags        = flags;
    texture.format       = PixelFormat::Palette8;
    texture.components   = TexComponents::RGB;
    texture.function     = TexFunction::Modulate;
    texture.magFilter    = filter;
    texture.minFilter    = filter;
    texture.textureChain = nullptr;
    texture.atlas        = nullptr; // the caller packs it into a scrap afterwards, if it fits
    texture.atlasX       = 0;
    texture.atlasY       = 0;
    texture.vramAddr     = Texture::kNotResident;
    texture.dirtyPixels  = false;

    const bool inserted = m_lookup.Insert(LookupKey(texture.name, texture.type), slot);
    PS2_AssertMsg(inserted, "Duplicate texture name+type!");

    return texture;
}

void TextureCache::FillTexture(Texture & texture, const void * pixels, const int width, const int height,
                               const PixelFormat format, const TexComponents components, const TexFlags flags)
{
    PS2_Assert(width > 0 && height > 0 && pixels != nullptr);
    PS2_AssertMsg(width <= INT16_MAX && height <= INT16_MAX, "Texture width/height too big!");

    // Builtin is the slot's own; everything else, Loading above all, is this fill's to say.
    const bool builtin = HasFlag(texture.flags, TexFlags::Builtin);

    texture.pixels      = pixels;
    texture.width       = static_cast<s16>(width);
    texture.height      = static_cast<s16>(height);
    texture.srcWidth    = static_cast<s16>(width);  // LoadFromFile overrides these when it resamples
    texture.srcHeight   = static_cast<s16>(height);
    texture.format      = format;
    texture.components  = components;
    texture.flags       = builtin ? (flags | TexFlags::Builtin) : flags;
    texture.dirtyPixels = !builtin; // loader-written pixels may still sit in the dcache;
                                    // the first upload must flush them (built-ins were
                                    // written by the ELF loader and need no flush).
}

Texture & TextureCache::Register(const char * name, const void * pixels, int width, int height,
                                 PixelFormat format, TexComponents components,
                                 ImageType type, TexFlags flags)
{
    Texture & texture = AllocSlot(name, type, flags);
    FillTexture(texture, pixels, width, height, format, components, flags);
    return texture;
}

void TextureCache::Init()
{
    m_texturePool.Init(); // One-shot; asserts if called twice.
    m_regSequence = 1;    // See the member declaration for why it starts here.

    struct BuiltinImage
    {
        const char *  name;
        const void *  pixels;
        int           width;
        int           height;
        PixelFormat   format;
        TexComponents components;
    };
    const BuiltinImage builtins[] =
    {
        // The embedded images are 8-bit palette indices (imgdump "pal" mode)
        // sampling through the shared global-palette CLUT - a quarter of the
        // RGBA32 footprint in VRAM. conchars keeps RGBA components: its
        // transparent pixels are palette index 255 (alpha 0 in the CLUT),
        // which the alpha test cuts out. Only the generated debug
        // checkerboards below stay RGB16. The id logo is in the ELF for the boot
        // splash, so the main menu takes it from there too; its ragged corners cut
        // out the way conchars' background does.
        { "pics/conchars.pcx",    conchars_data,         conchars_width,    conchars_height,    PixelFormat::Palette8, TexComponents::RGBA },
        { "pics/conback.pcx",     conback_data,          conback_width,     conback_height,     PixelFormat::Palette8, TexComponents::RGB  },
        { "pics/backtile.pcx",    backtile_data,         backtile_width,    backtile_height,    PixelFormat::Palette8, TexComponents::RGB  },
        { "pics/m_main_logo.pcx", m_main_logo_data,      m_main_logo_width, m_main_logo_height, PixelFormat::Palette8, TexComponents::RGBA },
        { "pics/debug0.pcx",      MakeCheckerPattern(0), kCheckerDim,       kCheckerDim,        PixelFormat::RGB16,    TexComponents::RGB  },
#if PS2_QUAKE_DEBUG // Extra debug textures for the textured cube test:
        { "pics/debug1.pcx",      MakeCheckerPattern(1), kCheckerDim,       kCheckerDim,        PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug2.pcx",      MakeCheckerPattern(2), kCheckerDim,       kCheckerDim,        PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug3.pcx",      MakeCheckerPattern(3), kCheckerDim,       kCheckerDim,        PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug4.pcx",      MakeCheckerPattern(4), kCheckerDim,       kCheckerDim,        PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug5.pcx",      MakeCheckerPattern(5), kCheckerDim,       kCheckerDim,        PixelFormat::RGB16,    TexComponents::RGB  },
#endif // PS2_QUAKE_DEBUG
    };

    for (const BuiltinImage & builtin : builtins)
    {
        Register(builtin.name, builtin.pixels, builtin.width, builtin.height,
                 builtin.format, builtin.components, ImageType::Pic, TexFlags::Builtin);
    }

    // The checkerboards and the particle images below are the built-ins we
    // generate here at runtime rather than link into the ELF, so - unlike the
    // rest of the table - their pixels may still be sitting in the EE data
    // cache. Register() only marks non-built-ins dirty, so mark them by hand or
    // their first upload DMAs stale memory.
    for (int i = 0; i < kNumDebugTextures; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "debug%d", i);
        m_debugTextures[i] = Find(name, ImageType::Pic, /*async=*/false);
        PS2_Assert(m_debugTextures[i] != nullptr);
        m_debugTextures[i]->MarkPixelsDirty();
    }

    // The particle image is registered outside the table above so its filtering
    // can be set: the smooth falloff would band badly without bilinear.
    Texture & particle = Register("pics/particle.pcx", MakeParticlePattern(),
                                  kParticleDim, kParticleDim,
                                  PixelFormat::Alpha8, TexComponents::RGBA,
                                  ImageType::Pic, TexFlags::Builtin);
    particle.magFilter = TexFilter::Linear;
    particle.minFilter = TexFilter::Linear;
    particle.MarkPixelsDirty();
    m_particleTexture = &particle;

    Com_Printf("Texture cache initialised: %u built-in images registered.\n", m_texturePool.UsedCount());
}

static TextureCache s_cache;

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init(const float intensity)
{
    PS2_Assert(intensity >= 1.0f);
    s_intensityScale = intensity;

    s_cache.Init();
}

void BeginRegistration()
{
    s_cache.BeginRegistration();
}

void EndRegistration()
{
    s_cache.EndRegistration();
}

void SetTouchOnly(const bool enable)
{
    s_cache.SetTouchOnly(enable);
}

bool IsTouchOnly()
{
    return s_cache.IsTouchOnly();
}

void FreeUnregistered(const ImageType onlyType)
{
    s_cache.FreeUnregistered(onlyType, /*early=*/true);
}

const Texture * Find(const char * name, const ImageType type)
{
    return s_cache.Find(name, type, /*async=*/false);
}

const Texture * FindAsync(const char * name, const ImageType type)
{
    return s_cache.Find(name, type, /*async=*/true);
}

void PumpLoads(const unsigned budgetCycles)
{
    s_cache.PumpLoads(budgetCycles);
}

void DrainLoads()
{
    s_cache.DrainLoads();
}

void SetSkyDownsample(const bool enable)
{
    s_skyDownsample = enable;
}

bool WallMipmaps()
{
    return s_wallMipmaps;
}

void SetWallMipmaps(const bool enable)
{
    if (enable == s_wallMipmaps)
    {
        return;
    }
    s_wallMipmaps = enable;

    // Nothing is stamped yet this early in a registration, so this is every wall. Not an early
    // free as the touch pass means it: these reload by design, and are no miss to report.
    s_cache.FreeUnregistered(ImageType::Wall, /*early=*/false);
}

void TouchTexture(const Texture & texture)
{
    s_cache.MarkReferenced(texture);
}

const Texture & DebugTexture(int variant)
{
    return s_cache.DebugTexture(variant);
}

const Texture & ParticleTexture()
{
    return s_cache.ParticleTexture();
}

} // namespace ps2::tex
