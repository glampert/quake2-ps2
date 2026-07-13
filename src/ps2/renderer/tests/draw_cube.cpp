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
    constexpr bool kOverrideVertexColors = false;

    const Corner & corner = kCorners[cornerIdx];
    vert.x = corner.x;
    vert.y = corner.y;
    vert.z = corner.z;
    vert.w = 1.0f;
    vert.r = kOverrideVertexColors ? 255 : corner.r;
    vert.g = kOverrideVertexColors ? 255 : corner.g;
    vert.b = kOverrideVertexColors ? 255 : corner.b;
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
    static const cvar_t * s_testCube = Cvar_Get("ps2_testcube", "1", 0);
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

    // One debug texture variant per face - 6 tiny batches instead of one - so
    // a single spin of the cube exercises repeated texture switching against
    // the VRAM streaming path.
    //
    // With ps2_testcube_vram_tex_eviction on, the faces instead share a
    // 3-variant window that slides every 2 seconds: the per-frame texture set
    // keeps changing, and with the heap shrunk (kDebugHeapLimitWords in
    // vram.cpp) every slide evicts the stalest variant and re-uploads a
    // previously evicted one - the face colors changing is proof of the
    // re-uploads. It defaults on to pair with the debug heap limit: the full
    // 6-variant set (26 pages with the fullscreen console) would assert on
    // boot at the 24-page test heap, before the cvar could be toggled.
    static_assert(tex::kNumDebugTextures >= 6, "One variant per cube face");
    static const cvar_t * s_testEviction = Cvar_Get("ps2_testcube_vram_tex_eviction", "0", 0);

    const int tick = Sys_Milliseconds() / 2000;
    for (int face = 0; face < 6; ++face)
    {
        const int variant = (s_testEviction->value != 0.0f)
                          ? ((face % 3) + tick) % tex::kNumDebugTextures
                          : face;

        vu1::DrawTriangles(mvp, tex::DebugTexture(variant), &s_cubeVerts[face * 6], 6);
    }
}

} // namespace ps2::test
