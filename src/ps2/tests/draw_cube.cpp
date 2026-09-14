/* ================================================================================================
 * File: draw_cube.cpp
 * Brief: Debug scene for the VU1 3D bring-up. See draw_cube.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/common.h"
#include "ps2/tests/draw_cube.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_system.h"
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

// Largest ps2_testcube_tess value: per-axis quads per face; caps a face's vertex count.
constexpr int kMaxTess = 8;
constexpr int kMaxFaceVerts = kMaxTess * kMaxTess * 6;

// Per-vertex attributes for the ps2_testcube_vulerp path, standing in for the baked vertex array
// an MD2 hands rs::LerpStream: every face tessellates the same grid, so all six draws point at
// this one copy and the stream gathers nothing but positions, exactly as it does for a model.
//
// Static rather than gathered into the command buffer for the same reason a model's vertices are:
// the chain only ever references it. Rebuilt at the top of each cube, which is safe because
// rs::BeginFrame fences the previous frame before anything of this one is built.
static vu1::LerpDrawAttrib s_faceAttribs[kMaxFaceVerts];

// Walks the tess x tess grid of quads covering a face and hands 'emitTriangle' the (u, v) of
// each triangle's three corners, wound like the face's own. tess^2 * 2 triangles in all.
//
// A template rather than a "where does vertex n land" helper so that the grid arithmetic stays
// where it belongs, once per cell: the divide and the two corner coordinates are the same for all
// six vertices of a cell, and working them out per vertex costs an integer division apiece.
template<typename EmitTriangle>
void ForEachFaceTriangle(const int tess, EmitTriangle && emitTriangle)
{
    const float step = 1.0f / static_cast<float>(tess);

    for (int cy = 0; cy < tess; ++cy)
    {
        for (int cx = 0; cx < tess; ++cx)
        {
            const float u0 = static_cast<float>(cx) * step;
            const float v0 = static_cast<float>(cy) * step;
            const float u1 = u0 + step;
            const float v1 = v0 + step;

            emitTriangle(u0, v0, u1, v0, u1, v1);
            emitTriangle(u0, v0, u1, v1, u0, v1);
        }
    }
}

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

// Gathers a tess x tess grid of quads (two triangles each) covering the face into the stream.
// Tess 1 is the plain 2-triangle face; 5+ exceeds vu1::kMaxVertsPerBatch and so exercises the
// chunked submission path behind rs::TriangleStream::Flush.
void EmitFace(rs::TriangleStream & trisStream, const int corners[4], const int tess)
{
    ForEachFaceTriangle(tess, [&](float ua, float va, float ub, float vb, float uc, float vc)
    {
        vu1::DrawVertex * const tri = trisStream.PushTriangle();
        EmitVertex(tri[0], corners, ua, va);
        EmitVertex(tri[1], corners, ub, vb);
        EmitVertex(tri[2], corners, uc, vc);
    });
}

// One vertex's position, quantized into the two keyframe byte streams the lerped program reads:
// byte = (coord + H) * 255 / (2H), the exact inverse of the frontv/backv scale and the row-3
// offset DrawRotatingCube sets up, both keyframes carrying the same bytes.
void QuantizeVertex(vu1::LerpVertexBytes & dst, const int corners[4], const float u, const float v)
{
    constexpr float kQuant = 255.0f / (2.0f * kCubeHalfSize);

    vu1::DrawVertex vert;
    EmitVertex(vert, corners, u, v);

    const u32 bx = static_cast<u32>((vert.x + kCubeHalfSize) * kQuant + 0.5f);
    const u32 by = static_cast<u32>((vert.y + kCubeHalfSize) * kQuant + 0.5f);
    const u32 bz = static_cast<u32>((vert.z + kCubeHalfSize) * kQuant + 0.5f);

    const u32 packed = bx | (by << 8) | (bz << 16); // 4th byte free for the shade

    // The old frame's 4th byte carries the quantized shade term (shade * 128), which the
    // microprogram reads instead of an attribute lane - so 128 is a shade of exactly 1.0 and the
    // cube lights at face value. The current frame's stays the MD2 normal index the VU never
    // reads.
    dst.cur = packed;
    dst.old = packed | (128u << 24);
}

// The same grid through the MD2 keyframe path: only the quantized positions are gathered, and
// the attributes come from s_faceAttribs.
void EmitFaceLerped(rs::LerpStream & lerpStream, const int corners[4], const int tess)
{
    ForEachFaceTriangle(tess, [&](float ua, float va, float ub, float vb, float uc, float vc)
    {
        vu1::LerpVertexBytes * const tri = lerpStream.PushTriangle();
        QuantizeVertex(tri[0], corners, ua, va);
        QuantizeVertex(tri[1], corners, ub, vb);
        QuantizeVertex(tri[2], corners, uc, vc);
    });
}

// Fills s_faceAttribs for a tess x tess grid. Lane 0 is the keyframe index a model would have
// there and the microprogram never reads it.
void BuildFaceAttribs(const int tess)
{
    vu1::LerpDrawAttrib * attrib = s_faceAttribs;

    ForEachFaceTriangle(tess, [&](float ua, float va, float ub, float vb, float uc, float vc)
    {
        *attrib++ = { 0u, ua, va, 1.0f };
        *attrib++ = { 0u, ub, vb, 1.0f };
        *attrib++ = { 0u, uc, vc, 1.0f };
    });
}

// The batch light for a face, which is all of its colour that survives the lerped path: that
// program computes its colour from a scalar shade term times one per-batch light, so the cube's
// per-vertex gradient cannot come through. Every vertex shades at 1.0 and the face's first
// corner becomes the light, which leaves the six faces differently coloured but flat. This is a
// bring-up scene for the transform and texturing, so that is enough.
//
// Divided by 128 to match the shade byte EmitFaceLerped writes: the microprogram multiplies this
// by shade * 128, so the light it wants is the GS colour over that scale. The .w is the vertex
// alpha in GS units and keeps its own scale.
math::Vec4 FaceShadeLight(const int corners[4])
{
    constexpr float kPerShadeUnit = 1.0f / 128.0f;

    // Emitted rather than read off kCorners, so the debug colour override in EmitVertex reaches
    // this path too.
    vu1::DrawVertex corner;
    EmitVertex(corner, corners, 0.0f, 0.0f);

    const u32 rgba = corner.rgba;
    return { static_cast<float>(rgba & 0xFFu)         * kPerShadeUnit,
             static_cast<float>((rgba >> 8) & 0xFFu)  * kPerShadeUnit,
             static_cast<float>((rgba >> 16) & 0xFFu) * kPerShadeUnit,
             static_cast<float>((rgba >> 24) & 0xFFu) };
}

// Which debug texture a face samples. With ps2_testcube_vram_tex_eviction on the faces share a
// 3-variant window that slides every 2 seconds instead of taking one each.
int FaceVariant(const int face, const int tick, const cvar_t * const evictionCvar)
{
    return (evictionCvar->value != 0.0f) ? (((face % 3) + tick) % tex::kNumDebugTextures) : face;
}

} // namespace

void DrawRotatingCube()
{
    static const cvar_t * s_testCube = Cvar_Get("ps2_testcube", "0", 0);
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
                                             static_cast<float>(gs::Width()),
                                             static_cast<float>(gs::Height()),
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

    // With ps2_testcube_vulerp on, the faces render through the MD2 keyframe
    // path instead: positions quantized to MD2-style bytes and decoded back
    // on VU1, with the decode scale split between the two keyframe streams
    // (both carry the same bytes, so the split must cancel out) and the
    // decode offset folded into the MVP's row 3 exactly as render_md2 folds
    // the lerp's 'move' term. Same cube, give or take 8-bit quantization -
    // a pixel-comparable smoke test of the V4_8 unpack, the itof0 conversion
    // and the lerp, with no model data in the loop.
    static const cvar_t * s_testVuLerp = Cvar_Get("ps2_testcube_vulerp", "0", 0);
    const bool vuLerp = (s_testVuLerp->value != 0.0f);

    Mat4 mvpLerp = {};
    Vec3 frontv  = {};
    Vec3 backv   = {};
    if (vuLerp)
    {
        // A moving split so both streams and the add are exercised; the sum
        // is constant, so any wobble or shimmer is a lerp-path bug.
        const float backlerp = 0.5f + 0.5f * Sinf(t);
        const float decode   = (2.0f * kCubeHalfSize) / 255.0f;
        frontv = { decode * (1.0f - backlerp), decode * (1.0f - backlerp), decode * (1.0f - backlerp) };
        backv  = { decode * backlerp, decode * backlerp, decode * backlerp };

        const Vec4 row3 = Transform(Vec4{ -kCubeHalfSize, -kCubeHalfSize, -kCubeHalfSize, 1.0f }, mvp);
        mvpLerp = mvp;
        mvpLerp.m[3][0] = row3.x;
        mvpLerp.m[3][1] = row3.y;
        mvpLerp.m[3][2] = row3.z;
        mvpLerp.m[3][3] = row3.w;
    }

    // Both paths gather through the same streams the renderer's own passes use, so this scene
    // exercises the real submission path and knows nothing of the chain budget. The stream also
    // closes the pending 2D section when it claims - this runs at the end of the frame, after the
    // console and the HUD, so one is open and holding a DMA tag that an allocation cannot land
    // inside.
    //
    // One stream for the whole cube, flushed per face by the texture change: six batches, as
    // before. A face fits one flush cycle whatever the tessellation, so its vertices go out in
    // one batch and only the draw itself chunks them.
    const int tick     = Sys_Milliseconds() / 2000;
    const int numVerts = tess * tess * 6;

    if (vuLerp)
    {
        BuildFaceAttribs(tess);

        auto lerpStream = rs::Begin<rs::LerpStream>(kMaxFaceVerts);

        for (int face = 0; face < 6; ++face)
        {
            lerpStream.SetTransform(mvpLerp);
            lerpStream.SetTexture(tex::DebugTexture(FaceVariant(face, tick, s_testEviction)));
            lerpStream.SetLerpParams(frontv, backv, FaceShadeLight(kFaces[face]));
            lerpStream.SetAttribSource(s_faceAttribs);

            lerpStream.BeginVerts(numVerts);
            EmitFaceLerped(lerpStream, kFaces[face], tess);
        }

        rs::Submit(lerpStream);
    }
    else
    {
        auto trisStream = rs::Begin<rs::TriangleStream>(kMaxFaceVerts);

        for (int face = 0; face < 6; ++face)
        {
            trisStream.SetTransform(mvp);
            trisStream.SetTexture(tex::DebugTexture(FaceVariant(face, tick, s_testEviction)));

            trisStream.BeginVerts(numVerts);
            EmitFace(trisStream, kFaces[face], tess);
        }

        rs::Submit(trisStream);
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
