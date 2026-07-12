/* ================================================================================================
 * File: texture.cpp
 * Brief: Texture objects and the texture cache. See texture.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/texture.h"
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
    case PixelFormat::RGBA32 : return GS_PSM_32;
    case PixelFormat::RGB16  : return GS_PSM_16;
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

// Expands a game image name into the full path key used by the cache, the same
// way ref_gl resolved pic names: bare names live under "pics/" as .pcx files, a
// leading path separator means 'name' is already the full path.
void NormalizeName(const char * name, char (&out)[MAX_QPATH])
{
    if (name[0] != '/' && name[0] != '\\')
    {
        std::snprintf(out, MAX_QPATH, "pics/%s.pcx", name);
    }
    else
    {
        std::snprintf(out, MAX_QPATH, "%s", name + 1);
    }
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
    const u16 variantColors[kNumDebugTextures] = {
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
    const Texture * Find(const char * name, const ps2::tex::ImageType type) const;
    const Texture & DebugTexture(int index) const;

private:
    Texture & Register(const char * name, const void * pixels, int width, int height,
                       PixelFormat format, TexComponents components);

    static constexpr u32 kMaxTextures = 32; // Builtins only for now; grows with asset loading.
    using TexturePool = SmallPool<Texture, kMaxTextures>;

    TexturePool m_texturePool;
    const Texture * m_debugTextures[kNumDebugTextures] = {};

    // Lookup: FNV-1a hash of the full path + image type -> pool slot of the texture.
    std::unordered_map<u64, u16> m_lookup;
};

const Texture * TextureCache::Find(const char * name, const ps2::tex::ImageType type) const
{
    PS2_Assert(name != nullptr && *name != '\0');
    PS2_Assert(type != ImageType::Null);

    char fullname[MAX_QPATH];
    NormalizeName(name, fullname);

    const auto it = m_lookup.find(LookupKey(fullname, type));
    if (it == m_lookup.end())
    {
        return nullptr;
    }

    const Texture & texture = m_texturePool.Slot(it->second);

    // 64-bit FNV-1a collisions are vanishingly rare, but a miss here would
    // silently draw the wrong image - verify the actual name and type.
    PS2_AssertMsg(std::strcmp(texture.name, fullname) == 0 && texture.type == type,
                  "Texture lookup hash collision!");

    return &texture;
}

const Texture & TextureCache::DebugTexture(int index) const
{
    PS2_Assert(index >= 0 && index < kNumDebugTextures);
    return *m_debugTextures[index];
}

Texture & TextureCache::Register(const char * name, const void * pixels, int width, int height,
                                 PixelFormat format, TexComponents components)
{
    PS2_Assert(width > 0 && height > 0 && pixels != nullptr);

    const u16 slot = m_texturePool.Alloc();
    PS2_AssertMsg(slot != TexturePool::kInvalidIndex, "Out of texture cache slots!");

    Texture & texture  = m_texturePool.Slot(slot);
    texture.pixels     = pixels;
    texture.width      = width;
    texture.height     = height;
    texture.vramAddr   = Texture::kNotResident;
    texture.format     = format;
    texture.components = components;
    texture.function   = TexFunction::Modulate;
    texture.magFilter  = TexFilter::Nearest;
    texture.minFilter  = TexFilter::Nearest;
    texture.type       = ImageType::Pic;    // All built-ins are 2D UI images. TODO: File loading has to set this accordingly!
    texture.flags      = TexFlags::Builtin; // TODO: File-loaded textures won't have this flag!
    std::snprintf(texture.name, sizeof(texture.name), "%s", name);

    const auto inserted = m_lookup.emplace(LookupKey(texture.name, texture.type), slot);
    PS2_AssertMsg(inserted.second, "Duplicate texture name+type!");

    return texture;
}

void TextureCache::Init()
{
    m_texturePool.Init(); // One-shot; asserts if called twice.

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
        // conchars carries real alpha for the glyph transparency; the rest are opaque RGB16.
        { "pics/conchars.pcx",  conchars_data,         conchars_width,  conchars_height,  PixelFormat::RGBA32, TexComponents::RGBA },
        { "pics/conback.pcx",   conback_data,          conback_width,   conback_height,   PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/backtile.pcx",  backtile_data,         backtile_width,  backtile_height,  PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/inventory.pcx", inventory_data,        inventory_width, inventory_height, PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/help.pcx",      help_data,             help_width,      help_height,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug0.pcx",    MakeCheckerPattern(0), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug1.pcx",    MakeCheckerPattern(1), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug2.pcx",    MakeCheckerPattern(2), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug3.pcx",    MakeCheckerPattern(3), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug4.pcx",    MakeCheckerPattern(4), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug5.pcx",    MakeCheckerPattern(5), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
    };

    for (const BuiltinImage & builtin : builtins)
    {
        Register(builtin.name, builtin.pixels, builtin.width,
                 builtin.height, builtin.format, builtin.components);
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

const Texture * Find(const char * name, const ps2::tex::ImageType type)
{
    return s_cache.Find(name, type);
}

const Texture & DebugTexture(int index)
{
    return s_cache.DebugTexture(index);
}

} // namespace ps2::tex
