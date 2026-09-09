/* ================================================================================================
 * File: render_sky.cpp
 * Brief: Skybox rendering, ported from ref_gl's gl_warp.c.
 *
 *  Quake's sky is not geometry the map ships. SURF_SKY faces are holes: they
 *  are never rasterized, and all the world pass does with one is project it
 *  onto an imaginary cube centred on the eye and remember which part of which
 *  cube face it covered (AddSurface). Once the world is down, DrawSkyBox draws
 *  exactly those parts - at most six textured quads, usually one or two, and
 *  nothing at all indoors.
 *
 *  Projecting a polygon onto the cube is the fiddly half: a sky face large
 *  enough to span a cube edge belongs to two faces at once, so ClipSkyPolygon
 *  first cuts it along the six diagonal planes through the origin that separate
 *  the cube's faces, and only the pieces that come out the far side get
 *  projected. Skipping that step and binning each polygon by its centroid
 *  leaves a wedge of sky untextured wherever one straddles an edge.
 *
 *  The cube is drawn at a finite 2300 units, as ref_gl draws it, so the world's
 *  depth values reject the parts of it hidden behind geometry - the sky costs
 *  fill only where it is actually visible. It writes no depth of its own
 *  (vu1::DrawFlags::NoDepthWrite), which is where this departs from ref_gl:
 *  there the sky occluded anything drawn later past 2300 units, which on the
 *  larger outdoor maps eats distant entities and rail trails.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/render_sky.h"
#include "ps2/renderer/render_view.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/model.h"
#include "ps2/renderer/clip.h"
#include "ps2/renderer/batch.h"
#include "ps2/renderer/vu1.h"
#include "ps2/math/vec_mat.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ps2::sky {
namespace {

// ------------------------------------------------------------------------------------------------
// Cvars and constants
// ------------------------------------------------------------------------------------------------

static const cvar_t * s_skipSky    = nullptr;
static const cvar_t * s_fullBounds = nullptr;
static const cvar_t * s_skyMip     = nullptr;

// Half-extent of the sky cube, in world units (ref_gl's magic 2300). The
// corners reach 2300*sqrt(3) = 3983, just inside the world projection's 4096
// far plane - which is the constraint that picked the number.
constexpr float kSkyDistance = 2300.0f;

// Vertex colour for the sky: GS modulate 128 = texels unchanged, alpha 0x80 = 1.0.
constexpr u32 kSkyColor = vu1::PackColorRGBA(128, 128, 128, 0x80);

// The six faces, in the order their suffixes name them.
constexpr int kNumSkyFaces = 6;

// ref_gl's ON_EPSILON, the plane-side slack of the cube-face split.
constexpr float kOnPlaneEpsilon = 0.1f;

// Which side of a splitting plane a vertex fell on (ref_gl's SIDE_*, which
// live in the renderers' own headers rather than the shared game ones).
enum PlaneSide : u8 { kSideFront, kSideBack, kSideOn };

// Room for one sky polygon mid-split. Stock maps top out at 20 vertices on a
// single sky face; each of the six planes can add one more, and ClipSkyPolygon
// appends a wrap-around copy of the first vertex past the end (as ref_gl does,
// hence its own MAX_CLIP_VERTS-2 guard).
constexpr int kMaxSkyClipVerts = 40;
constexpr int kSkyClipStages   = 6;

// ------------------------------------------------------------------------------------------------
// ref_gl tables (gl_warp.c), kept verbatim
// ------------------------------------------------------------------------------------------------

// The six diagonal planes through the origin that separate the cube's faces.
// Un-normalized on purpose: only the sign of the dot product and the ratio of
// two distances matter, and neither cares about scale.
static const vec3_t s_skyClip[kNumSkyFaces] = {
    {  1.0f,  1.0f, 0.0f },
    {  1.0f, -1.0f, 0.0f },
    {  0.0f, -1.0f, 1.0f },
    {  0.0f,  1.0f, 1.0f },
    {  1.0f,  0.0f, 1.0f },
    { -1.0f,  0.0f, 1.0f }
};

// Face-local (s, t, dist) -> world direction, and its inverse. An entry k means
// "component |k|-1, negated when k is negative", where 1/2/3 stand for s, t and
// the face's own axis.
static const int s_stToVec[kNumSkyFaces][3] = {
    {  3, -1,  2 },
    { -3,  1,  2 },
    {  1,  3,  2 },
    { -1, -3,  2 },
    { -2, -1,  3 }, // 0 degrees yaw, look straight up
    {  2, -1, -3 }  // look straight down
};

// s = [0]/[2], t = [1]/[2]
static const int s_vecToSt[kNumSkyFaces][3] = {
    { -2,  3,  1 },
    {  2,  3, -1 },
    {  1,  3,  2 },
    { -1,  3, -2 },
    { -2, -1,  3 },
    { -2,  1, -3 }
};

// Cube face index -> index into s_faces[]. The cube's axis order and the order
// the suffixes load in are not the same; this is applied at draw time only.
static const int s_skyTexOrder[kNumSkyFaces] = { 0, 2, 1, 3, 4, 5 };

// 3D Studio environment map suffixes, the order s_faces[] is loaded in.
static const char * const s_suffixes[kNumSkyFaces] = { "rt", "bk", "lf", "ft", "up", "dn" };

// ------------------------------------------------------------------------------------------------
// Sky state
// ------------------------------------------------------------------------------------------------

static char s_skyName[MAX_QPATH] = {};
static float s_skyRotate = 0.0f;
static vec3_t s_skyAxis  = {}; // Normalized; zero when there is no rotation.

static const tex::Texture * s_faces[kNumSkyFaces] = {};

// The visible extent of each cube face this frame, in face-local [-1, 1] ST.
// An empty interval (mins > maxs) means no sky surface reached that face.
static float s_skyMins[2][kNumSkyFaces];
static float s_skyMaxs[2][kNumSkyFaces];

// Working buffers for ClipSkyPolygon, indexed by the stage that produced them.
// File-level rather than stack: at ~2.3 KB a frame, six deep and nested inside
// the world walk's own recursion, this was the largest stack consumer in the
// renderer for no reason. Safe because a stage's output is read only by the
// stage below it, and the first child's whole subtree finishes before the
// second child starts.
static vec3_t s_skyClipVerts[kSkyClipStages][2][kMaxSkyClipVerts];

// Triangle gather buffer, flushed per face (referenced in place by DMA). Six
// faces of two triangles, each of which can leave the clipper as a 9-gon, so
// 7 triangles: 42 verts per face is the true ceiling.
constexpr int kBatchMaxVerts = 3 * 64;
static batch::TriangleBatch<kBatchMaxVerts> s_batch;

// The sky draws at a finite distance so the world can occlude it, and must not
// occlude anything drawn after it in return - hence the masked depth writes.
constexpr vu1::DrawFlags kSkyDrawFlags = vu1::DrawFlags::NoDepthWrite;

// ------------------------------------------------------------------------------------------------
// Bounds accumulation (ref_gl's DrawSkyPolygon / ClipSkyPolygon)
// ------------------------------------------------------------------------------------------------

// Picks the cube face a fully split polygon lands on and grows that face's ST
// bounds to cover it. Draws nothing, despite ref_gl's name for it.
void AccumulateSkyPolygon(const int nump, const vec3_t * vecs)
{
    // The polygon is on one face by now, so the sum of its vertices points at
    // that face; the dominant component picks the axis and its sign the side.
    vec3_t v = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < nump; ++i)
    {
        VectorAdd(vecs[i], v, v);
    }

    const vec3_t av = { std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2]) };

    int axis;
    if (av[0] > av[1] && av[0] > av[2])
    {
        axis = (v[0] < 0.0f) ? 1 : 0;
    }
    else if (av[1] > av[2] && av[1] > av[0])
    {
        axis = (v[1] < 0.0f) ? 3 : 2;
    }
    else
    {
        axis = (v[2] < 0.0f) ? 5 : 4;
    }

    // Project onto the face and grow its bounds.
    for (int i = 0; i < nump; ++i)
    {
        const float * const vec = vecs[i];

        int j = s_vecToSt[axis][2];
        const float dv = (j > 0) ? vec[j - 1] : -vec[-j - 1];
        if (dv < 0.001f)
        {
            continue; // Don't divide by zero.
        }

        j = s_vecToSt[axis][0];
        const float s = (j < 0) ? -vec[-j - 1] / dv : vec[j - 1] / dv;

        j = s_vecToSt[axis][1];
        const float t = (j < 0) ? -vec[-j - 1] / dv : vec[j - 1] / dv;

        if (s < s_skyMins[0][axis]) { s_skyMins[0][axis] = s; }
        if (t < s_skyMins[1][axis]) { s_skyMins[1][axis] = t; }
        if (s > s_skyMaxs[0][axis]) { s_skyMaxs[0][axis] = s; }
        if (t > s_skyMaxs[1][axis]) { s_skyMaxs[1][axis] = t; }
    }
}

// Splits an eye-relative polygon along the cube's face-dividing planes, one
// stage per plane, until every piece belongs to exactly one face.
//
// 'vecs' must have room for one vertex past 'nump': the split writes a
// wrap-around copy of the first vertex there so the edge loop can read i+1
// without a modulo.
void ClipSkyPolygon(const int nump, vec3_t * vecs, const int stage)
{
    if (nump > kMaxSkyClipVerts - 2)
    {
        Com_DPrintf("WARNING: ClipSkyPolygon overflow (%d verts), sky polygon dropped.\n", nump);
        return;
    }
    if (stage == kSkyClipStages)
    {
        AccumulateSkyPolygon(nump, vecs); // Fully split: it is on one face now.
        return;
    }

    const float * const norm = s_skyClip[stage];

    float dists[kMaxSkyClipVerts];
    PlaneSide sides[kMaxSkyClipVerts];

    bool front = false;
    bool back  = false;

    for (int i = 0; i < nump; ++i)
    {
        const float d = DotProduct(vecs[i], norm);
        if (d > kOnPlaneEpsilon)
        {
            front    = true;
            sides[i] = kSideFront;
        }
        else if (d < -kOnPlaneEpsilon)
        {
            back     = true;
            sides[i] = kSideBack;
        }
        else
        {
            sides[i] = kSideOn;
        }
        dists[i] = d;
    }

    if (!front || !back)
    {
        ClipSkyPolygon(nump, vecs, stage + 1); // Entirely on one side; nothing to cut.
        return;
    }

    // Close the edge loop.
    sides[nump] = sides[0];
    dists[nump] = dists[0];
    VectorCopy(vecs[0], vecs[nump]);

    vec3_t (&newv)[2][kMaxSkyClipVerts] = s_skyClipVerts[stage];
    int newc[2] = { 0, 0 };

    for (int i = 0; i < nump; ++i)
    {
        const float * const v = vecs[i];

        // A convex polygon cut by a plane keeps at most nump+1 vertices per
        // side, and BSP faces are convex, so neither side can reach the end of
        // its buffer. Asserted rather than assumed: these are fixed-size and
        // the writes below happen before the recursion re-checks the count.
        PS2_AssertMsg(newc[0] + 2 <= kMaxSkyClipVerts && newc[1] + 2 <= kMaxSkyClipVerts,
                      "Sky polygon split overflowed its buffer!");

        switch (sides[i])
        {
        case kSideFront:
            VectorCopy(v, newv[0][newc[0]]);
            newc[0]++;
            break;
        case kSideBack:
            VectorCopy(v, newv[1][newc[1]]);
            newc[1]++;
            break;
        case kSideOn:
            VectorCopy(v, newv[0][newc[0]]);
            newc[0]++;
            VectorCopy(v, newv[1][newc[1]]);
            newc[1]++;
            break;
        default:
            break;
        }

        if (sides[i] == kSideOn || sides[i + 1] == kSideOn || sides[i + 1] == sides[i])
        {
            continue; // This edge doesn't cross the plane.
        }

        const float frac = dists[i] / (dists[i] - dists[i + 1]);
        for (int j = 0; j < 3; ++j)
        {
            const float e = v[j] + frac * (vecs[i + 1][j] - v[j]);
            newv[0][newc[0]][j] = e;
            newv[1][newc[1]][j] = e;
        }
        newc[0]++;
        newc[1]++;
    }

    ClipSkyPolygon(newc[0], newv[0], stage + 1);
    ClipSkyPolygon(newc[1], newv[1], stage + 1);
}

// ------------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------------

// Clips one sky triangle against the volume the VU judges and appends the
// survivors to the gather buffer. A cube face is a single quad spanning 90
// degrees, so unlike most geometry here this is expected to clip, not
// exceptional: at the world projection's scale the guard band runs out around
// 79 degrees off-axis.
//
// The sky is flat-shaded: every vertex takes the same colour, whatever the
// clipper left behind.
Q_ALWAYS_INLINE void GatherSkyTriangle(clip::ClipVertex (&corners)[3], const math::Mat4 & viewProj, const tex::Texture & texture)
{
    s_batch.GatherTriangle(corners, viewProj, texture, kSkyDrawFlags,
                           [](const clip::ClipVertex &) { return kSkyColor; });
}

// One corner of a cube face: face-local ST in [-1, 1] to a world-space vertex
// on the cube around 'eye', with the texture coordinates that go with it.
//
// The ST inset is not cosmetic. Texture wrapping is REPEAT for the whole GS
// environment and sky faces filter bilinearly, so a coordinate landing exactly
// on 0 or 1 blends with the texel that wrapped around from the opposite edge
// and draws a bright seam along the cube's edges. Half a texel in from each
// side is enough to keep the filter kernel inside the image; the width comes
// from the face itself so it stays right whatever ps2_skymip did to it.
clip::ClipVertex MakeSkyVertex(float s, float t, const int axis,
                               const tex::Texture & face,
                               const vec3_t eye, const float rotateDegrees)
{
    const vec3_t b = { s * kSkyDistance, t * kSkyDistance, kSkyDistance };

    vec3_t v;
    for (int j = 0; j < 3; ++j)
    {
        const int k = s_stToVec[axis][j];
        v[j] = (k < 0) ? -b[-k - 1] : b[k - 1];
    }

    if (rotateDegrees != 0.0f)
    {
        vec3_t rotated;
        RotatePointAroundVector(rotated, s_skyAxis, v, rotateDegrees);
        VectorCopy(rotated, v);
    }

    const float texelS = 0.5f / static_cast<float>(face.width);
    const float texelT = 0.5f / static_cast<float>(face.height);

    s = (s + 1.0f) * 0.5f;
    t = (t + 1.0f) * 0.5f;

    if (s < texelS) { s = texelS; }
    else if (s > 1.0f - texelS) { s = 1.0f - texelS; }

    if (t < texelT) { t = texelT; }
    else if (t > 1.0f - texelT) { t = 1.0f - texelT; }

    clip::ClipVertex out;
    out.pos = { eye[0] + v[0], eye[1] + v[1], eye[2] + v[2], 1.0f };
    out.st  = { s, 1.0f - t, 0.0f, 0.0f };
    return out;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void InitSkyRendering()
{
    s_skipSky    = Cvar_Get("ps2_skip_sky",        "0", 0); // Debug: drop the sky pass entirely.
    s_fullBounds = Cvar_Get("ps2_sky_full_bounds", "0", 0); // Debug: draw all six faces whole, ignoring what is visible.
    s_skyMip     = Cvar_Get("ps2_skymip",          "0", 0); // Load sky faces at half resolution (ref_gl's gl_skymip).

    ClearBounds();
}

void BeginRegistration()
{
    // The level's Sky textures are about to be swept up by
    // tex::EndRegistration() if this map doesn't ask for them again.
    for (int i = 0; i < kNumSkyFaces; ++i)
    {
        s_faces[i] = nullptr;
    }
    s_skyName[0] = '\0';
    s_skyRotate  = 0.0f;
    VectorClear(s_skyAxis);
}

void SetSky(const char * name, const float rotate, const vec3_t axis)
{
    BeginRegistration(); // Drop whatever the last map had.

    if (name == nullptr || name[0] == '\0')
    {
        return; // No sky for this map.
    }

    std::snprintf(s_skyName, sizeof(s_skyName), "%s", name);
    s_skyRotate = rotate;

    // RotatePointAroundVector transposes its basis to invert it, which is only
    // the inverse for a unit axis - unlike glRotatef, it does not normalize
    // what it is handed. The maps that rotate pass "0 1 1" and "1 1 0", and
    // every map that doesn't still sends an all-zero axis (g_spawn.c writes
    // the key unconditionally), which would normalize to garbage.
    VectorCopy(axis, s_skyAxis);
    if (VectorNormalize(s_skyAxis) == 0.0f)
    {
        s_skyRotate = 0.0f;
    }

    // A rotating sky can never bound itself to a couple of faces - it forces
    // all six resident at once - so it takes the smaller ones, as ref_gl's
    // "chop down rotating skies for less memory" did.
    tex::SetSkyDownsample(s_skyMip->value != 0.0f || s_skyRotate != 0.0f);

    char path[MAX_QPATH];
    for (int i = 0; i < kNumSkyFaces; ++i)
    {
        std::snprintf(path, sizeof(path), "env/%s%s.pcx", s_skyName, s_suffixes[i]);

        s_faces[i] = tex::Find(path, tex::ImageType::Sky);
        if (s_faces[i] == nullptr)
        {
            Com_DPrintf("WARNING: Missing sky face '%s'!\n", path);
            s_faces[i] = &tex::DebugTexture();
        }
    }

    tex::SetSkyDownsample(false);
}

void ClearBounds()
{
    for (int i = 0; i < kNumSkyFaces; ++i)
    {
        s_skyMins[0][i] = s_skyMins[1][i] =  9999.0f;
        s_skyMaxs[0][i] = s_skyMaxs[1][i] = -9999.0f;
    }
}

void AddSurface(const mod::ModelSurface & surf, const vec3_t viewOrigin)
{
    if (s_faces[0] == nullptr)
    {
        return; // No sky loaded; the surface stays a hole.
    }

    // The split works in eye-relative space, but with the world's axes: what
    // it decides is which way the sky is from the player, and the cube is
    // never rotated with the view.
    for (const mod::ModelPoly * poly = surf.polys; poly != nullptr; poly = poly->next)
    {
        if (poly->numVerts > kMaxSkyClipVerts - 2)
        {
            continue; // Guarded here too so the recursion never has to unwind.
        }

        vec3_t verts[kMaxSkyClipVerts];
        for (int i = 0; i < poly->numVerts; ++i)
        {
            const math::Vec3 & p = poly->vertexes[i].position;
            verts[i][0] = p.x - viewOrigin[0];
            verts[i][1] = p.y - viewOrigin[1];
            verts[i][2] = p.z - viewOrigin[2];
        }

        ClipSkyPolygon(poly->numVerts, verts, 0);
    }
}

void DrawSkyBox(const refdef_t & viewDef, const math::Mat4 & viewProj)
{
    if (s_faces[0] == nullptr || s_skipSky->value != 0.0f)
    {
        return;
    }

    const bool fullBounds = (s_fullBounds->value != 0.0f);

    // Is any sky visible at all? Indoor maps answer no here and pay nothing
    // else. Checked before the rotating-sky override below, or a rotating map
    // would draw its sky from inside a sealed room.
    if (!fullBounds)
    {
        int i = 0;
        for (; i < kNumSkyFaces; ++i)
        {
            if (s_skyMins[0][i] < s_skyMaxs[0][i] && s_skyMins[1][i] < s_skyMaxs[1][i])
            {
                break;
            }
        }
        if (i == kNumSkyFaces)
        {
            return;
        }
    }

    // Degrees, not radians: RotatePointAroundVector takes degrees, and
    // skyrotate is degrees per second (ref_gl passes the same to glRotatef).
    const float rotateDegrees = viewDef.time * s_skyRotate;

    for (int i = 0; i < kNumSkyFaces; ++i)
    {
        if (s_skyRotate != 0.0f || fullBounds)
        {
            // A rotated cube's bounds were accumulated in the unrotated frame,
            // so they no longer say where the sky is. Draw the faces whole.
            s_skyMins[0][i] = s_skyMins[1][i] = -1.0f;
            s_skyMaxs[0][i] = s_skyMaxs[1][i] =  1.0f;
        }

        if (s_skyMins[0][i] >= s_skyMaxs[0][i] || s_skyMins[1][i] >= s_skyMaxs[1][i])
        {
            continue; // Nothing of this face is visible.
        }

        const tex::Texture & face = *s_faces[s_skyTexOrder[i]];

        // The face's visible rectangle, wound as a quad: (min,min), (min,max),
        // (max,max), (max,min).
        const clip::ClipVertex quad[4] = {
            MakeSkyVertex(s_skyMins[0][i], s_skyMins[1][i], i, face, viewDef.vieworg, rotateDegrees),
            MakeSkyVertex(s_skyMins[0][i], s_skyMaxs[1][i], i, face, viewDef.vieworg, rotateDegrees),
            MakeSkyVertex(s_skyMaxs[0][i], s_skyMaxs[1][i], i, face, viewDef.vieworg, rotateDegrees),
            MakeSkyVertex(s_skyMaxs[0][i], s_skyMins[1][i], i, face, viewDef.vieworg, rotateDegrees)
        };

        // Winding is free here: vu1::DrawTriangles has no back-face test of its
        // own, and the sky has nothing to cull against.
        clip::ClipVertex tri0[3] = { quad[0], quad[1], quad[2] };
        clip::ClipVertex tri1[3] = { quad[0], quad[2], quad[3] };
        GatherSkyTriangle(tri0, viewProj, face);
        GatherSkyTriangle(tri1, viewProj, face);

        // One batch per face: each binds its own texture, so they could never
        // have shared one anyway.
        s_batch.Flush(viewProj, face, kSkyDrawFlags);
        ++view::GetDrawStats().skyFaces;
    }
}

} // namespace ps2::sky
