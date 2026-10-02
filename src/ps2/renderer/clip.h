#pragma once
/* ================================================================================================
 * File: clip.h
 * Brief: Triangle clipping against the clip volume the VU1 microprogram judges, on the EE.
 *
 *  The microprogram clips for itself now (vu_clip.i), against near and the four
 *  guard-band sides, and every world and model triangle is cut there. This EE
 *  clipper remains for sky alone, whose faces are single quads spanning ninety
 *  degrees - the heaviest possible user of the VU's clipper, which would need
 *  room reserved for the worst-case fan on every one.
 *
 *  It cuts six planes, the ones the VU judges - near and far (z is judged
 *  exactly) and the four guard-band sides. Far could go: across 606,931 clipped
 *  triangles in the perf demos it was never once straddled, which is why the VU
 *  clipper leaves it out. The sides matter just as much as near: a polygon
 *  clipped at the near plane right under the camera lands at tiny w and enormous
 *  |x/w|, far outside any band the GS 12.4 coordinates could hold.
 *
 *  Clipping runs in clip space - a vertex is inside while w-z >= 0 (near; also
 *  excludes everything behind the camera), w+z >= 0 (far), and G*w +/- x/y >= 0
 *  (sides, G = vu1::kGuardBandNdcLimit). Everything a vertex carries - position,
 *  UVs, its colour payload and the six distances themselves - is linear under a
 *  plane cut, so a split interpolates the whole ClipVertex as five quadword
 *  lerps on VU0, no scalar float math. Whole-triangle-inside is the common case
 *  and copies nothing.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/math/vec_mat.h"
#include "ps2/qwords.h"
#include "ps2/renderer/vu1.h"

namespace ps2::clip {

constexpr int kNumClipPlanes = 6;

// One bit per plane in the mask ClipTriangle reports, in the order SetClipDists
// writes the distances. Which planes a triangle actually straddles is what says
// whether a clipper handling only some of them could stand on its own - a cut
// against a plane whose corners are all inside cannot push a vertex out of it,
// but one against a plane they straddle leaves work behind.
constexpr u32 kPlaneNearBit  = 1u << 0;
constexpr u32 kPlaneFarBit   = 1u << 1;
constexpr u32 kPlaneSideBits = 0xFu << 2;

// Each pass can add one vertex, so a triangle survives as at most 3 + 6 corners.
constexpr int kMaxClippedVerts = 3 + kNumClipPlanes;

// Clip a hair early so the VU's judgement never flags a vertex this clipper
// just placed on the boundary (clip-space units, i.e. ~world units here).
constexpr float kClipEpsilon = 0.01f;

// Signed distances to the clip planes; >= 0 is inside. Held as two whole quadwords
// (six planes, two spare lanes) so the whole set interpolates with two vector lerps
// while the per-plane tests still read it as a plain float array.
union ClipDists
{
    math::Vec4 q[2];
    float f[8];
};

// Everything a clipped vertex carries, laid out as five quadwords so a plane cut
// interpolates it with five aligned vector lerps and no scalar float math at all.
struct ClipVertex
{
    math::Vec4 pos; // position in the space 'mvp' expects, w = 1
    math::Vec4 st;  // diffuse texture coords in xy; .z and .w are the caller's

    // A per-vertex colour payload, linearly interpolated across a cut like
    // everything else here. Its meaning is the caller's: the world passes use
    // it as a 0..1 luxel tint over a flat batch colour, the alias model path as
    // unpacked 0..255 RGBA, since MD2 shades per vertex. White (untinted) unless
    // the caller fills it in, so paths that do not need it can leave it alone;
    // it costs them one quadword store and rides through the lerps either way.
    math::Vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };

    ClipDists  d;
};
static_assert(sizeof(ClipVertex) == 80, "ClipVertex must be exactly five quadwords");

// The clipper's ping-pong buffers. A parameter rather than internal state so
// ClipTriangle stays a pure function - which is what lets a host harness drive
// it - and so the common, nothing-to-clip case touches neither buffer.
struct Scratch
{
    ClipVertex a[kMaxClippedVerts];
    ClipVertex b[kMaxClippedVerts];
};

// One Scratch for the whole renderer, which is all the renderer has ever needed.
//
// A Scratch's live range is a single ClipTriangle call plus the caller's loop over
// what it returned - *outVerts points into a or b - and clips never nest: a triangle
// is finished before the next one starts, and the world, sky and alias passes run in
// turn rather than interleaved.
//
namespace detail {
extern Scratch g_sharedScratch;
} // namespace detail

Q_ALWAYS_INLINE Scratch & SharedScratch()
{
    return detail::g_sharedScratch;
}

// Fills in a corner's six clip-plane distances from its position. The vertex
// draws untransformed and the VU applies 'mvp', so the judgement has to happen
// in that same clip space.
inline void SetClipDists(ClipVertex & c, const math::Mat4 & mvp)
{
    const math::Vec4 cs = math::Transform(c.pos, mvp);
    const float gw = vu1::kGuardBandNdcLimit * cs.w;
    c.d.f[0] = (cs.w - cs.z) - kClipEpsilon; // near (and behind-camera)
    c.d.f[1] = (cs.w + cs.z) - kClipEpsilon; // far
    c.d.f[2] = (gw - cs.x) - kClipEpsilon;   // guard band sides
    c.d.f[3] = (gw + cs.x) - kClipEpsilon;
    c.d.f[4] = (gw - cs.y) - kClipEpsilon;
    c.d.f[5] = (gw + cs.y) - kClipEpsilon;
    c.d.f[6] = 0.0f; // Spare lanes: never read as planes, but they ride
    c.d.f[7] = 0.0f; // along through the lerps, so keep them finite.
}

// out = a + t * (b - a) across all five quadwords of a vertex: five math::LerpTo calls in one,
// doing exactly their arithmetic. Apart, each LerpTo moves t into the vector unit again and runs
// one dependent chain - subtract, multiply, add, store - that waits out the FMAC latency at every
// step. Together the five subtracts are independent and issue back to back, and only the
// multiply-adds still queue, behind the one accumulator they share. t is taken from the FPU only
// after the loads, so the divide that produced it finishes behind them instead of stalling the move.
//
// Everything is loaded before anything is stored, so 'out' may alias 'a' or 'b'.
inline void LerpClipVertex(ClipVertex & out, const ClipVertex & a, const ClipVertex & b, const float t)
{
    [[maybe_unused]] u32 tmp;
    asm (
        "lqc2    $vf1,  0x00(%3)     \n\t" // vf1-vf5 = a
        "lqc2    $vf2,  0x10(%3)     \n\t"
        "lqc2    $vf3,  0x20(%3)     \n\t"
        "lqc2    $vf4,  0x30(%3)     \n\t"
        "lqc2    $vf5,  0x40(%3)     \n\t"
        "lqc2    $vf6,  0x00(%4)     \n\t" // vf6-vf10 = b
        "lqc2    $vf7,  0x10(%4)     \n\t"
        "lqc2    $vf8,  0x20(%4)     \n\t"
        "lqc2    $vf9,  0x30(%4)     \n\t"
        "lqc2    $vf10, 0x40(%4)     \n\t"
        "mfc1    %0,    %5           \n\t" // vf11.x = t; the first subtract fills the
        "vsub    $vf6,  $vf6,  $vf1  \n\t" // mfc1's delay slot (vf6-vf10 = b - a)
        "qmtc2   %0,    $vf11        \n\t"
        "vsub    $vf7,  $vf7,  $vf2  \n\t"
        "vsub    $vf8,  $vf8,  $vf3  \n\t"
        "vsub    $vf9,  $vf9,  $vf4  \n\t"
        "vsub    $vf10, $vf10, $vf5  \n\t"
        "vmulax  $ACC,  $vf6,  $vf11 \n\t" // vf6-vf10 = (b - a) * t + a * 1
        "vmaddw  $vf6,  $vf1,  $vf0  \n\t"
        "vmulax  $ACC,  $vf7,  $vf11 \n\t"
        "vmaddw  $vf7,  $vf2,  $vf0  \n\t"
        "vmulax  $ACC,  $vf8,  $vf11 \n\t"
        "vmaddw  $vf8,  $vf3,  $vf0  \n\t"
        "vmulax  $ACC,  $vf9,  $vf11 \n\t"
        "vmaddw  $vf9,  $vf4,  $vf0  \n\t"
        "vmulax  $ACC,  $vf10, $vf11 \n\t"
        "vmaddw  $vf10, $vf5,  $vf0  \n\t"
        "sqc2    $vf6,  0x00(%2)     \n\t"
        "sqc2    $vf7,  0x10(%2)     \n\t"
        "sqc2    $vf8,  0x20(%2)     \n\t"
        "sqc2    $vf9,  0x30(%2)     \n\t"
        "sqc2    $vf10, 0x40(%2)     \n\t"
        : "=&r" (tmp), "=m" (out)
        : "r" (&out), "r" (&a), "r" (&b), "f" (t), "m" (a), "m" (b)
    );
}

// Sutherland-Hodgman pass of a convex polygon against one plane. 'out' must
// hold inCount + 1 vertexes. Returns the clipped vertex count.
//
// A vertex is tested as the 'b' of the edge it ends, and the verdict and
// distance carry over to the next edge's 'a' rather than being worked out
// again - which would mean reloading 'a' too: the copy ahead of the cut stores
// to 'out', and the compiler cannot prove that misses 'in'.
inline int ClipAgainstPlane(const ClipVertex * in, const int inCount, ClipVertex * out, const int plane)
{
    int   outCount = 0;
    float aDist    = in[0].d.f[plane];
    bool  aInside  = (aDist >= 0.0f);
    for (int i = 0; i < inCount; ++i)
    {
        const ClipVertex & a = in[i];
        const ClipVertex & b = in[(i + 1 == inCount) ? 0 : i + 1];

        const float bDist   = b.d.f[plane];
        const bool  bInside = (bDist >= 0.0f);

        if (aInside)
        {
            // Five qwords: a plain assignment is a loop of ld/sd pairs (see qwords.h).
            CopyQwords<sizeof(ClipVertex) / 16>(&out[outCount++], &a);
        }
        if (aInside != bInside) // Edge crosses the plane.
        {
            LerpClipVertex(out[outCount++], a, b, aDist / (aDist - bDist));
        }

        aDist   = bDist;
        aInside = bInside;
    }
    return outCount;
}

// Clips one triangle against the whole volume. The corners arrive with their
// position, UVs and colour set; their clip distances are computed here.
//
// Returns the survivor count - 0 when the triangle is entirely outside, else 3
// to kMaxClippedVerts corners of a convex polygon for the caller to fan
// triangulate - and points '*outVerts' at them: 'corners' itself when nothing
// needed cutting (the common case, which copies no vertices at all), otherwise
// into 'scratch'. '*outPlanesCrossed' reports which planes it straddled, and is
// zero when nothing needed cutting - which is what distinguishes the two for
// draw statistics.
inline int ClipTriangle(ClipVertex (&corners)[3], const math::Mat4 & mvp, Scratch & scratch,
                        const ClipVertex ** outVerts, u32 * outPlanesCrossed)
{
    *outPlanesCrossed = 0;

    int insidePerPlane[kNumClipPlanes] = {};
    for (ClipVertex & c : corners)
    {
        SetClipDists(c, mvp);
        for (int p = 0; p < kNumClipPlanes; ++p)
        {
            insidePerPlane[p] += (c.d.f[p] >= 0.0f);
        }
    }

    int insideTotal = 0;
    bool outsideAny = false;
    for (int p = 0; p < kNumClipPlanes; ++p)
    {
        insideTotal += insidePerPlane[p];
        outsideAny  |= (insidePerPlane[p] == 0);
    }

    if (outsideAny)
    {
        return 0; // All three corners outside one plane: entirely out.
    }

    if (insideTotal == 3 * kNumClipPlanes)
    {
        *outVerts = corners; // Whole triangle inside: the common case, no clipping.
        return 3;
    }

    // Straddling triangle: clip against every plane in turn. Each pass
    // can add one vertex (3 -> at most kMaxClippedVerts).
    //
    // Which planes those are is reported to the caller. Zero corners inside
    // would read as a straddle here too, but 'outsideAny' has already returned.
    for (int p = 0; p < kNumClipPlanes; ++p)
    {
        if (insidePerPlane[p] != 3)
        {
            *outPlanesCrossed |= (1u << p);
        }
    }

    const ClipVertex * in = corners;
    ClipVertex * out = scratch.a;
    int count = 3;
    for (int p = 0; p < kNumClipPlanes && count >= 3; ++p)
    {
        if (insidePerPlane[p] == 3)
        {
            continue; // All corners inside this plane: every clipped
                      // vertex is a convex mix of them, so none can
                      // cross it either.
        }
        count = ClipAgainstPlane(in, count, out, p);
        in  = out;
        out = (out == scratch.a) ? scratch.b : scratch.a;
    }

    if (count < 3)
    {
        return 0;
    }

    *outVerts = in;
    return count;
}

} // namespace ps2::clip
