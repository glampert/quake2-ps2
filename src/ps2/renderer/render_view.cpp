/* ================================================================================================
 * File: render_view.cpp
 * Brief: View/3D frame rendering: the world geometry pass behind PS2_RenderFrame.
 *
 *  RenderFrame walks the world BSP for the refdef's camera: MarkLeaves stamps the
 *  nodes reachable from the current PVS cluster, RecursiveWorldNode descends the
 *  tree front-to-back culling against the view frustum and threads every visible
 *  opaque surface onto its texture's draw chain, and DrawTextureChains then
 *  gathers each chain's triangles into a scratch buffer and submits them through
 *  vu1::DrawTriangles - one synchronous batch per texture. Translucent surfaces
 *  are routed aside and drawn back-to-front at the end of the frame by
 *  RenderAlphaSurfaces, and sky surfaces aside to render_sky.cpp, which draws
 *  the skybox behind them once the opaque world is down.
 *
 *  Camera mapping: Quake is Z-up with AngleVectors giving forward/right/up; those
 *  feed math::LookAt directly (its right = cross(up, -forward) lands on Quake's
 *  own right vector) and PerspectiveProjection's Y-flip puts +up up on screen, so
 *  no axis juggling is needed between the engine and the GS.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/render_profile.h"
#include "ps2/renderer/render_view.h"
#include "ps2/renderer/render_md2.h"
#include "ps2/renderer/render_sky.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/model.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/clip.h"
#include "ps2/renderer/batch.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/gs.h"
#include "ps2/math/vec_mat.h"
#include "ps2/builtin/builtin.h" // global_palette (beam and particle colours)

#include <cmath>
#include <cstring>

namespace ps2::view {
namespace {

// ------------------------------------------------------------------------------------------------
// Cvars / common constants
// ------------------------------------------------------------------------------------------------

// Depth range for the world projection (ref_gl's values).
constexpr float kZNear = 4.0f;
constexpr float kZFar  = 4096.0f;

// The view weapon gets its own, much closer near plane. Its models sit at the
// view origin with the stock and hands reaching back past it, so at kZNear a
// good part of the gun straddles the near plane - and the VU rejects straddling
// triangles whole rather than cutting them, which would punch holes in it. The
// weapon's depth is remapped into a fixed slice of the z-buffer regardless of
// the projection (vu1::DrawFlags::DepthHack), so a near plane this close costs
// it no precision it can use: the gun still spans thousands of z values inside
// its slice.
constexpr float kZNearWeapon = 0.25f;

// Vertex colour for the not-yet-lit world: GS modulate 128 = texels unchanged.
constexpr u32 kFullBright = vu1::PackColorRGBA(128, 128, 128, 0x80);

// Render view cvars:
static const cvar_t * s_backFaceCull      = nullptr;
static const cvar_t * s_skipWorld         = nullptr;
static const cvar_t * s_skipAlphaSurfaces = nullptr;
static const cvar_t * s_skipBrushModels   = nullptr;
static const cvar_t * s_skipSprites       = nullptr;
static const cvar_t * s_skipEntities      = nullptr;
static const cvar_t * s_skipParticles     = nullptr;
static const cvar_t * s_forceNullModels   = nullptr;
static const cvar_t * s_skipWeaponModel   = nullptr;
static const cvar_t * s_dynamicLightmaps  = nullptr;
static const cvar_t * s_lightmaps         = nullptr;
static const cvar_t * s_lightmapOnly      = nullptr;
static const cvar_t * s_lightmapColor     = nullptr;
static const cvar_t * s_lightmapModulate  = nullptr;
static const cvar_t * s_polyblend         = nullptr;

// Not ours to read: SetLightLevel writes the sampled light level back into it
// every frame for the game code (hence non-const). Registered by the client
// (cl_main.c), which forwards it to the server in each usercmd.
static cvar_t * s_lightLevel = nullptr;

// ------------------------------------------------------------------------------------------------
// Frame state
// ------------------------------------------------------------------------------------------------

// Game time of the frame, in seconds; drives the water warp animation.
static float s_frameTime = 0.0f;

// Frame counters used to mark drawable surfaces:
static int s_frameCount    = 0; // Bumped per RenderFrame; stamps surfaces marked for draw.
static int s_visFrameCount = 0; // Bumped when the PVS changes; stamps reachable nodes.

// Quake 2 view clusters; BeginRegistration() resets them for a new map.
constexpr int kInvalidCluster = -1;
static int s_viewCluster      = kInvalidCluster;
static int s_viewCluster2     = kInvalidCluster;
static int s_oldViewCluster   = kInvalidCluster;
static int s_oldViewCluster2  = kInvalidCluster;

// World-space camera position for the frame, in the format the VU0 back-face
// helper wants (16-byte aligned, w = 1).
static math::Vec4 s_eyePosition = {};

// Scene camera basis for the frame (Quake coordinates, from AngleVectors).
static vec3_t s_forwardVec = {};
static vec3_t s_rightVec   = {};
static vec3_t s_upVec      = {};

// World-to-clip transform for the frame (world geometry draws in world space).
static math::Mat4 s_viewProjMatrix = {};

// The same transform with the view weapon's closer near plane (kZNearWeapon).
static math::Mat4 s_weaponViewProjMatrix = {};

// View frustum side planes (left, right, bottom, top) for bounding-box culling.
static cplane_t s_frustum[4] = {};

// The same four planes packed for VU0; rebuilt with them by SetUpFrustum.
alignas(16) static math::Mat4 s_frustumPlanes = {};

// Wall texture animation frame (viewDef.time * 2, as in ref_gl).
static int s_textureAnimFrame = 0;

// Textures that received surfaces this frame; DrawTextureChains draws and
// resets exactly these. One entry per live texture is the true ceiling, so it
// tracks the cache's own capacity.
constexpr int kMaxChainTextures = static_cast<int>(tex::kMaxTextures);
static const tex::Texture * s_chainTextures[kMaxChainTextures];
static int s_chainTextureCount = 0;

// Surfaces with transparency (glass, water, lava, slime), collected while
// walking the BSP and while drawing brush model entities, then drawn last by
// RenderAlphaSurfaces over the finished opaque scene.
//
// Recorded here rather than threaded through ModelSurface::textureChain like
// the opaque chains because a brush model's surfaces have to remember the
// entity transform they were collected under. ref_gl loses it - its
// R_DrawAlphaSurfaces reloads the world matrix, so a moving submodel's water
// draws back at the map's rest position - and the records cost little.
struct AlphaSurface
{
    const mod::ModelSurface * surf;
    const tex::Texture *      texture; // Resolved on collection: the animation frame is the entity's.
    const math::Mat4 *        mvp;     // &s_viewProjMatrix, or into s_alphaEntityMatrices below.
};

constexpr int kMaxAlphaSurfaces = 1024;
static AlphaSurface s_alphaSurfaces[kMaxAlphaSurfaces];
static int s_alphaSurfaceCount = 0;

// One transform per brush model entity that contributed a translucent surface;
// DrawBrushModelEntity's own is a local, long gone by the time the alpha pass
// runs. MAX_ENTITIES is the hard ceiling on contributors, so it cannot overflow.
alignas(16) static math::Mat4 s_alphaEntityMatrices[MAX_ENTITIES];
static int s_alphaEntityMatrixCount = 0;

// Triangle gather buffer: texture chains append here and flush through
// vu1::DrawTriangles when full (see batch.h).
constexpr int kBatchMaxVerts = 3 * 768; // 768 whole triangles per batch
static batch::TriangleBatch<kBatchMaxVerts> s_batch;

// Performance counters for the frame, reset by RenderFrame and read through
// GetDrawStats() by the ps2_show_drawstats overlay.
static DrawStats s_drawStats = {};

// ------------------------------------------------------------------------------------------------
// Translucent surface collection
// ------------------------------------------------------------------------------------------------

// Defers one translucent surface to the RenderAlphaSurfaces pass at the end of
// the frame, remembering the texture (the animation frame is the caller's) and
// the transform it draws under.
void PushAlphaSurface(const mod::ModelSurface & surf, const tex::Texture & texture, const math::Mat4 & mvp)
{
    PS2_AssertMsg(s_alphaSurfaceCount < kMaxAlphaSurfaces, "Out of alpha surface slots!");
    if (s_alphaSurfaceCount == kMaxAlphaSurfaces)
    {
        Com_DPrintf("Out of alpha surface slots!\n");
        return; // Fail and drop the overflow surface on no-asserts build.
    }

    AlphaSurface & entry = s_alphaSurfaces[s_alphaSurfaceCount++];
    entry.surf    = &surf;
    entry.texture = &texture;
    entry.mvp     = &mvp;

    ++s_drawStats.surfacesAlpha;
}

// Parks a brush model entity's transform where the deferred pass can still
// reach it. One slot per contributing entity - callers hold on to the returned
// pointer for the rest of their surfaces. Null only if the table is full.
const math::Mat4 * StoreAlphaEntityMatrix(const math::Mat4 & mvp)
{
    PS2_AssertMsg(s_alphaEntityMatrixCount < MAX_ENTITIES, "Out of alpha entity matrix slots!");
    if (s_alphaEntityMatrixCount == MAX_ENTITIES)
    {
        Com_DPrintf("Out of alpha entity matrix slots!\n");
        return nullptr;
    }

    math::Mat4 & slot = s_alphaEntityMatrices[s_alphaEntityMatrixCount++];
    slot = mvp;
    return &slot;
}

// ------------------------------------------------------------------------------------------------
// Frame setup: camera matrices and frustum
// ------------------------------------------------------------------------------------------------

inline int SignBitsForPlane(const cplane_t & plane)
{
    // Sign bits are used for fast box-on-plane-side tests.
    int bits = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (plane.normal[i] < 0.0f)
        {
            bits |= (1 << i);
        }
    }
    return bits;
}

// Builds the four frustum side planes by rotating the view direction around
// the up/right axes by half the FOV (ref_gl's R_SetFrustum construction).
void SetUpFrustum(const refdef_t & viewDef)
{
    RotatePointAroundVector(s_frustum[0].normal, s_upVec,    s_forwardVec, -(90.0f - viewDef.fov_x * 0.5f));
    RotatePointAroundVector(s_frustum[1].normal, s_upVec,    s_forwardVec,  (90.0f - viewDef.fov_x * 0.5f));
    RotatePointAroundVector(s_frustum[2].normal, s_rightVec, s_forwardVec,  (90.0f - viewDef.fov_y * 0.5f));
    RotatePointAroundVector(s_frustum[3].normal, s_rightVec, s_forwardVec, -(90.0f - viewDef.fov_y * 0.5f));

    for (cplane_t & plane : s_frustum)
    {
        plane.type     = PLANE_ANYZ;
        plane.dist     = DotProduct(viewDef.vieworg, plane.normal);
        plane.signbits = static_cast<byte>(SignBitsForPlane(plane));
    }

    // The same four planes as a transform, so a point's distance to all of them
    // comes out of one VU0 pass instead of four scalar dot products: column p
    // holds plane p's normal with -dist in the translation row, which makes
    // component p of (point * this) exactly dot(point, n[p]) - dist[p].
    s_frustumPlanes = {{
        { s_frustum[0].normal[0], s_frustum[1].normal[0], s_frustum[2].normal[0], s_frustum[3].normal[0] },
        { s_frustum[0].normal[1], s_frustum[1].normal[1], s_frustum[2].normal[1], s_frustum[3].normal[1] },
        { s_frustum[0].normal[2], s_frustum[1].normal[2], s_frustum[2].normal[2], s_frustum[3].normal[2] },
        { -s_frustum[0].dist,     -s_frustum[1].dist,     -s_frustum[2].dist,     -s_frustum[3].dist     },
    }};
}

// True when the box is completely outside the frustum and must not draw.
inline bool ShouldCullBBox(float * mins, float * maxs)
{
    for (cplane_t & plane : s_frustum)
    {
        if (BOX_ON_PLANE_SIDE(mins, maxs, &plane) == 2)
        {
            ++s_drawStats.boxesCulled;
            return true;
        }
    }
    return false;
}

// Whether the per-triangle back-face test runs at all. Note the world and
// brush model passes already reject whole surfaces on the same side test
// (their triangles are coplanar with the surface), so enabling this test
// actually doesn't gain us anything. Left as a reference, disabled by default.
inline bool WorldBackFaceCullEnabled()
{
    return s_backFaceCull->value != 0.0f;
}

void SetupFrame(const refdef_t & viewDef)
{
    ++s_frameCount;

    s_eyePosition = { viewDef.vieworg[0], viewDef.vieworg[1], viewDef.vieworg[2], 1.0f };

    // Animated walls flip frames at 2 Hz of game time (as in ref_gl).
    s_frameTime        = viewDef.time;
    s_textureAnimFrame = static_cast<int>(viewDef.time * 2.0f);

    // Camera basis vectors from the view angles.
    math::AngleVectors(viewDef.viewangles, s_forwardVec, s_rightVec, s_upVec);

    const math::Vec3 eye    = { viewDef.vieworg[0], viewDef.vieworg[1], viewDef.vieworg[2] };
    const math::Vec3 target = { eye.x + s_forwardVec[0], eye.y + s_forwardVec[1], eye.z + s_forwardVec[2] };
    const math::Vec3 up     = { s_upVec[0], s_upVec[1], s_upVec[2] };

    const float fovY    = math::DegToRad(viewDef.fov_y);
    const float aspect  = static_cast<float>(viewDef.width) / static_cast<float>(viewDef.height);
    const float screenW = static_cast<float>(gs::Width());
    const float screenH = static_cast<float>(gs::Height());

    const math::Mat4 view = math::LookAt(eye, target, up);
    const math::Mat4 proj = math::PerspectiveProjection(fovY, aspect, screenW, screenH, kZNear, kZFar);
    s_viewProjMatrix = view * proj;

    // The view weapon's variant differs only in the near plane, so it shares
    // the camera and every screen mapping; nothing else may use it.
    const math::Mat4 weaponProj = math::PerspectiveProjection(fovY, aspect, screenW, screenH, kZNearWeapon, kZFar);
    s_weaponViewProjMatrix = view * weaponProj;

    SetUpFrustum(viewDef);
}

// ------------------------------------------------------------------------------------------------
// PVS / visibility
// ------------------------------------------------------------------------------------------------

const mod::ModelLeaf * FindLeafNodeForPoint(const float * point, const mod::ModelInstance & model)
{
    PS2_AssertMsg(model.nodes != nullptr, "World model has no nodes!");

    const mod::ModelNode * node = model.nodes;
    for (;;)
    {
        if (node->contents != -1)
        {
            return reinterpret_cast<const mod::ModelLeaf *>(node);
        }

        const cplane_t * const plane = node->plane;
        const float d = DotProduct(point, plane->normal) - plane->dist;
        node = (d > 0.0f) ? node->children[0] : node->children[1];
    }
}

// Returns the decompressed PVS row for 'cluster'.
//
// This used to decompress from a copy of the VISIBILITY lump kept in the world
// hunk. The collision model has the identical lump in map_visibility[] and an
// identical decoder, so the copy is gone (up to 376 KB of the hunk on jail5) and
// this defers to CM_ClusterPVS - whose decoder is the better of the two, since it
// clamps a zero-run to the row length instead of running off the end.
//
// The row lives in a shared buffer that the next call overwrites, so don't hold
// on to it (MarkLeaves copies it into a temp before asking for the second one).
inline const u8 * GetClusterPVS(const int cluster)
{
    PS2_Assert(cluster != kInvalidCluster); // MarkLeaves handles that case itself.
    return CM_ClusterPVS(cluster);
}

// Finds the clusters the camera sees from this frame. Two clusters when near a
// solid water surface, so crossing it doesn't draw wrong (checked by sampling a
// second leaf 16 units above/below the eye).
void SetUpViewClusters(const refdef_t & viewDef, const mod::ModelInstance & world)
{
    const mod::ModelLeaf * leaf = FindLeafNodeForPoint(viewDef.vieworg, world);

    s_oldViewCluster  = s_viewCluster;
    s_oldViewCluster2 = s_viewCluster2;
    s_viewCluster = s_viewCluster2 = leaf->cluster;

    vec3_t temp;
    VectorCopy(viewDef.vieworg, temp);
    temp[2] += (leaf->contents == 0) ? -16.0f : 16.0f;

    leaf = FindLeafNodeForPoint(temp, world);
    if (!(leaf->contents & CONTENTS_SOLID) && (leaf->cluster != s_viewCluster2))
    {
        s_viewCluster2 = leaf->cluster;
    }
}

// Stamps the leafs in the current clusters' PVS - and the node chains above
// them - with the new vis frame count. Skipped entirely while the camera stays
// in the same cluster(s), which is the common case.
void MarkLeaves(const mod::ModelInstance & world)
{
    if (s_oldViewCluster  == s_viewCluster  &&
        s_oldViewCluster2 == s_viewCluster2 &&
        s_viewCluster != kInvalidCluster)
    {
        return; // Same clusters as the previous frame; marks still valid.
    }

    ++s_visFrameCount;
    s_oldViewCluster  = s_viewCluster;
    s_oldViewCluster2 = s_viewCluster2;

    if (s_viewCluster == kInvalidCluster || !CM_HasVisibility())
    {
        // Outside the map or no PVS data: mark everything visible.
        for (int i = 0; i < world.numLeafs; ++i)
        {
            world.leafs[i].visFrame = s_visFrameCount;
        }
        for (int i = 0; i < world.numNodes; ++i)
        {
            world.nodes[i].visFrame = s_visFrameCount;
        }
        return;
    }

    const u8 * vis = GetClusterPVS(s_viewCluster);

    // Scratch for the two-cluster PVS union used when the camera straddles a solid
    // water boundary. The single-cluster row comes from CM_ClusterPVS, which owns its
    // own buffer - this only exists because combining two clusters needs the first row
    // kept while the second is decompressed over it.
    alignas(16) u8 fatPvs[MAX_MAP_LEAFS / 8];

    // May have to combine two clusters because of solid water boundaries:
    if (s_viewCluster2 != s_viewCluster)
    {
        // Copy the first row out before asking for the second: CM_ClusterPVS
        // decompresses both into the same buffer.
        std::memcpy(fatPvs, vis, static_cast<size_t>((world.numLeafs + 7) / 8));
        vis = GetClusterPVS(s_viewCluster2);

        // Both buffers are 16-byte aligned, so OR them a word at a time.
        u32 * fat = static_cast<u32 *>(static_cast<void *>(fatPvs));
        const u32 * add = static_cast<const u32 *>(static_cast<const void *>(vis));

        const int words = (world.numLeafs + 31) / 32;
        for (int i = 0; i < words; ++i)
        {
            fat[i] |= add[i];
        }
        vis = fatPvs;
    }

    mod::ModelLeaf * leaf = world.leafs;
    for (int i = 0; i < world.numLeafs; ++i, ++leaf)
    {
        const int cluster = leaf->cluster;
        if (cluster == kInvalidCluster)
        {
            continue;
        }

        if (vis[cluster >> 3] & (1 << (cluster & 7)))
        {
            auto * node = reinterpret_cast<mod::ModelNode *>(leaf);
            do
            {
                if (node->visFrame == s_visFrameCount)
                {
                    break; // This branch is already marked up to the root.
                }
                node->visFrame = s_visFrameCount;
                node = node->parent;
            } while (node != nullptr);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// World BSP walk and texture chains
// ------------------------------------------------------------------------------------------------

// Returns the texture a surface draws with, following the animation chain for
// animated walls (torches, screens). Never null: the model loader substitutes
// the debug checkerboard for missing wall textures. World surfaces step the
// chain on game time; brush model entities step it on entity.frame instead,
// which is how the game scripts a door or button changing its own texture.
const tex::Texture * TextureAnimation(const mod::ModelTexInfo * texInfo, int animFrame)
{
    PS2_Assert(texInfo != nullptr && texInfo->texture != nullptr);

    if (texInfo->next == nullptr)
    {
        return texInfo->texture; // Not animated.
    }

    int c = animFrame % texInfo->numFrames;
    while (c-- > 0)
    {
        texInfo = texInfo->next;
    }
    return texInfo->texture;
}

// Recursively marks and chains the visible world surfaces: walks the BSP
// front-to-back, culling nodes against the PVS marks and the view frustum,
// and threads each drawable surface onto its texture's chain so the next
// DrawTextureChains() call renders what was collected here.
void RecursiveWorldNode(const refdef_t & viewDef, const mod::ModelInstance & world, mod::ModelNode * node)
{
    if (node->contents == CONTENTS_SOLID)
    {
        return;
    }
    if (node->visFrame != s_visFrameCount)
    {
        return; // Not reachable from the current PVS cluster.
    }
    if (ShouldCullBBox(node->minmaxs, node->minmaxs + 3))
    {
        return; // Entirely outside the view frustum.
    }

    ++s_drawStats.nodesWalked;

    // Leaf: stamp its surfaces as drawable this frame.
    if (node->contents != -1)
    {
        auto * leaf = reinterpret_cast<mod::ModelLeaf *>(node);

        // Check for door-connected areas:
        if (viewDef.areabits != nullptr)
        {
            if (!(viewDef.areabits[leaf->area >> 3] & (1 << (leaf->area & 7))))
            {
                return; // Not visible.
            }
        }

        mod::ModelSurface ** mark = leaf->firstMarkSurface;
        for (int i = 0; i < leaf->numMarkSurfaces; ++i, ++mark)
        {
            (*mark)->visFrame = s_frameCount;
        }
        return;
    }

    // Decision node: find which side of its plane the camera is on.
    float dot;
    const cplane_t * const plane = node->plane;
    switch (plane->type)
    {
    case PLANE_X:
        dot = viewDef.vieworg[0] - plane->dist;
        break;
    case PLANE_Y:
        dot = viewDef.vieworg[1] - plane->dist;
        break;
    case PLANE_Z:
        dot = viewDef.vieworg[2] - plane->dist;
        break;
    default:
        dot = DotProduct(viewDef.vieworg, plane->normal) - plane->dist;
        break;
    }

    const int  side         = (dot >= 0.0f) ? 0 : 1;
    const bool cameraOnBack = (side == 1);

    // Recurse down the camera side first (front-to-back order)...
    RecursiveWorldNode(viewDef, world, node->children[side]);

    // ...then chain this node's surfaces that face the camera...
    mod::ModelSurface * surf = world.surfaces + node->firstSurface;
    for (int i = 0; i < node->numSurfaces; ++i, ++surf)
    {
        if (surf->visFrame != s_frameCount)
        {
            continue; // Not in a visible leaf.
        }
        if (HasFlag(surf->flags, mod::SurfaceFlags::PlaneBack) != cameraOnBack)
        {
            continue; // Facing away from the camera.
        }

        const int texFlags = surf->texInfo->flags;
        if (texFlags & SURF_SKY)
        {
            // Never drawn: a sky surface is a hole, and all it contributes is
            // which part of the skybox the player can see through it.
            sky::AddSurface(*surf, viewDef.vieworg);
            continue;
        }

        if (texFlags & (SURF_TRANS33 | SURF_TRANS66 | SURF_WARP))
        {
            // Translucent or turbulent: deferred to the back-to-front pass at
            // the end of the frame. World geometry draws in world space, so
            // the plain view-projection is transform enough.
            PushAlphaSurface(*surf, *TextureAnimation(surf->texInfo, s_textureAnimFrame), s_viewProjMatrix);
            continue;
        }

        // Opaque: thread onto its texture's draw chain.
        ++s_drawStats.surfaces;
        const tex::Texture * texture = TextureAnimation(surf->texInfo, s_textureAnimFrame);
        if (texture->textureChain == nullptr)
        {
            // First surface for this texture this frame; remember the chain.
            PS2_AssertMsg(s_chainTextureCount < kMaxChainTextures, "Out of texture chain slots!");
            s_chainTextures[s_chainTextureCount++] = texture;
        }
        surf->textureChain    = texture->textureChain;
        texture->textureChain = surf;

        // Rebuild the surface's luxels if its lighting moved since they were
        // baked, and thread it onto its atlas's chain for the lightmap pass.
        // Here rather than at draw time because this walk reaches each visible
        // surface exactly once, so the rebuild cannot be done twice over.
        if (surf->lightmapTextureNum != mod::kNotLightmapped)
        {
            lm::ChainSurface(*surf, viewDef, s_frameCount);
        }
    }

    // ...and finally recurse down the far side.
    RecursiveWorldNode(viewDef, world, node->children[side ^ 1]);
}

// ------------------------------------------------------------------------------------------------
// Triangle gathering and submission
// ------------------------------------------------------------------------------------------------

// Everything the gather path needs beyond the geometry itself. The world pass
// draws untransformed, fullbright and opaque; a brush model entity carries its
// own model-view-projection and may blend, so the same clipper and scratch
// buffer serve both.
struct SurfaceDrawState
{
    const math::Mat4 * mvp;   // Clips and draws with this; the world's is the plain view-projection.
    math::Vec4         eye;   // Camera in the same space as the triangles, for the back-face test.
    u32                rgba;  // Packed vertex colour (GS modulate: 128 = unchanged, alpha 0x80 = 1.0).
    vu1::DrawFlags     flags; // Batch flags, i.e. whether the submission blends.
    bool               cullBackFaces;

    // Gouraud alpha: take each vertex's alpha from its own ClipVertex::st.z
    // (0..1) instead of from 'rgba', whose RGB is still used for all three
    // corners. The gather path is otherwise flat-shaded - one colour per
    // batch - and this is the cheapest way out of that, since st is already
    // a whole quadword the clipper interpolates and .z was spare.
    bool               vertexAlpha;

    // Feed the gather the vertices' lightmap UVs instead of their diffuse
    // ones - the second pass over the same geometry that modulates in the
    // lightmap. Defaulted because only that one pass wants it; every other
    // draw leaves it alone.
    bool               lightmapUVs = false;

    // Chroma mirror of the atlas the surface being gathered is packed into, or
    // null to leave the vertex colour flat. Set per surface by the diffuse
    // passes: the lightmap pass can only deliver a luxel's intensity, so its
    // colour is sampled here instead, per vertex, and folded into the vertex
    // colour the GS modulates the wall texture by. Mutually exclusive with
    // vertexAlpha, which owns that colour's alpha byte.
    const u16 *        lightmapColors = nullptr;
};

// ------------------------------------------------------------------------------------------------
// Triangle gathering through the clipper
//
// The VU rejects a straddling triangle whole rather than cutting it, so world
// geometry is pre-clipped on the EE against the six planes it judges. The
// clipper (clip.h) and the gather buffer it feeds (batch.h) are shared with the
// sky and alias model paths; what follows is this file's use of them, which is
// the per-vertex colour and nothing else. ClipVertex::st carries the vertex
// alpha in .z under SurfaceDrawState::vertexAlpha, and ClipVertex::color the
// luxel chroma as a 0..1 tint under SurfaceDrawState::lightmapColors.
// ------------------------------------------------------------------------------------------------

using clip::ClipVertex;

// Swaps the batch colour's alpha for this vertex's own, clamped onto the GS's
// 0..0x80 = 0..1.0 alpha scale.
inline u32 WithVertexAlpha(u32 rgba, float alpha)
{
    const float scaled = alpha * 128.0f;
    const u32   packed = (scaled >= 128.0f) ? 128u
                       : (scaled <= 0.0f)   ? 0u
                                            : static_cast<u32>(scaled);
    return (rgba & 0x00FFFFFFu) | (packed << 24);
}

// Scales one 0-255 colour channel by a 0..1 factor, rounded so a factor of 1
// leaves it exactly where it was.
inline u32 ScaleChannel(u32 channel, float scale)
{
    const float scaled = (static_cast<float>(channel) * scale) + 0.5f;
    return (scaled <= 0.0f)   ? 0u
         : (scaled >= 255.0f) ? 255u
                              : static_cast<u32>(scaled);
}

// Tints the batch colour by this vertex's own, leaving the alpha byte alone -
// the diffuse pass is opaque, and the lightmap pass that follows needs the
// modulate identity there.
inline u32 WithVertexColor(u32 rgba, const math::Vec4 & tint)
{
    return ScaleChannel( rgba        & 0xFF, tint.x)
        | (ScaleChannel((rgba >>  8) & 0xFF, tint.y) <<  8)
        | (ScaleChannel((rgba >> 16) & 0xFF, tint.z) << 16)
        | (rgba & 0xFF000000u);
}

// The colour one gathered vertex draws with: the batch colour, tinted by the
// luxel chroma or wearing this vertex's own alpha, per the draw state.
inline u32 VertexColor(const ClipVertex & v, const SurfaceDrawState & state)
{
    return (state.lightmapColors != nullptr) ? WithVertexColor(state.rgba, v.color)
          : state.vertexAlpha                ? WithVertexAlpha(state.rgba, v.st.z)
                                             : state.rgba;
}

// Clips one triangle against the VU clip volume and appends the survivors to
// the gather buffer, flushing it when full. The corners arrive with their
// position and UVs set; their clip distances are computed by the clipper.
inline void GatherTriangle(ClipVertex (&corners)[3], const tex::Texture & texture, const SurfaceDrawState & state)
{
    // Reject a triangle facing away from the camera before any clipping work.
    // The test is cheaper than the six plane distances the clipper takes, and a
    // rejected triangle costs the clipper, the gather buffer and the VU nothing.
    if (state.cullBackFaces &&
        math::CullBackFacingTriangle(state.eye, corners[0].pos, corners[1].pos, corners[2].pos))
    {
        ++s_drawStats.trisBackFacing;
        return;
    }

    s_batch.GatherTriangle(corners, *state.mvp, texture, state.flags,
                           [&state](const ClipVertex & v) { return VertexColor(v, state); });
}

// Sends the gathered triangles as one batch and empties the buffer.
inline void FlushScratch(const tex::Texture & texture, const SurfaceDrawState & state)
{
    s_batch.Flush(*state.mvp, texture, state.flags);
}

// Point-samples the luxel chroma a vertex's lightmap UVs land on - the half of
// the luxel the lightmap pass cannot carry, since the GS can only blend by a
// scalar alpha. The UVs are normalised over the atlas, so scaling by its
// dimensions gives the luxel to read, and the half-luxel offset the loader baked
// into them puts the result on a texel centre, so truncating picks that luxel
// rather than a neighbour. Clamped because nothing guarantees otherwise: a block
// packed flush against the atlas edge can round a hair past it.
//
// A point sample, deliberately - this runs per vertex per frame over every
// visible world surface, and the term it is fetching barely varies.
inline math::Vec4 SampleLightmapColor(const u16 * const colors, const float s, const float t)
{
    constexpr float kMaxS = static_cast<float>(lm::kLightmapTextureWidth  - 1);
    constexpr float kMaxT = static_cast<float>(lm::kLightmapTextureHeight - 1);

    const float fs = s * static_cast<float>(lm::kLightmapTextureWidth);
    const float ft = t * static_cast<float>(lm::kLightmapTextureHeight);

    const int ls = static_cast<int>((fs <= 0.0f) ? 0.0f : (fs >= kMaxS) ? kMaxS : fs);
    const int lt = static_cast<int>((ft <= 0.0f) ? 0.0f : (ft >= kMaxT) ? kMaxT : ft);

    const lm::AtlasColor c = lm::UnpackAtlasColor(colors[(lt * lm::kLightmapTextureWidth) + ls]);
    return { c.r, c.g, c.b, 1.0f };
}

// Appends a polygon's triangles to the scratch buffer, clipping the ones that
// cross the VU clip volume and flushing when full.
void GatherPolyTriangles(const mod::ModelPoly & poly,
                         const tex::Texture & texture,
                         const SurfaceDrawState & state)
{
    const int numTriangles = poly.numVerts - 2;
    for (int t = 0; t < numTriangles; ++t)
    {
        const mod::ModelTriangle & tri = poly.triangles[t];

        // Polygons the triangulation couldn't complete leave zeroed
        // (degenerate) triangles behind; skip them.
        if (tri.vertexes[0] == tri.vertexes[1])
        {
            continue;
        }

        ClipVertex corners[3];
        for (int v = 0; v < 3; ++v)
        {
            const mod::PolyVertex & src = poly.vertexes[tri.vertexes[v]];
            const float uvS = state.lightmapUVs ? src.lightmap_s : src.texture_s;
            const float uvT = state.lightmapUVs ? src.lightmap_t : src.texture_t;

            corners[v].pos = { src.position.x, src.position.y, src.position.z, 1.0f };
            corners[v].st  = { uvS, uvT, 0.0f, 0.0f };

            if (state.lightmapColors != nullptr)
            {
                corners[v].color = SampleLightmapColor(state.lightmapColors,
                                                       src.lightmap_s, src.lightmap_t);
            }
        }

        GatherTriangle(corners, texture, state);
    }
}

// ------------------------------------------------------------------------------------------------
// Turbulent (warped) surfaces: water, lava, slime
// ------------------------------------------------------------------------------------------------

// ref_gl's r_turbsin lookup (gl_warp.c, values in warpsin.h): 8*sin(i*2pi/256),
// halved once at startup by R_Init, so the effective amplitude is 4 texels.
// Built at init rather than copied in - it is a pure function of the index,
// and this is more accurate than the 6-significant-digit literals ref_gl ships.
constexpr int kTurbSinSize = 256;
constexpr float kTurbSinAmplitude = 4.0f;
static float s_turbSin[kTurbSinSize];

// Phase to table index: the table spans exactly one period (ref_gl's TURBSCALE).
constexpr float kTurbScale = static_cast<float>(kTurbSinSize) / (2.0f * math::kPI);

// A subdivided warp polygon is a fan: a centre vertex, the ring, then a
// duplicate of the first to close it. SubdividePolygon splits at 64-unit
// boundaries, so a leaf never exceeds that many ring vertices.
constexpr int kMaxWarpPolyVerts = 64 + 2;

inline float TurbSin(const float phase)
{
    // Truncation toward zero and a two's complement mask, exactly as ref_gl
    // indexes the table - negative phases included.
    const int index = static_cast<int>(phase * kTurbScale) & (kTurbSinSize - 1);
    return s_turbSin[index];
}

// Gathers a turbulent surface with ref_gl's warp animation (EmitWaterPolys):
// every vertex's texture coordinates are pushed around by a sine of the
// *other* axis plus time, which is what makes the surface ripple while the
// geometry stays put.
//
// These polygons are shaped differently from ordinary ones, so this cannot go
// through GatherPolyTriangles: the loader's SubdivideSurface leaves each as a
// fan with no triangle list at all (reading poly.triangles would dereference
// null), and with texture coordinates still in raw texel units. Both follow
// ref_gl, whose GL_SubdivideSurface builds fans for glBegin(GL_TRIANGLE_FAN)
// and leaves the texel-to-image division to EmitWaterPolys.
//
// TODO: Consider moving the polygon warping work to the VU1.
void DrawAnimatedWaterPolys(const mod::ModelSurface & surf,
                            const tex::Texture & texture,
                            const SurfaceDrawState & state)
{
    // SURF_FLOWING drifts the surface along S by a whole 64-texel tile every
    // two seconds. (ref_gl truncates with an int cast; the time is never
    // negative, so this is the same fractional part.)
    float scroll = 0.0f;
    if (surf.texInfo->flags & SURF_FLOWING)
    {
        const float halfTime = s_frameTime * 0.5f;
        scroll = -64.0f * (halfTime - std::floor(halfTime));
    }

    // ref_gl divides by a hardcoded 64 at this point. Every warp texture in
    // pak0 is 64x64, so these agree on the real data, and this one stays
    // right if some mod ships another size. The size on disk, like the BSP's
    // own texture coordinates: a stretched wall still tiles at its original
    // size (see tex::Texture::srcWidth).
    const float invWidth  = 1.0f / static_cast<float>(texture.srcWidth);
    const float invHeight = 1.0f / static_cast<float>(texture.srcHeight);

    for (const mod::ModelPoly * poly = surf.polys; poly != nullptr; poly = poly->next)
    {
        if (poly->numVerts < 3) // Need at least one triangle.
        {
            continue;
        }
        PS2_AssertMsg(poly->numVerts <= kMaxWarpPolyVerts, "Warp polygon larger than a subdivision leaf!");

        // Warped once per vertex: the fan below reads each of them twice.
        float warpedS[kMaxWarpPolyVerts];
        float warpedT[kMaxWarpPolyVerts];
        for (int i = 0; i < poly->numVerts; ++i)
        {
            const float os = poly->vertexes[i].texture_s;
            const float ot = poly->vertexes[i].texture_t;

            warpedS[i] = (os + TurbSin((ot * 0.125f) + s_frameTime) + scroll) * invWidth;
            warpedT[i] = (ot + TurbSin((os * 0.125f) + s_frameTime)) * invHeight;
        }

        for (int v = 1; v < poly->numVerts - 1; ++v)
        {
            const int fan[3] = { 0, v, v + 1 };

            ClipVertex corners[3];
            for (int c = 0; c < 3; ++c)
            {
                const mod::PolyVertex & src = poly->vertexes[fan[c]];
                corners[c].pos = { src.position.x, src.position.y, src.position.z, 1.0f };
                corners[c].st  = { warpedS[fan[c]], warpedT[fan[c]], 0.0f, 0.0f };
            }

            GatherTriangle(corners, texture, state);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// DrawTextureChains
// ------------------------------------------------------------------------------------------------

// The transform and culling the world pass draws with. World geometry sits in
// world space already, so its "model" transform is the plain view-projection
// and the back-face test takes the world camera. Shared by the diffuse and
// lightmap passes, which must agree on all of it or their triangles would not
// land on the same pixels.
inline SurfaceDrawState WorldSurfaceDrawState()
{
    return SurfaceDrawState {
        .mvp   = &s_viewProjMatrix,
        .eye   = s_eyePosition,
        .rgba  = kFullBright,
        .flags = vu1::DrawFlags::None,
        .cullBackFaces = WorldBackFaceCullEnabled(),
        .vertexAlpha   = false
    };
}

// Whether the diffuse passes should tint their vertices by the luxel chroma.
// Gated on the lightmap pass as well as its own cvar: the chroma is only half a
// luxel, and laying it down without the intensity that goes with it would tint a
// fullbright world rather than light it.
inline bool LightmapColorEnabled()
{
    return (s_lightmaps->value != 0.0f) && (s_lightmapColor->value != 0.0f);
}

// The chroma mirror to sample a surface's vertices from, or null when it has no
// lightmap at all - which for the world means sky, since RecursiveWorldNode
// sends turbulent and translucent faces down the alpha pass instead.
inline const u16 * SurfaceLightmapColors(const mod::ModelSurface & surf)
{
    return (surf.lightmapTextureNum != mod::kNotLightmapped)
         ? lm::AtlasColors(surf.lightmapTextureNum)
         : nullptr;
}

// Draws every texture chain built by RecursiveWorldNode and resets them.
void DrawTextureChains(const SurfaceDrawState & base)
{
    SurfaceDrawState state = base;

    // Lightmap-only debug view: drop the diffuse texture and lay down flat
    // white, so what survives the lightmap pass over it is the lighting alone.
    if (s_lightmapOnly->value != 0.0f)
    {
        state.flags = vu1::DrawFlags::Untextured;
        state.rgba  = vu1::PackColorRGBA(255, 255, 255, 0x80);
    }

    const bool tinted = LightmapColorEnabled();

    for (int i = 0; i < s_chainTextureCount; ++i)
    {
        const tex::Texture * texture = s_chainTextures[i];

        for (const mod::ModelSurface * surf = texture->textureChain; surf != nullptr; surf = surf->textureChain)
        {
            state.lightmapColors = tinted ? SurfaceLightmapColors(*surf) : nullptr;

            for (const mod::ModelPoly * poly = surf->polys; poly != nullptr; poly = poly->next)
            {
                if (poly->numVerts >= 3) // Need at least one triangle.
                {
                    GatherPolyTriangles(*poly, *texture, state);
                }
            }
        }
        FlushScratch(*texture, state);

        texture->textureChain = nullptr; // Reset for the next frame.
    }
    s_chainTextureCount = 0;
}

// ------------------------------------------------------------------------------------------------
// Lightmap pass
// ------------------------------------------------------------------------------------------------

// Modulates the surfaces the diffuse pass just laid down by their luxel
// intensity, one batch per lightmap atlas. This is the second half of the two
// pass lightmapping: same geometry, same transform, but sampling the atlas
// through the vertices' second UV set and blending with Cd * As, so each pixel
// is scaled by how lit it is. Intensity only - see vu1::DrawFlags::Modulate for
// why the GS cannot carry the colour too, and SurfaceDrawState::lightmapColors
// for where it goes instead.
//
// 'base' is the draw state of the pass being lit - the world's or a brush model
// entity's - so the two agree on transform and culling and their triangles land
// on the same pixels. Depth writes are masked and the z-test is GREATER_EQUAL,
// so the pass re-covers exactly what pass one wrote without fighting it.
void DrawLightmapChains(const SurfaceDrawState & base)
{
    if (s_lightmaps->value == 0.0f)
    {
        lm::ClearChains(); // Chained anyway while walking; drop them unlit.
        return;
    }

    SurfaceDrawState state = base;
    state.rgba           = kFullBright; // alpha 0x80 keeps the luxel's own alpha
    state.flags          = vu1::DrawFlags::Modulate;
    state.vertexAlpha    = false;
    state.lightmapUVs    = true;
    state.lightmapColors = nullptr; // The chroma is the diffuse pass's half; this one carries the intensity.

    const int numLightmaps = lm::NumAtlases();
    for (int i = 0; i < numLightmaps; ++i)
    {
        const mod::ModelSurface * const chain = lm::AtlasChain(i);
        if (chain == nullptr)
        {
            continue; // Nothing visible packed into this atlas.
        }

        const tex::Texture & atlas = lm::AtlasTexture(i);

        for (const mod::ModelSurface * surf = chain; surf != nullptr; surf = surf->lightmapChain)
        {
            for (const mod::ModelPoly * poly = surf->polys; poly != nullptr; poly = poly->next)
            {
                if (poly->numVerts >= 3) // Need at least one triangle.
                {
                    GatherPolyTriangles(*poly, atlas, state);
                }
            }
        }
        FlushScratch(atlas, state);
    }

    lm::ClearChains();
}

// ------------------------------------------------------------------------------------------------
// Light level readback
// ------------------------------------------------------------------------------------------------

// Quake 2's channel for telling the game code how brightly lit the player is:
// the client reads r_lightlevel back out of the cvar system every frame and
// packs it into the usercmd (cl_input.c), where the server uses it to decide
// how visible the player is. Nothing about it is graphics - the renderer just
// happens to be the only thing that can sample the lightmaps.
//
// The value is the largest of the three sampled colour components scaled by
// 150, which is what the software renderer's mono light value worked out to.
// Written straight into cvar_t::value, as ref_gl's R_SetLightLevel does: this
// runs every frame and the cvar's string form is never read.
void SetLightLevel(const refdef_t & viewDef)
{
    if (viewDef.rdflags & RDF_NOWORLDMODEL)
    {
        return; // No world to sample; leave the last value alone (ref_gl does).
    }

    vec3_t shadeLight = { 1.0f, 1.0f, 1.0f };
    vec3_t lightSpot  = {}; // Unused here; the shadow anchor is for entity models.
    CalcPointLightColor(viewDef, viewDef.vieworg, shadeLight, lightSpot);

    float brightest = shadeLight[0];
    if (shadeLight[1] > brightest) { brightest = shadeLight[1]; }
    if (shadeLight[2] > brightest) { brightest = shadeLight[2]; }

    s_lightLevel->value = 150.0f * brightest;
}

// ------------------------------------------------------------------------------------------------
// Full screen colour blend (ref_gl's R_Flash / R_PolyBlend)
// ------------------------------------------------------------------------------------------------

// Scales a 0..1 blend channel onto 0..255, clamped: refdef_t::blend comes
// straight off the wire (cl_ents.c) or out of cl_testblend, and nothing
// upstream promises the range a cast would need.
inline u8 BlendChannelToByte(const float channel)
{
    const float scaled = channel * 255.0f;
    return (scaled >= 255.0f) ? 255u
         : (scaled <= 0.0f)   ? 0u
                              : static_cast<u8>(scaled);
}

// The damage/powerup/underwater tint the game code accumulates in
// refdef_t::blend (SV_CalcBlend in p_view.c), as one blended rectangle over
// the finished 3D scene.
//
// Fills the whole framebuffer rather than the refdef's view rectangle, which
// is where ref_gl's viewport put it: the 3D path here sets no scissor, so at
// scr_viewsize < 100 the scene has already been drawn over the border that
// SCR_TileClear laid down, and tinting only the rectangle would leave that
// overdrawn border untinted.
//
// This opens the deferred 2D batch, which is what puts it under the HUD: every
// 2D primitive the client draws after re.RenderFrame returns appends to the
// same batch, and nothing flushes it until gs::EndFrame.
void RenderBlendedOverlay(const refdef_t & viewDef)
{
    if (s_polyblend->value == 0.0f)
    {
        return;
    }
    if (viewDef.blend[3] <= 0.0f)
    {
        return; // Fully transparent: nothing to tint.
    }

    gs::FillRect(0, 0, gs::Width(), gs::Height(),
                 BlendChannelToByte(viewDef.blend[0]),
                 BlendChannelToByte(viewDef.blend[1]),
                 BlendChannelToByte(viewDef.blend[2]),
                 BlendChannelToByte(viewDef.blend[3]));
}

// ------------------------------------------------------------------------------------------------
// Translucent surface pass
// ------------------------------------------------------------------------------------------------

// Vertex colour for a deferred surface: ref_gl's R_DrawAlphaSurfaces alphas on
// the GS's 0x80 = 1.0 scale. Surfaces that are turbulent but not explicitly
// translucent (lava, slime) still go through the blend at full opacity, as
// they do there.
inline u32 AlphaSurfaceColor(const int texFlags)
{
    u32 alpha = 0x80; // 1.0
    if (texFlags & SURF_TRANS33)
    {
        alpha = 42; // 0.33
    }
    else if (texFlags & SURF_TRANS66)
    {
        alpha = 84; // 0.66
    }
    return vu1::PackColorRGBA(128, 128, 128, alpha);
}

// Draws everything RecursiveWorldNode and DrawBrushModelEntity set aside -
// glass, water, lava, slime - blended over the finished opaque scene. Called
// last in the frame, where ref_gl calls R_DrawAlphaSurfaces.
//
// Walked in reverse of collection order, which is back to front: the BSP walk
// that collected them ran front to back (ref_gl reaches the same order by
// prepending to a linked list). The entries are depth-ordered rather than
// grouped by texture, so unlike the opaque pass the batch has to break
// whenever the texture, the transform or the alpha changes.
//
// Brush model surfaces were collected after the whole world walk, so reversing
// puts them ahead of even the farthest world surface. That is a real mis-sort
// - and the same one ref_gl has - but it only shows when a translucent
// submodel sits behind translucent world geometry. Sorting the entries by
// view distance would fix it, at the price of the depth order the BSP walk
// hands us for free.
void RenderAlphaSurfaces()
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::AlphaSurfs);

    if (s_alphaSurfaceCount == 0 || s_skipAlphaSurfaces->value != 0.0f)
    {
        s_alphaSurfaceCount      = 0;
        s_alphaEntityMatrixCount = 0;
        return;
    }

    SurfaceDrawState state = {
        .mvp   = nullptr, // Per entry, below; no entry ever carries null, so the first always switches.
        .eye   = s_eyePosition,
        .rgba  = 0,
        .flags = vu1::DrawFlags::Blended,
        // Both collectors already dropped the surfaces facing away from the
        // camera - the world walk by plane side, brush models by the same
        // test in model space - so the triangle test has nothing left to find here.
        .cullBackFaces = false,
        .vertexAlpha   = false
    };

    const tex::Texture * batchTexture = nullptr;

    for (int i = s_alphaSurfaceCount - 1; i >= 0; --i)
    {
        const AlphaSurface & entry = s_alphaSurfaces[i];

        const int texFlags = entry.surf->texInfo->flags;
        const u32 rgba     = AlphaSurfaceColor(texFlags);

        if (entry.texture != batchTexture || entry.mvp != state.mvp || rgba != state.rgba)
        {
            if (batchTexture != nullptr)
            {
                FlushScratch(*batchTexture, state); // Still the outgoing state: flush before switching.
            }
            batchTexture = entry.texture;
            state.mvp    = entry.mvp;
            state.rgba   = rgba;
        }

        if (texFlags & SURF_WARP)
        {
            // Turbulent: its own fan walk, its own animated coordinates.
            DrawAnimatedWaterPolys(*entry.surf, *entry.texture, state);
            continue;
        }

        for (const mod::ModelPoly * poly = entry.surf->polys; poly != nullptr; poly = poly->next)
        {
            if (poly->numVerts >= 3) // Need at least one triangle.
            {
                GatherPolyTriangles(*poly, *entry.texture, state);
            }
        }
    }

    if (batchTexture != nullptr)
    {
        FlushScratch(*batchTexture, state);
    }

    s_alphaSurfaceCount      = 0;
    s_alphaEntityMatrixCount = 0;
}

// ------------------------------------------------------------------------------------------------
// Dynamic Lights (dlights)
// ------------------------------------------------------------------------------------------------

constexpr float kDLightCutoff = 64.0f;

// Rim points around a flare. ref_gl walks i = 16 down to 0, whose first and
// last points coincide (a = 2*PI and a = 0) purely to close the fan, so there
// are 16 distinct ones and 16 wedges.
constexpr int kNumFlareSegs = 16;

// ref_gl's 0.2 dimming of the light colour, on the 0-255 scale an untextured
// vertex needs. Not the 128 modulate identity the textured paths use: with
// PRIM's TME bit clear there is no texture function to be the identity of, so
// the vertex byte lands on screen as-is and 128 would halve every flare.
//
// Clamped at both ends because neither is guaranteed: dlight colours are 0..1
// by convention, but nothing bounds the top, and the client hands out negative
// ones for its "dark light" effects (cl_ents.c's V_AddLight(..., -1, -1, -1)),
// which would wrap catastrophically through the unsigned cast. OpenGL clamps
// these for free in glColor3f; we do it by hand.
inline u32 FlareChannel(float colorComponent)
{
    const float scaled = colorComponent * 0.2f * 255.0f;
    return (scaled >= 255.0f) ? 255u : ((scaled <= 0.0f) ? 0u : static_cast<u32>(scaled));
}

// A Quake2 Dynamic Light (dlight) is a point light simulated with a circular billboarded
// sprite that follows the light source. This is used to simulate gunshot flares for example.
// The sprite is rendered with additive blending (e.g. glBlendFunc(GL_ONE, GL_ONE) in ref_gl).
// This is the fallback codepath for when dynamic lightmaps are not enabled.
//
// Geometry is ref_gl's R_RenderDlight: a fan whose rim lies on the camera
// plane and whose centre is pulled one radius *towards* the camera, so it is
// really a shallow cone pointed at the eye rather than a flat disc.
//
// ref_gl fades the flare out by interpolating the vertex colour from the light
// colour at the centre to black at the rim, and adding that. We interpolate
// the vertex *alpha* from 1 to 0 over a flat-coloured fan instead, which the
// additive blend (Cs * As + Cd) makes arithmetically identical - both add
// colour * (1 - r) at radius fraction r - and which the flat-shaded gather
// path can actually express, since only the alpha has to vary per vertex.
// It also lets the batch's alpha test discard the invisible outer rim rather
// than blending zeroes over it.
void RenderDLights(const refdef_t & viewDef)
{
    if (s_dynamicLightmaps->value != 0.0f)
    {
        // Dynamic lights are simulated via the dynamic lightmap texture instead.
        return;
    }

    const int numDlights = viewDef.num_dlights;
    if (numDlights <= 0)
    {
        return;
    }

    // Untextured, but a batch still binds one.
    const tex::Texture & texture = tex::DebugTexture();

    SurfaceDrawState state = {
        .mvp   = &s_viewProjMatrix, // Billboards are built in world space.
        .eye   = s_eyePosition,
        .rgba  = 0, // Per light; filled in below.
        .flags = vu1::DrawFlags::Additive | vu1::DrawFlags::Untextured,
        // Camera-facing, and the wedges of one flare never overlap, so there
        // is nothing to cull. Culling them would actively hurt: the apex sits
        // a full radius off the rim plane, tilting each wedge ~45 degrees off
        // the view axis, so a light near the camera and off to one side would
        // lose whole wedges and show a pie-slice notch.
        .cullBackFaces = false,
        .vertexAlpha   = true // The centre-to-rim fade rides in st.z.
    };

    const dlight_t * light = viewDef.dlights;
    for (int l = 0; l < numDlights; ++l, ++light)
    {
        const float radius = light->intensity * 0.35f;

        state.rgba = vu1::PackColorRGBA(FlareChannel(light->color[0]),
                                        FlareChannel(light->color[1]),
                                        FlareChannel(light->color[2]), 0x80);

        // The cone apex, at full alpha.
        ClipVertex centre;
        centre.pos = { light->origin[0] - (s_forwardVec[0] * radius),
                       light->origin[1] - (s_forwardVec[1] * radius),
                       light->origin[2] - (s_forwardVec[2] * radius), 1.0f };
        centre.st  = { 0.0f, 0.0f, 1.0f, 0.0f };

        // The rim, at zero alpha. ref_gl's descending loop is the same ring
        // walked the other way round, which is why the sine is negated -
        // keeping that preserves its winding.
        ClipVertex rim[kNumFlareSegs];
        for (int i = 0; i < kNumFlareSegs; ++i)
        {
            const float angle = (static_cast<float>(i) / kNumFlareSegs) * (math::kPI * 2.0f);
            const float c     =  math::Cosf(angle) * radius;
            const float s     = -math::Sinf(angle) * radius;

            rim[i].pos = { light->origin[0] + (s_rightVec[0] * c) + (s_upVec[0] * s),
                           light->origin[1] + (s_rightVec[1] * c) + (s_upVec[1] * s),
                           light->origin[2] + (s_rightVec[2] * c) + (s_upVec[2] * s), 1.0f };
            rim[i].st  = { 0.0f, 0.0f, 0.0f, 0.0f };
        }

        // Fan to triangle list, the only topology the VU path takes.
        for (int i = 0; i < kNumFlareSegs; ++i)
        {
            ClipVertex wedge[3] = { centre, rim[i], rim[(i + 1) % kNumFlareSegs] };
            GatherTriangle(wedge, texture, state);
        }

        ++s_drawStats.dlights;
    }

    FlushScratch(texture, state);
}

void MarkDLights(const dlight_t * light, const int bit, const mod::ModelInstance & world, const mod::ModelNode * node)
{
    PS2_Assert(s_dynamicLightmaps->value != 0.0f);

    if (node->contents != -1)
    {
        return;
    }

    const cplane_t * splitPlane = node->plane;
    const float dist = DotProduct(light->origin, splitPlane->normal) - splitPlane->dist;

    if (dist > light->intensity - kDLightCutoff)
    {
        MarkDLights(light, bit, world, node->children[0]);
        return;
    }
    if (dist < -light->intensity + kDLightCutoff)
    {
        MarkDLights(light, bit, world, node->children[1]);
        return;
    }

    mod::ModelSurface * surf = world.surfaces + node->firstSurface;
    const int numSurfaces = node->numSurfaces;

    // Mark the polygons:
    for (int i = 0; i < numSurfaces; ++i, ++surf)
    {
        if (surf->dlightFrame != s_frameCount)
        {
            surf->dlightBits  = 0;
            surf->dlightFrame = s_frameCount;
        }
        surf->dlightBits |= bit;
    }

    MarkDLights(light, bit, world, node->children[0]);
    MarkDLights(light, bit, world, node->children[1]);
}

void PushDLights(const refdef_t & viewDef, const mod::ModelInstance & world)
{
    if (s_dynamicLightmaps->value == 0.0f)
    {
        // Dynamic lights are rendered as semi-transparent sprites instead.
        // Below is the dynamic lightmaps code path.
        return;
    }

    const dlight_t * light = viewDef.dlights;
    const int numDlights = viewDef.num_dlights;

    for (int l = 0; l < numDlights; ++l, ++light)
    {
        MarkDLights(light, 1 << l, world, world.nodes);
    }
}

// ------------------------------------------------------------------------------------------------
// World model pass
// ------------------------------------------------------------------------------------------------

void RenderWorldModel(const refdef_t & viewDef)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::World);

    if (viewDef.rdflags & RDF_NOWORLDMODEL)
    {
        return; // Menu/loading screens render no world.
    }
    if (s_skipWorld->value != 0.0f)
    {
        return; // Debug: skip the world pass entirely.
    }

    const mod::ModelInstance * const world = mod::GetWorldModel();
    PS2_AssertMsg(world != nullptr, "RenderFrame without a world model!");

    // World visibility pass (bsp traversal):
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::Vis);
        sky::ClearBounds();
        PushDLights(viewDef, *world);
        SetUpViewClusters(viewDef, *world);
        MarkLeaves(*world);
        RecursiveWorldNode(viewDef, *world, world->nodes);
    }

    const SurfaceDrawState state = WorldSurfaceDrawState();

    // Diffuse first, then the lightmap over it - ref_gl's DrawTextureChains()
    // followed by R_BlendLightmaps(). Both passes draw the same triangles with
    // the same transform, so they share one draw state.
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::TexChains);
        DrawTextureChains(state);
    }

    // Only profile world lightmaps here.
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::LmChains);
        DrawLightmapChains(state);
    }

    // Last of the world, where ref_gl's R_DrawWorld puts it: the opaque pass
    // above has filled the depth buffer, so the sky only costs fill where it
    // is actually visible through it.
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::Sky);
        sky::DrawSkyBox(viewDef, s_viewProjMatrix);
    }
}

// ------------------------------------------------------------------------------------------------
// Point lighting (world lightmap sampling for entity models)
// ------------------------------------------------------------------------------------------------

enum LightSampleResult : int { NoHit = -1, Hit = 0, HitColorSampled = 1 };

// Descends the BSP along the start->end segment looking for the first lit
// surface it crosses (ref_gl's RecursiveLightPoint): finds the node plane the
// segment straddles, recurses the near side, and on the way back samples the
// lightmap of the node surface containing the crossing point - each style's
// sample scaled by its current lightstyle. Returns -1 for no hit, 0 for a hit
// with no light data, 1 for a sampled hit ('outColor' and 'outLightSpot' set).
LightSampleResult RecursiveLightPoint(const mod::ModelInstance & world, const mod::ModelNode * node,
                                      const lightstyle_t * lightstyles, const vec3_t start, const vec3_t end,
                                      vec3_t outColor, vec3_t outLightSpot)
{
    if (node->contents != -1)
    {
        return NoHit; // Leaf: didn't hit anything on the way down.
    }

    // Which side(s) of this node's plane does the segment touch?
    const cplane_t * const plane = node->plane;
    const float front = DotProduct(start, plane->normal) - plane->dist;
    const float back  = DotProduct(end,   plane->normal) - plane->dist;
    const int   side  = (front < 0.0f);

    if ((back < 0.0f) == side)
    {
        // Whole segment on one side; no surface of this node can be crossed.
        return RecursiveLightPoint(world, node->children[side], lightstyles, start, end, outColor, outLightSpot);
    }

    // The segment crosses the plane at 'mid': trace the near half first.
    const float frac = front / (front - back);

    vec3_t mid;
    mid[0] = start[0] + (end[0] - start[0]) * frac;
    mid[1] = start[1] + (end[1] - start[1]) * frac;
    mid[2] = start[2] + (end[2] - start[2]) * frac;

    const auto r = RecursiveLightPoint(world, node->children[side], lightstyles, start, mid, outColor, outLightSpot);
    if (r >= Hit)
    {
        return r; // Hit something nearer.
    }

    VectorCopy(mid, outLightSpot);

    // Check the crossing point against this node's surfaces.
    const int numSurfaces = node->numSurfaces;
    const mod::ModelSurface * surf = world.surfaces + node->firstSurface;
    for (int i = 0; i < numSurfaces; ++i, ++surf)
    {
        if (HasFlag(surf->flags, mod::SurfaceFlags::DrawTurb | mod::SurfaceFlags::DrawSky))
        {
            continue; // No lightmaps on water/sky.
        }

        const mod::ModelTexInfo * tex = surf->texInfo;

        const int s = static_cast<int>(DotProduct(mid, tex->vecs[0]) + tex->vecs[0][3]);
        const int t = static_cast<int>(DotProduct(mid, tex->vecs[1]) + tex->vecs[1][3]);

        if (s < surf->textureMins[0] || t < surf->textureMins[1])
        {
            continue;
        }

        int ds = s - surf->textureMins[0];
        int dt = t - surf->textureMins[1];

        if (ds > surf->extents[0] || dt > surf->extents[1])
        {
            continue;
        }

        if (surf->samples == nullptr)
        {
            return Hit; // Hit, but the surface carries no light data.
        }

        // 16-texel lightmap granularity, one RGB triplet per luxel, one
        // whole map per active style.
        ds >>= 4;
        dt >>= 4;

        VectorClear(outColor);

        const u8 * lightmap = surf->samples;
        lightmap += 3 * (dt * ((surf->extents[0] >> 4) + 1) + ds);

        // Scaled by ps2_lightmap_modulate for the same reason the atlases are
        // (ref_gl applies gl_modulate in both places): an entity standing on a
        // surface should take the brightness that surface is drawn at, or
        // turning the knob up would light the world and leave everything in it
        // behind.
        const float modulate = s_lightmapModulate->value * (1.0f / 255.0f);

        for (int map = 0; map < mod::kMaxLightmaps && surf->styles[map] != 255; ++map)
        {
            const float * styleRGB = lightstyles[surf->styles[map]].rgb;

            outColor[0] += lightmap[0] * styleRGB[0] * modulate;
            outColor[1] += lightmap[1] * styleRGB[1] * modulate;
            outColor[2] += lightmap[2] * styleRGB[2] * modulate;

            lightmap += 3 * ((surf->extents[0] >> 4) + 1) * ((surf->extents[1] >> 4) + 1);
        }

        return HitColorSampled;
    }

    // Nothing on this node; carry on down the far half of the segment.
    return RecursiveLightPoint(world, node->children[!side], lightstyles, mid, end, outColor, outLightSpot);
}

// ------------------------------------------------------------------------------------------------
// Brush model entities (inline BSP submodels: doors, plats, trains, buttons)
// ------------------------------------------------------------------------------------------------

// A surface is drawn when the camera is on its front side by more than this,
// in world units (ref_gl's BACKFACE_EPSILON). Surfaces the camera is level
// with are edge-on and contribute nothing.
constexpr float kBackFaceEpsilon = 0.01f;

// An inline submodel's surfaces live in the world model's surface array but
// hang off the submodel's own BSP subtree, so the world walk never reaches
// them - they are drawn here instead, transformed by the entity that carries
// them. There is no PVS or texture chaining involved: the surface count is
// small and the transform is per-entity, so each one is gathered immediately
// and batched only across runs of the same texture.
void DrawBrushModelEntity(const refdef_t & viewDef, const entity_t & entity)
{
    if (s_skipBrushModels->value != 0.0f)
    {
        return; // Debug: skip brush model entities.
    }

    const auto * model = reinterpret_cast<const mod::ModelInstance *>(entity.model);
    PS2_Assert(model != nullptr);

    if (model->numModelSurfaces == 0)
    {
        return; // Submodel with no faces of its own (a pure clip brush).
    }

    // Frustum cull. A rotated submodel has no axis-aligned box that survives
    // the rotation, so it falls back to the model's bounding sphere.
    const bool rotated = (entity.angles[0] != 0.0f ||
                          entity.angles[1] != 0.0f ||
                          entity.angles[2] != 0.0f);
    vec3_t mins, maxs;
    if (rotated)
    {
        for (int i = 0; i < 3; ++i)
        {
            mins[i] = entity.origin[i] - model->radius;
            maxs[i] = entity.origin[i] + model->radius;
        }
    }
    else
    {
        const float * const modelMins = &model->mins.x;
        const float * const modelMaxs = &model->maxs.x;
        for (int i = 0; i < 3; ++i)
        {
            mins[i] = entity.origin[i] + modelMins[i];
            maxs[i] = entity.origin[i] + modelMaxs[i];
        }
    }
    if (ShouldCullBBox(mins, maxs))
    {
        return;
    }

    ++s_drawStats.entities;

    // The per-surface side test runs in model space, so the camera goes there
    // rather than every surface plane coming out - one transform instead of N.
    vec3_t modelOrigin;
    VectorSubtract(viewDef.vieworg, entity.origin, modelOrigin);
    if (rotated)
    {
        vec3_t temp, forward, right, up;
        VectorCopy(modelOrigin, temp);
        math::AngleVectors(entity.angles, forward, right, up);

        modelOrigin[0] =  DotProduct(temp, forward);
        modelOrigin[1] = -DotProduct(temp, right);
        modelOrigin[2] =  DotProduct(temp, up);
    }

    const math::Mat4 mvp = MakeEntityMatrix(entity, /*flipPitchAngle=*/false) * s_viewProjMatrix;

    // Calculate dynamic lighting for bmodel
    if (s_dynamicLightmaps->value != 0.0f)
    {
        const mod::ModelInstance * const world = mod::GetWorldModel();
        PS2_Assert(world != nullptr);

        const int numDlights = viewDef.num_dlights;
        const dlight_t * light = viewDef.dlights;

        for (int l = 0; l < numDlights; ++l, ++light)
        {
            MarkDLights(light, 1 << l, *world, model->nodes + model->firstNode);
        }
    }

    // ref_gl draws translucent brush models at a flat quarter alpha rather
    // than the entity's own (glColor4f(1,1,1,0.25) in R_DrawBrushModel).
    const bool translucent = (entity.flags & RF_TRANSLUCENT) != 0;
    SurfaceDrawState state = {
        .mvp   = &mvp,
        // The surfaces are model-space, so the back-face test takes the same
        // model-space camera the plane-side test above uses.
        .eye   = { modelOrigin[0], modelOrigin[1], modelOrigin[2], 1.0f },
        .rgba  = translucent ? vu1::PackColorRGBA(128, 128, 128, 0x80 / 4) : kFullBright,
        .flags = translucent ? vu1::DrawFlags::Blended : vu1::DrawFlags::None,
        .cullBackFaces = WorldBackFaceCullEnabled(),
        .vertexAlpha   = false
    };

    const bool tinted = LightmapColorEnabled();

    // Consecutive surfaces usually share a texture, so the batch only breaks
    // when it actually changes.
    const tex::Texture * batchTexture = nullptr;

    // This entity's transform, parked for the deferred alpha pass on the first
    // translucent surface that needs it (most models have none at all).
    const math::Mat4 * alphaMvp = nullptr;

    mod::ModelSurface * surf = model->surfaces + model->firstModelSurface;
    for (int i = 0; i < model->numModelSurfaces; ++i, ++surf)
    {
        const cplane_t & plane = *surf->plane;
        const float dot = DotProduct(modelOrigin, plane.normal) - plane.dist;

        const bool planeBack = HasFlag(surf->flags, mod::SurfaceFlags::PlaneBack);
        if (!(( planeBack && dot < -kBackFaceEpsilon) ||
              (!planeBack && dot >  kBackFaceEpsilon)))
        {
            continue; // Facing away from the camera.
        }

        if (surf->texInfo->flags & (SURF_TRANS33 | SURF_TRANS66 | SURF_WARP))
        {
            // Deferred to the back-to-front alpha pass, along with this
            // entity's transform: unlike ref_gl's, that pass draws a moving
            // submodel's water/glass where the submodel actually is rather
            // than at the map's rest position. Parked on the first such
            // surface; every later one shares the slot.
            if (alphaMvp == nullptr)
            {
                alphaMvp = StoreAlphaEntityMatrix(mvp);
            }
            if (alphaMvp != nullptr)
            {
                PushAlphaSurface(*surf, *TextureAnimation(surf->texInfo, entity.frame), *alphaMvp);
            }
            continue;
        }

        // Rebuild the surface's luxels if its lighting moved and chain it for
        // this entity's lightmap pass below. Before the gather, not after: the
        // gather samples the chroma those luxels were just rebuilt into, and
        // would otherwise read a frame behind under a moving dynamic light.
        //
        // Skipped entirely for a translucent submodel: its surfaces are blended
        // into the scene at a flat quarter alpha, so modulating the framebuffer
        // afterwards would darken whatever shows through them as well.
        const bool lit = !translucent && (surf->lightmapTextureNum != mod::kNotLightmapped);
        if (lit)
        {
            lm::ChainSurface(*surf, viewDef, s_frameCount);
        }
        state.lightmapColors = (lit && tinted) ? SurfaceLightmapColors(*surf) : nullptr;

        const tex::Texture * texture = TextureAnimation(surf->texInfo, entity.frame);
        if (texture != batchTexture)
        {
            if (batchTexture != nullptr)
            {
                FlushScratch(*batchTexture, state);
            }
            batchTexture = texture;
        }

        ++s_drawStats.surfaces;
        for (const mod::ModelPoly * poly = surf->polys; poly != nullptr; poly = poly->next)
        {
            if (poly->numVerts >= 3)
            {
                GatherPolyTriangles(*poly, *texture, state);
            }
        }
    }

    if (batchTexture != nullptr)
    {
        FlushScratch(*batchTexture, state);
    }

    // Light the surfaces just drawn, in this entity's own space. The chains are
    // per-atlas and shared with the world pass, but that one has already drawn
    // and cleared them, so what is queued here is only this model's.
    if (!translucent)
    {
        DrawLightmapChains(state);
    }
}

