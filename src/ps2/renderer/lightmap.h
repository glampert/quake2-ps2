#pragma once
/* ================================================================================================
 * File: lightmap.h
 * Brief: World lightmaps: the atlases the BSP's baked light samples are packed into,
 *        and the per-frame rebuilds that fold in animated light styles and dynamic
 *        lights. Owned by an internal manager (see lightmap.cpp); this is its API.
 *
 *  Build time: the BSP face loader brackets its loop with BeginBuildingLightmaps /
 *  EndBuildingLightmaps and calls CreateSurfaceLightmap per lit face, which packs the
 *  face's luxels into an atlas and records where they landed on the surface
 *  (light_s/light_t/lightmapTextureNum). The polygon builder then bakes those into
 *  the vertices' second UV set, so nothing has to be recomputed at draw time.
 *
 *  Frame time: the view renderer calls ChainSurface for every visible lit surface,
 *  which rebuilds the ones whose lighting moved and threads each onto its atlas's
 *  draw chain. The lightmap pass then walks one chain per atlas (AtlasTexture /
 *  AtlasChain) and clears them with ClearChains.
 *
 *  A luxel reaches the screen split in two, because the GS cannot deliver it whole:
 *  the blend unit computes (A - B) * C + D where C is a scalar alpha and never a
 *  second colour, so diffuse x coloured-lightmap is not expressible as one blend.
 *  So the atlases the hardware samples are Alpha8 and carry only the luxel's
 *  *intensity*, which the lightmap pass multiplies in per pixel; the chroma left
 *  over rides in a mirror in EE RAM, sampled per vertex whenever the luxels are
 *  baked (CacheSurfaceVertexColors) and cached on the vertex, for the diffuse
 *  pass to fold into the vertex colour the GS modulates the wall texture by.
 *  Intensity times chroma is the luxel again, so the two together reproduce it -
 *  at full resolution in the term that varies per pixel, and at vertex
 *  resolution in the one that barely varies at all.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include <tamtypes.h>

namespace ps2::tex { struct Texture; }
namespace ps2::mod { struct ModelSurface; }

namespace ps2::lm {

// Atlas dimensions, in luxels. The model loader normalises the lightmap UVs
// against these, so the two must agree - it takes them from here.
constexpr int kLightmapTextureWidth  = 256;
constexpr int kLightmapTextureHeight = 256;

// World units one luxel covers. Baked into the BSP format rather than chosen:
// surface extents are snapped to this grid, which is what makes a surface's
// luxel count (extents >> 4) + 1.
constexpr int kLuxelSizeUnits = 16;

// Atlases a single map may use. Measured over all 39 retail maps: they need 2
// (fact3) to 6 (bunk1, power2, space, ...), packing 58-91% full, so this is
// roughly double the worst case. Atlases are allocated on demand, so the extra
// headroom costs nothing until a map actually reaches for it; running out is a
// Sys_Error telling you to raise it.
//
// Six atlases is ~384 KB of GS VRAM out of a ~1.27 MB heap, which is the real
// budget to watch - see the note on vram::Allocate failures in lightmap.cpp.
constexpr int kMaxLightmapTextures = 12;

// ------------------------------------------------------------------------------------------------
// Luxel chroma
//
// What is left of a luxel once its intensity has been taken out: its rgb rescaled
// so the brightest channel is full, packed 5:6:5 with red high. Halves what the
// mirror costs in EE RAM - 128 KB an atlas instead of 256 - for a precision loss
// that cannot show, since the term is a slowly varying tint sampled at vertices
// rather than at pixels.
//
// Note this is *not* tex::PixelFormat::RGB16, which is the 5551-with-red-low
// layout the hardware wants; nothing packed here ever reaches the GS.
// ------------------------------------------------------------------------------------------------

// An unpacked chroma: 0..1 per channel, of which the brightest is always 1.
struct AtlasColor
{
    float r, g, b;
};

// Rounded rather than truncated, so a channel already at full scale survives the
// round trip exactly and an untinted luxel stays untinted.
constexpr u16 PackAtlasColor(int r, int g, int b)
{
    return static_cast<u16>((((r * 31 + 127) / 255) << 11) |
                            (((g * 63 + 127) / 255) <<  5) |
                             ((b * 31 + 127) / 255));
}

inline AtlasColor UnpackAtlasColor(u16 packed)
{
    constexpr float k5 = 1.0f / 31.0f;
    constexpr float k6 = 1.0f / 63.0f;
    return AtlasColor{ static_cast<float>((packed >> 11) & 0x1F) * k5,
                       static_cast<float>((packed >>  5) & 0x3F) * k6,
                       static_cast<float>( packed        & 0x1F) * k5 };
}

// Registers the lightmap cvars. Call once, after tex::Init().
void Init();

// ------------------------------------------------------------------------------------------------
// Level build - bracket the BSP face loop (ref_gl's GL_Begin/EndBuildingLightmaps)
// ------------------------------------------------------------------------------------------------

// Releases the previous map's atlases (EE RAM and GS VRAM) and resets the packer.
void BeginBuildingLightmaps();

// Packs the surface's luxels into an atlas and bakes its static lighting there,
// filling in surf.light_s / light_t / lightmapTextureNum. Must run before the
// surface's polygon is built - the lightmap UVs bake light_s/light_t in - and
// only for lit faces: the caller skips sky, turbulent and translucent ones,
// which keep mod::kNotLightmapped.
void CreateSurfaceLightmap(mod::ModelSurface & surf);

// Closes the atlas being filled. Nothing may be packed until the next
// BeginBuildingLightmaps.
void EndBuildingLightmaps();

// Frees the atlases (EE RAM and GS VRAM) without starting a new build.
// BeginBuildingLightmaps does this for you; call it directly only to give the
// memory back early, and only together with dropping the world model - the
// atlases are reachable only through ModelSurface::lightmapTextureNum, so
// releasing them while surfaces still point at them leaves dangling indices.
void ReleaseAtlases();

// ------------------------------------------------------------------------------------------------
// Frame time
// ------------------------------------------------------------------------------------------------

// Resets the per-frame debug counters. Call once at the top of the frame.
void BeginFrame();

// Rebuilds the surface's luxels if its lighting changed since they were baked -
// an animated light style, or a dynamic light touching it this frame - and
// threads it onto its atlas's draw chain. Call once per visible lit surface,
// before the lightmap pass draws. Surfaces with mod::kNotLightmapped are not
// valid arguments; the caller filters them out.
void ChainSurface(mod::ModelSurface & surf, const refdef_t & viewDef, int frameCount);

// Re-samples the luxel chroma under every vertex of the surface's polygons and
// caches it in PolyVertex::rgba.
//
// Must run after the surface's polygons exist and after every rebake of its
// luxels - the load path calls it once the polygons are built, and ChainSurface
// calls it whenever it rebuilds, which is what keeps the dynamic lightmap mode
// (ps2_dynamic_lightmaps 1) correct as lights move across a surface.
void CacheSurfaceVertexColors(mod::ModelSurface & surf);

// Atlases in use by the current map, and the texture to bind for one. Indices
// are the surfaces' lightmapTextureNum.
int NumAtlases();
const tex::Texture & AtlasTexture(int index);

// Head of the atlas's draw chain, as built by ChainSurface this frame; null when
// nothing visible uses it. Walk it through ModelSurface::lightmapChain.
const mod::ModelSurface * AtlasChain(int index);

// Empties every atlas draw chain. Call right after drawing them, so the next
// caller (a brush model entity, or the next frame's world) starts clean.
void ClearChains();

// Per-frame counters for the ps2_show_drawstats overlay. Cleared by BeginFrame.
struct Stats
{
    int atlases;        // Atlases the current map packed into (not per frame).
    int styleUpdates;   // Surfaces rebuilt because an animated light style moved.
    int dynamicUpdates; // Surfaces rebuilt with a dynamic light folded in.
    int restoreUpdates; // Surfaces rebuilt back to their static lighting.
};

Stats GetStats();

} // namespace ps2::lm
