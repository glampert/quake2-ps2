#pragma once
/* ================================================================================================
 * File: gamma.h
 * Brief: The video menu's brightness and contrast sliders: vid_gamma, id's own cvar (ref_soft built
 *        it into the palette, ref_gl into its textures or the display's gamma ramp), and
 *        vid_contrast, as the Quake port has it from QuakeSpasm. Both shape the picture as
 *        pow(colour * contrast, gamma), colour 0 to 1. The GS has no stage that could do that to the
 *        finished frame, so here it goes into the palette instead, as ref_soft's gamma did: nearly
 *        everything the renderer draws samples Quake's palette through a CLUT, so the curve goes
 *        into the CLUTs and into the few colours the EE picks from the palette itself (particles,
 *        beams, fills, the cinematics' palette).
 *
 *        The light takes the curve on its own, so a lit surface comes out as the curve would make
 *        the lit result: pow(t * l, g) is pow(t, g) * pow(l, g). The walls' texture part is in the
 *        lit palette's CLUT and the luxel's in the light ramp CLUT the lightmap atlases sample; a
 *        model's in its skin's CLUT and the shade light, through LightCurve, per model. The
 *        contrast is in the palette alone, so a bright texel saturates there whatever its light.
 *        Left linear: a model's per-vertex shading term, the luxels' chroma, the dynamic lights,
 *        the flares, the shells and the full-screen blend. True-colour images (none in the retail
 *        data) have no CLUT, and draw as their texels are.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>
#include <cmath>

namespace ps2::gamma {

// Registers vid_gamma and vid_contrast and builds the tables from Quake's palette for their values,
// with ps2_intensity's 'intensity' in the lit palette (see LitPalette). PS2_RefInit calls it once,
// before gs::Init uploads the CLUTs from the tables: config.cfg has run, so a value it set shows
// from the first frame.
void Init(float intensity);

// Rebuilds the tables and re-uploads the CLUTs when vid_gamma or vid_contrast changed since the
// last call. PS2_BeginFrame calls it, just before rs::BeginFrame: it finishes the frame left drawing
// first, since the CLUTs can't change under it.
void ApplyPendingChange();

// Extern globals, and the curve's colour arithmetic, so the accessors below can be inlined. Rebuild
// in gamma.cpp is the one writer.
namespace detail {

// LightCurve's table: lights 0 to 2 (a model's shade light runs past 1) in steps of 1/128, the
// steps the vertex colours it ends up in are quantized to. Brighter than that takes std::pow.
constexpr int   kLightCurveSteps = 256;
constexpr float kLightCurveScale = 128.0f; // table steps per unit of light
constexpr float kLightCurveMax   = static_cast<float>(kLightCurveSteps) / kLightCurveScale;

extern u32   g_palette[];
extern u32   g_litPalette[];
extern u8    g_lightRamp[];
extern float g_lightCurve[];
extern float g_gamma;
extern bool  g_identity; // gamma 1, contrast 1: the tables are what they were without them
extern u32   g_generation;

// An RGBA colour through the curve, contrast and all, the alpha as it is.
u32 CurveRgba(u32 rgba);

} // namespace detail

// Quake's palette through the curve: the global palette CLUT's entries, and the colour a palette
// index shows as for colours that reach the GS from the EE rather than through a CLUT.
inline const u32 * Palette()
{
    return detail::g_palette;
}

// The same palette brightened by 'intensity' first, as ref_gl's intensitytable did, then through
// the curve: the lit palette CLUT, sampled by the images a light multiplies back down (walls,
// skins, sprites; see tex::TakesIntensity).
inline const u32 * LitPalette()
{
    return detail::g_litPalette;
}

// The alpha the light ramp CLUT gives each luxel value: light 0 to 1 as GS alpha 0 to 128, through
// the curve. Index 0 stays alpha 0, which the alpha test drops, and the rest stay 1 or more.
inline const u8 * LightRamp()
{
    return detail::g_lightRamp;
}

// A light that reaches the GS as a vertex colour (a model's shade light), through the curve.
inline float LightCurve(const float light)
{
    if (detail::g_identity)
    {
        return light;
    }
    if (light <= 0.0f)
    {
        return 0.0f;
    }
    if (light >= detail::kLightCurveMax)
    {
        return std::pow(light, detail::g_gamma);
    }

    // Linear between the table's steps.
    const float position = light * detail::kLightCurveScale;
    const int   step     = static_cast<int>(position);
    const float frac     = position - static_cast<float>(step);
    return detail::g_lightCurve[step] + ((detail::g_lightCurve[step + 1] - detail::g_lightCurve[step]) * frac);
}

// A colour, RGBA as the palette holds it, through the curve: for a palette of the EE's own, the
// cinematics'. The alpha stays as it is.
inline u32 CurveColor(const u32 rgba)
{
    return detail::g_identity ? rgba : detail::CurveRgba(rgba);
}

// Counts the curve's changes, so a table built from CurveColor knows when to rebuild.
inline u32 Generation()
{
    return detail::g_generation;
}

} // namespace ps2::gamma
