/* ================================================================================================
 * File: texture.cpp
 * Brief: Texture objects and the texture cache. See texture.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/texture.h"
#include "ps2/renderer/gs.h"
#include "ps2/builtin/builtin.h"

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

// 64-bit FNV-1a. Local to the texture module for now; move to a shared
// utils header when a second user appears.
constexpr std::uint64_t HashStr64(const char * str)
{
    if (str == nullptr || *str == '\0')
    {
        return 0;
    }

    std::uint64_t hash = 14695981039346656037ull;
    while (*str != '\0')
    {
        hash ^= static_cast<std::uint8_t>(*str++);
        hash *= 1099511628211ull;
    }

    return hash;
}

// Expands a game image name into the full path key used by the cache, the same
// way ref_gl resolved pic names: bare names live under "pics/" as .pcx files, a
// leading path separator means 'name' is already the full path.
void NormalizeName(const char * name, char (&out)[MAX_QPATH])
{
    if (name[0] != '/' && name[0] != '\\')
    {
        std::snprintf(out, sizeof(out), "pics/%s.pcx", name);
    }
    else
    {
        std::snprintf(out, sizeof(out), "%s", name + 1);
    }
}

// Pink/black checkerboard for DebugTexture(), RGB16.
constexpr int kCheckerDim     = 64;
constexpr int kCheckerSquares = 4;

const std::uint16_t * MakeCheckerPattern()
{
    constexpr auto Rgb16 = [](int r, int g, int b) -> std::uint16_t
    {
        return static_cast<std::uint16_t>((1u << 15) | ((unsigned(b) >> 3) << 10) |
                                          ((unsigned(g) >> 3) << 5) | (unsigned(r) >> 3));
    };
    const std::uint16_t colors[2] = { Rgb16(255, 100, 255), Rgb16(0, 0, 0) };

    static std::uint16_t s_buffer[kCheckerDim * kCheckerDim] __attribute__((aligned(16)));

    constexpr int squareSize = kCheckerDim / kCheckerSquares;
    for (int y = 0; y < kCheckerDim; ++y)
    {
        for (int x = 0; x < kCheckerDim; ++x)
        {
            const int colorIndex = ((y / squareSize) + (x / squareSize)) % 2;
            s_buffer[x + (y * kCheckerDim)] = colors[colorIndex];
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

    const Texture * Find(const char * name)
    {
        PS2_Assert(name != nullptr && *name != '\0');

        char fullname[MAX_QPATH];
        NormalizeName(name, fullname);

        const auto it = m_lookup.find(HashStr64(fullname));
        if (it == m_lookup.end())
        {
            return nullptr;
        }

        Texture * texture = &m_textures[it->second];
        // 64-bit FNV-1a collisions are vanishingly rare, but a miss here would
        // silently draw the wrong image - verify the actual name.
        PS2_AssertMsg(std::strcmp(texture->name, fullname) == 0, "Texture name hash collision!");

        return texture;
    }

    const Texture & DebugTexture() const { return *m_debugTexture; }

private:
    Texture & Register(const char * name, const void * pixels, int width, int height,
                       PixelFormat format, TexComponents components);

    static constexpr int kMaxTextures = 32; // Builtins only for now; grows with asset loading.

    int m_used = 0;
    Texture m_textures[kMaxTextures] = {};
    const Texture * m_debugTexture = nullptr;

    // Name lookup: FNV-1a hash of the full path -> index into m_textures[].
    std::unordered_map<std::uint64_t, int> m_lookup;
};

Texture & TextureCache::Register(const char * name, const void * pixels, int width, int height,
                                 PixelFormat format, TexComponents components)
{
    PS2_AssertMsg(m_used < kMaxTextures, "Out of texture cache slots!");
    PS2_Assert(width > 0 && height > 0 && pixels != nullptr);

    Texture & texture = m_textures[m_used];

    texture.pixels     = pixels;
    texture.width      = width;
    texture.height     = height;
    texture.vramAddr   = Texture::kNotResident;
    texture.format     = format;
    texture.components = components;
    texture.function   = TexFunction::Modulate;
    texture.magFilter  = TexFilter::Nearest;
    texture.minFilter  = TexFilter::Nearest;
    texture.type       = ImageType::Builtin;
    std::snprintf(texture.name, sizeof(texture.name), "%s", name);

    const auto inserted = m_lookup.emplace(HashStr64(texture.name), m_used);
    PS2_AssertMsg(inserted.second, "Duplicate texture name!");

    ++m_used;
    return texture;
}

void TextureCache::Init()
{
    PS2_AssertMsg(m_used == 0, "TextureCache::Init called twice!");

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
        { "pics/conchars.pcx",  conchars_data,        conchars_width,  conchars_height,  PixelFormat::RGBA32, TexComponents::RGBA },
        { "pics/conback.pcx",   conback_data,         conback_width,   conback_height,   PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/backtile.pcx",  backtile_data,        backtile_width,  backtile_height,  PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/inventory.pcx", inventory_data,       inventory_width, inventory_height, PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/help.pcx",      help_data,            help_width,      help_height,      PixelFormat::RGB16,  TexComponents::RGB  },
        { "pics/debug.pcx",     MakeCheckerPattern(), kCheckerDim,     kCheckerDim,      PixelFormat::RGB16,  TexComponents::RGB  },
    };

    for (const BuiltinImage & builtin : builtins)
    {
        Texture & texture = Register(builtin.name, builtin.pixels, builtin.width,
                                     builtin.height, builtin.format, builtin.components);
        gs::UploadTexture(texture);
    }

    m_debugTexture = Find("debug");
    PS2_Assert(m_debugTexture != nullptr);

    Com_Printf("Texture cache initialised: %d built-in images resident in VRAM.\n", m_used);
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

const Texture * Find(const char * name)
{
    return s_cache.Find(name);
}

const Texture & DebugTexture()
{
    return s_cache.DebugTexture();
}

} // namespace ps2::tex
