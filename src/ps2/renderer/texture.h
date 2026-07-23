#pragma once
/* ================================================================================================
 * File: texture.h
 * Brief: Texture/image objects for the PS2 renderer and the cache that owns them.
 *        The embedded built-in images (console font/background, HUD tiles) are
 *        registered up front; everything else loads from disk (PCX/TGA/WAL) on
 *        the first Find and is freed by the registration sequence when a level
 *        change stops referencing it.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vram.h"

#include <tamtypes.h>
#include <draw_buffers.h> // texbuffer_t

namespace ps2::tex {

// What a texture is used for by the game. Mirrors the image classes of the
// original renderers and is part of the cache lookup key, so the same file
// may be cached once per type; it also drives end-of-level eviction (level
// asset types are freed when unused, Pics stick around - see EndRegistration).
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
    RGBA32,  // 4 bytes/texel, 8888.
    RGB16,   // 2 bytes/texel, 5551 (alpha bit present but unused as TexComponents::RGB).
    Palette8 // 1 byte/texel: PSMT8 indices into the shared global-palette CLUT
             // (gs::Init uploads it once; color and alpha come from the palette entry).
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
    char          name[MAX_QPATH]; // Game path, e.g. "pics/conback.pcx" (must be the first field - game code assumes this).
    u32           regSequence;     // Registration sequence the texture was last found in; stale level assets are freed at EndRegistration().
    const void *  pixels;          // Pixel data in EE RAM (static memory for built-ins, heap for file loads).
    int           width;           // In pixels, > 0.
    int           height;          // In pixels, > 0.
    ImageType     type;
    TexFlags      flags;
    PixelFormat   format;
    TexComponents components;
    TexFunction   function;
    TexFilter     magFilter;
    TexFilter     minFilter;

    // Residency is a cache managed by gs/vram: binding a const Texture may
    // upload it (or evict others), so these mutate behind the const API.
    static constexpr auto kNotResident = vram::Address::Invalid;
    mutable vram::Address vramAddr;    // GS VRAM word address; kNotResident when not uploaded.
    mutable texbuffer_t   texbuf;      // libdraw descriptor used when binding (filled on upload).
    mutable bool          dirtyPixels; // CPU rewrote 'pixels'; the next bind re-uploads them.

    // For dynamic textures (cinematic frames/lightmaps/scrap atlas).
    // Called after rewriting 'pixels' so the next bind refreshes GS VRAM.
    void MarkPixelsDirty() const { dirtyPixels = true; }

    // Later additions for world rendering: scrap-atlas UVs, per-texture surface chain.
    // TODO: Consider texture mipmaps support.
};

// Bytes of EE RAM one texel occupies in each PixelFormat (Palette8 = 1).
int BytesPerTexel(PixelFormat format);

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

// Level asset lifetimes, driven by the engine's registration sequence:
// BeginRegistration starts a new sequence (level load); every texture found
// or loaded afterwards is stamped with it. EndRegistration then frees the
// level assets (Skin/Sprite/Wall/Sky) left with an older stamp - pixel memory,
// GS VRAM and cache slot. Pics are exempt like in ref_gl (the client caches
// pointers to them across levels), and built-ins are permanent.
void BeginRegistration();
void EndRegistration();

// Looks up a texture by game name and type, loading it from disk (PCX/WAL/TGA,
// by extension) on a cache miss; the type is part of the cache key, so the same
// file may live in the cache once per ImageType. Pic names follow the ref_gl
// convention: bare names expand to "pics/<name>.pcx", a leading '/' or '\'
// means the full path was given. Other types always give the full path.
// Returns nullptr when the file is missing or fails to decode.
const Texture * Find(const char * name, ImageType type);

// Re-stamps an already-resolved texture as used in the current registration
// cycle, so EndRegistration() won't evict it. The model cache calls this when
// a model is found in-cache: its texture pointers are reused directly, without
// a Find() to refresh their sequence number.
void TouchTexture(const Texture & texture);

// Number of built-in debug checkerboard variants (distinct colors).
constexpr int kNumDebugTextures = 6;

// Checkerboard stand-ins ("pics/debug0..5.pcx"). Variant 0 is the pink/black
// checker drawn wherever an image is missing; the others give test scenes
// several distinct textures to exercise VRAM streaming.
const Texture & DebugTexture(int index = 0);

} // namespace ps2::tex
