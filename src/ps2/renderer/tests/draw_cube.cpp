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
#include "ps2/renderer/gs.h"
#include "ps2/math/vec_mat.h"

namespace ps2::test {
namespace {

constexpr float kCubeHalfSize = 20.0f;

// The 8 cube corners, each with its own color so every face gets a gradient.
// Channels are 0-255 but kept as floats for the bilinear blend in EmitVertex.
struct Corner
{
    float x, y, z;
    float r, g, b;
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

// Largest ps2_testcube_tess value: per-axis quads per face; caps s_faceVerts.
constexpr int kMaxTess = 8;

// One face's worth of vertices, refilled before each face draw - since
// DrawTriangles is synchronous a single buffer can serve all six faces in
// turn, referenced in place by the DMA chain.
alignas(16) static vu1::DrawVertex s_faceVerts[kMaxTess * kMaxTess * 6];

// Emits the vertex at (u, v) in [0,1]^2 of a face: position and color are the
// bilinear blend of the face's four corners (in winding order), s/t map the
// full texture range across the face. At the face corners this reproduces the
// original untessellated cube exactly.
void EmitVertex(vu1::DrawVertex & vert, const int corners[4], float u, float v)
{
    constexpr bool kOverrideVertexColors = false;

    const Corner & c0 = kCorners[corners[0]]; // (0,0)
    const Corner & c1 = kCorners[corners[1]]; // (1,0)
    const Corner & c2 = kCorners[corners[2]]; // (1,1)
    const Corner & c3 = kCorners[corners[3]]; // (0,1)

    const float w0 = (1.0f - u) * (1.0f - v);
    const float w1 = u * (1.0f - v);
    const float w2 = u * v;
    const float w3 = (1.0f - u) * v;

    vert.x = c0.x * w0 + c1.x * w1 + c2.x * w2 + c3.x * w3;
    vert.y = c0.y * w0 + c1.y * w1 + c2.y * w2 + c3.y * w3;
    vert.z = c0.z * w0 + c1.z * w1 + c2.z * w2 + c3.z * w3;
    vert.w = 1.0f;

    const u32 r = static_cast<u32>(c0.r * w0 + c1.r * w1 + c2.r * w2 + c3.r * w3);
    const u32 g = static_cast<u32>(c0.g * w0 + c1.g * w1 + c2.g * w2 + c3.g * w3);
    const u32 b = static_cast<u32>(c0.b * w0 + c1.b * w1 + c2.b * w2 + c3.b * w3);

    vert.rgba = kOverrideVertexColors
              ? vu1::PackColorRGBA(255, 255, 255, 0x80)
              : vu1::PackColorRGBA(r, g, b, 0x80); // 0x80 = alpha 1.0 on the GS
    vert.s = u;
    vert.t = v;
    vert.q = 1.0f;
}

// Fills destVerts with a tess x tess grid of quads (two triangles each)
// covering the face; returns the vertex count, tess^2 * 6. Tess 1 is the
// plain 2-triangle face; 5+ exceeds kMaxVertsPerBatch and so exercises the
// chunked submission path in DrawTriangles.
int EmitFace(vu1::DrawVertex * destVerts, const int corners[4], int tess)
{
    vu1::DrawVertex * vert = destVerts;
    const float step = 1.0f / static_cast<float>(tess);

    for (int cy = 0; cy < tess; ++cy)
    {
        for (int cx = 0; cx < tess; ++cx)
        {
            const float u0 = static_cast<float>(cx) * step;
            const float v0 = static_cast<float>(cy) * step;
            const float u1 = u0 + step;
            const float v1 = v0 + step;

            // Two triangles per cell, wound like the original face corners.
            EmitVertex(*vert++, corners, u0, v0);
            EmitVertex(*vert++, corners, u1, v0);
            EmitVertex(*vert++, corners, u1, v1);
            EmitVertex(*vert++, corners, u0, v0);
            EmitVertex(*vert++, corners, u1, v1);
            EmitVertex(*vert++, corners, u0, v1);
        }
    }

    return tess * tess * 6;
}

} // namespace

void DrawRotatingCube()
{
    static const cvar_t * s_testCube = Cvar_Get("ps2_testcube", "1", 0);
    if (s_testCube->value == 0.0f)
    {
        return;
    }

    // Face tessellation: each face becomes a tess x tess quad grid, pushing
    // vertex counts past kMaxVertsPerBatch to exercise DrawTriangles' chunked
    // submission (tess 5 = 150 verts per face = 2 chunks; tess 8 = 384 = 4).
    // The cube looks identical at any setting - denser mesh, same surface.
    static const cvar_t * s_testTess = Cvar_Get("ps2_testcube_tess", "8", 0);
    int tess = static_cast<int>(s_testTess->value);
    tess = (tess < 1) ? 1 : ((tess > kMaxTess) ? kMaxTess : tess);

    using namespace ps2::math;

    const float t = MsecToSec(static_cast<float>(Sys_Milliseconds()));

    const Mat4 model = RotationY(t) * RotationX(t * 0.7f);
    const Mat4 view  = LookAt(Vec3{ 0.0f, 25.0f, -80.0f },
                              Vec3{ 0.0f, 0.0f, 0.0f },
                              Vec3{ 0.0f, 1.0f, 0.0f });
    const Mat4 proj  = PerspectiveProjection(DegToRad(60.0f), 4.0f / 3.0f,
                                             static_cast<float>(gs::Width()), static_cast<float>(gs::Height()),
                                             2.0f, 2000.0f);

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
    // re-uploads. Enable it together with the heap limit: the full 6-variant
    // set (26 pages with the fullscreen console) does not fit a heap that small.
    static_assert(tex::kNumDebugTextures >= 6, "One variant per cube face");
    static const cvar_t * s_testEviction = Cvar_Get("ps2_testcube_vram_tex_eviction", "0", 0);

    const int tick = Sys_Milliseconds() / 2000;
    for (int face = 0; face < 6; ++face)
    {
        const int variant = (s_testEviction->value != 0.0f)
                          ? ((face % 3) + tick) % tex::kNumDebugTextures
                          : face;

        const int numVerts = EmitFace(s_faceVerts, kFaces[face], tess);
        vu1::DrawTriangles(mvp, tex::DebugTexture(variant), s_faceVerts, numVerts);
    }
}

} // namespace ps2::test