// ------------------------------------------------------------------------------------------------
// Sprite entities (.sp2: explosions, flashes, the power-up glows)
// ------------------------------------------------------------------------------------------------

// A sprite is one camera-facing quad, sized and anchored by its frame: the
// frame's origin_x/origin_y say where in the image the entity's origin sits,
// so the quad hangs off the view's right/up vectors from there. No culling -
// it is two triangles, and the clipper rejects it if it is off screen anyway.
void DrawSpriteEntity(const entity_t & entity)
{
    if (s_skipSprites->value != 0.0f)
    {
        return; // Debug: skip sprite entities.
    }

    const auto * model = reinterpret_cast<const mod::ModelInstance *>(entity.model);
    PS2_Assert(model != nullptr && model->hunkBase != nullptr);

    // The sprite hunk holds the SP2 file image verbatim (see model_load.cpp).
    const auto * sprite = static_cast<const dsprite_t *>(model->hunkBase);
    if (sprite->numframes <= 0)
    {
        return;
    }

    // The engine cycles entity.frame freely and expects the sprite to wrap.
    const int frameNum = (entity.frame >= 0) ? (entity.frame % sprite->numframes) : 0;
    const dsprframe_t & frame = sprite->frames[frameNum];

    const tex::Texture * skin = model->skins[frameNum];
    if (skin == nullptr)
    {
        skin = &tex::DebugTexture(); // Frame's .pcx failed to load.
    }

    ++s_drawStats.entities;

    // Sprite images are rarely power-of-two (48x48, 144x144), and normalized
    // ST spans the power-of-two TEX0 extent, not the image.
    float stScaleS, stScaleT;
    tex::StScaleFor(*skin, &stScaleS, &stScaleT);

    // Clamped because the alpha is whatever the game code put on the entity;
    // on the GS 0x80 is 1.0 and anything above it reads as overbright.
    float alpha = (entity.flags & RF_TRANSLUCENT) ? entity.alpha : 1.0f;
    alpha = (alpha < 0.0f) ? 0.0f : ((alpha > 1.0f) ? 1.0f : alpha);

    const SurfaceDrawState state = {
        .mvp   = &s_viewProjMatrix, // The quad is built in world space already.
        .eye   = s_eyePosition,
        .rgba  = vu1::PackColorRGBA(128, 128, 128, static_cast<u32>(alpha * 128.0f)),
        .flags = (alpha < 1.0f) ? vu1::DrawFlags::Blended : vu1::DrawFlags::None,
        // Never back-face culled: the quad is built from the camera's own
        // right/up vectors, so it cannot face away and the test would be useless.
        .cullBackFaces = false,
        .vertexAlpha   = false
    };

    // The four corners, in ref_gl's order: bottom-left, top-left, top-right,
    // bottom-right, with T running down the image the way the pixels are
    // stored. 'right' and 'up' are the camera's, which is what makes the quad
    // face it.
    const float leftOffset   = -static_cast<float>(frame.origin_x);
    const float rightOffset  =  static_cast<float>(frame.width - frame.origin_x);
    const float bottomOffset = -static_cast<float>(frame.origin_y);
    const float topOffset    =  static_cast<float>(frame.height - frame.origin_y);

    const struct { float along, up, s, t; } layout[4] = {
        { leftOffset,  bottomOffset, 0.0f, 1.0f },
        { leftOffset,  topOffset,    0.0f, 0.0f },
        { rightOffset, topOffset,    1.0f, 0.0f },
        { rightOffset, bottomOffset, 1.0f, 1.0f },
    };

    ClipVertex quad[4];
    for (int i = 0; i < 4; ++i)
    {
        quad[i].pos = {
            entity.origin[0] + (s_rightVec[0] * layout[i].along) + (s_upVec[0] * layout[i].up),
            entity.origin[1] + (s_rightVec[1] * layout[i].along) + (s_upVec[1] * layout[i].up),
            entity.origin[2] + (s_rightVec[2] * layout[i].along) + (s_upVec[2] * layout[i].up),
            1.0f
        };
        quad[i].st = { layout[i].s * stScaleS, layout[i].t * stScaleT, 0.0f, 0.0f };
    }

    ClipVertex triangle[3] = { quad[0], quad[1], quad[2] };
    GatherTriangle(triangle, *skin, state);

    triangle[0] = quad[0];
    triangle[1] = quad[2];
    triangle[2] = quad[3];
    GatherTriangle(triangle, *skin, state);

    FlushScratch(*skin, state);
}

