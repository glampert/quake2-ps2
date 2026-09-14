#pragma once
/* ================================================================================================
 * File: sky.h
 * Brief: Skybox rendering - the six textured cube faces behind SURF_SKY surfaces.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"

namespace ps2::mod { struct ModelSurface; }

namespace ps2::sky {

// Registers the sky cvars. Call once from view::Init().
void Init();

// Forgets the current map's sky. Call when a new map loads
// (PS2_BeginRegistration): tex::EndRegistration() frees the level's Sky
// textures, so the cached face pointers must not outlive it.
void BeginRegistration();

// Loads the six faces of sky 'name' from env/<name>{rt,bk,lf,ft,up,dn}.pcx and
// stores the rotation the map asked for ('rotate' in degrees per second about
// 'axis'). The client calls this once per level, from CL_PrepRefresh, between
// BeginRegistration and EndRegistration - which is what keeps the faces from
// being swept up as stale. An empty name disables the sky.
void SetSky(const char * name, float rotate, const vec3_t axis);

// Empties the visible-sky bounds for a new frame (ref_gl's R_ClearSkyBox).
void ClearBounds();

// Folds one SURF_SKY world surface into the visible-sky bounds (ref_gl's
// R_AddSkySurface). Sky surfaces are never drawn themselves - all they do is
// record which part of which cube face the player can actually see through
// them, so DrawSkyBox can draw that part and nothing more.
void AddSurface(const mod::ModelSurface & surf, const vec3_t viewOrigin);

// Draws the cube faces the frame's sky surfaces exposed, centred on the eye
// (ref_gl's R_DrawSkyBox). Draws nothing when no sky was visible. Call after
// the opaque world pass, so the z-test can reject the faces behind it, and
// before the entities.
void DrawSkyBox(const refdef_t & viewDef, const math::Mat4 & viewProj);

} // namespace ps2::sky
