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

#include <cstdio>
#include <cstring>

namespace ps2::tex {
namespace {

// Cache lookup key: the name hash continued with the image type as one extra
// FNV-1a byte, so the same file may be cached independently per ImageType.
inline u64 LookupKey(const char * fullname, ImageType type)
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

    // TODO: Precompute and cache ramp values.
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

// Set by tex::SetSkyDownsample(); consumed by LoadFromFile for Sky images.
static bool s_skyDownsample = false;

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

// The two particle images, generated rather than loaded - Quake 2 never
// shipped either as a file, ref_gl builds its own in R_InitParticleTexture.
//
// Both are Alpha8: one coverage byte per texel, sampled through the shared
// alpha-ramp CLUT, which supplies the GS modulate identity (128) as the colour
// and the byte itself as the alpha. So a particle's colour rides entirely on
// its vertex colour and the image contributes only its shape. The ramp maps
// coverage 255 to alpha 128 (= 1.0 on the GS), so a fully opaque particle
// blends at exactly 1x rather than the ~2x an 0xFF alpha would give; coverage 0
// maps to alpha 0, and those texels never reach the blender at all - the
// batch's alpha test drops them.
//
// Both dimensions are powers of two, so no ST rescale is needed
// (unlike the model skins - see StScaleFor).
constexpr int kParticleDotDim = 8;
constexpr int kParticleHdDim  = 32;

// The classic blocky dot, exactly ref_gl's 8x8 pattern. It sits in the
// top-left corner because the particle draws as a single triangle covering
// only that half of the image.
const u8 * MakeParticleDotPattern()
{
    constexpr u8 prtDot[kParticleDotDim][kParticleDotDim] = {
        { 0,0,0,0,0,0,0,0 },
        { 0,0,1,1,0,0,0,0 },
        { 0,1,1,1,1,0,0,0 },
        { 0,1,1,1,1,0,0,0 },
        { 0,0,1,1,0,0,0,0 },
        { 0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0 },
        { 0,0,0,0,0,0,0,0 },
    };

    alignas(16) static u8 s_buffer[kParticleDotDim * kParticleDotDim];
    for (int y = 0; y < kParticleDotDim; ++y)
    {
        for (int x = 0; x < kParticleDotDim; ++x)
        {
            s_buffer[x + (y * kParticleDotDim)] = prtDot[y][x] ? 255 : 0;
        }
    }
    return s_buffer;
}

// The soft round particle: coverage falling off smoothly from the centre to
// nothing at the edge, drawn as a full quad. The falloff is 1 - d^2 over the
// radius, squared again, which keeps a bright core and a long thin tail
// instead of the linear ramp's visible disc edge.
const u8 * MakeParticleHdPattern()
{
    constexpr float kCentre = (kParticleHdDim - 1) * 0.5f;
    constexpr float kRadius = kParticleHdDim * 0.5f;

    alignas(16) static u8 s_buffer[kParticleHdDim * kParticleHdDim];
    for (int y = 0; y < kParticleHdDim; ++y)
    {
        for (int x = 0; x < kParticleHdDim; ++x)
        {
            const float dx = (static_cast<float>(x) - kCentre) / kRadius;
            const float dy = (static_cast<float>(y) - kCentre) / kRadius;

            float falloff = 1.0f - ((dx * dx) + (dy * dy));
            falloff = (falloff <= 0.0f) ? 0.0f : (falloff * falloff);

            const u32 coverage = static_cast<u32>(falloff * 255.0f);
            s_buffer[x + (y * kParticleHdDim)] = static_cast<u8>((coverage > 255u) ? 255u : coverage);
        }
    }
    return s_buffer;
}

// Owns the texture pool and the name lookup. Internal singleton (s_cache);
// the module API below is the public face.
class TextureCache final
{
public:
    void Init();
    const Texture * Find(const char * name, const ImageType type);
    const Texture & DebugTexture(int variant) const;
    const Texture & ParticleTexture(bool highQuality) const;

    void BeginRegistration();
    void EndRegistration();

    // Stamp a texture as used this registration cycle (see tex::TouchTexture).
    void MarkReferenced(const Texture & texture)
    {
        const_cast<Texture &>(texture).regSequence = m_regSequence;
    }

private:
    Texture & Register(const char * name, const void * pixels, int width, int height,
                       PixelFormat format, TexComponents components,
                       ImageType type, TexFlags flags);

    const Texture * LoadFromFile(const char * fullname, ImageType type);
    void Unload(u16 slot);