// ------------------------------------------------------------------------------------------------
// Beam entities (RF_BEAM: the lightning/railgun cylinders)
// ------------------------------------------------------------------------------------------------

// A beam is a cylinder spanning entity.origin -> entity.oldorigin, built as a
// ring of segments around that axis. It has no model and no texture: the
// colour is a palette index in skinnum and the diameter is entity.frame
// (ref_gl's R_DrawBeam).
constexpr int kNumBeamSegs = 6;

void DrawBeamEntity(const entity_t & entity)
{
    vec3_t direction, normalizedDirection;
    VectorSubtract(entity.oldorigin, entity.origin, direction);
    VectorCopy(direction, normalizedDirection);

    if (VectorNormalize(normalizedDirection) == 0.0f)
    {
        return; // Zero length. Also how the client's ex_flash explosions - which
                // borrow RF_BEAM purely as a "don't draw me" marker, comment and
                // all - end up drawing nothing.
    }

    vec3_t perpVec;
    PerpendicularVector(perpVec, normalizedDirection);
    VectorScale(perpVec, static_cast<float>(entity.frame) / 2.0f, perpVec);

    ++s_drawStats.entities;

    const float alpha = (entity.alpha > 0.0f && entity.alpha <= 1.0f) ? entity.alpha : 1.0f;

    const SurfaceDrawState state = {
        .mvp   = &s_viewProjMatrix, // Built in world space.
        .eye   = s_eyePosition,
        .rgba  = (global_palette[entity.skinnum & 0xFF] & 0x00FFFFFF) |
                 (static_cast<u32>(alpha * 128.0f) << 24),
        .flags = vu1::DrawFlags::Blended | vu1::DrawFlags::Untextured,
        // A cylinder does have a far half, but keeping it only makes the blend
        // slightly denser - whereas culling it with the ring's winding guessed
        // wrong would turn the beam inside out.
        .cullBackFaces = false,
        .vertexAlpha   = false
    };

    math::Vec4 startPoints[kNumBeamSegs];
    math::Vec4 endPoints[kNumBeamSegs];

    for (int i = 0; i < kNumBeamSegs; ++i)
    {
        vec3_t start;
        RotatePointAroundVector(start, normalizedDirection, perpVec,
                                (360.0f / kNumBeamSegs) * static_cast<float>(i));
        VectorAdd(start, entity.origin, start);

        startPoints[i] = { start[0], start[1], start[2], 1.0f };
        endPoints[i]   = { start[0] + direction[0],
                           start[1] + direction[1],
                           start[2] + direction[2], 1.0f };
    }

    // Untextured, but a batch still binds one.
    const tex::Texture & texture = tex::DebugTexture();
    const math::Vec4 zero = { 0.0f, 0.0f, 0.0f, 0.0f };

    // ref_gl walks the ring as one triangle strip of (start[i], end[i],
    // start[i+1], end[i+1]) groups; expanded to the triangle lists the VU
    // path takes, each group is the two triangles closing one wall quad.
    for (int i = 0; i < kNumBeamSegs; ++i)
    {
        const int next = (i + 1) % kNumBeamSegs;

        ClipVertex quad[4];
        quad[0].pos = startPoints[i];
        quad[1].pos = endPoints[i];
        quad[2].pos = startPoints[next];
        quad[3].pos = endPoints[next];
        for (ClipVertex & c : quad)
        {
            c.st = zero;
        }

        ClipVertex triangle[3] = { quad[0], quad[1], quad[2] };
        GatherTriangle(triangle, texture, state);

        triangle[0] = quad[2];
        triangle[1] = quad[1];
        triangle[2] = quad[3];
        GatherTriangle(triangle, texture, state);
    }

    FlushScratch(texture, state);
}

