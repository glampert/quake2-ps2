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
#include "ps2/renderer/lightmap.h"

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

constexpr float kTriangulationEpsilon  = 0.001f;
constexpr int   kTriangulationMaxVerts = 128; // Per polygon.

constexpr float kSubdivideSizeF = static_cast<float>(kSubdivideSize);

// ------------------------------------------------------------------------------------------------
// World arena
//
// The world hunk is the largest single allocation the program makes - 6.67 MB on
// power2 - and it is allocated and freed on every map change. dlmalloc cannot move
// live blocks, so once a few hundred longer-lived allocations have settled into the
// holes left behind, no contiguous run that big survives.
//
// So the world hunk does not come from the general heap at all. It is carved out of
// one block reserved at startup and never returned, which makes it immune to that
// by construction rather than merely less likely to hit it. The staging buffer the
// streamed loader uses gets the same treatment for the same reason.
//
// Both capacities come from build/tools/bspinfo, which mirrors the sizers here and
// reports the worst case over a map set:
//
//     WORST HUNK   : power2.bsp needs 6991792 bytes (6.67 MB)
//     WORST SCRATCH: lab.bsp    needs  954048 bytes (0.91 MB)
//
// with ~4% on top for maps that are not in pak0. Re-run bspinfo after adding a
// mission pack or custom maps, or after changing any struct the hunk holds; a map
// that does not fit says so and stops, rather than falling back to the heap and
// quietly reintroducing the problem.
// ------------------------------------------------------------------------------------------------

// Alignment of every hunk sub-allocation. Rounding each allocation up keeps the
// running offset aligned, so the pre-pass total is order-independent and matches
// the real run exactly, and leaves vertex arrays qword-aligned for later DMA.
constexpr u32 kHunkAlign = 16;

// Both are the measured worst case plus ~4%. The margin is deliberately thin: every
// byte reserved here is a byte the general heap does not get, and that heap turned
// out to be the tighter of the two - an earlier 10% margin cost 384 KB and moved the
// failure from the world hunk to a 1 MB model load. A map that overruns either
// capacity says so and names the constant to raise, so being wrong is loud.
constexpr u32 kWorldHunkCapacity    = 7100u * 1024u; // 6.94 MB, power2.bsp + 4.0%
constexpr u32 kWorldScratchCapacity = 972u  * 1024u; // 0.95 MB, lab.bsp + 4.3%
constexpr u32 kWorldArenaBytes      = kWorldHunkCapacity + kWorldScratchCapacity;

// Reserved once by ReserveWorldArena, never freed. The hunk occupies the first
// kWorldHunkCapacity bytes and the scratch the rest.
static u8 * s_worldArena = nullptr;

// The most any map has actually needed, for the load-time log line. Worth watching:
// it is the evidence that the capacities above are still right.
static u32 s_hunkPeakUsed    = 0;
static u32 s_scratchPeakUsed = 0;

inline u8 * WorldHunkBase()    { return s_worldArena; }
inline u8 * WorldScratchBase() { return s_worldArena + kWorldHunkCapacity; }

