/* ================================================================================================
 * File: texture.cpp
 * Brief: Texture objects and the texture cache. See texture.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/texture.h"
#include "ps2/renderer/image_load.h"
#include "ps2/renderer/gs.h" // gs::ReleaseTexture (end-of-level eviction)
#include "ps2/builtin/builtin.h"
#include "ps2/small_pool.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <draw_sampling.h> // LOD_*
#include <gs_psm.h>

namespace ps2::tex {

// ------------------------------------------------------------------------------------------------
// Enum -> SDK constant mappings
// ------------------------------------------------------------------------------------------------

int GsPsm(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBA32   : return GS_PSM_32;
    case PixelFormat::RGB16    : return GS_PSM_16;
    case PixelFormat::Palette8 : return GS_PSM_8;
    }
    return GS_PSM_32; // Unreachable; keeps GCC's -Wreturn-type happy.
}

int GsComponents(TexComponents components)
{
    return (components == TexComponents::RGBA) ? TEXTURE_COMPONENTS_RGBA : TEXTURE_COMPONENTS_RGB;
}

int GsFunction(TexFunction function)
{
    return (function == TexFunction::Decal) ? TEXTURE_FUNCTION_DECAL : TEXTURE_FUNCTION_MODULATE;
}

int GsMagFilter(TexFilter filter)
{
    return (filter == TexFilter::Linear) ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
}

int GsMinFilter(TexFilter filter)
{
    return (filter == TexFilter::Linear) ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
}

int BytesPerTexel(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBA32   : return 4;
    case PixelFormat::RGB16    : return 2;
    case PixelFormat::Palette8 : return 1;
    }
    return 4; // Unreachable; keeps GCC's -Wreturn-type happy.
}

// ------------------------------------------------------------------------------------------------
// TextureCache
// ------------------------------------------------------------------------------------------------

namespace {

constexpr u64 kFnvSeed  = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

// 64-bit FNV-1a. Local to the texture module for now; move to a shared
// utils header when a second user appears.
constexpr u64 HashStr64(const char * str)
{
    if (str == nullptr || *str == '\0')
    {
        return 0;
    }

    u64 hash = kFnvSeed;
    while (*str != '\0')
    {
        hash ^= static_cast<u8>(*str++);
        hash *= kFnvPrime;
    }

    return hash;
}

// Cache lookup key: the name hash continued with the image type as one extra
// FNV-1a byte, so the same file may be cached independently per ImageType.
constexpr u64 LookupKey(const char * fullname, ImageType type)
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

// Checkerboards for the DebugTexture() variants, RGB16. Variant 0 (pink) is
// the classic missing-image stand-in; the others give test scenes several
// distinct textures to exercise VRAM streaming.
constexpr int kCheckerDim     = 32;
constexpr int kCheckerSquares = 4;

const u16 * MakeCheckerPattern(int variant)
{
    PS2_Assert(variant >= 0 && variant < kNumDebugTextures);

    constexpr auto Rgb16 = [](u32 r, u32 g, u32 b) -> u16
    {
        return static_cast<u16>((1u << 15) | ((b >> 3) << 10) |
                               ((g >> 3) << 5) | (r >> 3));
    };

    // One bright color per variant, checkered against black.
    constexpr u16 variantColors[kNumDebugTextures] = {
        Rgb16(255, 100, 255), // pink
        Rgb16(255,  60,  60), // red
        Rgb16( 60, 255,  60), // green
        Rgb16( 80,  80, 255), // blue
        Rgb16(255, 255,  60), // yellow
        Rgb16( 60, 255, 255), // cyan
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

// Owns the texture pool and the name lookup. Internal singleton (s_cache);
// the module API below is the public face.
class TextureCache final
{
public:
    void Init();
    const Texture * Find(const char * name, const ImageType type);
    const Texture & DebugTexture(int index) const;

    void BeginRegistration();
    void EndRegistration();

private:
    Texture & Register(const char * name, const void * pixels, int width, int height,
                       PixelFormat format, TexComponents components,
                       ImageType type, TexFlags flags);

    const Texture * LoadFromFile(const char * fullname, ImageType type);
    void Unload(u16 slot);

    // Worst case for a full level plus UI (walls, skins, sprites, sky and
    // pics); matches ref_gl's MAX_GLTEXTURES. Fixed-size pool: running out is
    // a Sys_Error telling you to bump this.
    static constexpr u32 kMaxTextures = 1024;
    using TexturePool = SmallPool<Texture, kMaxTextures>;

    TexturePool m_texturePool;
    const Texture * m_debugTextures[kNumDebugTextures] = {};

    // Level load/change cycle counter; textures stamped with an older value
    // are the ones EndRegistration() frees. See tex::BeginRegistration().
    u32 m_regSequence = 1;

    // Lookup: FNV-1a hash of the full path + image type -> pool slot of the texture.
    std::unordered_map<u64, u16> m_lookup;
};

const Texture * TextureCache::Find(const char * name, const ImageType type)
{
    PS2_Assert(name != nullptr && *name != '\0');
    PS2_Assert(type != ImageType::Null);

    char fullname[MAX_QPATH];
    NormalizeName(name, type, fullname);

    const auto it = m_lookup.find(LookupKey(fullname, type));
    if (it == m_lookup.end())
    {
        return LoadFromFile(fullname, type);
    }

    Texture & texture = m_texturePool.Slot(it->second);

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

        format     = PixelFormat::Palette8;
        components = HasTransparentTexels(pic8, width * height) ? TexComponents::RGBA : TexComponents::RGB;
        pixels     = pic8;
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

    const int pixelBytes = texture.width * texture.height * BytesPerTexel(texture.format);
    PS2_MemFree(const_cast<void *>(texture.pixels), static_cast<size_t>(pixelBytes), MEMTAG_TEXIMAGE);

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
    int freedCount = 0;
    for (auto it = m_lookup.begin(); it != m_lookup.end(); )
    {
        const Texture & texture = m_texturePool.Slot(it->second);
        if (HasFlag(texture.flags, TexFlags::Builtin) ||
            texture.type == ImageType::Pic ||
            texture.regSequence == m_regSequence)
        {
            ++it;
            continue;
        }

        Com_DPrintf("Freeing unused texture '%s'\n", texture.name);
        Unload(it->second);
        it = m_lookup.erase(it);
        ++freedCount;
    }

    if (freedCount > 0)
    {
        Com_DPrintf("Texture cache: freed %d unused textures.\n", freedCount);
    }
}

const Texture & TextureCache::DebugTexture(int index) const
{
    PS2_Assert(index >= 0 && index < kNumDebugTextures);
    return *m_debugTextures[index];
}

Texture & TextureCache::Register(const char * name, const void * pixels, int width, int height,
                                 PixelFormat format, TexComponents components,
                                 ImageType type, TexFlags flags)
{
    PS2_Assert(width > 0 && height > 0 && pixels != nullptr);

    const u16 slot = m_texturePool.Alloc();
    if (slot == TexturePool::kInvalidIndex)
    {
        Sys_Error("Out of texture cache slots for '%s'! Bump TextureCache::kMaxTextures (%u).",
                  name, kMaxTextures);
    }

    const bool builtin = HasFlag(flags, TexFlags::Builtin);

    // Pics and sprites keep crisp texels (and their transparency cutouts
    // fringe-free); skins, walls and sky get smoothed by bilinear sampling.
    // The GS filters the post-CLUT colors, so Palette8 works with Linear too.
    const TexFilter filter = (type == ImageType::Pic || type == ImageType::Sprite)
                             ? TexFilter::Nearest : TexFilter::Linear;

    Texture & texture   = m_texturePool.Slot(slot);
    texture.pixels      = pixels;
    texture.width       = width;
    texture.height      = height;
    texture.vramAddr    = Texture::kNotResident;
    texture.dirtyPixels = !builtin; // loader-written pixels may still sit in the dcache;
                                    // the first upload must flush them (built-ins were
                                    // written by the ELF loader and need no flush).
    texture.format      = format;
    texture.components  = components;
    texture.function    = TexFunction::Modulate;
    texture.magFilter   = filter;
    texture.minFilter   = filter;
    texture.type        = type;
    texture.flags       = flags;
    texture.regSequence = m_regSequence;
    std::snprintf(texture.name, sizeof(texture.name), "%s", name);

    const auto inserted = m_lookup.emplace(LookupKey(texture.name, texture.type), slot);
    PS2_AssertMsg(inserted.second, "Duplicate texture name+type!");

    return texture;
}

void TextureCache::Init()
{
    m_texturePool.Init(); // One-shot; asserts if called twice.

    // A level load registers hundreds of textures; reserving up front keeps
    // the lookup from rehashing in the middle of it.
    m_lookup.reserve(kMaxTextures);

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
        { "pics/inventory.pcx", inventory_data,        inventory_width, inventory_height, PixelFormat::Palette8, TexComponents::RGB  },
        { "pics/help.pcx",      help_data,             help_width,      help_height,      PixelFormat::Palette8, TexComponents::RGB  },
        { "pics/debug0.pcx",    MakeCheckerPattern(0), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug1.pcx",    MakeCheckerPattern(1), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug2.pcx",    MakeCheckerPattern(2), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug3.pcx",    MakeCheckerPattern(3), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug4.pcx",    MakeCheckerPattern(4), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
        { "pics/debug5.pcx",    MakeCheckerPattern(5), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,    TexComponents::RGB  },
    };

    for (const BuiltinImage & builtin : builtins)
    {
        Register(builtin.name, builtin.pixels, builtin.width, builtin.height,
                 builtin.format, builtin.components, ImageType::Pic, TexFlags::Builtin);
    }

    for (int i = 0; i < kNumDebugTextures; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "debug%d", i);
        m_debugTextures[i] = Find(name, ImageType::Pic);
        PS2_Assert(m_debugTextures[i] != nullptr);
    }

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

const Texture & DebugTexture(int index)
{
    return s_cache.DebugTexture(index);
}

} // namespace ps2::tex
