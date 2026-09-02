/* ================================================================================================
 * File: lightmap.cpp
 * Brief: The lightmap atlases and their per-frame rebuilds. See lightmap.h.
 *
 *  Ported from ref_gl's gl_rsurf.c/gl_light.c, with two deliberate departures.
 *
 *  Storage: ref_gl keeps RGBA8 atlases and modulates them over the diffuse pass with
 *  glBlendFunc(GL_ZERO, GL_SRC_COLOR). The GS blend unit computes (A - B) * C + D
 *  where C is a scalar alpha, never a second colour, so that multiply is not
 *  expressible here. What the GS can do is Cd * As, so each luxel is split in two:
 *  its *intensity* goes into an Alpha8 atlas the hardware samples per pixel through
 *  the alpha-ramp CLUT, and the chroma left over goes into a mirror beside it in EE
 *  RAM (5:6:5, never uploaded) that the diffuse pass samples per vertex and folds
 *  into its vertex colour. The two multiply back to the luxel, so the split costs
 *  resolution only in the chroma - which is the term that barely varies.
 *
 *  Dynamic lights: ref_gl relocates a dlit surface into a shared scratch atlas at a
 *  second (dlight_s, dlight_t) and biases its UVs, flushing whenever the scratch
 *  fills; MrQ2 pairs every atlas with a dynamic twin. Both cost VRAM this renderer
 *  does not have. Instead a dlit surface is rebuilt in place, over its own block,
 *  and rebuilt once more from surf->samples when the light stops touching it - so
 *  there is exactly one set of atlases, the baked UVs always address them, and no
 *  draw batch ever has to split.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/model.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/scrap_atlas.h" // SkylinePacker, shared with the 2D scrap atlases
#include "ps2/renderer/gs.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ps2::lm {
namespace {

// ------------------------------------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------------------------------------

// Largest surface the accumulator takes. Quake's qbsp subdivides faces so their
// extents stay within 256 units, and every lit face across all 39 retail maps
// measures 17x17 luxels or under - ref_gl sizes s_blocklights for 34x34 and
// hard-errors past it, so keep the same bound and the same error.
constexpr int kMaxSurfaceLuxelDim = 34;
constexpr int kMaxSurfaceLuxels   = kMaxSurfaceLuxelDim * kMaxSurfaceLuxelDim;

// Dynamic light contributions dimmer than this are dropped (ref_gl's DLIGHT_CUTOFF).
constexpr float kDLightCutoff = 64.0f;

// ModelSurface::lightmapDynamicFrame when the surface's block holds nothing but
// its static lighting. Zero is safe as the "none" value because the view
// renderer pre-increments its frame counter, so no surface is ever stamped with
// frame 0 - and it is what the zero-filled model hunk starts out at.
constexpr int kNoDynamicFrame = 0;

// Luxels per atlas, and the bytes each of the two per-atlas buffers takes.
constexpr int kAtlasLuxels     = kLightmapTextureWidth * kLightmapTextureHeight;
constexpr int kAtlasAlphaBytes = kAtlasLuxels * static_cast<int>(sizeof(u8));
constexpr int kAtlasColorBytes = kAtlasLuxels * static_cast<int>(sizeof(u16));

// Light styles as they stand before the first frame renders: every style fully
// on. CreateSurfaceLightmap bakes against these, so a surface needs no rebuild
// until a style actually animates - ref_gl primes r_newrefdef.lightstyles the
// same way in GL_BeginBuildingLightmaps. File-static rather than a local: 4 KB
// is more stack than the model loader should be spending.
static lightstyle_t s_defaultLightStyles[MAX_LIGHTSTYLES];

// Luxels a surface spans, from its texture-space extents.
inline int LuxelsWide(const mod::ModelSurface & surf) { return (surf.extents[0] >> 4) + 1; }
inline int LuxelsHigh(const mod::ModelSurface & surf) { return (surf.extents[1] >> 4) + 1; }

// ------------------------------------------------------------------------------------------------
// LightmapManager
// ------------------------------------------------------------------------------------------------

// Owns the atlases and the packer. Internal singleton (s_manager); the module
// API below is the public face.
class LightmapManager final
{
public:
    void Init();

    void BeginBuilding();
    void CreateSurfaceLightmap(mod::ModelSurface & surf);
    void EndBuilding();
    void ReleaseAtlases();

    void BeginFrame();
    void ChainSurface(mod::ModelSurface & surf, const refdef_t & viewDef, int frameCount);
    void ClearChains();

    int NumAtlases() const { return m_atlasCount; }

    const tex::Texture & AtlasTexture(int index) const
    {
        PS2_Assert(index >= 0 && index < m_atlasCount);
        return m_atlases[index];
    }

    const u16 * AtlasColors(int index) const
    {
        PS2_Assert(index >= 0 && index < m_atlasCount);
        return m_mirror[index];
    }

    const mod::ModelSurface * AtlasChain(int index) const
    {
        PS2_Assert(index >= 0 && index < m_atlasCount);
        return m_chains[index];
    }

    Stats GetStats() const { return m_stats; }

private:
    void NextAtlas();
    int  CurrentAtlas() const { return m_atlasCount - 1; }

    void ResetBlocks() { m_packer.Reset(); }
    bool AllocBlock(int w, int h, int * outX, int * outY) { return m_packer.Alloc(w, h, outX, outY); }

    void BuildLightmap(const mod::ModelSurface & surf, const lightstyle_t * lightstyles,
                       const dlight_t * dlights, int numDlights, bool addDynamic);
    void AddDynamicLights(const mod::ModelSurface & surf, const dlight_t * dlights, int numDlights);
    void StoreLightmap(const mod::ModelSurface & surf);
    void SetCacheState(mod::ModelSurface & surf, const lightstyle_t * lightstyles);

    int  m_atlasCount = 0;
    bool m_building   = false;

    // The atlases, one luxel split across the pair: m_alpha holds the intensity
    // the GS samples (and is what Texture::pixels points at), m_mirror the
    // chroma the diffuse pass samples off the EE, allocated in step with it.
    // Both are heap blocks, so a map only pays for the atlases it fills.
    tex::Texture m_atlases[kMaxLightmapTextures] = {};
    u8 *         m_alpha[kMaxLightmapTextures]   = {};
    u16 *        m_mirror[kMaxLightmapTextures]  = {};

    // Per-atlas draw chains, rebuilt every frame by ChainSurface.
    const mod::ModelSurface * m_chains[kMaxLightmapTextures] = {};

    // Packs each surface's luxel block into the atlas being built. Shared with
    // the 2D scrap atlases, which pack the same way at the same size.
    SkylinePacker<kLightmapTextureWidth, kLightmapTextureHeight> m_packer = {};

    // Where a surface's luxels are accumulated before being clamped and stored.
    // Floating point and unclamped, so styles and dynamic lights can push a
    // luxel past full brightness and the store step can rescale it in one go.
    float m_blockLights[kMaxSurfaceLuxels * 3] = {};

    Stats m_stats = {};

    // ref_gl's gl_modulate: scales every luxel as it is accumulated.
    const cvar_t * m_modulate = nullptr;
};

void LightmapManager::Init()
{
    m_modulate = Cvar_Get("ps2_lightmap_modulate", "1", CVAR_ARCHIVE);

    for (lightstyle_t & style : s_defaultLightStyles)
    {
        style.rgb[0] = 1.0f;
        style.rgb[1] = 1.0f;
        style.rgb[2] = 1.0f;
        style.white  = 3.0f;
    }
}

// ------------------------------------------------------------------------------------------------
// Atlas lifetime
// ------------------------------------------------------------------------------------------------

void LightmapManager::ReleaseAtlases()
{
    for (int i = 0; i < m_atlasCount; ++i)
    {
        gs::ReleaseTexture(m_atlases[i]); // no-op when it was never resident

        ps2::heap::Free(m_alpha[i],  kAtlasAlphaBytes, ps2::heap::MemTag::Lightmap);
        ps2::heap::Free(m_mirror[i], kAtlasColorBytes, ps2::heap::MemTag::Lightmap);

        m_alpha[i]   = nullptr;
        m_mirror[i]  = nullptr;
        m_chains[i]  = nullptr;

        // Zeroing the whole struct would leave vramAddr reading as "resident at
        // word 0" rather than not-resident. NextAtlas rewrites every field
        // before the slot is used again, but a released texture should never be
        // one stray call away from freeing someone else's VRAM.
        m_atlases[i] = {};
        m_atlases[i].vramAddr = tex::Texture::kNotResident;
    }
    m_atlasCount = 0;
}

void LightmapManager::NextAtlas()
{
    if (m_atlasCount == kMaxLightmapTextures) [[unlikely]]
    {
        Sys_Error("Out of lightmap atlases! Bump lm::kMaxLightmapTextures (%d).",
                  kMaxLightmapTextures);
    }

    const int index = m_atlasCount++;

    m_alpha[index]  = static_cast<u8  *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(16), kAtlasAlphaBytes, ps2::heap::MemTag::Lightmap));
    m_mirror[index] = static_cast<u16 *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(16), kAtlasColorBytes, ps2::heap::MemTag::Lightmap));

    // Clear to white so the gaps the packer leaves modulate to 1.0 rather than
    // to black - a surface whose UVs stray a texel outside its block then reads
    // "unlit by anything" instead of a hard shadow. (ref_gl leaves them
    // uninitialised; MrQ2 clears them, which is the better behaviour.)
    std::memset(m_alpha[index],  0xFF, kAtlasAlphaBytes);
    std::memset(m_mirror[index], 0xFF, kAtlasColorBytes);

    // The atlases live outside the texture cache: they are never looked up by
    // name and their lifetime is the map's, not the registration sequence's.
    tex::Texture & atlas = m_atlases[index];
    std::snprintf(atlas.name, sizeof(atlas.name), "*lightmap%d", index);

    atlas.regSequence  = 0;
    atlas.pixels       = m_alpha[index];
    atlas.width        = kLightmapTextureWidth;
    atlas.height       = kLightmapTextureHeight;
    atlas.srcWidth     = kLightmapTextureWidth;  // never resampled: already a power of two
    atlas.srcHeight    = kLightmapTextureHeight;
    atlas.dirtyPixels  = true; // CPU-written; the first upload has to flush the dcache
    atlas.type         = tex::ImageType::Wall; // world data; only has to not read as a free slot
    atlas.flags        = tex::TexFlags::None;
    atlas.format       = tex::PixelFormat::Alpha8;
    atlas.components   = tex::TexComponents::RGBA; // the texture function must keep the alpha
    atlas.function     = tex::TexFunction::Modulate;
    atlas.magFilter    = tex::TexFilter::Linear; // luxels are 16 units across; nearest would tile badly
    atlas.minFilter    = tex::TexFilter::Linear;
    atlas.textureChain = nullptr;
    atlas.vramAddr     = tex::Texture::kNotResident;

    ResetBlocks();
}

void LightmapManager::BeginBuilding()
{
    ReleaseAtlases();
    NextAtlas();
    m_stats    = {};
    m_building = true;
}

void LightmapManager::EndBuilding()
{
    m_building = false;
    m_stats.atlases = m_atlasCount;

    Com_DPrintf("Lightmaps: %d atlas(es) of %dx%d, %d KB of EE RAM.\n",
                m_atlasCount, kLightmapTextureWidth, kLightmapTextureHeight,
                (m_atlasCount * (kAtlasAlphaBytes + kAtlasColorBytes)) / 1024);
}

// ------------------------------------------------------------------------------------------------
// Luxel accumulation (ref_gl's R_BuildLightMap / R_AddDynamicLights)
// ------------------------------------------------------------------------------------------------

// Sums the surface's baked light samples - one map per active style, each scaled
// by that style's current colour - into m_blockLights, optionally adding the
// frame's dynamic lights on top. Leaves the result unclamped for StoreLightmap.
void LightmapManager::BuildLightmap(const mod::ModelSurface & surf, const lightstyle_t * lightstyles,
                                    const dlight_t * dlights, const int numDlights, const bool addDynamic)
{
    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);
    const int size = smax * tmax;

    PS2_AssertMsg(size <= kMaxSurfaceLuxels, "Surface lightmap too large for the accumulator!");

    // No baked samples: full bright, and no dynamic lights either (ref_gl does
    // the same - there is no static basis for them to add to).
    if (surf.samples == nullptr)
    {
        for (int i = 0; i < size * 3; ++i)
        {
            m_blockLights[i] = 255.0f;
        }
        return;
    }

    const float modulate = m_modulate->value;
    const u8 * samples = surf.samples;

    for (int map = 0; map < mod::kMaxLightmaps && surf.styles[map] != 255; ++map)
    {
        const float * styleRgb = lightstyles[surf.styles[map]].rgb;
        const float scale[3] = { modulate * styleRgb[0],
                                 modulate * styleRgb[1],
                                 modulate * styleRgb[2] };

        float * bl = m_blockLights;

        // The first style assigns, so the common single-style surface needs no
        // clearing pass; the rest accumulate over it.
        if (map == 0)
        {
            for (int i = 0; i < size; ++i, bl += 3)
            {
                bl[0] = static_cast<float>(samples[(i * 3) + 0]) * scale[0];
                bl[1] = static_cast<float>(samples[(i * 3) + 1]) * scale[1];
                bl[2] = static_cast<float>(samples[(i * 3) + 2]) * scale[2];
            }
        }
        else
        {
            for (int i = 0; i < size; ++i, bl += 3)
            {
                bl[0] += static_cast<float>(samples[(i * 3) + 0]) * scale[0];
                bl[1] += static_cast<float>(samples[(i * 3) + 1]) * scale[1];
                bl[2] += static_cast<float>(samples[(i * 3) + 2]) * scale[2];
            }
        }

        samples += size * 3; // Next style's map.
    }

    if (addDynamic)
    {
        AddDynamicLights(surf, dlights, numDlights);
    }
}

// Adds the frame's dynamic lights to the accumulated luxels. Only the lights
// MarkDLights flagged in surf.dlightBits are considered, and each is attenuated
// linearly over an octagonal distance approximation - max + min/2, no square
// root anywhere (ref_gl's R_AddDynamicLights).
void LightmapManager::AddDynamicLights(const mod::ModelSurface & surf,
                                       const dlight_t * dlights, const int numDlights)
{
    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);
    const mod::ModelTexInfo * const tex = surf.texInfo;

    for (int lnum = 0; lnum < numDlights; ++lnum)
    {
        if (!(surf.dlightBits & (1 << lnum)))
        {
            continue; // Not lit by this one.
        }

        const dlight_t & dl = dlights[lnum];

        // How much of the light's intensity survives the trip to the surface's
        // plane - that is the brightest any luxel on it can get.
        const float planeDist = DotProduct(dl.origin, surf.plane->normal) - surf.plane->dist;
        const float frad = dl.intensity - std::fabs(planeDist);
        if (frad < kDLightCutoff)
        {
            continue;
        }
        const float fminlight = frad - kDLightCutoff;

        // The light projected onto the surface's plane, then into its texture
        // space, relative to the origin its luxel grid starts at.
        vec3_t impact;
        for (int i = 0; i < 3; ++i)
        {
            impact[i] = dl.origin[i] - (surf.plane->normal[i] * planeDist);
        }

        const float local[2] = {
            DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3] - static_cast<float>(surf.textureMins[0]),
            DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3] - static_cast<float>(surf.textureMins[1])
        };

        float * bl = m_blockLights;
        float ftacc = 0.0f;

        for (int t = 0; t < tmax; ++t, ftacc += static_cast<float>(kLuxelSizeUnits))
        {
            int td = static_cast<int>(local[1] - ftacc);
            if (td < 0)
            {
                td = -td;
            }

            float fsacc = 0.0f;
            for (int s = 0; s < smax; ++s, fsacc += static_cast<float>(kLuxelSizeUnits), bl += 3)
            {
                int sd = static_cast<int>(local[0] - fsacc);
                if (sd < 0)
                {
                    sd = -sd;
                }

                const float dist = static_cast<float>((sd > td) ? (sd + (td >> 1)) : (td + (sd >> 1)));
                if (dist < fminlight)
                {
                    const float add = frad - dist;
                    bl[0] += add * dl.color[0];
                    bl[1] += add * dl.color[1];
                    bl[2] += add * dl.color[2];
                }
            }
        }
    }
}

// Clamps the accumulated luxels and writes them into the surface's block of its
// atlas, split into the two terms the hardware needs them in: the intensity into
// the Alpha8 texture the GS samples, the chroma into the EE-side mirror beside it.
void LightmapManager::StoreLightmap(const mod::ModelSurface & surf)
{
    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);

    const int atlas  = surf.lightmapTextureNum;
    const int offset = (surf.light_t * kLightmapTextureWidth) + surf.light_s;

    u8  * dstAlpha = m_alpha[atlas]  + offset;
    u16 * dstColor = m_mirror[atlas] + offset;

    const float * bl = m_blockLights;

    for (int t = 0; t < tmax; ++t)
    {
        for (int s = 0; s < smax; ++s, bl += 3)
        {
            int r = static_cast<int>(bl[0]);
            int g = static_cast<int>(bl[1]);
            int b = static_cast<int>(bl[2]);

            // Negative lights are a thing in Quake maps.
            if (r < 0) { r = 0; }
            if (g < 0) { g = 0; }
            if (b < 0) { b = 0; }

            int max = (r > g) ? r : g;
            if (b > max)
            {
                max = b;
            }

            // When the brightest channel clips, scale all three by the same
            // factor rather than clamping each on its own - an overbright luxel
            // then keeps its hue instead of washing out toward white.
            if (max > 255)
            {
                const float rescale = 255.0f / static_cast<float>(max);
                r = static_cast<int>(static_cast<float>(r) * rescale);
                g = static_cast<int>(static_cast<float>(g) * rescale);
                b = static_cast<int>(static_cast<float>(b) * rescale);
                max = 255;
            }

            // Floor the intensity at 1: the alpha ramp maps index 0 to alpha 0,
            // and the batch's alpha test drops those fragments outright - which
            // would leave the diffuse pass *unmodulated* where the surface is
            // darkest, the exact inverse of the shadow we want.
            dstAlpha[s] = static_cast<u8>((max < 1) ? 1 : max);

            // The chroma is what is left once that intensity is divided back
            // out - rgb rescaled so its brightest channel is full. Storing the
            // colour as-is would carry the intensity a second time and the
            // diffuse pass, which multiplies by this, would square it. A luxel
            // with no colour at all has no chroma to speak of, so it takes
            // white and lets the (floored) intensity alone darken it.
            if (max > 0)
            {
                const float toFull = 255.0f / static_cast<float>(max);
                dstColor[s] = PackAtlasColor(static_cast<int>(static_cast<float>(r) * toFull + 0.5f),
                                             static_cast<int>(static_cast<float>(g) * toFull + 0.5f),
                                             static_cast<int>(static_cast<float>(b) * toFull + 0.5f));
            }
            else
            {
                dstColor[s] = PackAtlasColor(255, 255, 255);
            }
        }

        dstAlpha += kLightmapTextureWidth;
        dstColor += kLightmapTextureWidth;
    }
}

// Records the style intensities the surface's luxels were just baked at, so a
// later frame can tell whether an animated style has moved since.
void LightmapManager::SetCacheState(mod::ModelSurface & surf, const lightstyle_t * lightstyles)
{
    for (int map = 0; map < mod::kMaxLightmaps && surf.styles[map] != 255; ++map)
    {
        surf.cachedLight[map] = lightstyles[surf.styles[map]].white;
    }
}

// ------------------------------------------------------------------------------------------------
// Build time
// ------------------------------------------------------------------------------------------------

void LightmapManager::CreateSurfaceLightmap(mod::ModelSurface & surf)
{
    PS2_AssertMsg(m_building, "CreateSurfaceLightmap outside Begin/EndBuildingLightmaps!");

    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);

    // ModelSurface stores the atlas coordinates as s16 (they are luxel offsets
    // into a lightmap texture, so they cannot approach the limit), while
    // AllocBlock works in int - hence the locals.
    int lightS = 0;
    int lightT = 0;

    if (!AllocBlock(smax, tmax, &lightS, &lightT))
    {
        // Atlas full: start a fresh one and try once more. A surface that will
        // not fit an empty atlas is a broken map, not a packing failure.
        NextAtlas();

        if (!AllocBlock(smax, tmax, &lightS, &lightT)) [[unlikely]]
        {
            Sys_Error("Consecutive lightmap AllocBlock(%d, %d) failures!", smax, tmax);
        }
    }

    surf.light_s = static_cast<s16>(lightS);
    surf.light_t = static_cast<s16>(lightT);

    surf.lightmapTextureNum   = static_cast<s16>(CurrentAtlas());
    surf.lightmapDynamicFrame = kNoDynamicFrame;

    BuildLightmap(surf, s_defaultLightStyles, nullptr, 0, /* addDynamic = */ false);
    StoreLightmap(surf);
    SetCacheState(surf, s_defaultLightStyles);
}

