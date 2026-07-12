#pragma once
/* ================================================================================================
 * File: texture.h
 * Brief: Texture/image objects for the PS2 renderer and the cache that owns them.
 *        For now only the embedded built-in images (console font/background, HUD
 *        tiles) are registered; file loading (PCX/TGA/WAL) plugs in here later.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <tamtypes.h>
#include <draw_buffers.h> // texbuffer_t

namespace ps2::tex {

// What a texture is used for by the game. Mirrors the image classes of the
// original renderers and is part of the cache lookup key, so the same file
// may be cached once per type; later also drives end-of-level eviction.
enum class ImageType : u8
{
    Null,   // Free slot in the cache.
    Pic,    // 2D UI/HUD image.
    Skin,   // Model skin.
    Sprite, // Sprite frame.
    Wall,   // World texture.
    Sky     // Skybox face.
};

// Bit-flag texture properties, orthogonal to the ImageType.
enum class TexFlags : u8
{
    None    = 0,
    Builtin = 1 << 0 // Embedded in the ELF; always available, never unloaded.
};

constexpr TexFlags operator|(TexFlags lhs, TexFlags rhs)
{
    return TexFlags(static_cast<u8>(lhs) | static_cast<u8>(rhs));
}

constexpr bool HasFlag(TexFlags flags, TexFlags test)
{
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

// Pixel storage formats we support, mapped to GS PSMs by GsPsm().
enum class PixelFormat : u8
{
    RGBA32, // 4 bytes/texel, 8888.
    RGB16   // 2 bytes/texel, 5551 (alpha bit present but unused as TexComponents::RGB).
};

// Whether the texture's own alpha participates in the texture function (GS TCC bit).
enum class TexComponents : u8 { RGB, RGBA };

// GS texture function applied when a primitive samples the texture.
enum class TexFunction : u8 { Modulate, Decal };

// Texel filtering.
enum class TexFilter : u8 { Nearest, Linear };

// A texture or 2D image. Plain data; owned by the internal texture cache.
struct Texture final
{
    static constexpr int kNotResident = -1;

    // Residency is a cache managed by gs/vram: binding a const Texture may
    // upload it (or evict others), so these two mutate behind the const API.
    mutable int         vramAddr; // GS VRAM word address; kNotResident when not uploaded.
    mutable texbuffer_t texbuf;   // libdraw descriptor used when binding (filled on upload).

    const void *  pixels; // Pixel data in EE RAM (static memory for built-ins).
    int           width;  // In pixels, > 0.
    int           height; // In pixels, > 0.
    PixelFormat   format;
    TexComponents components;
    TexFunction   function;
    TexFilter     magFilter;
    TexFilter     minFilter;
    ImageType     type;
    TexFlags      flags;
    char          name[MAX_QPATH]; // Game path, e.g. "pics/conback.pcx".

    // Later additions when file/asset loading lands: registration sequence for
    // end-of-level eviction, scrap-atlas UVs, per-texture surface chain.

    // TODO: Consider texture mipmaps support and native palettized formats (CLUT).
};

// Mappings from the strongly typed enums above to the plain integer constants
// libdraw/GS registers expect. SDK constants stay out of the rest of the backend.
int GsPsm(PixelFormat format);
int GsComponents(TexComponents components);
int GsFunction(TexFunction function);
int GsMagFilter(TexFilter filter);
int GsMinFilter(TexFilter filter);

// Registers the built-in images (they stream into GS VRAM on first bind).
// Call once, after gs::Init().
void Init();

// Looks up a texture by game name and type; the type is part of the cache
// key, so the same file may live in the cache once per ImageType. Bare names
// expand Quake-style to "pics/<name>.pcx"; a leading '/' or '\' means the
// full path was given. Returns nullptr if nothing matches (no file loading yet).
const Texture * Find(const char * name, ImageType type);

// Number of built-in debug checkerboard variants (distinct colors).
constexpr int kNumDebugTextures = 6;

// Checkerboard stand-ins ("pics/debug0..5.pcx"). Variant 0 is the pink/black
// checker drawn wherever an image is missing; the others give test scenes
// several distinct textures to exercise VRAM streaming.
const Texture & DebugTexture(int index = 0);

} // namespace ps2::tex
