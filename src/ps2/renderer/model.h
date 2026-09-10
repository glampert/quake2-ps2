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
#include "ps2/renderer/vu1.h"

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

// Entries in the shared Quake vertex-normal table (client/anorms.h), which every
// keyframe vertex's lightnormalindex indexes. LoadAliasMD2Model clamps to this at
// load, so the draw paths index the normal and shade-color tables unmasked.
constexpr int kNumVertexNormals = 162;

// Widest polygon TriangulatePolygon will accept, per polygon. A wider one is
// refused at load with an error and left with a degenerate triangle list, so it
// draws nothing; the draw paths rely on that to bound their own per-polygon
// working sets by this.
constexpr int kTriangulationMaxVerts = 128;

// The vertex colour a surface bakes into its PolyVertex::rgba, and the one the
// world passes draw an untinted surface with. 128 is the GS's modulate identity
// and 0x80 its 1.0 alpha, so this leaves a texel exactly as it is.
constexpr u32 kFullBrightColor = vu1::PackColorRGBA(128, 128, 128, 0x80);

// Blend alpha for the two translucent surface flags, in GS units (0x80 = 1.0).
// The loader bakes these into the vertices of a SURF_TRANS33/66 surface so the
// deferred alpha pass draws them without a per-batch colour of its own.
constexpr u32 kTrans33Alpha = 42; // 0.33
constexpr u32 kTrans66Alpha = 84; // 0.66

// ModelSurface::lightmapTextureNum when the surface has no lightmap at all -
// sky, turbulent and translucent surfaces, which the lightmap builder skips.
constexpr int kNotLightmapped = -1;

// ------------------------------------------------------------------------------------------------
// In-memory representation of 3D models (world and entities)
// ------------------------------------------------------------------------------------------------

//
// One vertex of an MD2's expanded triangle list, built at load time from the
// model's glcmds (see LoadAliasMD2Model). Three of these per triangle, in
// triangle order, so a draw path walks them linearly with no strip/fan state.
//
// Deliberately the same 16 bytes as vu1::LerpDrawAttrib, with the keyframe index
// where that struct keeps its packed color: the draw loop copies the whole qword
// into the batch's attribute slot and then overwrites lane 0 with the shaded
// color, so the index costs nothing to store and nothing to strip back out.
//
struct alignas(16) AliasVertex
{
    u32 index;  // into the keyframe vertex array; becomes rgba at draw time
    float s, t; // normalized skin coords, exactly as the glcmds held them
    float q;    // always 1.0f
};

static_assert(sizeof(AliasVertex) == 16, "AliasVertex must match vu1::LerpDrawAttrib!");

//
// Vertex format used by ModelPoly - laid out as a vu1::DrawVertex so the world
// passes copy it into a batch rather than building one out of it.
//
// The two sets of texture coordinates are what makes that a tight fit. A world
// vertex needs position (12), diffuse ST (8), colour (4) and lightmap ST (8) -
// exactly 32 bytes, exactly what DrawVertex is. The lightmap pair goes in the two
// lanes DrawVertex holds constants in: the microprograms synthesise both (the MVP
// row is scaled by vf00's hardwired 1.0, and Q comes from the reciprocal, not from
// the vertex), so nothing reads them off the wire.
//
struct alignas(16) PolyVertex
{
    // Model position - the same three floats DrawVertex opens with. Where that
    // struct keeps w, this keeps half of the lightmap coordinate pair.
    Vec3  position;
    float lightmap_s;

    // Baked at load: the luxel chroma this vertex sits on for a lit surface, the
    // surface's flat blend colour for a translucent or turbulent one, and the
    // modulate identity for everything else. Only changes when the surface's
    // luxels are rebaked, which for static lighting is never.
    u32 rgba;

    // Diffuse texture coordinates, and - where DrawVertex keeps q - the other half
    // of the lightmap pair.
    float s, t;
    float lightmap_t;
};

// See comment below on ModelSurface about why we need this.
static_assert(sizeof(PolyVertex) == 32, "Update SZ_POLY_VERTEX in src/tools/bspinfo.cpp!");

//
// Model triangle vertex indexes, into the owning ModelPoly's vertexes[].
// A byte each: TriangulatePolygon refuses polygons above kTriangulationMaxVerts,
// so an index never reaches 128, and there is one of these per triangle of every
// world face - the two bytes saved add up.
//
struct ModelTriangle
{
    u8 vertexes[3];
};

