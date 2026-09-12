/* ================================================================================================
 * File: render_md2.cpp
 * Brief: MD2 "alias" entity model rendering.
 *
 *  An MD2 pose is two keyframes of byte-quantized vertices interpolated by the
 *  entity's backlerp: position = move + vCur * frontv + vOld * backv, where the
 *  three uniform vectors fold together the frames' decode scale/translate, the
 *  lerp factors and the entity's origin delta. The mesh itself is a flat list of
 *  mod::AliasVertex, three per triangle, which model_load.cpp expanded once at
 *  load from the file's "glcmds" tristrips and trifans - so the draw paths walk
 *  it linearly with no primitive state, and every vertex index and normal index
 *  in it was validated and clamped there rather than here. The keyframes are the
 *  one part of the file the hunk still holds verbatim.
 *
 *  The interpolation itself runs on VU1 (lerped_triangles.vcl): the expansion
 *  streams the two keyframes' dtrivertx_t bytes verbatim and the microprogram
 *  converts and lerps them, with the pose's uniform 'move' translation folded
 *  into the MVP's row 3 so only the two scale vectors ride with each batch.
 *  A scalar EE path (ref_gl's shape: lerp into s_lerpedPositions[], draw
 *  through the plain textured program) is kept behind ps2_md2_vu_lerp=0 as
 *  the A/B debug path, and carries the powersuit-shell models, whose
 *  per-vertex normal extrusion needs an EE-side table lookup. It also carries
 *  the view weapon, which is the one model the camera sits inside and so has
 *  to be clipped on the EE rather than whole-triangle rejected by the VU.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/render_md2.h"
#include "ps2/renderer/render_view.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/model.h"
#include "ps2/renderer/clip.h"
#include "ps2/renderer/batch.h"
#include "ps2/renderer/vu1.h"
#include "ps2/math/vec_mat.h"
#include "ps2/renderer/render_profile.h"

namespace ps2::view {
namespace {

// ------------------------------------------------------------------------------------------------
// Config Cvars
// ------------------------------------------------------------------------------------------------

static const cvar_t * s_lerpModels = nullptr;
static const cvar_t * s_vuLerp     = nullptr;
static const cvar_t * s_cullFace   = nullptr;
static const cvar_t * s_shadows    = nullptr;
static const cvar_t * s_clipWeapon = nullptr;

// ------------------------------------------------------------------------------------------------
// Vertex lighting tables
// ------------------------------------------------------------------------------------------------

constexpr int kShadeDotQuant = 16;

// Guaranteed by LoadAliasMD2Model, which clamps every keyframe's
// lightnormalindex to this at load. That is what lets the tables below be
// indexed straight off a vertex word with no mask.
using mod::kNumVertexNormals;

// Pre-calculated dot products of the 162 MD2 vertex normals against the fixed
// shade direction, for 16 quantized model yaws (ref_gl's anormtab; the values
// bake in ref_gl's shading ramp, hence some exceed 1.0 - clamp after scaling),
// and the normals themselves, which the powersuit shell extrudes along.
// Both tables are kept verbatim from id's ref_gl, whose double literals lack
// the .f suffix - hence the -Wfloat-conversion waiver.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-conversion"
static const float s_vertexNormalDots[kShadeDotQuant][256] = {
    #include "client/anormtab.h"
};
static const float s_vertexNormals[kNumVertexNormals][3] = {
    #include "client/anorms.h"
};
#pragma GCC diagnostic pop

// The shade-dot row for the entity's yaw.
Q_ALWAYS_INLINE const float * GetShadeDotsForEntity(const entity_t & entity)
{
    const u32 row = static_cast<u32>(static_cast<int>(
        entity.angles[YAW] * (kShadeDotQuant / 360.0f))) & (kShadeDotQuant - 1);
    return s_vertexNormalDots[row];
}

// ------------------------------------------------------------------------------------------------
// Converted mesh accessors
// ------------------------------------------------------------------------------------------------

Q_ALWAYS_INLINE const mod::ModelInstance::AliasData & GetAliasMesh(const mod::ModelInstance & model)
{
    PS2_Assert(model.type == mod::ModelType::AliasMD2 && model.hunkBase != nullptr);
    return model.Alias();
}

// Keyframes are the one part of the file the loader keeps verbatim, so they are
// still daliasframe_t records - just packed at a stride of our own rather than
// behind the file's ofs_frames.
Q_ALWAYS_INLINE const daliasframe_t * GetAliasFrame(const mod::ModelInstance::AliasData & mesh, const int frameIndex)
{
    return static_cast<const daliasframe_t *>(static_cast<const void *>(
        mesh.frames + (static_cast<u32>(frameIndex) * mesh.frameStride)));
}

// A keyframe's vertex array read as words rather than dtrivertx_t.
//
// One load then fetches a whole vertex: the struct is four bytes with
// lightnormalindex last (DTRIVERTX_LNI), so the position bits and the normal
// index come out together and the index is the top byte on the little-endian
// PS2. daliasframe_t puts the array at offset 40, after six floats and a
// 16-byte name, so it is word aligned.
//
// As a pointer to u32 rather than a bit cast per element, and this is not a
// style choice: dtrivertx_t has alignment 1, so __builtin_bit_cast through a
// dtrivertx_t* makes the compiler copy the struct a byte at a time through the
// stack - nine instructions where the aligned load is one.
Q_ALWAYS_INLINE const u32 * KeyframeVertWords(const daliasframe_t * const frame)
{
    static_assert(DTRIVERTX_SIZE == sizeof(u32) && sizeof(dtrivertx_t) == sizeof(u32),
                  "dtrivertx_t must be exactly one word!");

    return static_cast<const u32 *>(static_cast<const void *>(frame->verts));
}

// ------------------------------------------------------------------------------------------------
// Frame state / scratch buffers
// ------------------------------------------------------------------------------------------------

// Triangle gather buffers, flushed when full (referenced in place by DMA out of
// the frame chain). The two paths gather into their own: the EE lerp path into
// the DrawVertex batch (batch.h, which also carries the clipper), the VU lerp
// path into the keyframe/attribute chunk groups of the lerp batch. Only one of
// them is ever active for a given model at a time, and both are locals of the
// entity draw.
constexpr int kBatchMaxVerts = 3 * 512;
using AliasBatch = batch::TriangleBatch<kBatchMaxVerts>;

// The VU path's capacity decides whether a model's shadow costs anything: a
// model that fits in one batch leaves its whole position stream behind for the
// shadow to redraw, and one that does not has to be walked again (see the shadow
// call site). 768 triangles covers 115 of the 119 stock models - everything but
// the four bosses, which appear once each in a playthrough and fallback to a full shadow pass.
constexpr int kLerpBatchMaxVerts = 3 * 768;
using LerpBatch = batch::VULerpTriangleBatch<kLerpBatchMaxVerts>;

// ------------------------------------------------------------------------------------------------
// Entity transform and frustum cull
// ------------------------------------------------------------------------------------------------

// The entity transform is shared with the brush model path;
// alias models are the ones that take +pitch.
Q_ALWAYS_INLINE math::Mat4 MakeAliasMatrix(const entity_t & entity)
{
    return MakeEntityMatrix(entity, /*flipPitchAngle=*/true);
}

