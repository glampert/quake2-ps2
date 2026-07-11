/* ================================================================================================
 * File: draw_cube.cpp
 * Brief: Debug scene for the VU1 3D bring-up. See draw_cube.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/tests/draw_cube.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vu1.h"
#include "ps2/math/vec_mat.h"

namespace ps2::test {
namespace {

constexpr float kCubeHalfSize = 20.0f;

// The 8 cube corners, each with its own color so every face gets a gradient.
struct Corner
{
    float x, y, z;
    u32 r, g, b;
};

constexpr Corner kCorners[8] = {
    { -kCubeHalfSize, -kCubeHalfSize, -kCubeHalfSize, 255,   0,   0 },
    {  kCubeHalfSize, -kCubeHalfSize, -kCubeHalfSize,   0, 255,   0 },
    {  kCubeHalfSize,  kCubeHalfSize, -kCubeHalfSize,   0,   0, 255 },
    { -kCubeHalfSize,  kCubeHalfSize, -kCubeHalfSize, 255, 255,   0 },
    { -kCubeHalfSize, -kCubeHalfSize,  kCubeHalfSize, 255,   0, 255 },
    {  kCubeHalfSize, -kCubeHalfSize,  kCubeHalfSize,   0, 255, 255 },
    {  kCubeHalfSize,  kCubeHalfSize,  kCubeHalfSize, 255, 255, 255 },
    { -kCubeHalfSize,  kCubeHalfSize,  kCubeHalfSize, 255, 128,   0 },
};

// The 6 faces as corner indices, wound consistently seen from outside.
constexpr int kFaces[6][4] = {
    { 0, 1, 2, 3 }, // back   (z-)
    { 5, 4, 7, 6 }, // front  (z+)
    { 4, 0, 3, 7 }, // left   (x-)
    { 1, 5, 6, 2 }, // right  (x+)
    { 4, 5, 1, 0 }, // bottom (y-)
    { 3, 2, 6, 7 }, // top    (y+)
};

// Full 0-1 texture range on every face.
constexpr float kFaceUVs[4][2] = {
    { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
};

// 6 faces * 2 triangles; built once, referenced in place by the DMA chain.
alignas(16) static vu1::DrawVertex s_cubeVerts[36];
static bool s_cubeBuilt = false;

void EmitVertex(vu1::DrawVertex & vert, int cornerIdx, int uvIdx)
{
    const Corner & corner = kCorners[cornerIdx];
    vert.x = corner.x;
    vert.y = corner.y;
    vert.z = corner.z;
    vert.w = 1.0f;
    vert.r = corner.r;
    vert.g = corner.g;
    vert.b = corner.b;
    vert.a = 0x80; // 1.0 on the GS
    vert.s = kFaceUVs[uvIdx][0];
    vert.t = kFaceUVs[uvIdx][1];
    vert.q = 1.0f;
    vert.pad = 0.0f;
}

void BuildCube()
{
    vu1::DrawVertex * vert = s_cubeVerts;
    for (const auto & face : kFaces)
    {
        // Two triangles per face: corners 0-1-2 and 0-2-3.
        EmitVertex(*vert++, face[0], 0);
        EmitVertex(*vert++, face[1], 1);
        EmitVertex(*vert++, face[2], 2);
        EmitVertex(*vert++, face[0], 0);
        EmitVertex(*vert++, face[2], 2);
        EmitVertex(*vert++, face[3], 3);
    }
}

} // namespace

void DrawRotatingCube()
{
    static cvar_t * s_testCube = Cvar_Get("ps2_testcube", "1", 0);
    if (s_testCube->value == 0.0f)
    {
        return;
    }

    if (!s_cubeBuilt)
    {
        s_cubeBuilt = true;
        BuildCube();
    }

    using namespace ps2::math;

    const float t = MsecToSec(static_cast<float>(Sys_Milliseconds()));

    const Mat4 model = RotationY(t) * RotationX(t * 0.7f);
    const Mat4 view  = LookAt(Vec3{ 0.0f, 25.0f, -80.0f },
                              Vec3{ 0.0f, 0.0f, 0.0f },
                              Vec3{ 0.0f, 1.0f, 0.0f });
    const Mat4 proj  = PerspectiveProjection(DegToRad(60.0f), 4.0f / 3.0f,
                                             640.0f, 448.0f, 2.0f, 2000.0f);

    const Mat4 mvp = model * view * proj;

    vu1::DrawTriangles(mvp, tex::DebugTexture(), s_cubeVerts, ArrayLength(s_cubeVerts));
}

} // namespace ps2::test