//
// Texture/material description.
//
struct ModelTexInfo
{
    const tex::Texture * texture;
    const ModelTexInfo * next; // Texture animation chain.
    float vecs[2][4];
    u16 flags; // SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP | etc
    u16 numFrames;
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

    // No surfedge range here: only the loader's polygon builders ever wanted it,
    // and they take it as a local off the dface_t (see SurfaceEdges in model_load.cpp).

    // lightmap tex coordinates, in luxels into the atlas - bounded by the
    // lightmap texture dimensions, far inside s16.
    s16 light_s;
    s16 light_t;
    s16 lightmapTextureNum; // kNotLightmapped if the surface has no lightmap.

    s16 textureMins[2]; // signed: turbulent surfaces use negative mins.
    s16 extents[2];

    SurfaceFlags flags; // u8-backed; see the enum.
    u8 styles[kMaxLightmaps];

    // Memo of the clip-volume test below, because the two world passes walk
    // different chains (textureChain and lightmapChain) and so cannot see each
    // other's answer - without it every visible surface is judged against six
    // planes twice a frame. Mutable for the same reason ModelNode::visFrame is:
    // the draw passes hold the surface by const pointer. The bool sits here to
    // land in the byte of padding that followed styles[].
    mutable bool clipVolumeInside;

    ModelPoly * polys; // multiple if warped.
    const ModelSurface * textureChain;
    const ModelSurface * lightmapChain; // next surface sharing this one's lightmap atlas.
    ModelTexInfo * texInfo;

    // World-space bounding sphere over every vertex of every polygon, computed
    // once at load - world geometry never moves. Lets the world passes prove a
    // whole surface sits inside the VU1 clip volume and skip the per-triangle
    // clip judgement for all of its triangles at once; see
    // view::SurfaceInsideClipVolume. Costs 16 bytes on a struct there are over
    // 11,000 of, which the judgement it skips pays back many times over.
    Vec3  boundsCenter;
    float boundsRadius;

    // dynamic lighting info:
    int dlightFrame;
    int dlightBits; // one bit per dlight, so this needs all 32.

    float cachedLight[kMaxLightmaps]; // values currently used in lightmap.
    u8 * samples; // [numstyles * surfsize]

    // s_frameCount when clipVolumeInside above was last computed.
    mutable int clipVolumeFrame;

    // Frame whose dynamic-light contribution is currently baked into this
    // surface's block of the atlas. Non-zero means the atlas holds dlit luxels
    // that must be rebuilt from 'samples' once the light stops touching it.
    int lightmapDynamicFrame;
};

// The world hunk is sized from these two in src/tools/bspinfo.cpp, which cannot
// include this header (it is a host build, 64-bit pointers). Asserted here so a
// layout change breaks the build rather than silently invalidating the world
// arena reservation.
static_assert(sizeof(ModelSurface) == 96, "Update SZ_MODEL_SURFACE in src/tools/bspinfo.cpp!");

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
    u16 numMarkSurfaces;
};

//
// Whole model instance (world or entity or sprite).
//
// A model is exactly one kind once it has loaded, so the per-type fields share
// storage rather than sitting side by side. 'type' is the discriminant, and the
// Brush()/Sprite()/Alias() accessors assert on it - reach for those rather than
// 'payload' directly.
//
struct ModelInstance final
{
    // World geometry. mins/maxs/radius live here because only DrawBrushModel
    // reads them; an alias model derives its bounds from its keyframes and a
    // sprite is sized per-frame at draw time.
    struct BrushData
    {
        // True if from the inline models pool.
        bool isInline;

        u16 firstModelSurface;
        u16 numModelSurfaces;

        // Sizes of the arrays that follow.
        u16 numSubModels;
        u16 numPlanes;
        u16 numLeafs; // Number of visible leafs, not counting 0.
        u16 numNodes;
        s16 firstNode;
        u16 numTexInfos;
        u16 numSurfaces;
        u16 numMarkSurfaces;

        // Arrays sized by the above counts.
        //
        // The BSP's vertex, edge and surfedge lumps are deliberately absent: they
        // exist only so the loader can rebuild face polygons, and the draw paths
        // read the baked PolyVertex instead.
        cplane_s * planes;
        ModelLeaf * leafs;
        ModelNode * nodes;
        ModelTexInfo * texInfos;
        ModelSurface * surfaces;
        ModelSurface ** markSurfaces;

        // No visibility lump here: the collision model already holds it verbatim
        // in map_visibility[], so the view walk asks CM_ClusterPVS instead of
        // carrying a second copy. See MarkLeaves.
        u8 * lightData;