// Conservative frustum cull (ref_gl's R_CullAliasModel): the model-space
// bounds of both keyframes, unioned, rotated to world and tested corner-wise.
// MD2 frames carry no explicit bounds - a frame's box is its byte-decode
// transform, translate + scale * [0, 255].
bool ShouldCullEntity(const entity_t & entity, const daliasframe_t * frame, const daliasframe_t * oldFrame)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::EntCull);

    vec3_t mins, maxs;
    for (int i = 0; i < 3; ++i)
    {
        const float minCur = frame->translate[i];
        const float maxCur = minCur + (frame->scale[i] * 255.0f);
        const float minOld = oldFrame->translate[i];
        const float maxOld = minOld + (oldFrame->scale[i] * 255.0f);

        mins[i] = (minCur < minOld) ? minCur : minOld;
        maxs[i] = (maxCur > maxOld) ? maxCur : maxOld;
    }

    // A bounding sphere around the entity origin settles most entities without
    // touching the Euler basis at all, which is the expensive part: AngleVectors
    // is three sine/cosine pairs, and the entity pass runs this over every
    // entity in the refdef to draw a fraction of them.
    //
    // The sphere is exact rather than a guess. Every corner is rotated about the
    // origin and the map below preserves length - an orthonormal basis with one
    // output component negated is a reflection, which preserves length just as
    // the rotation does - so no corner can escape a sphere whose radius is the
    // longest model-space corner. Taking the larger magnitude per axis picks
    // exactly that corner.
    float radiusSqr = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float extent = math::Maxf(math::Fabsf(mins[i]), math::Fabsf(maxs[i]));
        radiusSqr += extent * extent;
    }

    switch (FrustumCullsSphere(entity.origin, math::Sqrtf(radiusSqr)))
    {
    case SphereCull::Outside :
        return true;  // Wholly outside one plane: so is every corner.
    case SphereCull::Inside :
        return false; // Wholly inside all four: so is every corner.
    case SphereCull::Straddling :
        break;        // Only the corners can decide.
    }

    // Rotate the 8 corners into world space - note the same yaw negation and
    // Y flip the engine uses whenever it runs a basis through AngleVectors.
    vec3_t angles, vectors[3];
    VectorCopy(entity.angles, angles);
    angles[YAW] = -angles[YAW];
    math::AngleVectors(angles, vectors[0], vectors[1], vectors[2]);

    // The three dot products and the translation, folded into one row-vector
    // transform so a corner costs a single VU0 pass instead of three scalar dot
    // products and an add. Column j holds the basis vector whose dot lands in
    // output j, and the middle column is negated - that is where the Y flip
    // above goes.
    const math::Mat4 toWorld = {{
        { vectors[0][0], -vectors[1][0], vectors[2][0], 0.0f },
        { vectors[0][1], -vectors[1][1], vectors[2][1], 0.0f },
        { vectors[0][2], -vectors[1][2], vectors[2][2], 0.0f },
        { entity.origin[0], entity.origin[1], entity.origin[2], 1.0f },
    }};

    math::Vec4 corners[8];
    for (int i = 0; i < 8; ++i)
    {
        const math::Vec4 local = {
            (i & 1) ? mins[0] : maxs[0],
            (i & 2) ? mins[1] : maxs[1],
            (i & 4) ? mins[2] : maxs[2],
            1.0f
        };
        corners[i] = math::Transform(local, toWorld);
    }

    return FrustumCullsPoints(corners, ArrayLength(corners));
}

// ------------------------------------------------------------------------------------------------
// Entity shading
// ------------------------------------------------------------------------------------------------

constexpr int kShellFlags = RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE |
                            RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM;