// ------------------------------------------------------------------------------------------------
// Frame time
// ------------------------------------------------------------------------------------------------

void LightmapManager::BeginFrame()
{
    const int atlases = m_stats.atlases;
    m_stats = {};
    m_stats.atlases = atlases;
}

void LightmapManager::ChainSurface(mod::ModelSurface & surf, const refdef_t & viewDef, const int frameCount)
{
    const int atlas = surf.lightmapTextureNum;
    PS2_AssertMsg(atlas >= 0 && atlas < m_atlasCount, "ChainSurface on a surface with no lightmap!");

    // Is a dynamic light touching the surface this frame? MarkDLights stamped
    // the ones that are while walking the BSP.
    const bool dlit = (surf.dlightFrame == frameCount);

    if (dlit)
    {
        // Fold this frame's lights straight into the surface's own block. The
        // cached style state deliberately still describes the *static* bake, so
        // the restore below knows the block is dirty even if no style moved.
        BuildLightmap(surf, viewDef.lightstyles, viewDef.dlights, viewDef.num_dlights, /* addDynamic = */ true);
        StoreLightmap(surf);

        surf.lightmapDynamicFrame = frameCount;
        m_atlases[atlas].MarkPixelsDirty();
        ++m_stats.dynamicUpdates;
    }
    else
    {
        // Has an animated style moved since these luxels were baked?
        bool styleMoved = false;
        for (int map = 0; map < mod::kMaxLightmaps && surf.styles[map] != 255; ++map)
        {
            if (viewDef.lightstyles[surf.styles[map]].white != surf.cachedLight[map])
            {
                styleMoved = true;
                break;
            }
        }

        // A light that has stopped touching the surface leaves that frame's
        // luxels behind in the block, so it needs the same rebuild an animated
        // style does - just for a different reason. Counted apart because a
        // permanently-stuck restore is the failure mode worth spotting.
        const bool stale = (surf.lightmapDynamicFrame != kNoDynamicFrame);

        if (stale || styleMoved)
        {
            BuildLightmap(surf, viewDef.lightstyles, nullptr, 0, /* addDynamic = */ false);
            StoreLightmap(surf);
            SetCacheState(surf, viewDef.lightstyles);

            surf.lightmapDynamicFrame = kNoDynamicFrame;
            m_atlases[atlas].MarkPixelsDirty();

            if (stale) { ++m_stats.restoreUpdates; }
            else       { ++m_stats.styleUpdates;   }
        }
    }

    // Thread onto its atlas's chain for the lightmap pass.
    surf.lightmapChain = m_chains[atlas];
    m_chains[atlas] = &surf;
}