        // Volume occupied by the model graphics.
        float radius;
        Vec3 mins;
        Vec3 maxs;
    };

    // Sprite model. The hunk holds the SP2 file image - these are only the
    // resolved frame textures, so the draw path needs no name lookup.
    struct SpriteData
    {
        const tex::Texture * frames[kMaxMD2Skins];
    };

    // MD2 entity model, converted at load (see model_load.cpp). The hunk holds
    // the data two arrays below point into: the expanded triangle stream
    // and the keyframes.
    struct AliasData
    {
        // Resolved once at load and re-stamped by ReferenceAllTextures, never
        // re-resolved - which is why the skin name strings are not kept.
        const tex::Texture * skins[kMaxMD2Skins];
        u16 numSkins;

        u16 numTris;     // vertexes[] holds numTris * 3
        u16 numXyz;      // vertices per keyframe
        u16 frameStride; // sizeof(daliasframe_t) + numXyz * 4

        AliasVertex * vertexes; // numTris * 3, in triangle order
        const u8 * frames;      // ModelInstance::numFrames daliasframe_t records, verbatim
    };

    // File name with path (must be the first field - game code assumes this).
    char name[MAX_QPATH];

    // Registration number, so we know if it is currently referenced by the level being played.
    u32 regSequence;

    // Model type flag - selects the live payload member below.
    ModelType type;

    // Number of animation frames (usually = 2 for brush models: regular and alternate animation).
    u16 numFrames;

    // Backing store for everything loaded above: one heap block that all the
    // pointers index into, sized up front by a pre-pass and filled by a bump
    // allocator (see model_load.cpp). Freed in one shot on eviction. Only the
    // model that allocated it owns it; inline submodels alias the world model's
    // block and leave hunkBase null so they never double-free.
    void * hunkBase;
    u32 hunkSize;

    // Model variant - 'type' dictates which member is live. Every member is a
    // trivial aggregate, so SmallPool's `slot = {}` zeroes the whole union.
    union Payload
    {
        BrushData  brush;
        SpriteData sprite;
        AliasData  alias;
    };
    Payload payload;

    Q_ALWAYS_INLINE BrushData & Brush() { PS2_Assert(type == ModelType::Brush); return payload.brush; }
    Q_ALWAYS_INLINE const BrushData & Brush() const { PS2_Assert(type == ModelType::Brush); return payload.brush; }

    Q_ALWAYS_INLINE SpriteData & Sprite() { PS2_Assert(type == ModelType::Sprite); return payload.sprite; }
    Q_ALWAYS_INLINE const SpriteData & Sprite() const { PS2_Assert(type == ModelType::Sprite); return payload.sprite; }

    Q_ALWAYS_INLINE AliasData & Alias() { PS2_Assert(type == ModelType::AliasMD2); return payload.alias; }
    Q_ALWAYS_INLINE const AliasData & Alias() const { PS2_Assert(type == ModelType::AliasMD2); return payload.alias; }
};

// Asserted rather than commented because the model cache holds 544 of these (320
// pool slots plus MAX_MAP_MODELS inline submodels). A jump here means a field landed
// outside the union that should have been inside one.
static_assert(sizeof(ModelInstance) == 224, "Unexpected ModelInstance size!");

// Narrowing conversions that refuse to truncate. The BSP counts and indices they
// guard are validated at load, so a failure here is a malformed map rather than
// something to recover from.
Q_ALWAYS_INLINE u16 ToU16(const int value)
{
    if (value < 0 || value > UINT16_MAX) [[unlikely]]
    {
        Sys_Error("%i cannot be represented as u16!", value);
    }
    return static_cast<u16>(value);
}

Q_ALWAYS_INLINE s16 ToS16(const int value)
{
    if (value < INT16_MIN || value > INT16_MAX) [[unlikely]]
    {
        Sys_Error("%i cannot be represented as s16!", value);
    }
    return static_cast<s16>(value);
}

// Bounding-sphere radius of an axis-aligned box about the origin. Shared because
// the loader bakes it into brush models and the inline-model setup derives it for
// each submodel straight off the BSP lump.
Q_ALWAYS_INLINE float RadiusFromBounds(const Vec3 & mins, const Vec3 & maxs)
{
    const Vec3 corner = {
        math::Maxf(math::Fabsf(mins.x), math::Fabsf(maxs.x)),
        math::Maxf(math::Fabsf(mins.y), math::Fabsf(maxs.y)),
        math::Maxf(math::Fabsf(mins.z), math::Fabsf(maxs.z)),
    };
    return math::Length(corner);
}

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