// The uniform shade colour for the entity: shell colours for powersuit
// shells, all-ones for fullbright, the world lighting at the entity origin
// otherwise; then the minlight floor, the bonus-item pulse and the IR goggles
// override on top. Also yields the lightmap trace's ground hit ('outLightSpot',
// where the projected shadow anchors). Based on ref_gl.
math::Vec3 ShadeEntity(const refdef_t & viewDef, const entity_t & entity, vec3_t outLightSpot)
{
    vec3_t color = { 1.0f, 1.0f, 1.0f };

    if (entity.flags & kShellFlags)
    {
        // special case for godmode
        if ((entity.flags & RF_SHELL_RED) && (entity.flags & RF_SHELL_BLUE) && (entity.flags & RF_SHELL_GREEN))
        {
            VectorSet(color, 1.0f, 1.0f, 1.0f);
        }
        else if (entity.flags & (RF_SHELL_RED | RF_SHELL_BLUE | RF_SHELL_DOUBLE))
        {
            VectorClear(color);

            if (entity.flags & RF_SHELL_RED)
            {
                color[0] = 1.0f;
                if (entity.flags & (RF_SHELL_BLUE | RF_SHELL_DOUBLE))
                {
                    color[2] = 1.0f;
                }
            }
            else if (entity.flags & RF_SHELL_BLUE)
            {
                if (entity.flags & RF_SHELL_DOUBLE)
                {
                    color[1] = 1.0f;
                    color[2] = 1.0f;
                }
                else
                {
                    color[2] = 1.0f;
                }
            }
            else if (entity.flags & RF_SHELL_DOUBLE)
            {
                color[0] = 0.9f;
                color[1] = 0.7f;
            }
        }
        else if (entity.flags & (RF_SHELL_HALF_DAM | RF_SHELL_GREEN))
        {
            VectorClear(color);

            if (entity.flags & RF_SHELL_HALF_DAM)
            {
                VectorSet(color, 0.56f, 0.59f, 0.45f);
            }
            if (entity.flags & RF_SHELL_GREEN)
            {
                color[1] = 1.0f;
            }
        }
    }
    else if (entity.flags & RF_FULLBRIGHT)
    {
        VectorSet(color, 1.0f, 1.0f, 1.0f);
    }
    else
    {
        CalcPointLightColor(viewDef, entity.origin, color, outLightSpot);
    }

    if (entity.flags & RF_MINLIGHT)
    {
        int i;
        for (i = 0; i < 3; ++i)
        {
            if (color[i] > 0.1f)
            {
                break;
            }
        }
        if (i == 3)
        {
            VectorSet(color, 0.1f, 0.1f, 0.1f);
        }
    }

    if (entity.flags & RF_GLOW)
    {
        // Bonus items pulse with time.
        const float scale = 0.1f * math::Sinf(viewDef.time * 7.0f);
        for (int i = 0; i < 3; ++i)
        {
            const float min = color[i] * 0.8f;
            color[i] += scale;
            if (color[i] < min)
            {
                color[i] = min;
            }
        }
    }

    // IR goggles color override
    if ((viewDef.rdflags & RDF_IRGOGGLES) && (entity.flags & RF_IR_VISIBLE))
    {
        VectorSet(color, 1.0f, 0.0f, 0.0f);
    }

    return { color[0], color[1], color[2] };
}

// Clamps a shaded colour onto the 0-255 byte range. Overshoot is routine (the
// shade dots exceed 1.0 by design); undershoot is narrower but real, since
// CalcPointLightColor sums dlight contributions and the client emits
// negative-colour dlights for the tracker effects (cl_ents.c's
// V_AddLight(..., -1, -1, -1)). Both ends must be caught before the unsigned
// cast: converting a negative float to u32 is undefined, and the value it
// produces would flood every channel through PackColorRGBA's shifts rather
// than just darkening one.
Q_ALWAYS_INLINE u32 ClampColorChannel(float c)
{
    return (c >= 255.0f) ? 255u : ((c <= 0.0f) ? 0u : static_cast<u32>(c));
}

// The entity's alpha in GS units. 0x80 is the GS's 1.0, and it is clamped
// because 'alpha' is whatever the game put on the entity.
Q_ALWAYS_INLINE float ScaledEntityAlpha(const float alpha)
{
    const float scaled = alpha * 128.0f;
    return (scaled >= 128.0f) ? 128.0f : ((scaled <= 0.0f) ? 0.0f : scaled);
}

// The entity light the lerped microprogram multiplies each vertex's shade term
// by, in the same GS units the color LUT below packs: the shade times the
// modulate identity, with the alpha it should draw at in .w. This is the whole
// of what the VU path needs; no per-entity table at all.
Q_ALWAYS_INLINE math::Vec4 VertexShadeLight(const math::Vec3 & shadeLight, const float alpha)
{
    return { shadeLight.x * 128.0f, shadeLight.y * 128.0f, shadeLight.z * 128.0f, ScaledEntityAlpha(alpha) };
}

// Per-entity packed vertex colours, indexed by the current frame's
// lightnormalindex: min(shadeDots[n] * shadeLight * 128, 255) per channel
// (128 = unmodulated texels on the GS; the dots exceed 1.0 by design).
//
// Exactly kNumVertexNormals entries, because that is now an invariant of the
// data rather than a hope: LoadAliasMD2Model clamps every keyframe's
// lightnormalindex at load, so the draw paths index this straight off a vertex
// word. It used to be padded to 256 to give a malformed model somewhere
// harmless to land.
static u32 s_colorLUT[kNumVertexNormals];

// Largest value in the shade-dot table (client/anormtab.h). The tables bake in
// ref_gl's shading ramp, so the dots run [0.70, 1.99] rather than [0, 1].
constexpr float kMaxShadeDot = 1.99f;

// Above this, shadeDots[i] * 128 * shadeLight can exceed 255 and the LUT build
// has to clamp. At or below it, no entry can overshoot, which is the common case
// by far - only an entity sitting inside a bright dlight goes over.
constexpr float kNoClampShadeLight = 255.0f / (128.0f * kMaxShadeDot);