// ------------------------------------------------------------------------------------------------
// Null models (placeholder for an entity whose model is missing)
// ------------------------------------------------------------------------------------------------

// ref_gl's R_DrawNullModel: a small octahedron lit by the world at the
// entity's position, so a model that failed to load is loudly visible instead
// of silently absent.
void DrawNullModelEntity(const refdef_t & viewDef, const entity_t & entity)
{
    vec3_t color = { 1.0f, 1.0f, 1.0f };
    if (!(entity.flags & RF_FULLBRIGHT))
    {
        vec3_t lightSpot = {};
        CalcPointLightColor(viewDef, entity.origin, color, lightSpot);
    }

    ++s_drawStats.entities;

    const auto channel = [](float c) -> u32
    {
        const float scaled = c * 128.0f; // 128 = the GS modulate identity.
        return (scaled >= 255.0f) ? 255u : ((scaled <= 0.0f) ? 0u : static_cast<u32>(scaled));
    };

    // Null models take the brush convention: ref_gl calls R_RotateForEntity
    // directly here, without the pitch flip the alias path wraps it in.
    const math::Mat4 mvp = MakeEntityMatrix(entity, /*flipPitchAngle=*/false) * s_viewProjMatrix;

    const SurfaceDrawState state = {
        .mvp   = &mvp,
        .eye   = s_eyePosition, // Unused; the octahedron is not culled (below).
        .rgba  = vu1::PackColorRGBA(channel(color[0]), channel(color[1]), channel(color[2]), 0x80),
        .flags = vu1::DrawFlags::None,
        // Eight triangles for a debug marker: not worth risking the fans
        // coming out inside-out and hiding the very thing they exist to show.
        .cullBackFaces = false,
        .vertexAlpha   = false
    };

    constexpr float kRadius = 16.0f;
    constexpr float kApex   = 16.0f;

    // The square ring in the entity's XY plane both fans close over.
    math::Vec4 ring[5];
    for (int i = 0; i <= 4; ++i)
    {
        const float angle = static_cast<float>(i) * math::kHalfPI;
        ring[i] = { kRadius * math::Cosf(angle), kRadius * math::Sinf(angle), 0.0f, 1.0f };
    }

    // The pink checkerboard doubles as the "this model is missing" signal;
    // ref_gl draws the octahedron untextured, in flat shadelight.
    const tex::Texture & texture = tex::DebugTexture();

    for (int half = 0; half < 2; ++half)
    {
        ClipVertex apex;
        apex.pos = { 0.0f, 0.0f, (half == 0) ? -kApex : kApex, 1.0f };
        apex.st  = { 0.5f, 0.5f, 0.0f, 0.0f };

        for (int i = 0; i < 4; ++i)
        {
            // The top half walks the ring backwards, so both cones wind the
            // same way seen from outside.
            const int a = (half == 0) ? i : (4 - i);
            const int b = (half == 0) ? (i + 1) : (3 - i);

            ClipVertex triangle[3];
            triangle[0] = apex;
            triangle[1].pos = ring[a];
            triangle[1].st  = { 0.0f, 1.0f, 0.0f, 0.0f };
            triangle[2].pos = ring[b];
            triangle[2].st  = { 1.0f, 1.0f, 0.0f, 0.0f };

            GatherTriangle(triangle, texture, state);
        }
    }

    FlushScratch(texture, state);
}

