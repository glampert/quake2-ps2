/* ================================================================================================
 * File: model_load.cpp
 * Brief: Loaders for the Quake 2 on-disk model formats (world map, sprite, md2).
 *
 *  Adapted from ref_gl's model loading. The on-disk (d*_t) structures are
 *  expanded into the in-memory (Model*) structures declared in model.h. The EE
 *  is little-endian like the BSP/MD2/SP2 formats, so no byte-order fixups are
 *  needed (unlike the original, which called LittleLong/LittleShort/etc).
 *
 *  Memory: rather than reserving a fixed worst-case chunk up front, a brush
 *  model is sized by a pre-pass (ComputeBrushHunkSize) that walks the lumps and
 *  sums exactly what the loaders will allocate; the block is then filled by a
 *  bump-pointer allocator (HunkAllocator) and freed in one shot on eviction.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/model.h"
#include "ps2/renderer/model_load.h"
#include "ps2/renderer/texture.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
    #include "common/q_files.h" // dheader_t, lump_t, dvertex_t, etc.
}

namespace ps2::mod {
namespace {

// ------------------------------------------------------------------------------------------------
// Loader configuration
// ------------------------------------------------------------------------------------------------

// Extra debug printing during model load.
constexpr bool kVerboseModelLoading = false;

// Alignment of every hunk sub-allocation. Rounding each allocation up keeps the
// running offset aligned, so the pre-pass total is order-independent and matches
// the real run exactly, and leaves vertex arrays qword-aligned for later DMA.
constexpr u32 kHunkAlign = 16;

constexpr u32 AlignUp(u32 value, u32 alignment)
{
    return (value + (alignment - 1)) & ~(alignment - 1);
}

// Lightmap atlas dimensions the lightmap UVs are normalised against.
constexpr int kLightmapTextureWidth  = 128;
constexpr int kLightmapTextureHeight = 128;

constexpr float kTriangulationEpsilon  = 0.001f;
constexpr int   kTriangulationMaxVerts = 128; // Per polygon.

constexpr float kSubdivideSizeF = static_cast<float>(kSubdivideSize);

// ------------------------------------------------------------------------------------------------
// Hunk sizing and allocation
//
// Two types, deliberately: HunkSizer accumulates the byte size the pre-pass
// needs, and HunkAllocator hands out that memory for real. They share the same
// per-element rounding (via AlignUp), so a measuring pass and the real fill
// agree exactly. Keeping them separate means the allocator has no "no memory"
// path, so the loaders never dereference a possibly-null allocation.
// ------------------------------------------------------------------------------------------------

class HunkSizer final
{
public:
    void Add(u32 sizeBytes)
    {
        m_offset += AlignUp(sizeBytes, kHunkAlign);
    }

    template<typename T>
    void AddArray(int count)
    {
        static_assert(alignof(T) <= kHunkAlign);
        PS2_Assert(count >= 0);
        Add(static_cast<u32>(count) * static_cast<u32>(sizeof(T)));
    }

    u32 BytesUsed() const { return m_offset; }

private:
    u32 m_offset = 0;
};

class HunkAllocator final
{
public:
    // Allocates and zero-fills the block (loaders rely on zero-initialised
    // fields, matching ref_gl's Hunk_Alloc semantics). PS2_MemAllocAligned aborts
    // on OOM, so this always succeeds.
    void Init(u32 sizeBytes, PS2MemTag tag)
    {
        m_base     = static_cast<u8 *>(PS2_MemAllocAligned(kHunkAlign, sizeBytes, tag));
        m_offset   = 0;
        m_capacity = sizeBytes;
        std::memset(m_base, 0, sizeBytes);
    }

    void * Alloc(u32 sizeBytes)
    {
        const u32 aligned = AlignUp(sizeBytes, kHunkAlign);
        // A failure here means the pre-pass under-estimated - a loader bug.
        PS2_Assert(m_offset + aligned <= m_capacity);
        u8 * const ptr = m_base + m_offset;
        m_offset += aligned;
        return ptr;
    }

    template<typename T>
    T * AllocArray(int count)
    {
        static_assert(alignof(T) <= kHunkAlign);
        PS2_Assert(count >= 0);
        return static_cast<T *>(Alloc(static_cast<u32>(count) * static_cast<u32>(sizeof(T))));
    }

    void * Base()   const { return m_base; }
    u32 BytesUsed() const { return m_offset; }

private:
    u8 * m_base     = nullptr;
    u32  m_offset   = 0;
    u32  m_capacity = 0;
};

// ------------------------------------------------------------------------------------------------
// Small geometry helpers
// ------------------------------------------------------------------------------------------------

// Points into the model file data at a lump's offset. Cast through void* so the
// higher-alignment result pointer doesn't trip -Wcast-align (the heap buffer is
// aligned well past any of these structs).
template<typename T>
inline const T * LumpData(const void * const fileData, const lump_t & l)
{
    const void * const p = static_cast<const u8 *>(fileData) + l.fileofs;
    return static_cast<const T *>(p);
}

// Element count of a lump; used by the loaders after the pre-pass has validated
// that filelen divides evenly.
template<typename T>
inline int LumpElemCount(const lump_t & l)
{
    return l.filelen / static_cast<int>(sizeof(T));
}

inline Vec3 ToVec3(const float * const p)
{
    return { p[0], p[1], p[2] };
}

inline float Component(const Vec3 & v, int axis)
{
    return (axis == 0) ? v.x : (axis == 1) ? v.y : v.z;
}

// Texture-plane projection s = v . vec + vec[3] (vec is a texinfo vecs[] row).
inline float TexProject(const Vec3 & v, const float vec[4])
{
    return (v.x * vec[0]) + (v.y * vec[1]) + (v.z * vec[2]) + vec[3];
}

// Same projection without the constant offset (used for turbulent surfaces).
inline float Project3(const Vec3 & v, const float vec[4])
{
    return (v.x * vec[0]) + (v.y * vec[1]) + (v.z * vec[2]);
}

// Reconstructs a surface vertex position from a surfedge index (negative indices
// walk the edge backwards). Shared by every surface-processing helper.
inline const Vec3 & EdgeVertex(const ModelInstance & mdl, int surfEdgeIndex)
{
    if (surfEdgeIndex > 0)
    {
        return mdl.vertexes[mdl.edges[surfEdgeIndex].v[0]].position;
    }
    return mdl.vertexes[mdl.edges[-surfEdgeIndex].v[1]].position;
}

// ------------------------------------------------------------------------------------------------
// Brush model lumps
// ------------------------------------------------------------------------------------------------

void LoadVertexes(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    const auto * in = LumpData<dvertex_t>(fileData, l);
    const int count = LumpElemCount<dvertex_t>(l);

    ModelVertex * out = hunk.AllocArray<ModelVertex>(count);
    mdl.vertexes    = out;
    mdl.numVertexes = count;

    for (int i = 0; i < count; ++i)
    {
        out[i].position = ToVec3(in[i].point);
    }
}

void LoadEdges(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    const auto * in = LumpData<dedge_t>(fileData, l);
    const int count = LumpElemCount<dedge_t>(l);

    // One extra sentinel edge, matching ref_gl.
    ModelEdge * out = hunk.AllocArray<ModelEdge>(count + 1);
    mdl.edges    = out;
    mdl.numEdges = count;

    for (int i = 0; i < count; ++i)
    {
        out[i].v[0] = in[i].v[0];
        out[i].v[1] = in[i].v[1];
    }
}

void LoadSurfEdges(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    const int * in  = LumpData<int>(fileData, l);
    const int count = LumpElemCount<int>(l);

    int * out = hunk.AllocArray<int>(count);
    mdl.surfEdges    = out;
    mdl.numSurfEdges = count;

    std::memcpy(out, in, static_cast<size_t>(count) * sizeof(int));
}

void LoadLighting(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    if (l.filelen <= 0)
    {
        mdl.lightData = nullptr;
        return;
    }

    mdl.lightData = hunk.AllocArray<u8>(l.filelen);
    std::memcpy(mdl.lightData, LumpData<u8>(fileData, l), static_cast<size_t>(l.filelen));
}

void LoadPlanes(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    const auto * in = LumpData<dplane_t>(fileData, l);
    const int count = LumpElemCount<dplane_t>(l);

    // Twice the count, matching ref_gl (the extra slots back opposite planes).
    cplane_t * out = hunk.AllocArray<cplane_t>(count * 2);
    mdl.planes    = out;
    mdl.numPlanes = count;

    for (int i = 0; i < count; ++i)
    {
        int bits = 0;
        for (int j = 0; j < 3; ++j)
        {
            out[i].normal[j] = in[i].normal[j];
            if (out[i].normal[j] < 0.0f)
            {
                bits |= (1 << j); // Negative normal components set a sign bit.
            }
        }
        out[i].dist     = in[i].dist;
        out[i].type     = static_cast<byte>(in[i].type);
        out[i].signbits = static_cast<byte>(bits);
    }
}

void LoadTexInfo(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    const auto * in = LumpData<textureinfo_t>(fileData, l);
    const int count = LumpElemCount<textureinfo_t>(l);

    ModelTexInfo * out = hunk.AllocArray<ModelTexInfo>(count);
    mdl.texInfos    = out;
    mdl.numTexInfos = count;

    for (int i = 0; i < count; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            out[i].vecs[0][j] = in[i].vecs[0][j];
            out[i].vecs[1][j] = in[i].vecs[1][j];
        }

        out[i].flags = in[i].flags;

        const int next = in[i].nexttexinfo;
        out[i].next = (next > 0) ? (mdl.texInfos + next) : nullptr;

        char name[MAX_QPATH];
        std::snprintf(name, sizeof(name), "textures/%s.wal", in[i].texture);

        const tex::Texture * texture = tex::Find(name, tex::ImageType::Wall);
        if (texture == nullptr)
        {
            // A visible checkerboard stands in for a missing wall texture.
            texture = &tex::DebugTexture(0);
            Com_Printf("WARNING: Failed to load wall texture '%s'\n", name);
        }
        out[i].texture = texture;
    }

    // Count animation frames by walking each texture's animation chain.
    for (int i = 0; i < count; ++i)
    {
        ModelTexInfo * base = &mdl.texInfos[i];
        base->numFrames = 1;
        for (const ModelTexInfo * step = base->next; step != nullptr && step != base; step = step->next)
        {
            base->numFrames++;
        }
    }
}

void CalcSurfaceExtents(const ModelInstance & mdl, ModelSurface & surf)
{
    float mins[2] = { 999999.0f, 999999.0f };
    float maxs[2] = { -99999.0f, -99999.0f };

    const ModelTexInfo * const tex = surf.texInfo;
    PS2_Assert(tex != nullptr);

    for (int i = 0; i < surf.numEdges; ++i)
    {
        const Vec3 & pos = EdgeVertex(mdl, mdl.surfEdges[surf.firstEdge + i]);
        for (int j = 0; j < 2; ++j)
        {
            const float val = TexProject(pos, tex->vecs[j]);
            if (val < mins[j]) { mins[j] = val; }
            if (val > maxs[j]) { maxs[j] = val; }
        }
    }

    for (int i = 0; i < 2; ++i)
    {
        const int bmin = static_cast<int>(std::floor(mins[i] / 16.0f));
        const int bmax = static_cast<int>(std::ceil(maxs[i]  / 16.0f));

        surf.textureMins[i] = static_cast<s16>(bmin * 16);
        surf.extents[i]     = static_cast<s16>((bmax - bmin) * 16);
    }
}

// ------------------------------------------------------------------------------------------------
// Polygon triangulation (ear clipping)
//
// The renderer draws indexed triangles, so each BSP polygon is triangulated at
// load time. Adapted from the sample code in Eric Lengyel's "Mathematics for 3D
// Game Programming and Computer Graphics" (Listing 9.2).
// ------------------------------------------------------------------------------------------------

// Polygon normal via the sum of edge cross products (see iquilezles.org/articles/areas).
Vec3 ComputePolygonNormal(const ModelPoly & poly)
{
    Vec3 normal = { 0.0f, 0.0f, 0.0f };
    for (int v = 0; v < poly.numVerts; ++v)
    {
        const int vNext = (v + 1) % poly.numVerts;
        normal = normal + math::Cross(poly.vertexes[v].position, poly.vertexes[vNext].position);
    }
    return math::Normalize(normal);
}

int NextActive(int x, const int numVerts, const bool * const active)
{
    for (;;)
    {
        if (++x == numVerts) { x = 0; }
        if (active[x]) { return x; }
    }
}

int PrevActive(int x, const int numVerts, const bool * const active)
{
    for (;;)
    {
        if (--x == -1) { x = numVerts - 1; }
        if (active[x]) { return x; }
    }
}

bool TestTriangle(int pi1, int pi2, int pi3,
                  const Vec3 & p1, const Vec3 & p2, const Vec3 & p3, const Vec3 & normal,
                  const bool * const active, const ModelPoly & poly, float epsilon)
{
    const Vec3 n1 = math::Cross(normal, math::Normalize(p2 - p1));
    if (math::Dot(n1, p3 - p1) <= epsilon)
    {
        return false;
    }

    const Vec3 n2 = math::Cross(normal, math::Normalize(p3 - p2));
    const Vec3 n3 = math::Cross(normal, math::Normalize(p1 - p3));

    for (int v = 0; v < poly.numVerts; ++v)
    {
        // Reject the triangle if any other active vertex lies inside it.
        if (active[v] && v != pi1 && v != pi2 && v != pi3)
        {
            const Vec3 & pv = poly.vertexes[v].position;
            if (math::Dot(n1, math::Normalize(pv - p1)) > -epsilon &&
                math::Dot(n2, math::Normalize(pv - p2)) > -epsilon &&
                math::Dot(n3, math::Normalize(pv - p3)) > -epsilon)
            {
                return false;
            }
        }
    }

    return true;
}

void TriangulatePolygon(ModelPoly & poly)
{
    // Already a triangle, or a degenerate polygon.
    if (poly.numVerts <= 3)
    {
        if (poly.numVerts == 3)
        {
            PS2_Assert(poly.triangles != nullptr);
            poly.triangles->vertexes[0] = 0;
            poly.triangles->vertexes[1] = 1;
            poly.triangles->vertexes[2] = 2;
        }
        else
        {
            // Broken polygons are left with zeroed triangles (skipped at draw time).
            Com_Printf("WARNING: Broken polygon found in brush model!\n");
        }
        return;
    }

    const int numVerts     = poly.numVerts;
    const int numTriangles = numVerts - 2;

    if (numVerts > kTriangulationMaxVerts)
    {
        // Just make kTriangulationMaxVerts bigger if this ever fires (1 byte/entry).
        Com_Printf("ERROR: TriangulatePolygon: kTriangulationMaxVerts (%i) exceeded!\n", kTriangulationMaxVerts);
        return;
    }

    const Vec3 normal = ComputePolygonNormal(poly);

    int start = 0;
    int p1 = 0;
    int p2 = 1;
    int m1 = numVerts - 1;
    int m2 = numVerts - 2;
    bool lastPositive = false;

    int triesDone = 0;
    ModelTriangle * trisPtr = poly.triangles;

    // BSP polygons are small (under ~20 verts), so a stack buffer avoids a malloc.
    bool active[kTriangulationMaxVerts];
    for (int i = 0; i < numVerts; ++i)
    {
        active[i] = true;
    }

    auto EmitTriangle = [&triesDone, numTriangles, &trisPtr](int v0, int v1, int v2)
    {
        if (triesDone == numTriangles)
        {
            Com_Printf("ERROR: TriangulatePolygon: Triangle list overflowed!\n");
            return;
        }
        trisPtr->vertexes[0] = static_cast<u16>(v0);
        trisPtr->vertexes[1] = static_cast<u16>(v1);
        trisPtr->vertexes[2] = static_cast<u16>(v2);
        ++trisPtr;
        ++triesDone;
    };

    for (;;)
    {
        if (p2 == m2)
        {
            // Only three vertexes remain.
            EmitTriangle(m1, p1, p2);
            break;
        }

        const Vec3 & vp1 = poly.vertexes[p1].position;
        const Vec3 & vp2 = poly.vertexes[p2].position;
        const Vec3 & vm1 = poly.vertexes[m1].position;
        const Vec3 & vm2 = poly.vertexes[m2].position;

        bool positive = TestTriangle(p1, p2, m1, vp2, vm1, vp1, normal, active, poly, kTriangulationEpsilon);
        bool negative = TestTriangle(m1, m2, p1, vp1, vm2, vm1, normal, active, poly, kTriangulationEpsilon);

        // If both are valid, keep the one with the larger smallest angle.
        if (positive && negative)
        {
            const float pDot = math::Dot(math::Normalize(vp2 - vm1), math::Normalize(vm2 - vm1));
            const float mDot = math::Dot(math::Normalize(vm2 - vp1), math::Normalize(vp2 - vp1));

            if (math::Fabsf(pDot - mDot) < kTriangulationEpsilon)
            {
                if (lastPositive) { positive = false; }
                else              { negative = false; }
            }
            else if (pDot < mDot) { negative = false; }
            else                  { positive = false; }
        }

        if (positive)
        {
            active[p1] = false;
            EmitTriangle(m1, p1, p2);
            p1 = NextActive(p1, numVerts, active);
            p2 = NextActive(p2, numVerts, active);
            lastPositive = true;
            start = -1;
        }
        else if (negative)
        {
            active[m1] = false;
            EmitTriangle(m2, m1, p1);
            m1 = PrevActive(m1, numVerts, active);
            m2 = PrevActive(m2, numVerts, active);
            lastPositive = false;
            start = -1;
        }
        else // No valid triangle yet; advance the working set.
        {
            if (start == -1)
            {
                start = p2;
            }
            else if (p2 == start)
            {
                // Went all the way around without finding a valid triangle.
                break;
            }

            m2 = m1;
            m1 = p1;
            p1 = p2;
            p2 = NextActive(p2, numVerts, active);
        }
    }

    // Not a hard error: the algorithm may fail to produce the expected count on
    // pathological polygons. The unused triangles stay zeroed.
    if (triesDone != numTriangles)
    {
        Com_Printf("WARNING: TriangulatePolygon: Unexpected triangle count!\n");
    }
}

void BuildPolygonFromSurface(ModelInstance & mdl, HunkAllocator & hunk, ModelSurface & surf)
{
    PS2_Assert(mdl.vertexes != nullptr && mdl.edges != nullptr && mdl.surfEdges != nullptr);

    const int numVerts     = surf.numEdges;
    const int numTriangles = (numVerts >= 3) ? (numVerts - 2) : 0;

    ModelPoly * poly = hunk.AllocArray<ModelPoly>(1);
    poly->next  = surf.polys;
    surf.polys  = poly;

    poly->numVerts  = numVerts;
    poly->vertexes  = hunk.AllocArray<PolyVertex>(numVerts);
    poly->triangles = hunk.AllocArray<ModelTriangle>(numTriangles);

    const ModelTexInfo * const tex = surf.texInfo;
    const float texW = static_cast<float>(tex->texture->width);
    const float texH = static_cast<float>(tex->texture->height);

    for (int i = 0; i < numVerts; ++i)
    {
        const Vec3 & pos = EdgeVertex(mdl, mdl.surfEdges[surf.firstEdge + i]);
        poly->vertexes[i].position = pos;

        // Colour texture coordinates.
        poly->vertexes[i].texture_s = TexProject(pos, tex->vecs[0]) / texW;
        poly->vertexes[i].texture_t = TexProject(pos, tex->vecs[1]) / texH;

        // Lightmap texture coordinates (light_s/light_t stay 0 while lightmaps
        // are stubbed, but the UVs are baked so the vertex format is complete).
        const float lms = TexProject(pos, tex->vecs[0]) - static_cast<float>(surf.textureMins[0])
                        + static_cast<float>(surf.light_s * 16) + 8.0f;
        const float lmt = TexProject(pos, tex->vecs[1]) - static_cast<float>(surf.textureMins[1])
                        + static_cast<float>(surf.light_t * 16) + 8.0f;
        poly->vertexes[i].lightmap_s = lms / static_cast<float>(kLightmapTextureWidth  * 16);
        poly->vertexes[i].lightmap_t = lmt / static_cast<float>(kLightmapTextureHeight * 16);
    }

    TriangulatePolygon(*poly);
}

// ------------------------------------------------------------------------------------------------
// Turbulent (water) surface subdivision
//
// Warped surfaces are cut along the 64-unit grid so the turbulence warp stays
// well behaved. The recursive split is identical whether we are sizing the hunk
// or building geometry, so it is written once with the per-leaf action passed as
// a callback: the real build allocates and fills a polygon, the pre-pass just
// accounts the bytes. This keeps the memory estimate exact without a second copy
// of the algorithm.
// ------------------------------------------------------------------------------------------------

void BoundPoly(int numVerts, const Vec3 * verts, Vec3 & mins, Vec3 & maxs)
{
    mins = {  9999.0f,  9999.0f,  9999.0f };
    maxs = { -9999.0f, -9999.0f, -9999.0f };
    for (int i = 0; i < numVerts; ++i)
    {
        mins.x = math::Minf(mins.x, verts[i].x);
        mins.y = math::Minf(mins.y, verts[i].y);
        mins.z = math::Minf(mins.z, verts[i].z);
        maxs.x = math::Maxf(maxs.x, verts[i].x);
        maxs.y = math::Maxf(maxs.y, verts[i].y);
        maxs.z = math::Maxf(maxs.z, verts[i].z);
    }
}

template<typename EmitFn>
void SubdividePolygon(int numVerts, const Vec3 * verts, EmitFn emit)
{
    if (numVerts > kSubdivideSize - 4)
    {
        Sys_Error("SubdividePolygon: Too many verts (%i)", numVerts);
    }

    Vec3 mins, maxs;
    BoundPoly(numVerts, verts, mins, maxs);

    for (int axis = 0; axis < 3; ++axis)
    {
        const float mid = kSubdivideSizeF *
            std::floor(((Component(mins, axis) + Component(maxs, axis)) * 0.5f) / kSubdivideSizeF + 0.5f);

        if (Component(maxs, axis) - mid < 8.0f) { continue; }
        if (mid - Component(mins, axis) < 8.0f) { continue; }

        // Signed distance of each vertex to the split plane, with a wrap slot.
        float dist[kSubdivideSize + 1];
        for (int i = 0; i < numVerts; ++i)
        {
            dist[i] = Component(verts[i], axis) - mid;
        }
        dist[numVerts] = dist[0];

        Vec3 wrapped[kSubdivideSize + 1];
        for (int i = 0; i < numVerts; ++i)
        {
            wrapped[i] = verts[i];
        }
        wrapped[numVerts] = verts[0];

        Vec3 front[kSubdivideSize];
        Vec3 back[kSubdivideSize];
        int f = 0;
        int b = 0;

        for (int i = 0; i < numVerts; ++i)
        {
            if (dist[i] >= 0.0f) { front[f++] = wrapped[i]; }
            if (dist[i] <= 0.0f) { back[b++]  = wrapped[i]; }

            if (dist[i] == 0.0f || dist[i + 1] == 0.0f)
            {
                continue;
            }
            if ((dist[i] > 0.0f) != (dist[i + 1] > 0.0f))
            {
                // Split the edge at the plane crossing.
                const float frac = dist[i] / (dist[i] - dist[i + 1]);
                const Vec3 mid3  = wrapped[i] + (wrapped[i + 1] - wrapped[i]) * frac;
                front[f++] = mid3;
                back[b++]  = mid3;
            }
        }

        SubdividePolygon(f, front, emit);
        SubdividePolygon(b, back, emit);
        return;
    }

    emit(numVerts, verts);
}

// Gathers a surface's polygon into 'out' (up to kSubdivideSize verts). Returns
// the count, or -1 if the surface has more verts than the subdivision buffers hold.
int GatherSurfaceVerts(const ModelInstance & mdl, const ModelSurface & surf, Vec3 * out)
{
    int count = 0;
    for (int i = 0; i < surf.numEdges; ++i)
    {
        if (count >= kSubdivideSize)
        {
            return -1;
        }
        out[count++] = EdgeVertex(mdl, mdl.surfEdges[surf.firstEdge + i]);
    }
    return count;
}

void SubdivideSurface(ModelInstance & mdl, HunkAllocator & hunk, ModelSurface & surf)
{
    Vec3 verts[kSubdivideSize];
    const int count = GatherSurfaceVerts(mdl, surf, verts);
    if (count < 0)
    {
        Sys_Error("SubdivideSurface: Max verts exceeded!");
    }

    const ModelTexInfo * const tex = surf.texInfo;

    SubdividePolygon(count, verts, [&](int numLeafVerts, const Vec3 * leafVerts)
    {
        ModelPoly * poly = hunk.AllocArray<ModelPoly>(1);
        poly->next  = surf.polys;
        surf.polys  = poly;

        // +2: a center point (for the warp fan) plus a duplicate of the first.
        poly->numVerts  = numLeafVerts + 2;
        poly->vertexes  = hunk.AllocArray<PolyVertex>(numLeafVerts + 2);
        poly->triangles = nullptr; // Warped polygons are drawn as fans, not triangulated.

        Vec3 total    = { 0.0f, 0.0f, 0.0f };
        float totalS  = 0.0f;
        float totalT  = 0.0f;
        const float invCount = 1.0f / static_cast<float>(numLeafVerts);

        for (int i = 0; i < numLeafVerts; ++i)
        {
            const float s = Project3(leafVerts[i], tex->vecs[0]);
            const float t = Project3(leafVerts[i], tex->vecs[1]);
            totalS += s;
            totalT += t;
            total   = total + leafVerts[i];

            poly->vertexes[i + 1].position  = leafVerts[i];
            poly->vertexes[i + 1].texture_s = s;
            poly->vertexes[i + 1].texture_t = t;
        }

        // Center vertex, then close the fan by duplicating the first.
        poly->vertexes[0].position  = total * invCount;
        poly->vertexes[0].texture_s = totalS * invCount;
        poly->vertexes[0].texture_t = totalT * invCount;
        poly->vertexes[numLeafVerts + 1] = poly->vertexes[1];
    });
}

void LoadFaces(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    PS2_Assert(mdl.planes != nullptr && mdl.texInfos != nullptr); // Load these first.

    const auto * in = LumpData<dface_t>(fileData, l);
    const int count = LumpElemCount<dface_t>(l);

    ModelSurface * out = hunk.AllocArray<ModelSurface>(count);
    mdl.surfaces    = out;
    mdl.numSurfaces = count;

    for (int surfNum = 0; surfNum < count; ++surfNum)
    {
        ModelSurface & surf = out[surfNum];
        surf.firstEdge = in[surfNum].firstedge;
        surf.numEdges  = in[surfNum].numedges;
        surf.color     = 0xFFFFFFFF; // <-- NOTE: Can add a debug color here.
        surf.flags     = SurfaceFlags::None;
        surf.polys     = nullptr;
        surf.lightmapTextureNum = -1; // Not lightmapped (lightmaps stubbed).

        if (in[surfNum].side)
        {
            surf.flags = surf.flags | SurfaceFlags::PlaneBack;
        }
        surf.plane = mdl.planes + in[surfNum].planenum;

        const int texNum = in[surfNum].texinfo;
        if (texNum < 0 || texNum >= mdl.numTexInfos)
        {
            Sys_Error("LoadFaces: Bad texinfo number: %i", texNum);
        }
        surf.texInfo = mdl.texInfos + texNum;

        CalcSurfaceExtents(mdl, surf);

        // Lightmap styles / sample pointer (lightmap building itself is stubbed).
        for (int i = 0; i < kMaxLightmaps; ++i)
        {
            surf.styles[i] = in[surfNum].styles[i];
        }
        const int lightOfs = in[surfNum].lightofs;
        surf.samples = (lightOfs == -1) ? nullptr : (mdl.lightData + lightOfs);

        // Turbulent water surfaces: fixed extents, then subdivided for the warp.
        if (surf.texInfo->flags & SURF_WARP)
        {
            surf.flags = surf.flags | SurfaceFlags::DrawTurb;
            for (int i = 0; i < 2; ++i)
            {
                surf.extents[i]     = 16384;
                surf.textureMins[i] = -8192;
            }
            SubdivideSurface(mdl, hunk, surf);
        }
        else
        {
            BuildPolygonFromSurface(mdl, hunk, surf);
        }
    }
}

void LoadMarkSurfaces(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    PS2_Assert(mdl.surfaces != nullptr); // Load faces first.

    const auto * in = LumpData<s16>(fileData, l);
    const int count = LumpElemCount<s16>(l);

    ModelSurface ** out = hunk.AllocArray<ModelSurface *>(count);
    mdl.markSurfaces    = out;
    mdl.numMarkSurfaces = count;

    for (int i = 0; i < count; ++i)
    {
        const int j = in[i];
        if (j < 0 || j >= mdl.numSurfaces)
        {
            Sys_Error("LoadMarkSurfaces: Bad surface number: %i", j);
        }
        out[i] = mdl.surfaces + j;
    }
}

void LoadVisibility(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    if (l.filelen <= 0)
    {
        mdl.vis = nullptr;
        return;
    }

    mdl.vis = hunk.Alloc(static_cast<u32>(l.filelen));
    std::memcpy(mdl.vis, LumpData<u8>(fileData, l), static_cast<size_t>(l.filelen));
}

void LoadLeafs(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    PS2_Assert(mdl.markSurfaces != nullptr); // Load mark surfaces first.

    const auto * in = LumpData<dleaf_t>(fileData, l);
    const int count = LumpElemCount<dleaf_t>(l);

    ModelLeaf * out = hunk.AllocArray<ModelLeaf>(count);
    mdl.leafs    = out;
    mdl.numLeafs = count;

    for (int i = 0; i < count; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            out[i].minmaxs[j]     = in[i].mins[j];
            out[i].minmaxs[j + 3] = in[i].maxs[j];
        }

        out[i].contents = in[i].contents;
        out[i].cluster  = in[i].cluster;
        out[i].area     = in[i].area;

        out[i].firstMarkSurface = mdl.markSurfaces + in[i].firstleafface;
        out[i].numMarkSurfaces  = in[i].numleaffaces;
    }
}

void SetParentRecursive(ModelNode * node, ModelNode * parent)
{
    node->parent = parent;
    if (node->contents != -1)
    {
        return; // A leaf (reinterpreted as a node); stop here.
    }
    SetParentRecursive(node->children[0], node);
    SetParentRecursive(node->children[1], node);
}

void LoadNodes(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    PS2_Assert(mdl.planes != nullptr && mdl.leafs != nullptr); // Load these first.

    const auto * in = LumpData<dnode_t>(fileData, l);
    const int count = LumpElemCount<dnode_t>(l);

    ModelNode * out = hunk.AllocArray<ModelNode>(count);
    mdl.nodes    = out;
    mdl.numNodes = count;

    for (int i = 0; i < count; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            out[i].minmaxs[j]     = in[i].mins[j];
            out[i].minmaxs[j + 3] = in[i].maxs[j];
        }

        out[i].plane        = mdl.planes + in[i].planenum;
        out[i].firstSurface = in[i].firstface;
        out[i].numSurfaces  = in[i].numfaces;
        out[i].contents     = -1; // Differentiates nodes from leafs.

        for (int j = 0; j < 2; ++j)
        {
            const int p = in[i].children[j];
            if (p >= 0)
            {
                out[i].children[j] = mdl.nodes + p;
            }
            else
            {
                // Negative children index leafs, reinterpreted as nodes.
                out[i].children[j] = reinterpret_cast<ModelNode *>(mdl.leafs + (-1 - p));
            }
        }
    }

    SetParentRecursive(mdl.nodes, nullptr);
}

float RadiusFromBounds(const Vec3 & mins, const Vec3 & maxs)
{
    const Vec3 corner = {
        math::Maxf(math::Fabsf(mins.x), math::Fabsf(maxs.x)),
        math::Maxf(math::Fabsf(mins.y), math::Fabsf(maxs.y)),
        math::Maxf(math::Fabsf(mins.z), math::Fabsf(maxs.z)),
    };
    return math::Length(corner);
}

void LoadSubModels(ModelInstance & mdl, HunkAllocator & hunk, const void * const fileData, const lump_t & l)
{
    const auto * in = LumpData<dmodel_t>(fileData, l);
    const int count = LumpElemCount<dmodel_t>(l);

    SubModelInfo * out = hunk.AllocArray<SubModelInfo>(count);
    mdl.subModels    = out;
    mdl.numSubModels = count;

    for (int i = 0; i < count; ++i)
    {
        // Spread the bounds by a unit, matching ref_gl.
        out[i].mins   = { in[i].mins[0] - 1.0f, in[i].mins[1] - 1.0f, in[i].mins[2] - 1.0f };
        out[i].maxs   = { in[i].maxs[0] + 1.0f, in[i].maxs[1] + 1.0f, in[i].maxs[2] + 1.0f };
        out[i].origin = ToVec3(in[i].origin);

        out[i].radius    = RadiusFromBounds(out[i].mins, out[i].maxs);
        out[i].headNode  = in[i].headnode;
        out[i].firstFace = in[i].firstface;
        out[i].numFaces  = in[i].numfaces;
    }
}

// ------------------------------------------------------------------------------------------------
// Brush model memory pre-pass
//
// Walks the lumps and sums exactly what the loaders above will allocate, so the
// hunk can be sized to the model instead of a fixed worst case. Also validates
// lump sizes/counts up front, so a corrupt map fails here rather than mid-load.
// Returns false (and prints why) on any structural problem.
// ------------------------------------------------------------------------------------------------

// filelen must be a whole multiple of elemSize. Returns the element count, or -1.
int CheckedLumpCount(const lump_t & l, size_t elemSize, const char * what, const char * name)
{
    if ((static_cast<size_t>(l.filelen) % elemSize) != 0)
    {
        Com_Printf("ERROR: LoadBrushModel: Funny %s lump size in '%s'\n", what, name);
        return -1;
    }
    return static_cast<int>(static_cast<size_t>(l.filelen) / elemSize);
}

bool ComputeBrushHunkSize(const dheader_t * header, const void * fileData, const char * name, u32 & outSize)
{
    HunkSizer m{};

    const int numVertexes = CheckedLumpCount(header->lumps[LUMP_VERTEXES], sizeof(dvertex_t), "vertexes", name);
    if (numVertexes < 0) { return false; }
    m.AddArray<ModelVertex>(numVertexes);

    const int numEdges = CheckedLumpCount(header->lumps[LUMP_EDGES], sizeof(dedge_t), "edges", name);
    if (numEdges < 0) { return false; }
    m.AddArray<ModelEdge>(numEdges + 1);

    const int numSurfEdges = CheckedLumpCount(header->lumps[LUMP_SURFEDGES], sizeof(int), "surfedges", name);
    if (numSurfEdges < 1 || numSurfEdges >= MAX_MAP_SURFEDGES)
    {
        Com_Printf("ERROR: LoadBrushModel: Bad surfedges count in '%s': %i\n", name, numSurfEdges);
        return false;
    }
    m.AddArray<int>(numSurfEdges);

    const int lightingLen = header->lumps[LUMP_LIGHTING].filelen;
    if (lightingLen > 0) { m.Add(static_cast<u32>(lightingLen)); }

    const int numPlanes = CheckedLumpCount(header->lumps[LUMP_PLANES], sizeof(dplane_t), "planes", name);
    if (numPlanes < 0) { return false; }
    m.AddArray<cplane_t>(numPlanes * 2);

    const int numTexInfos = CheckedLumpCount(header->lumps[LUMP_TEXINFO], sizeof(textureinfo_t), "texinfo", name);
    if (numTexInfos < 0) { return false; }
    m.AddArray<ModelTexInfo>(numTexInfos);

    const int numFaces = CheckedLumpCount(header->lumps[LUMP_FACES], sizeof(dface_t), "faces", name);
    if (numFaces < 0) { return false; }
    m.AddArray<ModelSurface>(numFaces);

    // Per-face polygon memory. Predictable for normal faces (from numedges);
    // warped faces are measured by running the shared subdivision recursion.
    {
        const auto * faces     = LumpData<dface_t>(fileData, header->lumps[LUMP_FACES]);
        const auto * texInfos  = LumpData<textureinfo_t>(fileData, header->lumps[LUMP_TEXINFO]);
        const auto * surfEdges = LumpData<int>(fileData, header->lumps[LUMP_SURFEDGES]);
        const auto * edges     = LumpData<dedge_t>(fileData, header->lumps[LUMP_EDGES]);
        const auto * verts     = LumpData<dvertex_t>(fileData, header->lumps[LUMP_VERTEXES]);

        for (int f = 0; f < numFaces; ++f)
        {
            const int numEdgesForFace = faces[f].numedges;
            const int texNum = faces[f].texinfo;
            const bool warp  = (texNum >= 0 && texNum < numTexInfos) &&
                               (texInfos[texNum].flags & SURF_WARP);

            if (!warp)
            {
                const int numTris = (numEdgesForFace >= 3) ? (numEdgesForFace - 2) : 0;
                m.AddArray<ModelPoly>(1);
                m.AddArray<PolyVertex>(numEdgesForFace);
                m.AddArray<ModelTriangle>(numTris);
                continue;
            }

            // Gather the surface polygon straight from the raw lumps, then run
            // the same subdivision to count the sub-polygons it will produce.
            Vec3 polyVerts[kSubdivideSize];
            int vertCount = 0;
            const int firstEdge = faces[f].firstedge;
            for (int i = 0; i < numEdgesForFace; ++i)
            {
                if (vertCount >= kSubdivideSize)
                {
                    Com_Printf("ERROR: LoadBrushModel: Warp surface too large in '%s'\n", name);
                    return false;
                }
                const int e  = surfEdges[firstEdge + i];
                const int vi = (e > 0) ? edges[e].v[0] : edges[-e].v[1];
                polyVerts[vertCount++] = ToVec3(verts[vi].point);
            }

            SubdividePolygon(vertCount, polyVerts, [&m](int numLeafVerts, const Vec3 *)
            {
                m.AddArray<ModelPoly>(1);
                m.AddArray<PolyVertex>(numLeafVerts + 2);
            });
        }
    }

    const int numMarkSurfaces = CheckedLumpCount(header->lumps[LUMP_LEAFFACES], sizeof(s16), "leaffaces", name);
    if (numMarkSurfaces < 0) { return false; }
    m.AddArray<ModelSurface *>(numMarkSurfaces);

    const int visLen = header->lumps[LUMP_VISIBILITY].filelen;
    if (visLen > 0) { m.Add(static_cast<u32>(visLen)); }

    const int numLeafs = CheckedLumpCount(header->lumps[LUMP_LEAFS], sizeof(dleaf_t), "leafs", name);
    if (numLeafs < 0) { return false; }
    m.AddArray<ModelLeaf>(numLeafs);

    const int numNodes = CheckedLumpCount(header->lumps[LUMP_NODES], sizeof(dnode_t), "nodes", name);
    if (numNodes < 0) { return false; }
    m.AddArray<ModelNode>(numNodes);

    const int numSubModels = CheckedLumpCount(header->lumps[LUMP_MODELS], sizeof(dmodel_t), "models", name);
    if (numSubModels < 0) { return false; }
    m.AddArray<SubModelInfo>(numSubModels);

    outSize = m.BytesUsed();
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// BRUSH MODELS (WORLD MAP)
// ------------------------------------------------------------------------------------------------

bool LoadBrushModel(ModelInstance & mdl, const void * const modelData, const int dataLenBytes)
{
    PS2_Assert(modelData != nullptr);
    PS2_Assert(dataLenBytes > 0);

    const auto * header = static_cast<const dheader_t *>(modelData);
    if (header->ident != IDBSPHEADER)
    {
        Com_Printf("ERROR: LoadBrushModel: '%s' has bad file ident!\n", mdl.name);
        return false;
    }
    if (header->version != BSPVERSION)
    {
        Com_Printf("ERROR: LoadBrushModel: '%s' has wrong version (%i should be %i)\n",
                   mdl.name, header->version, BSPVERSION);
        return false;
    }

    // Size the hunk to the model, validating the lumps in the process.
    u32 hunkSize = 0;
    if (!ComputeBrushHunkSize(header, modelData, mdl.name, hunkSize))
    {
        return false;
    }

    HunkAllocator hunk{};
    hunk.Init(hunkSize, MEMTAG_MDL_WORLD);
    mdl.hunkBase = hunk.Base();
    mdl.hunkSize = hunkSize;
    mdl.type     = ModelType::Brush;

    // Load order matters: several lumps reference earlier ones.
    LoadVertexes(mdl, hunk, modelData, header->lumps[LUMP_VERTEXES]);
    LoadEdges(mdl, hunk, modelData, header->lumps[LUMP_EDGES]);
    LoadSurfEdges(mdl, hunk, modelData, header->lumps[LUMP_SURFEDGES]);
    LoadLighting(mdl, hunk, modelData, header->lumps[LUMP_LIGHTING]);
    LoadPlanes(mdl, hunk, modelData, header->lumps[LUMP_PLANES]);
    LoadTexInfo(mdl, hunk, modelData, header->lumps[LUMP_TEXINFO]);
    LoadFaces(mdl, hunk, modelData, header->lumps[LUMP_FACES]);
    LoadMarkSurfaces(mdl, hunk, modelData, header->lumps[LUMP_LEAFFACES]);
    LoadVisibility(mdl, hunk, modelData, header->lumps[LUMP_VISIBILITY]);
    LoadLeafs(mdl, hunk, modelData, header->lumps[LUMP_LEAFS]);
    LoadNodes(mdl, hunk, modelData, header->lumps[LUMP_NODES]);
    LoadSubModels(mdl, hunk, modelData, header->lumps[LUMP_MODELS]);

    mdl.numFrames = 2; // Regular and alternate animation.

    // The hunk was sized to exactly what we allocate; assert we did not drift.
    PS2_Assert(hunk.BytesUsed() == hunkSize);

    if (kVerboseModelLoading)
    {
        Com_DPrintf("Brush model '%s' loaded (%u KB hunk).\n", mdl.name, hunkSize / 1024);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// SPRITE MODELS
// ------------------------------------------------------------------------------------------------

bool LoadSpriteModel(ModelInstance & mdl, const void * const modelData, const int dataLenBytes)
{
    PS2_Assert(modelData != nullptr);
    PS2_Assert(dataLenBytes > 0);

    const auto * in = static_cast<const dsprite_t *>(modelData);
    if (in->version != SPRITE_VERSION)
    {
        Com_Printf("ERROR: Sprite '%s' has wrong version (%i should be %i)\n",
                   mdl.name, in->version, SPRITE_VERSION);
        return false;
    }
    if (in->numframes < 0 || in->numframes > kMaxMD2Skins)
    {
        Com_Printf("ERROR: Sprite '%s' has bad frame count (%i)\n", mdl.name, in->numframes);
        return false;
    }

    // A sprite is stored verbatim; the hunk just holds a copy of the file.
    const u32 hunkSize = AlignUp(static_cast<u32>(dataLenBytes), kHunkAlign);

    HunkAllocator hunk{};
    hunk.Init(hunkSize, MEMTAG_MDL_SPRITE);
    mdl.hunkBase = hunk.Base();
    mdl.hunkSize = hunkSize;
    mdl.type     = ModelType::Sprite;

    auto * out = static_cast<dsprite_t *>(hunk.Alloc(static_cast<u32>(dataLenBytes)));
    std::memcpy(out, in, static_cast<size_t>(dataLenBytes));

    for (int i = 0; i < out->numframes; ++i)
    {
        mdl.skins[i] = tex::Find(out->frames[i].name, tex::ImageType::Sprite);
    }
    mdl.numFrames = out->numframes;

    if (kVerboseModelLoading)
    {
        Com_DPrintf("Sprite model '%s' loaded.\n", mdl.name);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// ALIAS MD2 MODELS
// ------------------------------------------------------------------------------------------------

bool LoadAliasMD2Model(ModelInstance & mdl, const void * const modelData, const int dataLenBytes)
{
    PS2_Assert(modelData != nullptr);
    PS2_Assert(dataLenBytes > 0);

    const auto * in = static_cast<const dmdl_t *>(modelData);
    if (in->version != ALIAS_VERSION)
    {
        Com_Printf("ERROR: Model '%s' has wrong version (%i should be %i)\n",
                   mdl.name, in->version, ALIAS_VERSION);
        return false;
    }

    // Validate the header before trusting any of its offsets.
    if (in->ofs_end <= 0 || in->ofs_end > dataLenBytes)
    {
        Com_Printf("ERROR: Model '%s' has a bad end offset!\n", mdl.name);
        return false;
    }
    if (in->skinheight > kMaxMD2SkinHeight)
    {
        Com_Printf("ERROR: Model '%s' has a skin taller than %i.\n", mdl.name, kMaxMD2SkinHeight);
        return false;
    }
    if (in->num_xyz <= 0 || in->num_xyz > MAX_VERTS)
    {
        Com_Printf("ERROR: Model '%s' has a bad vertex count (%i)!\n", mdl.name, in->num_xyz);
        return false;
    }
    if (in->num_st <= 0 || in->num_tris <= 0 || in->num_frames <= 0)
    {
        Com_Printf("ERROR: Model '%s' has no st verts / triangles / frames!\n", mdl.name);
        return false;
    }
    if (in->num_skins < 0 || in->num_skins > kMaxMD2Skins)
    {
        Com_Printf("ERROR: Model '%s' has a bad skin count (%i)!\n", mdl.name, in->num_skins);
        return false;
    }

    // MD2 needs no per-field expansion (no byte swap on the EE), so the hunk
    // holds a verbatim copy of the file up to ofs_end.
    const u32 hunkSize = AlignUp(static_cast<u32>(in->ofs_end), kHunkAlign);

    HunkAllocator hunk{};
    hunk.Init(hunkSize, MEMTAG_MDL_ALIAS);
    mdl.hunkBase = hunk.Base();
    mdl.hunkSize = hunkSize;
    mdl.type     = ModelType::AliasMD2;

    auto * out = static_cast<dmdl_t *>(hunk.Alloc(static_cast<u32>(in->ofs_end)));
    std::memcpy(out, in, static_cast<size_t>(in->ofs_end));

    // Default bounds (MD2s carry no bounds; the game clips against these).
    mdl.mins      = { -32.0f, -32.0f, -32.0f };
    mdl.maxs      = {  32.0f,  32.0f,  32.0f };
    mdl.numFrames = out->num_frames;

    for (int i = 0; i < out->num_skins; ++i)
    {
        const char * skinName = reinterpret_cast<const char *>(out) + out->ofs_skins + (i * MAX_SKINNAME);
        mdl.skins[i] = tex::Find(skinName, tex::ImageType::Skin);
    }

    if (kVerboseModelLoading)
    {
        Com_DPrintf("Alias model '%s' loaded.\n", mdl.name);
    }
    return true;
}

} // namespace ps2::mod