// Packed per-normal colors for the paths that cannot compute them on the VU:
// the EE lerp paths feed the shared textured program, which the world also
// draws through and which therefore expects a color already packed. That is the
// view weapon and the powersuit shells, one or two entities a frame - the
// VU-lerp path, which is everything else, no longer calls this.
const u32 * BuildColorLUT(const entity_t & entity, const math::Vec3 & shadeLight, const float alpha)
{
    const u32 a = static_cast<u32>(ScaledEntityAlpha(alpha));

    if (entity.flags & kShellFlags)
    {
        // Shells shade flat - no normal-based modulation (ref_gl draws them
        // untextured in pure shell colour; the full effect lands with the
        // powersuit step).
        //
        // Scaled by 255, not the 128 the skin path below uses: this is the one
        // branch whose batch draws with PRIM's TME bit clear, so there is no
        // texture for a modulate identity to preserve and the byte reaches the
        // framebuffer as-is. At 128 every shell came out at half ref_gl's
        // brightness - a green shell (0,128,0) where GL gives (0,255,0).
        const u32 packed = vu1::PackColorRGBA(ClampColorChannel(shadeLight.x * 255.0f),
                                              ClampColorChannel(shadeLight.y * 255.0f),
                                              ClampColorChannel(shadeLight.z * 255.0f), a);
        for (int i = 0; i < kNumVertexNormals; ++i)
        {
            s_colorLUT[i] = packed;
        }
        return s_colorLUT;
    }

    const float * const shadeDots = GetShadeDotsForEntity(entity);

    // The clamp is required - CalcPointLightColor can hand back components above 1
    // or below 0 - but whether it can ever fire is decidable once per entity
    // instead of 486 times. Inside this window every product provably lands in
    // [0, 255], so the fast loop is bit-identical, not an approximation.
    if (shadeLight.x >= 0.0f && shadeLight.x <= kNoClampShadeLight &&
        shadeLight.y >= 0.0f && shadeLight.y <= kNoClampShadeLight &&
        shadeLight.z >= 0.0f && shadeLight.z <= kNoClampShadeLight)
    {
        for (int i = 0; i < kNumVertexNormals; ++i)
        {
            const float l = shadeDots[i] * 128.0f;
            s_colorLUT[i] = vu1::PackColorRGBA(static_cast<u32>(l * shadeLight.x),
                                               static_cast<u32>(l * shadeLight.y),
                                               static_cast<u32>(l * shadeLight.z), a);
        }
        return s_colorLUT;
    }

    // Slow path with ClampColorChannel.
    for (int i = 0; i < kNumVertexNormals; ++i)
    {
        const float l = shadeDots[i] * 128.0f;
        s_colorLUT[i] = vu1::PackColorRGBA(ClampColorChannel(l * shadeLight.x),
                                           ClampColorChannel(l * shadeLight.y),
                                           ClampColorChannel(l * shadeLight.z), a);
    }

    return s_colorLUT;
}

// ------------------------------------------------------------------------------------------------
// Keyframe interpolation
// ------------------------------------------------------------------------------------------------

// The uniform terms of the pose lerp: position = move + vCur*frontv + vOld*backv
// over the byte-quantized frame vertices.
struct LerpConsts
{
    math::Vec3 move;   // both frames' translate terms + the origin delta, pre-lerped
    math::Vec3 frontv; // current frame decode scale * (1 - backlerp)
    math::Vec3 backv;  // old frame decode scale * backlerp
};

LerpConsts SetUpLerp(const entity_t & entity, const daliasframe_t * frame,
                     const daliasframe_t * oldFrame, const float backlerp)
{
    const float frontlerp = 1.0f - backlerp;

    // The world-space step back to where the previous frame was rendered,
    // expressed in model space (hence the basis projection), so a moving
    // entity's pose interpolates along its own motion.
    vec3_t delta, vectors[3];
    VectorSubtract(entity.oldorigin, entity.origin, delta);
    math::AngleVectors(entity.angles, vectors[0], vectors[1], vectors[2]);

    vec3_t move;
    move[0] =  DotProduct(delta, vectors[0]); // forward
    move[1] = -DotProduct(delta, vectors[1]); // left
    move[2] =  DotProduct(delta, vectors[2]); // up
    VectorAdd(move, oldFrame->translate, move);

    return {
        .move = {
            (backlerp * move[0]) + (frontlerp * frame->translate[0]),
            (backlerp * move[1]) + (frontlerp * frame->translate[1]),
            (backlerp * move[2]) + (frontlerp * frame->translate[2])
        },
        .frontv = {
            frontlerp * frame->scale[0],
            frontlerp * frame->scale[1],
            frontlerp * frame->scale[2]
        },
        .backv = {
            backlerp * oldFrame->scale[0],
            backlerp * oldFrame->scale[1],
            backlerp * oldFrame->scale[2]
        }
    };
}

// EE-side pose lerp into s_lerpedPositions[]. This is the ps2_md2_vu_lerp=0
// debug path, and the only path for the powersuit-shell models: pushing
// each vertex out along its own normal is a per-vertex table lookup,
// what the VU cannot index cheaply.
const math::Vec3 * LerpVertsEE(const dtrivertx_t * verts, const dtrivertx_t * oldVerts,
                               const int numVerts, const LerpConsts & lc, const bool powersuit)
{
    // Lerped model-space positions of one entity's pose, indexed by the glcmds'
    // vertex index. 24 KB, and one instance for every entity in the frame: this is EE-only
    // working memory that the gather reads back and copies into the chain, so unlike the
    // gather buffers it never reaches the DMAC and nothing is still reading it when the next
    // entity starts.
    static math::Vec3 s_lerpedPositions[MAX_VERTS];

    PS2_Assert(numVerts <= static_cast<int>(ArrayLength(s_lerpedPositions)));

    for (int i = 0; i < numVerts; ++i)
    {
        s_lerpedPositions[i] = {
            lc.move.x + (oldVerts[i].v[0] * lc.backv.x) + (verts[i].v[0] * lc.frontv.x),
            lc.move.y + (oldVerts[i].v[1] * lc.backv.y) + (verts[i].v[1] * lc.frontv.y),
            lc.move.z + (oldVerts[i].v[2] * lc.backv.z) + (verts[i].v[2] * lc.frontv.z)
        };

        if (powersuit)
        {
            // Inflate the mesh along its normals so the shell surrounds the
            // model instead of z-fighting it.
            const float * normal = s_vertexNormals[verts[i].lightnormalindex];
            s_lerpedPositions[i].x += normal[0] * POWERSUIT_SCALE;
            s_lerpedPositions[i].y += normal[1] * POWERSUIT_SCALE;
            s_lerpedPositions[i].z += normal[2] * POWERSUIT_SCALE;
        }
    }

    return s_lerpedPositions;
}

