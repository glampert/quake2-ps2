#pragma once
/* ================================================================================================
 * File: clip.h
 * Brief: Triangle clipping against the clip volume the VU1 microprogram judges.
 *
 *  The microprogram does not clip: a triangle with any vertex outside its clip
 *  volume is rejected whole (ADC bit), so geometry that can cross those planes
 *  must be cut here on the EE first. The six planes are the ones the VU judges -
 *  near and far (z is judged exactly) and the four guard-band side planes. The
 *  sides matter just as much as near: a polygon clipped at the near plane right
 *  under the camera lands at tiny w and enormous |x/w|, far outside any band the
 *  GS 12.4 coordinates could hold.
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
#include "ps2/renderer/vu1.h"

namespace ps2::clip {

constexpr int kNumClipPlanes = 6;

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

// Sutherland-Hodgman pass of a convex polygon against one plane. 'out' must
// hold inCount + 1 vertexes. Returns the clipped vertex count.
inline int ClipAgainstPlane(const ClipVertex * in, const int inCount, ClipVertex * out, const int plane)
{
    int outCount = 0;
    for (int i = 0; i < inCount; ++i)
    {
        const ClipVertex & a = in[i];
        const ClipVertex & b = in[(i + 1 == inCount) ? 0 : i + 1];

        if (a.d.f[plane] >= 0.0f)
        {
            out[outCount++] = a;
        }
        if ((a.d.f[plane] >= 0.0f) != (b.d.f[plane] >= 0.0f)) // Edge crosses the plane.
        {
            const float t = a.d.f[plane] / (a.d.f[plane] - b.d.f[plane]);
            ClipVertex & o = out[outCount++];
            math::LerpTo(o.pos,    a.pos,    b.pos,    t);
            math::LerpTo(o.st,     a.st,     b.st,     t);
            math::LerpTo(o.color,  a.color,  b.color,  t);
            math::LerpTo(o.d.q[0], a.d.q[0], b.d.q[0], t);
            math::LerpTo(o.d.q[1], a.d.q[1], b.d.q[1], t);
        }
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
// into 'scratch'. '*outWasClipped' distinguishes the two for draw statistics.
inline int ClipTriangle(ClipVertex (&corners)[3], const math::Mat4 & mvp, Scratch & scratch,
                        const ClipVertex ** outVerts, bool * outWasClipped)
{
    *outWasClipped = false;

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
    *outWasClipped = true;

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
