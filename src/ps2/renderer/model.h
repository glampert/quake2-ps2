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

struct cplane_s;
struct dvis_s;

namespace ps2::mod {

// ------------------------------------------------------------------------------------------------
// Misc constants / flags
// ------------------------------------------------------------------------------------------------

using Vec3 = math::Vec3;

enum class PlaneSides : u32
{
    Front,
    Back,
    On,
};

enum class SurfaceFlags : u32
{
    // Misc surface flags (same values used by ref_gl)
    PlaneBack      = 2,
    DrawSky        = 4,
    DrawTurb       = 16,
    DrawBackground = 64,
    Underwater     = 128,
};

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
// Model triangle vertex indexes.
// Limited to 16bits to save space.
//
struct ModelTriangle
{
    u16 vertexes[3];
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
    int flags;
    int numFrames;
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
struct ModelSurface
{
    int visFrame; // should be drawn when node is crossed.

    cplane_s * plane;
    int flags;
    u32 color;

    int firstEdge; // look up in model->surfEdges[], negative numbers are backwards edges.
    int numEdges;

    u16 textureMins[2];
    u16 extents[2];

    // lightmap tex coordinates.
    int light_s;
    int light_t;

    ModelPoly * polys; // multiple if warped.
    const ModelSurface * textureChain;
    ModelTexInfo * texInfo;

    // dynamic lighting info:
    int dlightFrame;
    int dlightBits;

    int lightmapTextureNum; // -1 if not lightmapped.
    u8 styles[kMaxLightmaps];
    float cachedLight[kMaxLightmaps]; // values currently used in lightmap.
    u8 * samples; // [numstyles * surfsize]
};

//
// BSP world node.
//
struct ModelNode
{
    // common with leaf
    int contents; // -1, to differentiate from leafs
    int visFrame; // node needs to be traversed if current

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
    int visFrame; // node needs to be traversed if current

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
    int headNode;
    int visLeafs;
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

    int numFrames;
    int flags;

    // Volume occupied by the model graphics.
    Vec3 mins;
    Vec3 maxs;
    float radius;

    // Solid volume for clipping.
    Vec3 clipMins;
    Vec3 clipMaxs;
    bool clipBox;

    // Brush model.
    int firstModelSurface;
    int numModelSurfaces;
    int lightmap; // Only for submodels.

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

    dvis_s * vis;
    u8 * lightData;

    const tex::Texture * skins[kMaxMD2Skins]; // For alias models and skins.
};

// ------------------------------------------------------------------------------------------------
// Model loading and caching API
// ------------------------------------------------------------------------------------------------

void Init();

void BeginRegistration(const char * map_name);
void EndRegistration();

const ModelInstance * Find(const char * name);

} // namespace ps2::mod