constexpr u32 AlignUp(u32 value, u32 alignment)
{
    return (value + (alignment - 1)) & ~(alignment - 1);
}

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
    // fields, matching ref_gl's Hunk_Alloc semantics). ps2::heap::AllocAligned aborts
    // on OOM, so this always succeeds.
    void Init(u32 sizeBytes, ps2::heap::MemTag tag)
    {
        m_base     = static_cast<u8 *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(kHunkAlign), sizeBytes, tag));
        m_offset   = 0;
        m_capacity = sizeBytes;
        std::memset(m_base, 0, sizeBytes);
    }

    // Points the hunk at the reserved world arena instead of allocating. Only brush
    // models use this - sprites and MD2s are small, short-lived and numerous, which
    // is what the general heap is good at. The caller must not free the result; see
    // ReserveWorldArena and ModelCache::Unload.
    // False when the map does not fit the reservation, which the caller reports the
    // same way as any other malformed-map failure.
    bool InitFromWorldArena(u32 sizeBytes, const char * name)
    {
        PS2_AssertMsg(s_worldArena != nullptr, "World arena used before it was reserved!");

        if (sizeBytes > kWorldHunkCapacity) [[unlikely]]
        {
            Com_Printf("ERROR: LoadBrushModel: '%s' needs a %u KB world hunk but the reserved\n"
                       "       arena is only %u KB. Re-run build/tools/bspinfo over this map set\n"
                       "       and raise kWorldHunkCapacity in model_load.cpp.\n",
                       name, sizeBytes / 1024u, kWorldHunkCapacity / 1024u);
            return false;
        }

        m_base     = WorldHunkBase();
        m_offset   = 0;
        m_capacity = sizeBytes;

        // Only the part this map uses: the loaders rely on zero-initialised fields,
        // and clearing the whole 7.5 MB every load would cost more than it buys.
        std::memset(m_base, 0, sizeBytes);

        if (sizeBytes > s_hunkPeakUsed) { s_hunkPeakUsed = sizeBytes; }
        return true;
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

// A polygon's three arrays (the ModelPoly, its vertices, its triangles) are
// allocated as ONE hunk block rather than three. The hunk rounds every allocation
// up to kHunkAlign, and a typical 5-edge face has 3 triangles - 9 bytes rounded to
// 16, wasting more than it stores. Sizer and filler must agree to the byte (see
// the PS2_Assert at the end of LoadBrushModel), so the total lives in one place
// and both call it.
constexpr u32 PolyBlockBytes(const int numVerts, const int numTriangles)
{
    return AlignUp(static_cast<u32>(sizeof(ModelPoly))
                   + (static_cast<u32>(numVerts) * static_cast<u32>(sizeof(PolyVertex)))
                   + (static_cast<u32>(numTriangles) * static_cast<u32>(sizeof(ModelTriangle))),
                   kHunkAlign);
}

// Carves one such block. The block base is kHunkAlign-aligned and ModelPoly and
// PolyVertex are both 4-byte-aligned types laid out at multiples of 4 from it,
// with the byte-sized triangles last, so every member lands legally aligned.
ModelPoly * AllocPolyBlock(HunkAllocator & hunk, const int numVerts, const int numTriangles)
{
    void * const block = hunk.Alloc(PolyBlockBytes(numVerts, numTriangles));
    u8 * const bytes   = static_cast<u8 *>(block);

    ModelPoly * const poly = static_cast<ModelPoly *>(block);
    void * const vertsPtr  = bytes + sizeof(ModelPoly);

    poly->numVerts  = numVerts;
    poly->vertexes  = static_cast<PolyVertex *>(vertsPtr);
    poly->triangles = nullptr;

    if (numTriangles > 0)
    {
        void * const trisPtr = bytes + sizeof(ModelPoly)
                             + (static_cast<u32>(numVerts) * sizeof(PolyVertex));
        poly->triangles = static_cast<ModelTriangle *>(trisPtr);
    }

    return poly;
}

// The five lumps the hunk pre-pass has to walk together to measure the warp-face
// subdivision. Everything else it needs comes from the header's lump lengths.
struct PrePassLumps
{
    const void * faces;
    const void * texInfo;
    const void * surfEdges;
    const void * edges;
    const void * vertexes;
};

// ------------------------------------------------------------------------------------------------
// Streamed BSP reader
//
// A world .bsp is 2-3 MB and the hunk built from it is another 4-7 MB, so reading
// the whole file with FS_LoadFile meant both were resident at once - over 10 MB of
// transient on the biggest map. Worse, the file buffer got carved out of the hole
// the *previous* world hunk had just freed, splitting it in two and leaving neither
// half big enough for the new one.
//
// So lumps are read one at a time through a single scratch buffer instead:
//
//   * one allocation, one free, reused for every lump in between - per-lump
//     alloc/free churn would fragment the very hole this exists to protect;
//   * sized from the header, which carries all 19 lump lengths, so it is exact
//     per map and cannot silently under-run on a custom one;
//   * the two lumps copied verbatim (lighting, visibility) bypass it entirely and
//     are read straight into the hunk - lighting alone reaches 1.4 MB and would
//     otherwise dominate the buffer.
//
// The pre-pass that sizes the hunk needs five lumps at once (see PrePassLumps), so
// the buffer holds those together; every later lump is transformed one at a time,
// each cross-reference resolving against hunk data that is already built. Those
// five are therefore read twice, once to measure and once to fill - a little extra
// I/O in exchange for not holding the file.
// ------------------------------------------------------------------------------------------------

class BspFileReader final
{
public:
    BspFileReader() = default;
    BspFileReader(const BspFileReader &) = delete;
    BspFileReader & operator=(const BspFileReader &) = delete;

    ~BspFileReader() { Close(); }

    // Adopts an already-open file positioned at the .bsp's first byte, validates
    // the header and claims the scratch buffer. Does not take ownership: the
    // caller opened the file to read its format tag and closes it afterwards.
    bool Open(FILE * const file, const char * const name)
    {
        PS2_Assert(file != nullptr);
        m_file = file;

        // Lump offsets are relative to the start of the .bsp, which inside a pak
        // is not the start of the stream - the caller left us seeked there.
        m_baseOffset = std::ftell(m_file);
        if (m_baseOffset < 0)
        {
            Com_Printf("ERROR: LoadBrushModel: Cannot tell position in '%s'!\n", name);
            Close();
            return false;
        }

        FS_Read(&m_header, static_cast<int>(sizeof(m_header)), m_file);

        if (m_header.ident != IDBSPHEADER)
        {
            Com_Printf("ERROR: LoadBrushModel: '%s' has bad file ident!\n", name);
            Close();
            return false;
        }
        if (m_header.version != BSPVERSION)
        {
            Com_Printf("ERROR: LoadBrushModel: '%s' has wrong version (%i should be %i)\n",
                       name, m_header.version, BSPVERSION);
            Close();
            return false;
        }

        for (const auto & l : m_header.lumps)
        {
            if (l.fileofs < 0 || l.filelen < 0)
            {
                Com_Printf("ERROR: LoadBrushModel: '%s' has a negative lump offset/length!\n", name);
                Close();
                return false;
            }
        }

        // Use the arena scratch buffer to avoid fragmentation.
        PS2_AssertMsg(s_worldArena != nullptr, "World arena used before it was reserved!");

        m_scratchSize = RequiredScratchBytes();
        if (m_scratchSize > kWorldScratchCapacity)
        {
            Com_Printf("ERROR: LoadBrushModel: '%s' needs a %u KB lump scratch but the reserved\n"
                       "       arena is only %u KB. Re-run build/tools/bspinfo over this map set\n"
                       "       and raise kWorldScratchCapacity in model_load.cpp.\n",
                       name, m_scratchSize / 1024u, kWorldScratchCapacity / 1024u);
            Close();
            return false;
        }

        m_scratch = WorldScratchBase();
        if (m_scratchSize > s_scratchPeakUsed) { s_scratchPeakUsed = m_scratchSize; }
        return true;
    }

    void Close()
    {
        // Neither resource is ours to release: m_scratch points into the reserved
        // arena, and the file belongs to the caller. Just drop both references so a
        // use-after-close trips on null rather than reading a stale handle.
        m_scratch     = nullptr;
        m_scratchSize = 0;
        m_file        = nullptr;
    }

    const dheader_t & Header() const { return m_header; }

    // How much of the reserved scratch this map actually needs. Valid until Close().
    u32 ScratchSize() const { return m_scratchSize; }

    // Reads one lump into the scratch buffer at 'atOffset' and returns it. The
    // result stays valid until another read overlaps it.
    const void * ReadLump(const int lumpIndex, const u32 atOffset = 0)
    {
        const lump_t & l = m_header.lumps[lumpIndex];
        const u32 len    = static_cast<u32>(l.filelen);

        // The scratch was sized from these same lengths, so overflowing it means
        // the header changed under us or the sizing rule below is wrong.
        PS2_AssertMsg(atOffset + len <= m_scratchSize, "BSP lump overruns the scratch buffer!");

        u8 * const dest = m_scratch + atOffset;
        ReadAt(l, dest);
        return dest;
    }

    // Reads a lump straight to 'dest', bypassing the scratch. For the lumps the
    // loaders copy verbatim into the hunk.
    void ReadLumpInto(const int lumpIndex, void * const dest)
    {
        ReadAt(m_header.lumps[lumpIndex], dest);
    }

    // Fills 'out' with the five pre-pass lumps packed back to back in scratch.
    PrePassLumps ReadPrePassLumps()
    {
        PrePassLumps out{};
        u32 ofs = 0;
        for (int i = 0; i < kNumPrePassLumps; ++i)
        {
            const int lump = kPrePassLumps[i];
            const void * const p = ReadLump(lump, ofs);
            ofs += AlignUp(static_cast<u32>(m_header.lumps[lump].filelen), kHunkAlign);

            switch (lump)
            {
            case LUMP_FACES     : out.faces     = p; break;
            case LUMP_TEXINFO   : out.texInfo   = p; break;
            case LUMP_SURFEDGES : out.surfEdges = p; break;
            case LUMP_EDGES     : out.edges     = p; break;
            case LUMP_VERTEXES  : out.vertexes  = p; break;
            default             : break;
            }
        }
        return out;
    }

private:
    void ReadAt(const lump_t & l, void * const dest)
    {
        if (l.filelen <= 0)
        {
            return;
        }
        std::fseek(m_file, m_baseOffset + l.fileofs, SEEK_SET);
        FS_Read(dest, l.filelen, m_file);
    }

    // The five the pre-pass walks together, and every lump read through the
    // scratch afterwards. Lighting and visibility are absent from both: they go
    // straight into the hunk.
    static constexpr int kNumPrePassLumps = 5;
    static constexpr int kPrePassLumps[kNumPrePassLumps] = {
        LUMP_FACES, LUMP_TEXINFO, LUMP_SURFEDGES, LUMP_EDGES, LUMP_VERTEXES
    };
    static constexpr int kStreamedLumps[] = {
        LUMP_FACES, LUMP_TEXINFO, LUMP_SURFEDGES, LUMP_EDGES, LUMP_VERTEXES,
        LUMP_PLANES, LUMP_LEAFFACES, LUMP_LEAFS, LUMP_NODES, LUMP_MODELS
    };

    // Big enough for the pre-pass set held together, and for the largest single
    // lump transformed after it - whichever is larger. Measured over the stock
    // maps this peaks at ~0.91 MB (lab.bsp), against a 3.1 MB whole-file read.
    u32 RequiredScratchBytes() const
    {
        u32 prePassTotal = 0;
        for (int i = 0; i < kNumPrePassLumps; ++i)
        {
            prePassTotal += AlignUp(static_cast<u32>(m_header.lumps[kPrePassLumps[i]].filelen), kHunkAlign);
        }

        u32 largestSingle = 0;
        for (const int lump : kStreamedLumps)
        {
            const u32 len = AlignUp(static_cast<u32>(m_header.lumps[lump].filelen), kHunkAlign);
            if (len > largestSingle) { largestSingle = len; }
        }

        const u32 needed = (prePassTotal > largestSingle) ? prePassTotal : largestSingle;
        return (needed != 0u) ? needed : kHunkAlign; // never a zero-size allocation
    }

    FILE *    m_file        = nullptr;
    long      m_baseOffset  = 0;
    dheader_t m_header      = {};
    u8 *      m_scratch     = nullptr;
    u32       m_scratchSize = 0;
};

// ------------------------------------------------------------------------------------------------
// Small geometry helpers
// ------------------------------------------------------------------------------------------------

// Views a lump's bytes as its element type. The loaders are handed the lump's own
// buffer rather than the whole file (see BspFileReader), so this is a plain cast -
// through void* so the higher-alignment result doesn't trip -Wcast-align (the
// scratch buffer is aligned well past any of these structs).
template<typename T>
inline const T * LumpAs(const void * const lumpData)
{
    return static_cast<const T *>(lumpData);
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

inline u16 ToU16(const int value)
{
    if (value < 0 || value > UINT16_MAX) [[unlikely]]
    {
        Sys_Error("%i cannot be represented as u16!", value);
    }
    return static_cast<u16>(value);
}

inline s16 ToS16(const int value)
{
    if (value < INT16_MIN || value > INT16_MAX) [[unlikely]]
    {
        Sys_Error("%i cannot be represented as s16!", value);
    }
    return static_cast<s16>(value);
}

// ------------------------------------------------------------------------------------------------
// Brush model lumps
// ------------------------------------------------------------------------------------------------

void LoadVertexes(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    const auto * in = LumpAs<dvertex_t>(lumpData);
    const int count = LumpElemCount<dvertex_t>(l);

    ModelVertex * out = hunk.AllocArray<ModelVertex>(count);
    mdl.vertexes    = out;
    mdl.numVertexes = ToU16(count);

    for (int i = 0; i < count; ++i)
    {
        out[i].position = ToVec3(in[i].point);
    }
}

void LoadEdges(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    const auto * in = LumpAs<dedge_t>(lumpData);
    const int count = LumpElemCount<dedge_t>(l);

    // One extra sentinel edge, matching ref_gl.
    ModelEdge * out = hunk.AllocArray<ModelEdge>(count + 1);
    mdl.edges    = out;
    mdl.numEdges = ToU16(count);

    for (int i = 0; i < count; ++i)
    {
        out[i].v[0] = in[i].v[0];
        out[i].v[1] = in[i].v[1];
    }
}

void LoadSurfEdges(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    const int * in  = LumpAs<int>(lumpData);
    const int count = LumpElemCount<int>(l);

    int * out = hunk.AllocArray<int>(count);
    mdl.surfEdges    = out;
    mdl.numSurfEdges = ToU16(count);

    std::memcpy(out, in, static_cast<size_t>(count) * sizeof(int));
}

// Lighting is copied byte-for-byte, so it is read from the file straight into its
// hunk slot - it never passes through the scratch buffer. It is the largest lump
// on most maps (1.4 MB on space.bsp) and would otherwise size the scratch alone.
void LoadLightingInto(ModelInstance & mdl, HunkAllocator & hunk, BspFileReader & bsp, const lump_t & l)
{
    if (l.filelen <= 0)
    {
        mdl.lightData = nullptr;
        return;
    }

    mdl.lightData = hunk.AllocArray<u8>(l.filelen);
    bsp.ReadLumpInto(LUMP_LIGHTING, mdl.lightData);
}

void LoadPlanes(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    const auto * in = LumpAs<dplane_t>(lumpData);
    const int count = LumpElemCount<dplane_t>(l);

    // NOTE:
    // ref_gl allocates count*2 here and only ever fills count - the second half is
    // never written and never read, since numPlanes bounds every consumer. It looks
    // like it was meant to back opposite planes that were never implemented.
    cplane_t * out = hunk.AllocArray<cplane_t>(count);
    mdl.planes    = out;
    mdl.numPlanes = ToU16(count);

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

void LoadTexInfo(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    const auto * in = LumpAs<textureinfo_t>(lumpData);
    const int count = LumpElemCount<textureinfo_t>(l);

    ModelTexInfo * out = hunk.AllocArray<ModelTexInfo>(count);
    mdl.texInfos    = out;
    mdl.numTexInfos = ToU16(count);

    for (int i = 0; i < count; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            out[i].vecs[0][j] = in[i].vecs[0][j];
            out[i].vecs[1][j] = in[i].vecs[1][j];
        }

        out[i].flags = ToU16(in[i].flags);

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
            PS2_Assert(base->numFrames < UINT16_MAX);
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
// The sum is taken relative to the first vertex. The identity holds about any origin
// (the extra terms cancel around a closed loop), but world coordinates run to a few
// thousand units while a face's own area is comparatively tiny, so summing absolute
// positions is catastrophic cancellation: 24 mantissa bits leave the result of a
// small far-from-origin face with barely any significant digits.
Vec3 ComputePolygonNormal(const ModelPoly & poly)
{
    const Vec3 origin = poly.vertexes[0].position;

    Vec3 normal = { 0.0f, 0.0f, 0.0f };
    for (int v = 0; v < poly.numVerts; ++v)
    {
        const int vNext = (v + 1) % poly.numVerts;
        normal = normal + math::Cross(poly.vertexes[v].position - origin,
                                      poly.vertexes[vNext].position - origin);
    }
    return math::Normalize(normal);
}

// Two vertexes closer together than this are the same point as far as the
// triangulation is concerned; the direction between them cannot be normalized.
inline bool IsDegenerateEdge(const Vec3 & v)
{
    constexpr float kMinEdgeLengthSqrd = 1e-8f;
    return math::Dot(v, v) < kMinEdgeLengthSqrd;
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
    // Two corners in the same place: a zero-area candidate. It has to be handled
    // before anything is normalized, because qbsp's T-junction fixup does leave
    // repeated vertexes in face windings, and math::Normalize of a zero-length
    // vector is 1/sqrt(0). On a PC that is an infinity and every comparison below
    // it goes false, which happens to be harmless; the EE's FPU has no infinities
    // or NaNs, so it saturates to FLT_MAX and the vector comes back as (0,0,0),
    // which then quietly passes the "inside the triangle" test and vetoes ears
    // that are perfectly good. Clipping the zero-area ear instead is harmless (it
    // rasterizes to nothing) and it retires the duplicate vertex, so the sweep
    // makes progress rather than walking the whole ring and giving up.
    if (IsDegenerateEdge(p2 - p1) || IsDegenerateEdge(p3 - p2) || IsDegenerateEdge(p1 - p3))
    {
        return true;
    }

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

            // Same reason: a vertex duplicating one of the corners sits on the
            // boundary, not inside, and has no direction to normalize.
            if (IsDegenerateEdge(pv - p1) || IsDegenerateEdge(pv - p2) || IsDegenerateEdge(pv - p3))
            {
                continue;
            }

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
        trisPtr->vertexes[0] = static_cast<u8>(v0);
        trisPtr->vertexes[1] = static_cast<u8>(v1);
        trisPtr->vertexes[2] = static_cast<u8>(v2);
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

    // Not a hard error. A winding that retraces itself - qbsp's T-junction fixup
    // leaves a couple of those per map, a spike out to a vertex and straight back -
    // simply has fewer than numVerts - 2 non-degenerate triangles in it, so the
    // count can legitimately come up short. What was emitted still covers the
    // polygon's area; the unused triangles stay zeroed and draw nothing.
    if (triesDone != numTriangles)
    {
        Com_DPrintf("TriangulatePolygon: %i of %i triangles from a %i-vert polygon (degenerate winding).\n",
                    triesDone, numTriangles, numVerts);
    }
}

void BuildPolygonFromSurface(ModelInstance & mdl, HunkAllocator & hunk, ModelSurface & surf)
{
    PS2_Assert(mdl.vertexes != nullptr && mdl.edges != nullptr && mdl.surfEdges != nullptr);

    const int numVerts     = surf.numEdges;
    const int numTriangles = (numVerts >= 3) ? (numVerts - 2) : 0;

    ModelPoly * poly = AllocPolyBlock(hunk, numVerts, numTriangles);
    poly->next = surf.polys;
    surf.polys = poly;

    const ModelTexInfo * const tex = surf.texInfo;
    // The size the texture had on disk: a non-power-of-two wall was stretched
    // to the next power of two on load, and dividing by the stretched size
    // would tile it every 256 world units where the map wants 240 (see
    // tex::Texture::srcWidth).
    const float texW = static_cast<float>(tex->texture->srcWidth);
    const float texH = static_cast<float>(tex->texture->srcHeight);

    for (int i = 0; i < numVerts; ++i)
    {
        const Vec3 & pos = EdgeVertex(mdl, mdl.surfEdges[surf.firstEdge + i]);
        poly->vertexes[i].position = pos;

        // Colour texture coordinates.
        poly->vertexes[i].texture_s = TexProject(pos, tex->vecs[0]) / texW;
        poly->vertexes[i].texture_t = TexProject(pos, tex->vecs[1]) / texH;

        // Lightmap texture coordinates. The vertex projects into texture space
        // the same way, then shifts to where CreateSurfaceLightmap packed this
        // surface's luxels: relative to the face's own lightmap origin
        // (- textureMins), over to its block in the atlas (+ light_s/t luxels),
        // and half a luxel in so bilinear sampling lands on texel centres
        // instead of block edges. The atlas spans kLuxelSizeUnits world units
        // per luxel, which is what normalises the whole thing.
        constexpr float kHalfLuxel  = static_cast<float>(lm::kLuxelSizeUnits) * 0.5f;
        constexpr float kAtlasSpanS = static_cast<float>(lm::kLightmapTextureWidth  * lm::kLuxelSizeUnits);
        constexpr float kAtlasSpanT = static_cast<float>(lm::kLightmapTextureHeight * lm::kLuxelSizeUnits);

        const float lms = TexProject(pos, tex->vecs[0]) - static_cast<float>(surf.textureMins[0])
                        + static_cast<float>(surf.light_s * lm::kLuxelSizeUnits) + kHalfLuxel;
        const float lmt = TexProject(pos, tex->vecs[1]) - static_cast<float>(surf.textureMins[1])
                        + static_cast<float>(surf.light_t * lm::kLuxelSizeUnits) + kHalfLuxel;

        poly->vertexes[i].lightmap_s = lms / kAtlasSpanS;
        poly->vertexes[i].lightmap_t = lmt / kAtlasSpanT;
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
    if (numVerts > kSubdivideSize - 4) [[unlikely]]
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
    if (count < 0) [[unlikely]]
    {
        Sys_Error("SubdivideSurface: Max verts exceeded!");
    }

    const ModelTexInfo * const tex = surf.texInfo;

    SubdividePolygon(count, verts, [&](int numLeafVerts, const Vec3 * leafVerts)
    {
        // +2: a center point (for the warp fan) plus a duplicate of the first.
        // No triangles: warped polygons are drawn as fans, not triangulated,
        // so AllocPolyBlock leaves poly->triangles null.
        ModelPoly * poly = AllocPolyBlock(hunk, numLeafVerts + 2, 0);
        poly->next = surf.polys;
        surf.polys = poly;

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

void LoadFaces(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    PS2_Assert(mdl.planes != nullptr && mdl.texInfos != nullptr); // Load these first.

    const auto * in = LumpAs<dface_t>(lumpData);
    const int count = LumpElemCount<dface_t>(l);

    ModelSurface * out = hunk.AllocArray<ModelSurface>(count);
    mdl.surfaces    = out;
    mdl.numSurfaces = ToU16(count);

    // Drops the previous map's lightmap atlases and opens a fresh one to pack
    // this map's faces into (ref_gl's GL_BeginBuildingLightmaps).
    lm::BeginBuildingLightmaps();

    for (int surfNum = 0; surfNum < count; ++surfNum)
    {
        ModelSurface & surf = out[surfNum];
        surf.firstEdge = in[surfNum].firstedge;
        surf.numEdges  = in[surfNum].numedges;
        surf.flags     = SurfaceFlags::None;
        surf.polys     = nullptr;
        surf.lightmapTextureNum = kNotLightmapped;

        if (in[surfNum].side)
        {
            surf.flags = surf.flags | SurfaceFlags::PlaneBack;
        }
        surf.plane = mdl.planes + in[surfNum].planenum;

        const int texNum = in[surfNum].texinfo;
        if (texNum < 0 || texNum >= mdl.numTexInfos) [[unlikely]]
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

        // Pack the surface's luxels into a lightmap atlas before its polygon is
        // built: BuildPolygonFromSurface bakes where they landed into the second
        // UV set, so this has to run first. Sky, turbulent and translucent faces
        // are never lightmapped and keep kNotLightmapped - the same exclusion
        // ref_gl's Mod_LoadFaces makes.
        constexpr int kUnlitSurfaceFlags = (SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP);
        if (!(surf.texInfo->flags & kUnlitSurfaceFlags))
        {
            lm::CreateSurfaceLightmap(surf);
        }

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

    lm::EndBuildingLightmaps();
}

void LoadMarkSurfaces(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    PS2_Assert(mdl.surfaces != nullptr); // Load faces first.

    const auto * in = LumpAs<s16>(lumpData);
    const int count = LumpElemCount<s16>(l);

    ModelSurface ** out = hunk.AllocArray<ModelSurface *>(count);
    mdl.markSurfaces    = out;
    mdl.numMarkSurfaces = ToU16(count);

    for (int i = 0; i < count; ++i)
    {
        const int j = in[i];
        if (j < 0 || j >= mdl.numSurfaces) [[unlikely]]
        {
            Sys_Error("LoadMarkSurfaces: Bad surface number: %i", j);
        }
        out[i] = mdl.surfaces + j;
    }
}

void LoadLeafs(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    PS2_Assert(mdl.markSurfaces != nullptr); // Load mark surfaces first.

    const auto * in = LumpAs<dleaf_t>(lumpData);
    const int count = LumpElemCount<dleaf_t>(l);

    ModelLeaf * out = hunk.AllocArray<ModelLeaf>(count);
    mdl.leafs    = out;
    mdl.numLeafs = ToU16(count);

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

    // MarkLeaves indexes CM_ClusterPVS rows by these cluster numbers, so the
    // collision model has to be holding the same map we are. The load order
    // guarantees it (SV_SpawnServer runs CM_LoadMap before CL_PrepRefresh gets
    // here), and this is what catches it if that ever stops being true: both
    // counts come from this same lump, so they cannot legitimately disagree.
#if PS2_QUAKE_ASSERTS
    int maxCluster = -1;
    for (int i = 0; i < count; ++i)
    {
        if (out[i].cluster > maxCluster) { maxCluster = out[i].cluster; }
    }
    PS2_AssertMsg(maxCluster + 1 == CM_NumClusters(),
                  "World leafs and the collision model disagree on the cluster count!");
#endif // PS2_QUAKE_ASSERTS
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

void LoadNodes(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    PS2_Assert(mdl.planes != nullptr && mdl.leafs != nullptr); // Load these first.

    const auto * in = LumpAs<dnode_t>(lumpData);
    const int count = LumpElemCount<dnode_t>(l);

    ModelNode * out = hunk.AllocArray<ModelNode>(count);
    mdl.nodes    = out;
    mdl.numNodes = ToU16(count);

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

void LoadSubModels(ModelInstance & mdl, HunkAllocator & hunk, const void * const lumpData, const lump_t & l)
{
    const auto * in = LumpAs<dmodel_t>(lumpData);
    const int count = LumpElemCount<dmodel_t>(l);

    SubModelInfo * out = hunk.AllocArray<SubModelInfo>(count);
    mdl.subModels    = out;
    mdl.numSubModels = ToU16(count);

    for (int i = 0; i < count; ++i)
    {
        // Spread the bounds by a unit, matching ref_gl.
        out[i].mins   = { in[i].mins[0] - 1.0f, in[i].mins[1] - 1.0f, in[i].mins[2] - 1.0f };
        out[i].maxs   = { in[i].maxs[0] + 1.0f, in[i].maxs[1] + 1.0f, in[i].maxs[2] + 1.0f };
        out[i].origin = ToVec3(in[i].origin);

        out[i].radius    = RadiusFromBounds(out[i].mins, out[i].maxs);
        out[i].headNode  = ToS16(in[i].headnode);
        out[i].firstFace = ToU16(in[i].firstface);
        out[i].numFaces  = ToU16(in[i].numfaces);
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

bool ComputeBrushHunkSize(const dheader_t * header, const PrePassLumps & pre, const char * name, u32 & outSize)
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
    m.AddArray<cplane_t>(numPlanes);

    const int numTexInfos = CheckedLumpCount(header->lumps[LUMP_TEXINFO], sizeof(textureinfo_t), "texinfo", name);
    if (numTexInfos < 0) { return false; }
    m.AddArray<ModelTexInfo>(numTexInfos);

    const int numFaces = CheckedLumpCount(header->lumps[LUMP_FACES], sizeof(dface_t), "faces", name);
    if (numFaces < 0) { return false; }
    m.AddArray<ModelSurface>(numFaces);

    // Per-face polygon memory. Predictable for normal faces (from numedges);
    // warped faces are measured by running the shared subdivision recursion.
    {
        const auto * faces     = LumpAs<dface_t>(pre.faces);
        const auto * texInfos  = LumpAs<textureinfo_t>(pre.texInfo);
        const auto * surfEdges = LumpAs<int>(pre.surfEdges);
        const auto * edges     = LumpAs<dedge_t>(pre.edges);
        const auto * verts     = LumpAs<dvertex_t>(pre.vertexes);

        for (int f = 0; f < numFaces; ++f)
        {
            const int numEdgesForFace = faces[f].numedges;
            const int texNum = faces[f].texinfo;
            const bool warp  = (texNum >= 0 && texNum < numTexInfos) &&
                               (texInfos[texNum].flags & SURF_WARP);

            if (!warp)
            {
                const int numTris = (numEdgesForFace >= 3) ? (numEdgesForFace - 2) : 0;
                m.Add(PolyBlockBytes(numEdgesForFace, numTris));
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
                m.Add(PolyBlockBytes(numLeafVerts + 2, 0));
            });
        }
    }

    const int numMarkSurfaces = CheckedLumpCount(header->lumps[LUMP_LEAFFACES], sizeof(s16), "leaffaces", name);
    if (numMarkSurfaces < 0) { return false; }
    m.AddArray<ModelSurface *>(numMarkSurfaces);

    // No term for LUMP_VISIBILITY: cmodel.c holds that lump and the view walk
    // reads it through CM_ClusterPVS (see MarkLeaves), so it is not in the hunk.

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
// WORLD MODEL SCRATCH ARENA
// ------------------------------------------------------------------------------------------------

void ReserveWorldArena()
{
    PS2_AssertMsg(s_worldArena == nullptr, "ReserveWorldArena called twice!");

    s_worldArena = static_cast<u8 *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(kHunkAlign), kWorldArenaBytes, ps2::heap::MemTag::WorldMdl));

    Com_DPrintf("World arena reserved: %u KB (%u KB hunk + %u KB lump scratch), "
                "held for the life of the program.\n",
                kWorldArenaBytes / 1024u, kWorldHunkCapacity / 1024u, kWorldScratchCapacity / 1024u);
}

bool IsWorldArenaBlock(const void * const ptr)
{
    return ptr != nullptr && ptr == static_cast<const void *>(s_worldArena);
}

// ------------------------------------------------------------------------------------------------
// BRUSH MODELS (WORLD MAP)
// ------------------------------------------------------------------------------------------------

bool LoadBrushModel(ModelInstance & mdl, FILE * const file, const char * const fileName)
{
    PS2_Assert(file != nullptr && fileName != nullptr);

    // The view walk reads this map's PVS out of the collision model rather than
    // keeping its own copy (see MarkLeaves), so the two have to be the same .bsp.
    // The load order gives us that - SV_SpawnServer runs CM_LoadMap before the
    // client ever reaches CL_PrepRefresh - but "ever" has been wrong once already:
    // clearing cl.refresh_prepped while the client was still ca_active made
    // CL_Frame re-prep the *old* map on the spot, halfway through spawning a new
    // server. Checking here costs a strcmp and names the problem, instead of
    // surfacing a second later as mismatched cluster counts in LoadLeafs.
    PS2_AssertMsg(std::strcmp(fileName, CM_MapName()) == 0,
                  "Loading a world model the collision model is not holding!");

    BspFileReader bsp{};
    if (!bsp.Open(file, fileName))
    {
        return false; // Open() has already reported why.
    }

    const dheader_t & header = bsp.Header();

    // Pass 1: measure. Needs the five geometry lumps together, since the warp
    // faces are sized by actually running the subdivision over their vertices.
    u32 hunkSize = 0;
    {
        const PrePassLumps pre = bsp.ReadPrePassLumps();
        if (!ComputeBrushHunkSize(&header, pre, mdl.name, hunkSize))
        {
            return false;
        }
    }

    HunkAllocator hunk{};
    if (!hunk.InitFromWorldArena(hunkSize, mdl.name))
    {
        return false;
    }
    mdl.hunkBase = hunk.Base();
    mdl.hunkSize = hunkSize;
    mdl.type     = ModelType::Brush;

    // Pass 2: fill, one lump at a time through the scratch. Order matters -
    // several lumps reference earlier ones - but every such reference resolves
    // against hunk data that is already built, never against another lump still
    // in the file, which is what lets a single buffer serve them in turn.
    // LUMP_LIGHTING is a verbatim copy, read straight into the hunk and skipping
    // the scratch entirely; it keeps its original slot in the sequence because
    // LoadFaces resolves surf.samples against mdl.lightData, which therefore has
    // to be in place before it runs. LUMP_VISIBILITY is not read at all any more -
    // cmodel.c owns it (see MarkLeaves).
    LoadVertexes(mdl, hunk, bsp.ReadLump(LUMP_VERTEXES), header.lumps[LUMP_VERTEXES]);
    LoadEdges(mdl, hunk, bsp.ReadLump(LUMP_EDGES), header.lumps[LUMP_EDGES]);
    LoadSurfEdges(mdl, hunk, bsp.ReadLump(LUMP_SURFEDGES), header.lumps[LUMP_SURFEDGES]);
    LoadLightingInto(mdl, hunk, bsp, header.lumps[LUMP_LIGHTING]);
    LoadPlanes(mdl, hunk, bsp.ReadLump(LUMP_PLANES), header.lumps[LUMP_PLANES]);
    LoadTexInfo(mdl, hunk, bsp.ReadLump(LUMP_TEXINFO), header.lumps[LUMP_TEXINFO]);
    LoadFaces(mdl, hunk, bsp.ReadLump(LUMP_FACES), header.lumps[LUMP_FACES]);
    LoadMarkSurfaces(mdl, hunk, bsp.ReadLump(LUMP_LEAFFACES), header.lumps[LUMP_LEAFFACES]);
    LoadLeafs(mdl, hunk, bsp.ReadLump(LUMP_LEAFS), header.lumps[LUMP_LEAFS]);
    LoadNodes(mdl, hunk, bsp.ReadLump(LUMP_NODES), header.lumps[LUMP_NODES]);
    LoadSubModels(mdl, hunk, bsp.ReadLump(LUMP_MODELS), header.lumps[LUMP_MODELS]);

    mdl.numFrames = 2; // Regular and alternate animation.

    // The hunk was sized to exactly what we allocate; assert we did not drift.
    PS2_Assert(hunk.BytesUsed() == hunkSize);

    // Always logged, not just under kVerboseModelLoading: these numbers against their
    // capacities are the running evidence that the reservations are still sized
    // right, and a full map cycle is the only thing that produces them.
    Com_Printf("Brush model '%s': hunk %u/%u KB (peak %u), scratch %u/%u KB (peak %u).\n",
               mdl.name,
               hunkSize / 1024u, kWorldHunkCapacity / 1024u, s_hunkPeakUsed / 1024u,
               bsp.ScratchSize() / 1024u, kWorldScratchCapacity / 1024u, s_scratchPeakUsed / 1024u);
    return true;
}

// ------------------------------------------------------------------------------------------------
// SPRITE MODELS
// ------------------------------------------------------------------------------------------------

bool LoadSpriteModel(ModelInstance & mdl, FILE * const file, const int fileLen)
{
    PS2_Assert(file != nullptr);

    if (fileLen < static_cast<int>(sizeof(dsprite_t)))
    {
        Com_Printf("ERROR: Sprite '%s' is too small to hold a header (%i bytes)\n", mdl.name, fileLen);
        return false;
    }

    // The header first, so the file is only committed to a hunk once it is known
    // to be a sprite we can use.
    const long base = std::ftell(file);
    dsprite_t header{};
    FS_Read(&header, static_cast<int>(sizeof(header)), file);

    if (header.version != SPRITE_VERSION)
    {
        Com_Printf("ERROR: Sprite '%s' has wrong version (%i should be %i)\n",
                   mdl.name, header.version, SPRITE_VERSION);
        return false;
    }
    if (header.numframes < 0 || header.numframes > kMaxMD2Skins)
    {
        Com_Printf("ERROR: Sprite '%s' has bad frame count (%i)\n", mdl.name, header.numframes);
        return false;
    }

    // A sprite needs no decoding, so the hunk holds the file exactly as it is on
    // disk and the read lands directly in its final home - no staging copy.
    const u32 hunkSize = AlignUp(static_cast<u32>(fileLen), kHunkAlign);

    HunkAllocator hunk{};
    hunk.Init(hunkSize, ps2::heap::MemTag::SpriteMdl);
    mdl.hunkBase = hunk.Base();
    mdl.hunkSize = hunkSize;
    mdl.type     = ModelType::Sprite;

    auto * out = static_cast<dsprite_t *>(hunk.Alloc(static_cast<u32>(fileLen)));
    if (std::fseek(file, base, SEEK_SET) != 0)
    {
        Com_Printf("ERROR: Sprite '%s': cannot rewind to the start of the model\n", mdl.name);
        return false; // The caller's Unload releases the hunk.
    }
    FS_Read(out, fileLen, file);

    for (int i = 0; i < out->numframes; ++i)
    {
        mdl.skins[i] = tex::Find(out->frames[i].name, tex::ImageType::Sprite);
    }
    mdl.numFrames = ToU16(out->numframes);

    if (kVerboseModelLoading)
    {
        Com_DPrintf("Sprite model '%s' loaded (%u KB hunk, read in place).\n", mdl.name, hunkSize / 1024u);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// ALIAS MD2 MODELS
// ------------------------------------------------------------------------------------------------

bool LoadAliasMD2Model(ModelInstance & mdl, FILE * const file, const int fileLen)
{
    PS2_Assert(file != nullptr);

    if (fileLen < static_cast<int>(sizeof(dmdl_t)))
    {
        Com_Printf("ERROR: Model '%s' is too small to hold a header (%i bytes)\n", mdl.name, fileLen);
        return false;
    }

    // Read and validate the header before committing a hunk to it. Every offset
    // below is trusted afterwards, so this is the only place they are checked.
    const long base = std::ftell(file);
    dmdl_t header{};
    FS_Read(&header, static_cast<int>(sizeof(header)), file);

    if (header.version != ALIAS_VERSION)
    {
        Com_Printf("ERROR: Model '%s' has wrong version (%i should be %i)\n",
                   mdl.name, header.version, ALIAS_VERSION);
        return false;
    }
    if (header.ofs_end <= 0 || header.ofs_end > fileLen)
    {
        Com_Printf("ERROR: Model '%s' has a bad end offset!\n", mdl.name);
        return false;
    }
    if (header.skinheight > kMaxMD2SkinHeight)
    {
        Com_Printf("ERROR: Model '%s' has a skin taller than %i.\n", mdl.name, kMaxMD2SkinHeight);
        return false;
    }
    if (header.num_xyz <= 0 || header.num_xyz > MAX_VERTS)
    {
        Com_Printf("ERROR: Model '%s' has a bad vertex count (%i)!\n", mdl.name, header.num_xyz);
        return false;
    }
    if (header.num_st <= 0 || header.num_tris <= 0 || header.num_frames <= 0)
    {
        Com_Printf("ERROR: Model '%s' has no st verts / triangles / frames!\n", mdl.name);
        return false;
    }
    if (header.num_skins < 0 || header.num_skins > kMaxMD2Skins)
    {
        Com_Printf("ERROR: Model '%s' has a bad skin count (%i)!\n", mdl.name, header.num_skins);
        return false;
    }

    // MD2 needs no per-field expansion (no byte swap on the EE), so the hunk holds
    // the file verbatim up to ofs_end and the read lands straight in it. The
    // biggest MD2 in pak0 is just under 1 MB; staging it through a second buffer
    // used to put 2 MB of the general heap under a single model load.
    const u32 hunkSize = AlignUp(static_cast<u32>(header.ofs_end), kHunkAlign);

    HunkAllocator hunk{};
    hunk.Init(hunkSize, ps2::heap::MemTag::AliasMdl);
    mdl.hunkBase = hunk.Base();
    mdl.hunkSize = hunkSize;
    mdl.type     = ModelType::AliasMD2;

    auto * out = static_cast<dmdl_t *>(hunk.Alloc(static_cast<u32>(header.ofs_end)));
    if (std::fseek(file, base, SEEK_SET) != 0)
    {
        Com_Printf("ERROR: Model '%s': cannot rewind to the start of the model\n", mdl.name);
        return false; // The caller's Unload releases the hunk.
    }
    FS_Read(out, header.ofs_end, file);

    // Default bounds (MD2s carry no bounds; the game clips against these).
    mdl.mins = { -32.0f, -32.0f, -32.0f };
    mdl.maxs = {  32.0f,  32.0f,  32.0f };
    mdl.numFrames = ToU16(out->num_frames);

    for (int i = 0; i < out->num_skins; ++i)
    {
        const char * skinName = reinterpret_cast<const char *>(out) + out->ofs_skins + (i * MAX_SKINNAME);
        mdl.skins[i] = tex::Find(skinName, tex::ImageType::Skin);
    }

    if (kVerboseModelLoading)
    {
        Com_DPrintf("Alias model '%s' loaded (%u KB hunk, read in place).\n", mdl.name, hunkSize / 1024u);
    }
    return true;
}

} // namespace ps2::mod