// ------------------------------------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------------------------------------

// The client's particle list as camera-facing billboards, expanded entirely on
// VU1: the EE writes one quadword per particle - packed colour and world origin
// - and transforms nothing.
//
// Each particle is a soft round sprite anchored at its origin and spanning the
// camera's (up + right), blown up 1.5x as ref_gl does. Because up and right are
// orthogonal to forward, every corner shares the anchor's depth, so the quad
// projects to an axis-aligned screen rectangle and draws as a single GS sprite
// (see particles.vcl). The distance blow-up that keeps far particles a pixel
// wide rides along in the microprogram.
//
// Blended, and DrawFlags::Blended masks depth writes, which is what keeps a
// cloud of them from z-fighting itself (ref_gl's glDepthMask(FALSE)).
void RenderParticles(const refdef_t & viewDef)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::Particles);

    const int numParticles = (viewDef.num_particles < MAX_PARTICLES) ? viewDef.num_particles : MAX_PARTICLES;
    if (numParticles <= 0 || s_skipParticles->value != 0.0f)
    {
        return;
    }

    const tex::Texture & texture = tex::ParticleTexture();

    // The billboard's diagonal: ref_gl's 1.5x blow-up of the camera basis, and
    // the only direction the microprogram needs, since the sprite is described
    // by its anchor corner and the opposite one.
    const math::Vec3 quadOffset = {
        (s_upVec[0] + s_rightVec[0]) * 1.5f,
        (s_upVec[1] + s_rightVec[1]) * 1.5f,
        (s_upVec[2] + s_rightVec[2]) * 1.5f,
    };

    // One qword per particle, gathered here and referenced in place by the DMA.
    // File-level static for the same reason the triangle batches are: far too
    // large for the stack, and draws are synchronous, so one buffer serves the
    // whole list.
    alignas(16) static vu1::ParticleVertex s_particles[MAX_PARTICLES];

    for (int i = 0; i < numParticles; ++i)
    {
        const particle_t & p = viewDef.particles[i];

        const float alpha = (p.alpha < 0.0f) ? 0.0f : ((p.alpha > 1.0f) ? 1.0f : p.alpha);
        const u32   color = (global_palette[p.color & 0xFF] & 0x00FFFFFF) | (static_cast<u32>(alpha * 128.0f) << 24); 

        vu1::ParticleVertex & dst = s_particles[i];
        dst.rgba = color;
        dst.x = p.origin[0];
        dst.y = p.origin[1];
        dst.z = p.origin[2];
    }

    s_drawStats.particles += numParticles;
    ++s_drawStats.drawBatches;

    vu1::DrawParticles(s_viewProjMatrix, texture, quadOffset, s_particles, numParticles, vu1::DrawFlags::Blended);
}

