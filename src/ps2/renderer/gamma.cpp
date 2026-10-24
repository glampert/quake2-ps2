/* ================================================================================================
 * File: gamma.cpp
 * Brief: Brightness and contrast as palette curves. See gamma.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/builtin/builtin.h"
#include "ps2/renderer/gamma.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_system.h"

#include <algorithm>
#include <cmath>

namespace ps2::gamma {
namespace detail {

// The video menu's sliders step vid_gamma from 1.3 down to 0.5, as id's vid_menu.c did, and
// vid_contrast from 1 to 2, as QuakeSpasm's do. The console takes anything, so the curve clamps to
// what still shows a picture.
constexpr float kMinGamma    = 0.25f;
constexpr float kMaxGamma    = 4.0f;
constexpr float kMinContrast = 0.25f;
constexpr float kMaxContrast = 4.0f;

constexpr int kNumEntries = 256;

alignas(16) u32 g_palette[kNumEntries];
alignas(16) u32 g_litPalette[kNumEntries];
alignas(16) u8  g_lightRamp[kNumEntries];
float           g_lightCurve[kLightCurveSteps + 1];

float g_gamma      = 1.0f;
bool  g_identity   = true;
u32   g_generation = 0;

static const cvar_t * s_gammaVar    = nullptr;
static const cvar_t * s_contrastVar = nullptr;

static float s_intensity = 1.0f;
static float s_contrast  = 1.0f;

static float ClampedGamma()
{
    return std::clamp(s_gammaVar->value, kMinGamma, kMaxGamma);
}

static float ClampedContrast()
{
    return std::clamp(s_contrastVar->value, kMinContrast, kMaxContrast);
}

// A channel value 0 to 255 through the curve, contrast and all.
Q_ALWAYS_INLINE static u32 CurveChannel(const u32 value)
{
    const float x   = (static_cast<float>(value) / 255.0f) * s_contrast;
    const float out = std::min(std::pow(x, g_gamma), 1.0f);
    return static_cast<u32>((out * 255.0f) + 0.5f);
}

u32 CurveRgba(const u32 rgba)
{
    return  CurveChannel(rgba         & 0xFFu)
         | (CurveChannel((rgba >>  8) & 0xFFu) <<  8)
         | (CurveChannel((rgba >> 16) & 0xFFu) << 16)
         | (rgba & 0xFF000000u);
}

static void Rebuild(const float gamma, const float contrast)
{
    g_gamma    = gamma;
    s_contrast = contrast;
    g_identity = (gamma == 1.0f && contrast == 1.0f);
    ++g_generation;

    // ref_gl's intensitytable, which the lit palette has always been built with: each channel
    // scaled and clamped at full, truncated, the alpha left alone. It keeps a texture from going
    // dim the moment a light multiplies it back down: Quake's baked lightmaps are dark (a median
    // luxel across the retail maps is 46 of 255, and none of them exceed 196), so without it a lit
    // surface draws at a fraction of the brightness it was authored for. 256 entries share 256
    // channel values, so the scale is worked out once per value.
    u8 intensityTable[kNumEntries];
    for (int i = 0; i < kNumEntries; ++i)
    {
        const float scaled = static_cast<float>(i) * s_intensity;
        intensityTable[i] = static_cast<u8>((scaled >= 255.0f) ? 255.0f : scaled);
    }

    for (int i = 0; i < kNumEntries; ++i)
    {
        const u32 rgba = global_palette[i];
        const u32 lit  =  static_cast<u32>(intensityTable[ rgba        & 0xFFu])
                       | (static_cast<u32>(intensityTable[(rgba >>  8) & 0xFFu]) <<  8)
                       | (static_cast<u32>(intensityTable[(rgba >> 16) & 0xFFu]) << 16)
                       | (rgba & 0xFF000000u);

        g_palette[i]    = g_identity ? rgba : CurveRgba(rgba);
        g_litPalette[i] = g_identity ? lit  : CurveRgba(lit);
    }

    // A luxel's light is its alpha ramp entry over 128 (clut.h's BuildAlphaRamp, which the atlases
    // sampled before), through the curve without the contrast, which the texture it multiplies
    // carries. At gamma 1 that is the alpha ramp exactly: 128 * (a / 128) is a, whatever a is.
    // Index 0 stays 0, the alpha the alpha test drops, and nothing else falls to it: a luxel
    // darkened to nothing would draw its texel unlit instead.
    g_lightRamp[0] = 0;
    for (int i = 1; i < kNumEntries; ++i)
    {
        const float light = static_cast<float>((i + 1) >> 1) / 128.0f;
        const int   alpha = static_cast<int>((std::pow(light, gamma) * 128.0f) + 0.5f);
        g_lightRamp[i] = static_cast<u8>(std::clamp(alpha, 1, 128));
    }

    for (int i = 0; i <= kLightCurveSteps; ++i)
    {
        g_lightCurve[i] = std::pow(static_cast<float>(i) / kLightCurveScale, gamma);
    }
}

} // namespace detail

void Init(const float intensity)
{
    detail::s_gammaVar    = Cvar_Get("vid_gamma",    "1", CVAR_ARCHIVE);
    detail::s_contrastVar = Cvar_Get("vid_contrast", "1", CVAR_ARCHIVE);
    detail::s_intensity   = intensity;

    detail::Rebuild(detail::ClampedGamma(), detail::ClampedContrast());
}

void ApplyPendingChange()
{
    const float gamma    = detail::ClampedGamma();
    const float contrast = detail::ClampedContrast();
    if (gamma == detail::g_gamma && contrast == detail::s_contrast)
    {
        return;
    }
    detail::Rebuild(gamma, contrast);

    // The frame left drawing reads the CLUTs until it is done: shown now, on its vsync, so the GS
    // is idle for the upload and the next frame draws through the new ones.
    rs::FinishFrameInFlight();
    gs::UpdateCluts(detail::g_palette, detail::g_litPalette, detail::g_lightRamp);
}

} // namespace ps2::gamma
