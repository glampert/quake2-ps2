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
#include <gs_psm.h>
#include <draw_buffers.h>  // texbuffer_t
#include <draw_sampling.h> // LOD_*

namespace ps2::mod { struct ModelSurface; }

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

// Which images are pre-brightened by ps2_intensity before anything multiplies
// them back down again: a wall under its lightmap, a skin or a sprite under an
// entity's shade colour. Images drawn at face value - the HUD, the menus, the
// sky - are left alone, or the compensation would just wash them out.
constexpr bool TakesIntensity(ImageType type)
{
    return (type == ImageType::Wall) ||
           (type == ImageType::Skin) ||
           (type == ImageType::Sprite);
}

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
    RGBA32,   // 4 bytes/texel, 8888.
    RGB16,    // 2 bytes/texel, 5551 (alpha bit present but unused as TexComponents::RGB).
    Palette8, // 1 byte/texel: PSMT8 indices into the shared global-palette CLUT
              // (gs::Init uploads it once; color and alpha come from the palette entry).
    Alpha8    // 1 byte/texel: PSMT8 indices into the shared alpha-ramp CLUT, where the
              // index *is* the alpha and the color is pinned at the modulate identity.
              // For images that carry only a coverage/intensity signal and take their
              // color from the primitive: the particle sprites and the lightmap atlases.
              // Needs TexComponents::RGBA, or the texture function drops the alpha.
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
    s16           width;           // Of 'pixels', in pixels, > 0.
    s16           height;          // Of 'pixels', in pixels, > 0.
    s16           srcWidth;        // Size the image had on disk. Tiling world textures are resampled to the next power of two on load, width/height
    s16           srcHeight;       // hold the scaled size, what actually sits in memory and VRAM. Same as width/height for images that didn't need resampling.
    mutable bool  dirtyPixels;     // CPU rewrote 'pixels'; the next bind re-uploads them.
    ImageType     type;
    TexFlags      flags;
    PixelFormat   format;
    TexComponents components;
    TexFunction   function;
    TexFilter     magFilter;
    TexFilter     minFilter;

    // Head of this texture's world-surface draw chain. render_view.cpp threads
    // the frame's visible surfaces here while walking the BSP, then draws each
    // chain as one batch and resets it to null - it never outlives the frame.
    mutable const mod::ModelSurface * textureChain;

    // Set when the image lives inside a shared scrap atlas (see scrap_atlas.h)
    // rather than owning VRAM: bind 'atlas' and shift the draw's texel
    // coordinates by atlasX/atlasY, which gs::SetTextureFor2D does. 'width' and
    // 'height' stay the image's own, so Draw_GetPicSize and every caller's
    // layout math are unaffected, and 'pixels' points into the atlas buffer -
    // it is not a standalone allocation and must not be freed (see Unload).
    const Texture * atlas;
    s16             atlasX;
    s16             atlasY;

    // Residency is a cache managed by GS/VRAM: binding a const Texture may
    // upload it (or evict others), so vramAddr mutates behind the const API.
    static constexpr auto kNotResident = vram::Address::Invalid;
    mutable vram::Address vramAddr; // GS VRAM word address; kNotResident when not uploaded.

    // For dynamic textures (cinematic frames/lightmaps/scrap atlas).
    // Called after rewriting 'pixels' so the next bind refreshes GS VRAM.
    void MarkPixelsDirty() const { dirtyPixels = true; }

    // TODO: Consider texture mipmaps support.
};

// Mappings from the strongly typed enums above to the plain integer constants
// libdraw/GS registers expect. SDK constants stay out of the rest of the backend.
inline int GsComponents(TexComponents components)
{
    return (components == TexComponents::RGBA) ? TEXTURE_COMPONENTS_RGBA : TEXTURE_COMPONENTS_RGB;
}

inline int GsFunction(TexFunction function)
{
    return (function == TexFunction::Decal) ? TEXTURE_FUNCTION_DECAL : TEXTURE_FUNCTION_MODULATE;
}

inline int GsMagFilter(TexFilter filter)
{
    return (filter == TexFilter::Linear) ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
}

inline int GsMinFilter(TexFilter filter)
{
    return (filter == TexFilter::Linear) ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
}

inline int GsPsm(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBA32   : return GS_PSM_32;
    case PixelFormat::RGB16    : return GS_PSM_16;
    case PixelFormat::Palette8 : return GS_PSM_8;
    case PixelFormat::Alpha8   : return GS_PSM_8;
    }
    return GS_PSM_32; // Unreachable; keeps GCC's -Wreturn-type happy.
}

