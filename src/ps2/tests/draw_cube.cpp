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
#include "ps2/renderer/frame_chain.h"
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

// One face's worth of vertices, refilled before each face draw. EE-side only, and only for the
// ps2_testcube_vulerp path, which reads it back to requantize: a buffer the DMA chain
// references cannot be reused per face any more, because a draw is not consumed before the
// next one is built - it is consumed at EndFrame, with all six faces still in the chain. The
// plain path emits straight into a span of the chain instead.
static vu1::DrawVertex s_faceVerts[kMaxFaceVerts];

// Requantizes s_faceVerts[0..numVerts) into the two streams the vulerp draw takes:
// 'chunks' gets the byte positions - byte = (coord + H) * 255 / (2H), the exact inverse of the
// frontv/backv scale and row-3 offset the draw sets up, both keyframes the same - and
// 'attribs' the per-vertex ST. Unlike an MD2 the cube has no baked attribute array to
// reference, so it builds one; both live in the chain.
//
// The lerped program computes its colour from a scalar shade term times a
// per-batch light, so the cube's per-vertex colour gradient cannot survive the
// trip: every vertex shades at 1.0 and the face's first corner becomes the
// batch light, which leaves the six faces differently coloured but flat. This
// is a bring-up scene for the transform and texturing, so that is enough -
// returns the light for the caller to hand to the draw.
math::Vec4 QuantizeFaceForVuLerp(vu1::LerpPosChunk * const chunks,
                                 vu1::LerpDrawAttrib * const attribs, int numVerts)
{
    constexpr float kQuant = 255.0f / (2.0f * kCubeHalfSize);

    for (int v = 0; v < numVerts; ++v)
    {
        const vu1::DrawVertex & src = s_faceVerts[v];

        const u32 bx = static_cast<u32>((src.x + kCubeHalfSize) * kQuant + 0.5f);
        const u32 by = static_cast<u32>((src.y + kCubeHalfSize) * kQuant + 0.5f);
        const u32 bz = static_cast<u32>((src.z + kCubeHalfSize) * kQuant + 0.5f);

        const u32 packed = bx | (by << 8) | (bz << 16); // 4th byte free for the shade

        vu1::LerpPosChunk & chunk = chunks[v / vu1::kMaxLerpVertsPerBatch];
        const int i = v % vu1::kMaxLerpVertsPerBatch;

        // The old frame's 4th byte carries the quantized shade term (shade * 128),
        // which the microprogram reads instead of an attribute lane - so 128 is a
        // shade of exactly 1.0 and the cube lights at face value. The current
        // frame's stays the MD2 normal index the VU never reads.
        chunk.pos[i].cur = packed;
        chunk.pos[i].old = packed | (128u << 24);

        attribs[v] = { 0u, src.s, src.t, src.q };
    }

    // Divided by 128 to match the shade byte above: the microprogram multiplies
    // this by shade * 128, so the light it wants is the GS colour over that scale.
    constexpr float kPerShadeUnit = 1.0f / 128.0f;

    const u32 rgba = s_faceVerts[0].rgba;
    return { static_cast<float>(rgba & 0xFFu)         * kPerShadeUnit,
             static_cast<float>((rgba >> 8) & 0xFFu)  * kPerShadeUnit,
             static_cast<float>((rgba >> 16) & 0xFFu) * kPerShadeUnit,
             static_cast<float>((rgba >> 24) & 0xFFu) };
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

    // This runs at the end of the frame, after the console and the HUD, so the 2D
    // section is open and holding a DMA tag - and the faces below allocate from the
    // chain, which cannot happen inside one. Closing it here rather than leaving it
    // to the draw is the same rule the batches follow: the 2D->3D boundary is where
    // the chain is claimed, not where it is submitted.
    gs::FlushPending2D();

    const int tick = Sys_Milliseconds() / 2000;
    for (int face = 0; face < 6; ++face)
    {
        const int variant = (s_testEviction->value != 0.0f)
                          ? ((face % 3) + tick) % tex::kNumDebugTextures
                          : face;

        // Both paths gather into the frame chain, like every other 3D path: the face has to
        // stay valid until EndFrame kicks, and the reservation has to cover the draw's tags as
        // well as the data so that nothing here can rewind what the previous face left behind.
        const int numVerts = tess * tess * 6;

        if (vuLerp)
        {
            EmitFace(s_faceVerts, kFaces[face], tess);

            const int numChunks = vu1::ChunkCount(numVerts, vu1::kMaxLerpVertsPerBatch);
            chain::Reserve(chain::CalcAllocCost<vu1::LerpPosChunk>(numChunks)
                         + chain::CalcAllocCost<vu1::LerpDrawAttrib>(numVerts)
                         + vu1::DrawLerpedTrianglesChainCost(numVerts));

            // Two exact allocations rather than one committable block: both sizes are
            // known before anything is written, so neither has to be cut back.
            vu1::LerpPosChunk   * const chunks  = chain::Alloc<vu1::LerpPosChunk>(numChunks);
            vu1::LerpDrawAttrib * const attribs = chain::Alloc<vu1::LerpDrawAttrib>(numVerts);

            const math::Vec4 shadeLight = QuantizeFaceForVuLerp(chunks, attribs, numVerts);

            vu1::DrawLerpedTriangles(mvpLerp, tex::DebugTexture(variant), frontv, backv,
                                     shadeLight, chunks, attribs, numVerts);
        }
        else
        {
            chain::Reserve(chain::CalcAllocCost<vu1::DrawVertex>(numVerts)
                         + vu1::DrawTrianglesChainCost(numVerts));

            vu1::DrawVertex * const verts = chain::Alloc<vu1::DrawVertex>(numVerts);
            EmitFace(verts, kFaces[face], tess);

            vu1::DrawTriangles(mvp, tex::DebugTexture(variant), verts, numVerts);
        }
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