// ------------------------------------------------------------------------------------------------
// Entity pass
// ------------------------------------------------------------------------------------------------

void RenderEntities(const refdef_t & viewDef, const bool isTranslucentPass)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::Entities);

    if (s_skipEntities->value != 0.0f)
    {
        return; // Debug: skip all entity models.
    }

    const int numEntities = viewDef.num_entities;
    for (int e = 0; e < numEntities; ++e)
    {
        const entity_t & entity = viewDef.entities[e];

        // Translucents draw after every solid, so they blend over a finished
        // opaque scene rather than whatever happened to be drawn so far.
        const bool translucent = (entity.flags & RF_TRANSLUCENT) != 0;
        if (translucent != isTranslucentPass)
        {
            continue;
        }

        // Debug: Skip drawing the weapon model.
        if ((entity.flags & RF_WEAPONMODEL) && s_skipWeaponModel->value != 0.0f)
        {
            continue;
        }

        // RF_BEAM wins over whatever model the entity carries - the client
        // attaches one to its ex_flash explosions and still expects a beam
        // (a degenerate, invisible one) rather than that model.
        if (entity.flags & RF_BEAM)
        {
            DrawBeamEntity(entity);
            continue;
        }

        // entity_t::model is opaque outside the refresh module, hence the cast.
        const auto * model = reinterpret_cast<const mod::ModelInstance *>(entity.model);
        if (model == nullptr || s_forceNullModels->value != 0.0f)
        {
            DrawNullModelEntity(viewDef, entity);
            continue;
        }

        switch (model->type)
        {
        case mod::ModelType::AliasMD2:
            DrawAliasMD2Entity(viewDef, entity, (entity.flags & RF_WEAPONMODEL)
                                              ? s_weaponViewProjMatrix
                                              : s_viewProjMatrix);
            break;

        case mod::ModelType::Brush:
            DrawBrushModelEntity(viewDef, entity);
            break;

        case mod::ModelType::Sprite:
            DrawSpriteEntity(entity);
            break;
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    s_backFaceCull      = Cvar_Get("ps2_backface_cull",       "0", 0); // NOTE: Off by default. BSP already culls backfacing surfaces.
    s_skipWorld         = Cvar_Get("ps2_skip_world",          "0", 0);
    s_skipAlphaSurfaces = Cvar_Get("ps2_skip_alpha_surfaces", "0", 0); // Debug: drop the translucent glass/water pass.
    s_skipBrushModels   = Cvar_Get("ps2_skip_brushmodels",    "0", 0);
    s_skipSprites       = Cvar_Get("ps2_skip_sprites",        "0", 0);
    s_skipEntities      = Cvar_Get("ps2_skip_entities",       "0", 0);
    s_skipParticles     = Cvar_Get("ps2_skip_particles",      "0", 0);
    s_forceNullModels   = Cvar_Get("ps2_force_null_models",   "0", 0); // Debug: draw every entity as the octahedron placeholder.
    s_skipWeaponModel   = Cvar_Get("ps2_skip_weapon_model",   "0", 0);
    s_dynamicLightmaps  = Cvar_Get("ps2_dynamic_lightmaps",   "1", 0); // Uses the RenderDLights flare fallback path when = 0.
    s_lightmaps         = Cvar_Get("ps2_lightmaps",           "1", 0); // Debug: 0 drops the lightmap pass, leaving the world fullbright.
    s_lightmapOnly      = Cvar_Get("ps2_lightmap_only",       "0", 0); // Debug: 1 drops the diffuse textures, showing the lighting alone.
    s_lightmapColor     = Cvar_Get("ps2_lightmap_color",      "1", 0); // Debug: 0 drops the per-vertex luxel chroma, leaving lighting monochrome.
    s_polyblend         = Cvar_Get("ps2_polyblend",           "1", 0); // ref_gl's gl_polyblend: the full screen damage/powerup/underwater tint.

    // Registered by the lightmap manager, which owns it; this resolves the same
    // object so the entity lighting can scale by it too.
    s_lightmapModulate = Cvar_Get("ps2_lightmap_modulate", "1", CVAR_ARCHIVE);

    // Already registered by the client; this just resolves the same object.
    s_lightLevel = Cvar_Get("r_lightlevel", "0", 0);

    // ref_gl's r_turbsin, at full precision (see kTurbSinAmplitude).
    constexpr float kRadiansPerStep = (2.0f * math::kPI) / static_cast<float>(kTurbSinSize);
    for (int i = 0; i < kTurbSinSize; ++i)
    {
        s_turbSin[i] = kTurbSinAmplitude * math::Sinf(static_cast<float>(i) * kRadiansPerStep);
    }

    sky::InitSkyRendering();
    view::InitEntityRendering();
}

void BeginRegistration()
{
    s_drawStats = {};

    // New map: forget the previous map's clusters so the first frame re-marks.
    s_viewCluster     = kInvalidCluster;
    s_viewCluster2    = kInvalidCluster;
    s_oldViewCluster  = kInvalidCluster;
    s_oldViewCluster2 = kInvalidCluster;
}

DrawStats & GetDrawStats()
{
    return s_drawStats;
}

math::Mat4 MakeEntityMatrix(const entity_t & entity, const bool flipPitchAngle)
{
    const float pitch = flipPitchAngle ? entity.angles[PITCH] : -entity.angles[PITCH];

    return math::RotationX(math::DegToRad(-entity.angles[ROLL])) *
           math::RotationY(math::DegToRad(pitch))                *
           math::RotationZ(math::DegToRad(entity.angles[YAW]))   *
           math::Translation(entity.origin[0], entity.origin[1], entity.origin[2]);
}

void CalcPointLightColor(const refdef_t & viewDef, const vec3_t point,
                         vec3_t outColor, vec3_t outLightSpot)
{
    const mod::ModelInstance * world = mod::GetWorldModel();

    if (world == nullptr || world->lightData == nullptr)
    {
        // No world or a map compiled without light data: fullbright.
        VectorSet(outColor, 1.0f, 1.0f, 1.0f);
        return;
    }

    // Trace straight down; 2048 units reaches the floor from anywhere sane.
    const vec3_t endPoint = { point[0], point[1], point[2] - 2048.0f };

    vec3_t sampled = {};
    const auto r = RecursiveLightPoint(*world, world->nodes, viewDef.lightstyles,
                                       point, endPoint, sampled, outLightSpot);
    if (r == NoHit)
    {
        VectorClear(outColor); // Left the world without hitting anything.
    }
    else
    {
        VectorCopy(sampled, outColor);
    }

    // Add the frame's dynamic lights, falling off linearly with distance.
    const dlight_t * dl = viewDef.dlights;
    const int numDlights = viewDef.num_dlights;
    for (int i = 0; i < numDlights; ++i, ++dl)
    {
        vec3_t dist;
        VectorSubtract(point, dl->origin, dist);

        const float add = (dl->intensity - VectorLength(dist)) * (1.0f / 256.0f);
        if (add > 0.0f)
        {
            outColor[0] += add * dl->color[0];
            outColor[1] += add * dl->color[1];
            outColor[2] += add * dl->color[2];
        }
    }
}

bool FrustumCullsPoints(const math::Vec4 * points, int numPoints)
{
    // Each point's mask has one bit per side plane it is outside of; the AND
    // across all points is nonzero exactly when one plane excludes them all.
    // (Conservative: a box crossing a frustum corner passes and draws.)
    u32 aggregate = ~0u;
    for (int i = 0; i < numPoints; ++i)
    {
        // One transform yields all four signed plane distances at once; the
        // point's w must be 1 for the -dist row to land. Callers build these
        // corners with w = 1 already (see ShouldCullEntity).
        const math::Vec4 distances = math::Transform(points[i], s_frustumPlanes);

        u32 mask = 0;
        if (distances.x < 0.0f) { mask |= (1u << 0); }
        if (distances.y < 0.0f) { mask |= (1u << 1); }
        if (distances.z < 0.0f) { mask |= (1u << 2); }
        if (distances.w < 0.0f) { mask |= (1u << 3); }

        aggregate &= mask;
        if (aggregate == 0)
        {
            return false; // No plane excludes every point seen so far.
        }
    }
    return true;
}

SphereCull FrustumCullsSphere(const vec3_t center, const float radius)
{
    // The side planes all pass through the eye and carry unit normals (they are
    // rotations of the view basis - see SetUpFrustum), so the dot product minus
    // 'dist' is a true signed distance and comparing it against a radius is
    // exact rather than an approximation.
    bool allInside = true;

    for (const cplane_t & plane : s_frustum)
    {
        const float distance = DotProduct(center, plane.normal) - plane.dist;

        if (distance < -radius)
        {
            return SphereCull::Outside; // Wholly behind this plane.
        }
        if (distance < radius)
        {
            allInside = false; // Crosses it; some corner could be either side.
        }
    }

    return allInside ? SphereCull::Inside : SphereCull::Straddling;
}

void RenderFrame(const refdef_t & viewDef)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::View);

    PS2_Assert(viewDef.width > 0 && viewDef.height > 0);
    s_drawStats              = {};
    s_alphaSurfaceCount      = 0;
    s_alphaEntityMatrixCount = 0;
    lm::BeginFrame();

    SetupFrame(viewDef);

    // Opaque world surfaces and skybox, followed by opaque and translucent entities.
    RenderWorldModel(viewDef);
    RenderEntities(viewDef, /*isTranslucentPass=*/false);
    RenderEntities(viewDef, /*isTranslucentPass=*/true);

    // Simulated light sources with additive blending. Before the two passes
    // below, where ref_gl's R_RenderView puts R_RenderDlights: all three are
    // depth-write masked, so what the order decides is which of them get to
    // blend *over* a flare. Water and particles in front of one should dim it.
    RenderDLights(viewDef);

    // Particles next, as ref_gl does: they are blended and depth-write
    // masked, so they need the opaque scene already laid down behind them.
    RenderParticles(viewDef);

    // Then the translucent world and brush model surfaces the passes above
    // set aside, blended over the whole finished scene.
    RenderAlphaSurfaces();

    // Nothing to draw: hands the light at the camera back to the game code.
    // Where ref_gl's R_RenderFrame calls R_SetLightLevel.
    SetLightLevel(viewDef);

    // Last, over the finished scene: ref_gl's R_Flash/R_PolyBlend.
    // (powerups/damange fullscreen blended polygon).
    RenderBlendedOverlay(viewDef);
}

} // namespace ps2::view
