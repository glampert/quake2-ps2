#pragma once
/* ================================================================================================
 * File: draw_cube.h
 * Brief: Debug scene for the VU1 3D bring-up: a spinning cube with per-vertex colors and
 *        the checkerboard debug texture, drawn straight through vu1::DrawTriangles.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::test {

// Draws the cube for the current frame. Call between gs::Begin/EndFrame, after
// vu1::Init(). Gated by the "ps2_testcube" cvar; a no-op when it is 0.
void DrawRotatingCube();

} // namespace ps2::test