// ------------------------------------------------------------------------------------------------
// EE-side clipping (the view weapon)
// ------------------------------------------------------------------------------------------------

// MD2 shades per vertex, so the colour has to survive a cut: it rides through
// the clipper as unpacked 0..255 floats in ClipVertex::color, which interpolate
// linearly like everything else there, and pack back on the way out.
Q_ALWAYS_INLINE math::Vec4 UnpackClipColor(u32 rgba)
{
    return { static_cast<float>( rgba        & 0xFF),
             static_cast<float>((rgba >>  8) & 0xFF),
             static_cast<float>((rgba >> 16) & 0xFF),
             static_cast<float>((rgba >> 24) & 0xFF) };
}

Q_ALWAYS_INLINE u32 PackClipColor(const clip::ClipVertex & v)
{
    const auto channel = [](float f) -> u32
    {
        return (f <= 0.0f) ? 0u : (f >= 255.0f) ? 255u : static_cast<u32>(f + 0.5f);
    };
    const math::Vec4 & c = v.color;
    return channel(c.x) | (channel(c.y) << 8) | (channel(c.z) << 16) | (channel(c.w) << 24);
}

// Clips one model triangle against the volume the VU judges and appends the
// survivors to the gather buffer, flushing it when full. The vertex colour is
// the shade the clipper interpolated, packed back down on the way out.
Q_ALWAYS_INLINE void GatherClippedTriangle(AliasBatch & batch, clip::ClipVertex (&corners)[3],
                                           const math::Mat4 & mvp, const tex::Texture & texture,
                                           const vu1::DrawFlags flags)
{
    batch.GatherTriangle(corners, mvp, texture, flags, PackClipColor);
}

// ------------------------------------------------------------------------------------------------
// Projected shadow
// ------------------------------------------------------------------------------------------------

// The attribute a shadow vertex carries. Every lane of it is a placeholder: the
// colour is not in here (kShadowShadeLight is all zero, so any shade term
// multiplies out to black and only the alpha in that vector matters) and neither
// is the ST, since the draw is untextured and the GS never samples.
//
// Which is why the shadow that redraws the model's own stream does not need this
// at all - that stream's real attributes multiply out to exactly the same black,
// so there is no override and no repeating block. It exists only for the rebuild
// path below, which writes it per vertex rather than leave chain memory the last
// frame put something else in for the DMA to carry.
constexpr vu1::LerpDrawAttrib kShadowAttrib = { 0.0f, 0.0f, 0.0f, 1.0f };

// Draws the entity's planar projected shadow: the same keyframe byte streams
// the model just drew, run through the same VU1 lerp, with the flattening
// projection premultiplied into the matrix - the reference implementation's
// per-vertex squash (DrawAliasMD2Shadow) is affine, so it rides the MVP for
// free and no EE-side positions are ever needed. 'lightSpot' is where the
// shading lightmap trace hit the ground below the entity.
// The full model-to-screen transform a shadow draws under: the flattening
// squash, the entity placement, the view projection, and the pose's uniform
// translation folded into the last row.
math::Mat4 ShadowMatrix(const entity_t & entity, const LerpConsts & lc,
                        const math::Mat4 & viewProj, const vec3_t lightSpot)
{
    // The reference's squash, in matrix form (row-vector convention):
    // x' = x - sv.x * (z + lheight), y' likewise, z' = height - i.e. model
    // z collapses onto the ground plane, sheared along the yaw-unrotated
    // shade vector. Model-space, applied before the entity transform.
    const float lheight = entity.origin[2] - lightSpot[2];
    const float height  = -lheight + 1.0f;
    const float angle   = math::DegToRad(entity.angles[YAW]);
    const math::Vec3 sv = math::Normalize({ math::Cosf(-angle), math::Sinf(-angle), 1.0f });

    const math::Mat4 shadowProj = {{
        { 1.0f,             0.0f,             0.0f,   0.0f },
        { 0.0f,             1.0f,             0.0f,   0.0f },
        { -sv.x,           -sv.y,             0.0f,   0.0f },
        { -sv.x * lheight, -sv.y * lheight,   height, 1.0f } }};

    // Compose first, fold second: the pose's 'move' must translate before
    // the squash, so its fold goes into the full shadow composite.
    math::Mat4 mvp = shadowProj * MakeAliasMatrix(entity) * viewProj;
    const math::Vec4 row3 = math::Transform(math::Vec4{ lc.move.x, lc.move.y, lc.move.z, 1.0f }, mvp);

    mvp.m[3][0] = row3.x;
    mvp.m[3][1] = row3.y;
    mvp.m[3][2] = row3.z;
    mvp.m[3][3] = row3.w;
    return mvp;
}

// The flags a shadow batch draws with: flat, blended, and never textured.
constexpr vu1::DrawFlags kShadowFlags = vu1::DrawFlags::Blended | vu1::DrawFlags::Untextured;

// A shadow's light: no colour at all, so whatever shade term the attribute
// stream carries multiplies out to black, at the half alpha in .w. This is what
// lets the shadow reuse the model's own position stream with a constant
// attribute block - the colour never depended on the vertex to begin with.
constexpr math::Vec4 kShadowShadeLight = { 0.0f, 0.0f, 0.0f, 64.0f };

