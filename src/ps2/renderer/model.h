#pragma once
/* ================================================================================================
 * File: model.h
 * Brief: Structures and types representing the in-memory layout
 *        of 3D models / world geometry used by Quake 2.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"
#include "ps2/renderer/texture.h"

#include <tamtypes.h>

namespace ps2::mod {

// ------------------------------------------------------------------------------------------------
// Misc constants / flags
// ------------------------------------------------------------------------------------------------

using Vec3 = math::Vec3;

enum class SurfaceFlags : u8
{
    // Misc surface flags (same values used by ref_gl). These are the renderer's
    // own per-surface flags, distinct from the SURF_* texinfo flags on disk.
    None      = 0,
    PlaneBack = 2,
    DrawSky   = 4,
    DrawTurb  = 16,
};

constexpr SurfaceFlags operator|(SurfaceFlags lhs, SurfaceFlags rhs)
{
    return SurfaceFlags(static_cast<u8>(lhs) | static_cast<u8>(rhs));
}

constexpr bool HasFlag(SurfaceFlags flags, SurfaceFlags test)
{
    return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0;
}

enum class ModelType : u8
{
    Brush,    // World geometry.
    Sprite,   // Sprite model.
    AliasMD2, // MD2/Entity model.
};

constexpr float kBackFaceEpsilon = 0.01f;
constexpr int kSubdivideSize = 64;

// Max height in pixels of MD2 model skins.
constexpr int kMaxMD2SkinHeight = 480;

// From q_files.h
constexpr int kMaxMD2Skins  = 32;
constexpr int kMaxLightmaps = 4;

// ModelSurface::lightmapTextureNum when the surface has no lightmap at all -
// sky, turbulent and translucent surfaces, which the lightmap builder skips.
constexpr int kNotLightmapped = -1;

// ------------------------------------------------------------------------------------------------
// In-memory representation of 3D models (world and entities)
// ------------------------------------------------------------------------------------------------

//
// Vertex format used by ModelPoly.
// Has two sets of texture coordinates for lightmapping.
//
struct PolyVertex
{
    // model vertex position:
    Vec3 position;

    // main tex coords:
    float texture_s;
    float texture_t;

    // lightmap tex coords:
    float lightmap_s;
    float lightmap_t;
};

//
// Model vertex position.
//
struct ModelVertex
{
    Vec3 position;
};

//
// Model triangle vertex indexes, into the owning ModelPoly's vertexes[].
// A byte each: TriangulatePolygon refuses polygons above kTriangulationMaxVerts
// (128), so an index never reaches 128, and there is one of these per triangle
// of every world face - the two bytes saved add up.
//
struct ModelTriangle
{
    u8 vertexes[3];
};

//
// Edge description.
//
struct ModelEdge
{
    u16 v[2]; // Vertex numbers/indexes.
};

//
// Texture/material description.
//
struct ModelTexInfo
{
    float vecs[2][4];
    u16 flags; // SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP | etc
    u16 numFrames;
    const tex::Texture * texture;
    const ModelTexInfo * next; // Texture animation chain.
};

//
// Model polygon/face.
// List links are for draw-time sorting.
//
struct ModelPoly
{
    int numVerts;              // size of vertexes[], since it's dynamically allocated.
    PolyVertex * vertexes;     // array of polygon vertexes. Never null.
    ModelTriangle * triangles; // (numVerts - 2) triangles with indexes into vertexes[].
    ModelPoly * next;
};

//
// Surface description (holds a set of polygons).
//
// There is one of these per world face - over 11,000 on the biggest stock map -
// so the field widths are chosen to pack rather than for uniformity: anything
// that provably fits in 16 bits is s16, and the members are grouped so the
// narrow ones share words instead of each taking one.
//
struct ModelSurface
{
    int visFrame; // should be drawn when node is crossed.
    cplane_s * plane;

    int firstEdge; // look up in model->surfEdges[], negative numbers are backwards edges.
    s16 numEdges;  // dface_t::numedges is a s16 on disk, so this cannot truncate.

    // lightmap tex coordinates, in luxels into the atlas - bounded by the
    // lightmap texture dimensions, far inside s16.
    s16 light_s;
    s16 light_t;
    s16 lightmapTextureNum; // kNotLightmapped if the surface has no lightmap.

    s16 textureMins[2]; // signed: turbulent surfaces use negative mins.
    s16 extents[2];

    SurfaceFlags flags; // u8-backed; see the enum.
    u8 styles[kMaxLightmaps];

    ModelPoly * polys; // multiple if warped.
    const ModelSurface * textureChain;
    const ModelSurface * lightmapChain; // next surface sharing this one's lightmap atlas.
    ModelTexInfo * texInfo;

    // dynamic lighting info:
    int dlightFrame;
    int dlightBits; // one bit per dlight, so this needs all 32.

    float cachedLight[kMaxLightmaps]; // values currently used in lightmap.
    u8 * samples; // [numstyles * surfsize]

    // Frame whose dynamic-light contribution is currently baked into this
    // surface's block of the atlas. Non-zero means the atlas holds dlit luxels
    // that must be rebuilt from 'samples' once the light stops touching it.
    int lightmapDynamicFrame;
};

//
// BSP world node.
//
struct ModelNode
{
    // common with leaf
    int contents; // -1, to differentiate from leafs
    mutable int visFrame; // node needs to be traversed if current

    // for bounding box culling
    float minmaxs[6];

    ModelNode * parent;

    // node specific
    cplane_s  * plane;
    ModelNode * children[2];

    u16 firstSurface;
    u16 numSurfaces;
};

//
// Special BSP leaf node (a draw node).
//
struct ModelLeaf
{
    // common with node
    int contents; // will be a negative contents number
    mutable int visFrame; // node needs to be traversed if current

    // for bounding box culling
    float minmaxs[6];

    ModelNode * parent;

    // leaf specific
    int cluster;
    int area;

    ModelSurface ** firstMarkSurface;
    int numMarkSurfaces;
};

//
// Sub-model mesh information.
//
struct SubModelInfo
{
    Vec3 mins;
    Vec3 maxs;
    Vec3 origin;
    float radius;
    // TODO: Compress these to s16/u16?
    int headNode;
    int firstFace;
    int numFaces;
};

//
// Whole model instance (world or entity or sprite).
//
struct ModelInstance final
{
    // File name with path (must be the first field - game code assumes this).
    char name[MAX_QPATH];

    // Registration number, so we know if it is currently referenced by the level being played.
    u32 regSequence;

    // Model type flag.
    ModelType type;

    // True if from the inline models pool.
    bool isInline;

    // Number of animation frames (usually = 2 for brush models: regular and alternate animation).
    u16 numFrames;

    // Volume occupied by the model graphics.
    float radius;
    Vec3 mins;
    Vec3 maxs;

    // Solid volume for clipping.
    Vec3 clipMins;
    Vec3 clipMaxs;

    // TODO: Compress all counts/indices below to s16/u16 and retune the model hunk size with bspinfo!

    // Brush model.
    int firstModelSurface;
    int numModelSurfaces;

    int numSubModels;
    SubModelInfo * subModels;

    int numPlanes;
    cplane_s * planes;

    int numLeafs; // Number of visible leafs, not counting 0.
    ModelLeaf * leafs;

    int numVertexes;
    ModelVertex * vertexes;

    int numEdges;
    ModelEdge * edges;

    int numNodes;
    int firstNode;
    ModelNode * nodes;

    int numTexInfos;
    ModelTexInfo * texInfos;

    int numSurfaces;
    ModelSurface * surfaces;

    int numSurfEdges;
    int * surfEdges;

    int numMarkSurfaces;
    ModelSurface ** markSurfaces;

    // No visibility lump here: the collision model already holds it verbatim in
    // map_visibility[], so the view walk asks CM_ClusterPVS instead of carrying a
    // second copy. See MarkLeaves.
    u8 * lightData;

    // For alias models and skins.
    const tex::Texture * skins[kMaxMD2Skins];

    // Backing store for everything loaded above: one heap block that all the
    // pointers index into, sized up front by a pre-pass and filled by a bump
    // allocator (see model_load.cpp). Freed in one shot on eviction. Only the
    // model that allocated it owns it; inline submodels alias the world model's
    // block and leave hunkBase null so they never double-free.
    void * hunkBase;
    u32 hunkSize;
};

// ------------------------------------------------------------------------------------------------
// Model loading and caching API
// ------------------------------------------------------------------------------------------------

void Init();

void BeginRegistration(const char * mapName);
void EndRegistration();

const ModelInstance * Find(const char * name);

// Frees the resident world model, unless it is already 'fullName' (a "maps/*.bsp"
// path; null or empty releases unconditionally). BeginRegistration does this
// itself; call it directly to hand the hunk back before the next map's collision
// model is built. GetWorldModel() reads null until the next BeginRegistration.
//
// Returns true only if it actually freed the world. Anything whose lifetime is
// tied to the world - the lightmap atlases, which surfaces index by number - must
// be released on that answer, not alongside the call: on the keep-it path the
// surfaces survive, and so must whatever they point at.
bool ReleaseWorldModel(const char * fullName);

// The world map loaded by the last BeginRegistration; null before any map load.
// NOTE: the view renderer stamps per-frame visibility into the world as it
// draws (node/leaf/surface visFrame fields, per-texture surface chains).
const ModelInstance * GetWorldModel();

} // namespace ps2::mod