    // Worst case for a full level plus UI (walls, skins, sprites, sky and
    // pics). ref_gl's MAX_GLTEXTURES was 1024, but that is a PC-era bound and
    // each idle slot still costs .bss here - the cap lives in texture.h now
    // (render_view.cpp sizes its texture chain to it too).
    using TexturePool = SmallPool<Texture, kMaxTextures>;

    TexturePool m_texturePool;
    const Texture * m_debugTextures[kNumDebugTextures] = {};
    const Texture * m_particleTextures[2] = {}; // [0] = classic dot, [1] = HD

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
};

const Texture * TextureCache::Find(const char * name, const ImageType type)
{
    PS2_Assert(name != nullptr && *name != '\0');
    PS2_Assert(type != ImageType::Null);

    char fullname[MAX_QPATH];
    NormalizeName(name, type, fullname);

    const u16 slot = m_lookup.Find(LookupKey(fullname, type));
    if (slot == m_lookup.kInvalidValue)
    {
        return LoadFromFile(fullname, type);
    }

    Texture & texture = m_texturePool.Slot(slot);

    // 64-bit FNV-1a collisions are vanishingly rare, but a miss here would
    // silently draw the wrong image - verify the actual name and type.
    PS2_AssertMsg(std::strcmp(texture.name, fullname) == 0 && texture.type == type,
                  "Texture lookup hash collision!");

    texture.regSequence = m_regSequence; // still in use this cycle
    return &texture;
}

const Texture * TextureCache::LoadFromFile(const char * fullname, const ImageType type)
{
    const char * extension = std::strrchr(fullname, '.');
    if (extension == nullptr)
    {
        Com_DPrintf("WARNING: Image '%s' has no file extension!\n", fullname);
        return nullptr;
    }

    const void * pixels = nullptr;
    int width  = 0;
    int height = 0;
    PixelFormat format;
    TexComponents components;

    if (std::strcmp(extension, ".pcx") == 0 || std::strcmp(extension, ".wal") == 0)
    {
        // Both are 8-bit palette indices, kept that way: they sample through
        // the global-palette CLUT uploaded at init, at a quarter of the RGBA32
        // footprint in RAM and VRAM.
        u8 * pic8 = nullptr;
        const bool loaded = (extension[1] == 'p')
            ? img::LoadPcx(fullname, &pic8, &width, &height)
            : img::LoadWal(fullname, &pic8, &width, &height);
        if (!loaded)
        {
            return nullptr;
        }

        if (type == ImageType::Sky && s_skyDownsample)
        {
            pic8 = DownsampleIndexed2x(pic8, &width, &height);
        }

        format = PixelFormat::Palette8;
        pixels = pic8;

        // Sky faces always sample as RGB, whatever they contain. The 3D path's
        // alpha test cuts texels whose alpha is zero (vu1.cpp's MakeTestData),
        // and palette entry 255 is exactly that - so a sky face that happened
        // to use index 255 would be punched through to the clear colour
        // instead of drawing. No stock env/ face does, but ref_gl's sky upload
        // path skips its transparency handling for the same reason, and the
        // sky is the one texture with nothing behind it to show through to.
        components = (type != ImageType::Sky && HasTransparentTexels(pic8, width * height))
                   ? TexComponents::RGBA
                   : TexComponents::RGB;

        // Small HUD/menu images go into a shared scrap rather than each holding
        // a GS page of their own. Only Pics: world textures and sprites tile or
        // are large enough that the page waste is negligible, and a tiling
        // texture could not use an atlas anyway. Built-ins never reach here,
        // which is what keeps "backtile" out - it is exactly 64x64 but
        // Draw_TileClear addresses it in screen space and needs WRAP_REPEAT.
        if (type == ImageType::Pic && scrap::IsPackable(width, height))
        {
            const Texture * atlas = nullptr;
            int atlasX = 0;
            int atlasY = 0;

            if (scrap::TryPack(pic8, width, height, &atlas, &atlasX, &atlasY))
            {
                // The texels live in the atlas now; the decoded copy is dead.
                // Register still wants a non-null 'pixels', and every assert
                // that checks it should keep passing, so point it at the atlas -
                // Unload knows not to free it.
                ps2::heap::Free(pic8, static_cast<size_t>(width * height), ps2::heap::MemTag::TexImage);

                Texture & packed = Register(fullname, atlas->pixels, width, height,
                                            format, components, type, TexFlags::None);
                packed.atlas       = atlas;
                packed.atlasX      = static_cast<s16>(atlasX);
                packed.atlasY      = static_cast<s16>(atlasY);
                packed.dirtyPixels = false; // the atlas carries the dirty flag, not the view
                return &packed;
            }
        }
    }
    else if (std::strcmp(extension, ".tga") == 0)
    {
        u8 * pic32 = nullptr;
        bool hasAlpha = false;
        if (!img::LoadTga(fullname, &pic32, &width, &height, &hasAlpha))
        {
            return nullptr;
        }

        format     = PixelFormat::RGBA32;
        components = hasAlpha ? TexComponents::RGBA : TexComponents::RGB;
        pixels     = pic32;

        // True-colour images have no CLUT to carry ps2_intensity for them, so
        // a lit one takes the scale in its own texels here. That bakes in
        // whatever the value is at load time, unlike the palettized images the
        // retail game is made of, which follow the cvar live - a .tga picks up
        // a new value the next time it is loaded.
        if (TakesIntensity(type))
        {
            ScaleTexelsForIntensity(pic32, width * height, gs::IntensityScale());
        }
    }
    else
    {
        Com_DPrintf("WARNING: Unsupported image format '%s'!\n", fullname);
        return nullptr;
    }

    return &Register(fullname, pixels, width, height, format, components, type, TexFlags::None);
}

void TextureCache::Unload(u16 slot)
{
    Texture & texture = m_texturePool.Slot(slot);
    PS2_Assert(!HasFlag(texture.flags, TexFlags::Builtin));

    gs::ReleaseTexture(texture); // return its GS VRAM to the heap (no-op when not resident)

    // A scrapped image owns neither VRAM nor its pixels - 'pixels' points into
    // the shared atlas, and freeing it here would take the atlas with it (at the
    // wrong size, no less). Scraps are monotonic and never reclaim a slot, so
    // there is nothing to give back. Pics are exempt from eviction anyway, which
    // makes this unreachable today; it is here so it stays correct if that
    // policy ever changes.
    if (texture.atlas == nullptr)
    {
        const int pixelBytes = texture.width * texture.height * BytesPerTexel(texture.format);
        ps2::heap::Free(const_cast<void *>(texture.pixels), static_cast<size_t>(pixelBytes), ps2::heap::MemTag::TexImage);
    }

    m_texturePool.Free(slot); // resets the slot; its type reads Null again
}

void TextureCache::BeginRegistration()
{
    ++m_regSequence;
}

void TextureCache::EndRegistration()
{
    // Free the level assets this registration cycle no longer references.
    // Pics are exempt like in ref_gl - the client caches pointers to them
    // across levels and they are small; built-ins are permanent.
    const int freedCount = static_cast<int>(m_lookup.RemoveIf([this](u64, u16 slot) {
        const Texture & texture = m_texturePool.Slot(slot);
        if (HasFlag(texture.flags, TexFlags::Builtin) ||
            texture.type == ImageType::Pic ||
            texture.regSequence == m_regSequence)
        {
            return false;
        }

        Com_DPrintf("Freeing unused texture '%s'\n", texture.name);
        Unload(slot);
        return true;
    }));

    if (freedCount > 0)
    {
        Com_DPrintf("Texture cache: freed %d unused textures.\n", freedCount);
        gs::DefragVramHeap();
    }

    scrap::DumpUsage();
}

const Texture & TextureCache::DebugTexture(int variant) const
{
    if (variant < 0 || variant >= kNumDebugTextures)
    {
        variant = 0;
    }
    return *m_debugTextures[variant];
}

const Texture & TextureCache::ParticleTexture(bool highQuality) const
{
    return *m_particleTextures[highQuality ? 1 : 0];
}

Texture & TextureCache::Register(const char * name, const void * pixels, int width, int height,
                                 PixelFormat format, TexComponents components,
                                 ImageType type, TexFlags flags)
{
    PS2_Assert(width > 0 && height > 0 && pixels != nullptr);
    PS2_AssertMsg(width <= INT16_MAX && height <= INT16_MAX, "Texture width/height too big!");

    const u16 slot = m_texturePool.Alloc();
    if (slot == TexturePool::kInvalidIndex) [[unlikely]]
    {
        Sys_Error("Out of texture cache slots for '%s'! Bump tex::kMaxTextures (%u).",
                  name, kMaxTextures);
    }

    const bool builtin = HasFlag(flags, TexFlags::Builtin);

    // Pics and sprites keep crisp texels (and their transparency cutouts
    // fringe-free); skins, walls and sky get smoothed by bilinear sampling.
    // The GS filters the post-CLUT colors, so Palette8 works with Linear too.
    const TexFilter filter = (type == ImageType::Pic || type == ImageType::Sprite)
                             ? TexFilter::Nearest : TexFilter::Linear;

    Texture & texture = m_texturePool.Slot(slot);
    std::snprintf(texture.name, sizeof(texture.name), "%s", name);

    texture.regSequence  = m_regSequence;
    texture.pixels       = pixels;
    texture.width        = static_cast<s16>(width);
    texture.height       = static_cast<s16>(height);
    texture.type         = type;
    texture.flags        = flags;
    texture.format       = format;
    texture.components   = components;
    texture.function     = TexFunction::Modulate;
    texture.magFilter    = filter;
    texture.minFilter    = filter;
    texture.textureChain = nullptr;
    texture.atlas        = nullptr; // the caller packs it into a scrap afterwards, if it fits
    texture.atlasX       = 0;
    texture.atlasY       = 0;
    texture.vramAddr     = Texture::kNotResident;
    texture.dirtyPixels  = !builtin; // loader-written pixels may still sit in the dcache;
                                     // the first upload must flush them (built-ins were
                                     // written by the ELF loader and need no flush).

    const bool inserted = m_lookup.Insert(LookupKey(texture.name, texture.type), slot);
    PS2_AssertMsg(inserted, "Duplicate texture name+type!");

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
        // checkerboards below stay RGB16.
        { "pics/conchars.pcx",  conchars_data,         conchars_width,  conchars_height,  PixelFormat::Palette8, TexComponents::RGBA },
        { "pics/conback.pcx",   conback_data,          conback_width,   conback_height,   PixelFormat::Palette8, TexComponents::RGB  },
        { "pics/backtile.pcx",  backtile_data,         backtile_width,  backtile_height,  PixelFormat::Palette8, TexComponents::RGB  },
        { "pics/debug0.pcx",    MakeCheckerPattern(0), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
#if PS2_QUAKE_DEBUG // Extra debug textures for the textured cube test:
        { "pics/debug1.pcx",    MakeCheckerPattern(1), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug2.pcx",    MakeCheckerPattern(2), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug3.pcx",    MakeCheckerPattern(3), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug4.pcx",    MakeCheckerPattern(4), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug5.pcx",    MakeCheckerPattern(5), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
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
        m_debugTextures[i] = Find(name, ImageType::Pic);
        PS2_Assert(m_debugTextures[i] != nullptr);
        m_debugTextures[i]->MarkPixelsDirty();
    }

    // The particle images are registered outside the table above so their
    // filtering can be set per image: the classic dot wants nearest, keeping
    // it the hard-edged blocky square Quake 2 draws, while the HD one is a
    // smooth falloff that would band badly without bilinear.
    Texture & particleDot = Register("pics/particle.pcx", MakeParticleDotPattern(),
                                     kParticleDotDim, kParticleDotDim,
                                     PixelFormat::Alpha8, TexComponents::RGBA,
                                     ImageType::Pic, TexFlags::Builtin);
    particleDot.magFilter = TexFilter::Nearest;
    particleDot.minFilter = TexFilter::Nearest;
    particleDot.MarkPixelsDirty();
    m_particleTextures[0] = &particleDot;

    Texture & particleHd = Register("pics/particle_hd.pcx", MakeParticleHdPattern(),
                                    kParticleHdDim, kParticleHdDim,
                                    PixelFormat::Alpha8, TexComponents::RGBA,
                                    ImageType::Pic, TexFlags::Builtin);
    particleHd.magFilter = TexFilter::Linear;
    particleHd.minFilter = TexFilter::Linear;
    particleHd.MarkPixelsDirty();
    m_particleTextures[1] = &particleHd;

    Com_Printf("Texture cache initialised: %u built-in images registered.\n", m_texturePool.UsedCount());
}

static TextureCache s_cache;

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
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

const Texture * Find(const char * name, const ImageType type)
{
    return s_cache.Find(name, type);
}

void SetSkyDownsample(const bool enable)
{
    s_skyDownsample = enable;
}

void TouchTexture(const Texture & texture)
{
    s_cache.MarkReferenced(texture);
}

const Texture & DebugTexture(int variant)
{
    return s_cache.DebugTexture(variant);
}

const Texture & ParticleTexture(bool highQuality)
{
    return s_cache.ParticleTexture(highQuality);
}

void StScaleFor(const Texture & texture, float * outScaleS, float * outScaleT)
{
    // tex::Log2 rounds up, and it is the same call gs.cpp fills TEX0's TW/TH
    // with - so this stays exact whatever the texture is, resident or not.
    const int potWidth  = 1 << tex::Log2(static_cast<u32>(texture.width));
    const int potHeight = 1 << tex::Log2(static_cast<u32>(texture.height));

    *outScaleS = static_cast<float>(texture.width)  / static_cast<float>(potWidth);
    *outScaleT = static_cast<float>(texture.height) / static_cast<float>(potHeight);
}

} // namespace ps2::tex