// Rebuilds the model's position stream and draws it squashed. The slow path -
// used only when the model did not go out in a single batch, so the stream the
// main pass left behind is not the whole of it. See the call site.
void DrawAliasMD2Shadow(LerpBatch & batch, const entity_t & entity,
                        const mod::ModelInstance::AliasData & mesh,
                        const daliasframe_t * frame, const daliasframe_t * oldFrame,
                        const LerpConsts & lc, const math::Mat4 & viewProj,
                        const tex::Texture & skin, const vec3_t lightSpot,
                        const vu1::FaceCull faceCull)
{
    const math::Mat4 mvp = ShadowMatrix(entity, lc, viewProj, lightSpot);

    auto flushShadowVerts = [&]()
    {
        batch.Flush(mvp, skin, lc.frontv, lc.backv, kShadowShadeLight, faceCull, kShadowFlags);
    };

    // By value, not through the enclosing frame pointers: the loop stores through
    // the batch's streams, and under -fno-strict-aliasing a captured reference
    // would have to be re-read every iteration in case the store had changed it.
    const u32 * const curVerts = KeyframeVertWords(frame);
    const u32 * const oldVerts = KeyframeVertWords(oldFrame);
    const mod::AliasVertex * src = mesh.vertexes;
    const int numTris = mesh.numTris;

    for (int t = 0; t < numTris; ++t, src += 3)
    {
        if (batch.IsFull())
        {
            flushShadowVerts();
        }

        // __restrict for the reason the main gather loop gives.
        const auto tri = batch.PushTriangle();
        vu1::LerpVertexBytes * const __restrict triPos    = tri.pos;
        vu1::LerpDrawAttrib  * const __restrict triAttrib = tri.attrib;

        for (int i = 0; i < 3; ++i)
        {
            // Only the index; the shadow has no UVs, so this is the one path that
            // does not want the whole qword. Bounds were settled at load.
            const u32 index = src[i].index;

            triPos[i].cur = curVerts[index];
            triPos[i].old = oldVerts[index];

            // The attribute is the same qword for every shadow vertex, but it
            // still has to be written: the slot is chain memory the last frame
            // left something else in, and the DMA transfers it either way.
            triAttrib[i] = kShadowAttrib;
        }
    }
    flushShadowVerts();
}

// ------------------------------------------------------------------------------------------------
// Skin selection
// ------------------------------------------------------------------------------------------------