// Bytes of EE RAM one texel occupies in each PixelFormat (Palette8 = 1).
inline int BytesPerTexel(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBA32   : return 4;
    case PixelFormat::RGB16    : return 2;
    case PixelFormat::Palette8 : return 1;
    case PixelFormat::Alpha8   : return 1;
    }
    return 4; // Unreachable; keeps GCC's -Wreturn-type happy.
}

// Pixel stride the texture occupies VRAM with (the TEX0 TBW and transfer DBW).
// 8-bit textures must use a multiple of 128 (TBW must be even for PSMT8/4);
// other formats use their width as-is.
inline int TextureStridePixels(const Texture & texture, int psm)
{
    if (psm == GS_PSM_8)
    {
        return (texture.width + 127) & ~127;
    }
    return texture.width;
}

// Inline replacement for draw_log2() from libdraw.
inline u8 Log2(u32 x)
{
    // plzcw counts the leading zeros of x minus one, so 30 - lzc is
    // the index of the highest set bit (floor of the base 2 log).
    u32 lzc;
    asm volatile ("plzcw %0, %1\n\t" : "=r" (lzc) : "r" (x));

    u32 res = 30 - lzc;
    res += (x > (1u << res)) ? 1u : 0u; // Round up for non-power-of-two x.

    return static_cast<u8>(res);
}

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

// Halve every ImageType::Sky image loaded from now on, both dimensions, and
// keep doing it until switched back off. ref_gl's gl_skymip in the shape the
// PS2 needs it: a sky face is the largest single texture the renderer binds
// (256x256 = 64 KB of VRAM), and a rotating sky forces all six of them
// resident at once, so a quarter of that is worth having on hand.
//
// A mode flag rather than a Find() parameter because it belongs to the sky
// module's load loop, exactly as ref_gl bracketed its six GL_FindImage calls
// with gl_picmip++/gl_picmip--. Note the cache keys on name and type only, so
// a sky already resident from an earlier map keeps whichever size it loaded
// at; render_sky.cpp reads the face's real width back rather than assuming.
void SetSkyDownsample(bool enable);

// Re-stamps an already-resolved texture as used in the current registration
// cycle, so EndRegistration() won't evict it. The model cache calls this when
// a model is found in-cache: its texture pointers are reused directly, without
// a Find() to refresh their sequence number.
void TouchTexture(const Texture & texture);

// Capacity of the texture cache: world textures, model skins, HUD/menu pics.
// Exported because render_view.cpp sizes its per-frame texture chain array to
// the same bound - a chain can hold at most one entry per live texture.
// Running out is a Sys_Error telling you to bump this.
constexpr u32 kMaxTextures = 640;

// Number of built-in debug checkerboard variants (distinct colors).
constexpr int kNumDebugTextures = PS2_QUAKE_DEBUG ? 6 : 1;

// Checkerboard stand-ins ("pics/debug0..5.pcx"). Variant 0 is the pink/black
// checker drawn wherever an image is missing; the others give test scenes
// several distinct textures to exercise VRAM streaming.
const Texture & DebugTexture(int variant = 0);

// The built-in particle images, generated at Init rather than loaded (Quake 2
// ships neither as a file). 'highQuality' picks the soft round sprite drawn as
// a quad over the classic 8x8 dot drawn as a single triangle. Both carry their
// shape in alpha with every texel's colour at the modulate identity, so the
// particle's colour comes entirely from its vertices.
const Texture & ParticleTexture(bool highQuality);

// Converts image-normalized texture coordinates - 0..1 spanning the image,
// which is what Quake's MD2 glcmds store - into the GS's normalized ST space.
// The two are not the same thing: normalized ST spans the TEX0 TW/TH extent,
// the image size rounded UP to a power of two, so for the non-power-of-two
// images Quake is full of (a 276x194 model skin samples as 512x256) ST = 1.0
// lands well past the last real texel. Multiply by these to hit the image's
// true right/bottom edge; both come back 1.0 for power-of-two images.
//
// Only valid for coordinates that stay within [0, 1]. A tiling texture still
// wraps at the power-of-two extent, so a coordinate scale cannot fix one; the
// world textures are resampled on load instead (see Texture::srcWidth), which
// is why they come back 1.0 here.
void StScaleFor(const Texture & texture, float * outScaleS, float * outScaleT);

} // namespace ps2::tex