void LightmapManager::ClearChains()
{
    for (int i = 0; i < m_atlasCount; ++i)
    {
        m_chains[i] = nullptr;
    }
}

static LightmapManager s_manager;

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    s_manager.Init();
}

void BeginBuildingLightmaps()
{
    s_manager.BeginBuilding();
}

void CreateSurfaceLightmap(mod::ModelSurface & surf)
{
    s_manager.CreateSurfaceLightmap(surf);
}

void EndBuildingLightmaps()
{
    s_manager.EndBuilding();
}

void ReleaseAtlases()
{
    s_manager.ReleaseAtlases();
}

void BeginFrame()
{
    s_manager.BeginFrame();
}

void ChainSurface(mod::ModelSurface & surf, const refdef_t & viewDef, const int frameCount)
{
    s_manager.ChainSurface(surf, viewDef, frameCount);
}

int NumAtlases()
{
    return s_manager.NumAtlases();
}

const tex::Texture & AtlasTexture(const int index)
{
    return s_manager.AtlasTexture(index);
}

const u16 * AtlasColors(const int index)
{
    return s_manager.AtlasColors(index);
}

const mod::ModelSurface * AtlasChain(const int index)
{
    return s_manager.AtlasChain(index);
}

void ClearChains()
{
    s_manager.ClearChains();
}

Stats GetStats()
{
    return s_manager.GetStats();
}

} // namespace ps2::lm