const tex::Texture & SkinForEntity(const entity_t & entity, const mod::ModelInstance & model)
{
    // A custom skin (player models) overrides the model's own list.
    if (entity.skin != nullptr)
    {
        return *reinterpret_cast<const tex::Texture *>(entity.skin);
    }

    const tex::Texture * skin = nullptr;
    if (entity.skinnum >= 0 && entity.skinnum < mod::kMaxMD2Skins)
    {
        skin = model.Alias().skins[entity.skinnum];
    }
    if (skin == nullptr)
    {
        skin = model.Alias().skins[0];
    }
    if (skin == nullptr)
    {
        return tex::DebugTexture(); // Model has no usable skin at all.
    }
    return *skin;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void InitEntityRendering()
{
    s_lerpModels = Cvar_Get("ps2_md2_lerp_on",     "1", 0);
    s_vuLerp     = Cvar_Get("ps2_md2_vu_lerp",     "1", 0);
    s_cullFace   = Cvar_Get("ps2_md2_cullface",    "1", 0);
    s_shadows    = Cvar_Get("ps2_md2_shadows",     "1", 0);
    s_clipWeapon = Cvar_Get("ps2_md2_clip_weapon", "1", 0);

    for (u32 & color : s_colorLUT)
    {
        color = vu1::PackColorRGBA(0, 0, 0, 0x80);
    }
}

void DrawAliasMD2Entity(const refdef_t & viewDef, const entity_t & entity, const math::Mat4 & viewProj)
{
    const auto * model = reinterpret_cast<const mod::ModelInstance *>(entity.model);
    PS2_Assert(model != nullptr);

    const mod::ModelInstance::AliasData & mesh = GetAliasMesh(*model);
    const int numFrames = model->numFrames;

    // Bad animation frames render as frame 0 (ref_gl behaviour), not an error.
    // This one stays: it validates what the game handed us, not the model data.
    int frameIndex    = entity.frame;
    int oldFrameIndex = entity.oldframe;

    if (frameIndex < 0 || frameIndex >= numFrames) [[unlikely]]
    {
        Com_DPrintf("DrawAliasMD2Entity %s: no such frame %d\n", model->name, frameIndex);
        frameIndex    = 0;
        oldFrameIndex = 0;
    }
    if (oldFrameIndex < 0 || oldFrameIndex >= numFrames) [[unlikely]]
    {
        Com_DPrintf("DrawAliasMD2Entity %s: no such oldframe %d\n", model->name, oldFrameIndex);
        frameIndex    = 0;
        oldFrameIndex = 0;
    }

    const daliasframe_t * frame    = GetAliasFrame(mesh, frameIndex);
    const daliasframe_t * oldFrame = GetAliasFrame(mesh, oldFrameIndex);

    // The view weapon hugs the near plane and never leaves the view; the
    // corner test would false-cull it.
    if ((entity.flags & RF_WEAPONMODEL) == 0)
    {
        if (ShouldCullEntity(entity, frame, oldFrame))
        {
            ++GetDrawStats().boxesCulled;
            return;
        }
    }

    PS2_Assert(mesh.numXyz > 0 && mesh.numXyz <= MAX_VERTS);
    ++GetDrawStats().entities;

    // The entity's shade colour. The normal index is read from the *current*
    // frame only - the pose interpolates, the lighting does not (ref_gl
    // behaviour). 'lightSpot' anchors the shadow.
    vec3_t lightSpot = {};
    math::Vec3 shadeLight;
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntShade);
        shadeLight = ShadeEntity(viewDef, entity, lightSpot);
    }

    const float alpha = (entity.flags & RF_TRANSLUCENT) ? entity.alpha : 1.0f;
    const float backlerp = (s_lerpModels->value != 0.0f) ? entity.backlerp : 0.0f;
    const LerpConsts lc = SetUpLerp(entity, frame, oldFrame, backlerp);
    const tex::Texture & skin = SkinForEntity(entity, *model);
    math::Mat4 mvp = MakeAliasMatrix(entity) * viewProj;

    // The view weapon is clipped here on the EE instead of being left to the
    // VU's whole-triangle reject. It is the one model the camera sits inside:
    // parts of it pass behind the eye - the chaingun's spinning barrels most of
    // all - and dropping those triangles whole punches visible holes in it. No
    // other alias model needs this, and clipping every monster would not be
    // worth the EE time. Clipping needs the pose on the EE, so the weapon takes
    // the scalar lerp path; the VU's back-face cull goes with it, which costs
    // only some overdraw, since the gun is opaque and the z-buffer sorts it.
    const bool clipOnEE = (entity.flags & RF_WEAPONMODEL) && (s_clipWeapon->value != 0.0f);
    const bool vuLerp   = (s_vuLerp->value != 0.0f) && !(entity.flags & kShellFlags) && !clipOnEE;
    const auto faceCull = static_cast<vu1::FaceCull>(static_cast<u32>(s_cullFace->value) % 3u);

    // The VU path shades on the VU: it takes the entity's light as a batch
    // constant and each vertex's raw shade dot, so there is no table to build.
    // The EE paths draw through the shared textured program, which the world
    // also uses and which therefore wants a color already packed - they still
    // need the 162-entry LUT, but they are one or two entities a frame.
    const u32 * colorLUT = nullptr;
    if (!vuLerp)
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntColorLUT);
        colorLUT = BuildColorLUT(entity, shadeLight, alpha);
    }

    // Translucent entities blend over the finished opaque scene (the entity
    // pass draws them last) with alpha already folded into the colour LUT.
    //
    // RF_DEPTHHACK rides along as a draw flag rather than a change to 'mvp':
    // it is a depth *range*, and the microprogram applies it where OpenGL
    // does, to the window coordinate after the clip judgement. Remapping clip
    // z here instead would defeat that judgement for the one entity that most
    // needs it - see the DrawFlags::DepthHack notes in vu1.h.
    auto batchFlags = (entity.flags & RF_TRANSLUCENT)
                    ? vu1::DrawFlags::Blended
                    : vu1::DrawFlags::None;

    if (entity.flags & RF_DEPTHHACK)
    {
        batchFlags = batchFlags | vu1::DrawFlags::DepthHack;
    }

    // Expand the glcmds over the pose. Note MD2 triangles are not near-plane
    // clipped like the world's: the VU rejects straddlers whole (guard band).
    // The view weapon draws against a much closer near plane than the rest of
    // the scene (see kZNearWeapon) so it has almost nothing left to straddle.
    //
    // 'emittedVerts' tallies what was submitted, for the frame's triangle count
    // below. Only the two paths that gather verbatim count here; the EE clipping
    // path's triangles are counted by the gather buffer itself, since the
    // clipper is what decides how many of them there are.
    int emittedVerts = 0;

    // Scoped to the whole entity rather than to the VU lerp branch that fills it:
    // the shadow pass below draws out of it, either by redrawing the span the
    // model's own Flush left in the chain or by gathering a fresh one.
    LerpBatch lerpBatch;

    // The pose expansion and batch submission - everything from here to the
    // flush is per-triangle work, unlike Shade and Cull above.
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntGeom);

        if (vuLerp)
        {
            const math::Vec4 vertexShadeLight = VertexShadeLight(shadeLight, alpha);

            // The pose lerp runs on VU1: fold the uniform 'move' term into the matrix.
            const math::Vec4 row3 = math::Transform(
                math::Vec4{ lc.move.x, lc.move.y, lc.move.z, 1.0f }, mvp);

            mvp.m[3][0] = row3.x;
            mvp.m[3][1] = row3.y;
            mvp.m[3][2] = row3.z;
            mvp.m[3][3] = row3.w;

            // Everything the inner loop reads, by value. They look loop invariant
            // but the loop stores through the batch's streams, and the renderer
            // builds with -fno-strict-aliasing - captured by reference the
            // compiler must assume each store could have changed them and re-read
            // all of them every iteration.
            const u32 * const curVerts = KeyframeVertWords(frame);
            const u32 * const oldVerts = KeyframeVertWords(oldFrame);
            const float * const dots = GetShadeDotsForEntity(entity);

            const mod::AliasVertex * src = mesh.vertexes;
            const int numTris = mesh.numTris;

            for (int t = 0; t < numTris; ++t, src += 3)
            {
                if (lerpBatch.IsFull())
                {
                    lerpBatch.Flush(mvp, skin, lc.frontv, lc.backv, vertexShadeLight,
                                    faceCull, batchFlags);
                }

                // __restrict, and it earns its keep: the two cursors live in the
                // batch, the batch is a local whose address escapes, and every
                // store below is one gcc cannot prove disjoint from it under
                // -fno-strict-aliasing - so without this it spills both and
                // reloads them before each of the six stores. Worth 7 of the 16
                // instructions per triangle this loop gained when the gather
                // target stopped being a static.
                //
                // The promise holds: pos and attrib are disjoint sub-arrays of one
                // LerpChunk, and everything read here (the mesh, the keyframes,
                // the shade table) is model or .rodata, never chain. Note they are
                // declared *after* the flush above, so Flush - which does reach
                // the chain through the batch - is never in scope with them.
                const auto tri = lerpBatch.PushTriangle();
                vu1::LerpVertexBytes * const __restrict triPos    = tri.pos;
                vu1::LerpDrawAttrib  * const __restrict triAttrib = tri.attrib;

                for (int i = 0; i < 3; ++i)
                {
                    // Read before anything is stored. Every store below is to
                    // memory the compiler cannot prove disjoint from the mesh
                    // under -fno-strict-aliasing, so an index read afterwards
                    // becomes a reload of the same address.
                    const u32 index = src[i].index;

                    // The baked vertex is already a LerpDrawAttrib in everything
                    // but lane 0, which holds that index rather than the color:
                    // copy the qword whole, then write the shade over lane 0. ST
                    // needs no scaling here - the microprogram applies the skin's
                    // power-of-two correction.
                    vu1::CopyLerpAttrib(triAttrib[i], src[i]);

                    // One load of the keyframe vertex rather than two: the normal
                    // index is the top byte of the word already in hand, so
                    // reading it through the struct member would be a second trip
                    // to the same address. See KeyframeVertWords.
                    const u32 curBits = curVerts[index];

                    triPos[i].cur = curBits;
                    triPos[i].old = oldVerts[index];

                    // The raw shade dot, not a packed color: the microprogram
                    // multiplies the batch's shadeLight by it and converts. Same
                    // indexed load and store the color LUT cost, minus the table.
                    triAttrib[i].shade = dots[curBits >> (DTRIVERTX_LNI * 8)];
                }
                emittedVerts += 3;
            }
            lerpBatch.Flush(mvp, skin, lc.frontv, lc.backv, vertexShadeLight,
                            faceCull, batchFlags);
        }
        else
        {
            // Shells draw as a flat-coloured inflated silhouette: no skin, and
            // always blended, whether or not the client tagged them translucent.
            // OR-ed onto the batch flags rather than replacing them so an entity's
            // depth range survives (Blended is idempotent if it was already set).
            const bool powersuit = (entity.flags & kShellFlags) != 0;
            const auto flags = powersuit
                             ? (batchFlags | vu1::DrawFlags::Blended | vu1::DrawFlags::Untextured)
                             : batchFlags;

            // Scoped to the EE lerp path, which is the only one that gathers
            // DrawVertex; the VU path fills lerpBatch's chunk groups instead.
            AliasBatch batch;

            const math::Vec3 * const lerpedPositions =
                LerpVertsEE(frame->verts, oldFrame->verts, mesh.numXyz, lc, powersuit);

            // As in the VU path above: by value, so the stores into the gather
            // buffer cannot force these to be re-read every iteration.
            const u32 * const curVerts = KeyframeVertWords(frame);

            // A powersuit shell has no skin, so its coordinates are simply zero.
            float scaleS = 0.0f;
            float scaleT = 0.0f;
            if (!powersuit)
            {
                // The glcmds' coordinates are normalized against the skin image, but the
                // GS spreads normalized ST over the power-of-two TEX0 extent - and model
                // skins essentially never are one (276x194 is the common size). Without
                // this the skin samples squashed into a corner of a larger virtual image.
                tex::StScaleFor(skin, &scaleS, &scaleT);
            }

            const mod::AliasVertex * src = mesh.vertexes;
            const int numTris = mesh.numTris;

            if (clipOnEE)
            {
                for (int t = 0; t < numTris; ++t, src += 3)
                {
                    clip::ClipVertex corners[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        // All three read up front; see the note in the VU path.
                        const u32 index  = src[i].index;
                        const float texS = src[i].s;
                        const float texT = src[i].t;

                        const math::Vec3 & pos = lerpedPositions[index];

                        corners[i].pos   = { pos.x, pos.y, pos.z, 1.0f };
                        corners[i].st    = { texS * scaleS, texT * scaleT, 0.0f, 0.0f };
                        corners[i].color = UnpackClipColor(colorLUT[curVerts[index] >> (DTRIVERTX_LNI * 8)]);
                    }

                    GatherClippedTriangle(batch, corners, mvp, skin, flags);
                }
            }
            else
            {
                for (int t = 0; t < numTris; ++t, src += 3)
                {
                    if (batch.IsFull())
                    {
                        batch.Flush(mvp, skin, flags); // Capacity is a triangle multiple,
                    }                                  // so this only fires between them.

                    vu1::DrawVertex * const dst = batch.PushTriangle();
                    for (int i = 0; i < 3; ++i)
                    {
                        // All three read up front; see the note in the VU path.
                        const u32 index  = src[i].index;
                        const float texS = src[i].s;
                        const float texT = src[i].t;

                        const math::Vec3 & pos = lerpedPositions[index];

                        dst[i].x    = pos.x;
                        dst[i].y    = pos.y;
                        dst[i].z    = pos.z;
                        dst[i].w    = 1.0f;
                        dst[i].rgba = colorLUT[curVerts[index] >> (DTRIVERTX_LNI * 8)];
                        dst[i].s    = texS * scaleS;
                        dst[i].t    = texT * scaleT;
                        dst[i].q    = 1.0f;
                    }
                    emittedVerts += 3;
                }
            }
            batch.Flush(mvp, skin, flags);
        }

    }
    GetDrawStats().trisDrawn += emittedVerts / 3;

    // The projected blob shadow. Skipped for the view weapon,
    // translucents and shells/fullbright entities.
    if (s_shadows->value != 0.0f &&
        !(entity.flags & (RF_TRANSLUCENT | RF_WEAPONMODEL | RF_FULLBRIGHT | kShellFlags)))
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntShadow);

        // The shadow's vertices are the model's own, byte for byte - the squash
        // rides in the matrix, not the data. So if the whole model went out in a
        // single batch, that batch's chunk groups are still sitting in the frame
        // chain where its Flush committed them, and the shadow is one more set of
        // chunk tags over them rather than a second walk of the entire glcmds
        // list. 86% of the stock models fit.
        //
        // Both conditions are needed: only the VU lerp path fills those groups at
        // all, and only a model that never filled the batch mid-way left all of
        // itself in it rather than just its tail.
        if (vuLerp && emittedVerts > 0 && emittedVerts <= kLerpBatchMaxVerts)
        {
            lerpBatch.RedrawLastFlush(ShadowMatrix(entity, lc, viewProj, lightSpot), skin,
                                      lc.frontv, lc.backv, kShadowShadeLight, faceCull,
                                      kShadowFlags);
        }
        else
        {
            DrawAliasMD2Shadow(lerpBatch, entity, mesh, frame, oldFrame, lc, viewProj,
                               skin, lightSpot, faceCull);
        }
    }
}

} // namespace ps2::view
