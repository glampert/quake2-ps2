#pragma once
/* ================================================================================================
 * File: render_system.h
 * Brief: Render System (rs): the frame lifecycle, the 2D primitives, the VU1 draws and the vertex
 *        streams that gather for them. Everything a frame draws is recorded into cmdbuf's chain
 *        here and sent in one kick at EndFrame.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/profile.h"
#include "ps2/math/vec_mat.h"
#include "ps2/renderer/clip.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/cmd_buffer.h"

namespace ps2::tex { struct Texture; }

namespace ps2::rs {

// ------------------------------------------------------------------------------------------------
// Draw flags
// ------------------------------------------------------------------------------------------------

// Optional batch draw flags (OR-able).
//
// Blended, Additive and Modulate pick a blend equation and are mutually exclusive - passing more
// than one asserts. Each also turns the prim's ABE bit on and masks depth writes, so a blended
// batch sorts against opaque geometry but never occludes it. The rest are independent.
//
// Modulate scales the framebuffer by the batch's *alpha*; the GS blend unit has no second colour,
// so a luxel's colour cannot come through here and arrives via the diffuse pass's vertex colour
// instead (see lm::CacheSurfaceVertexColors).
enum class DrawFlags : u32
{
    None          = 0,
    Blended       = 1 << 0, // (Cs - Cd) * As / 128 + Cd: ordinary translucency.
    Untextured    = 1 << 1, // Pure gouraud colour; the texture is bound but not sampled.
    Additive      = 1 << 2, // Cs * As / 128 + Cd, saturating (COLCLAMP is on): flares, glows.
    Modulate      = 1 << 3, // Cd * As / 128: scales the framebuffer, adds nothing of its own.
    DepthHack     = 1 << 4, // Squeeze depth into the near slice of the z-buffer (RF_DEPTHHACK).
    NoDepthWrite  = 1 << 5, // Mask depth writes alone, without a blend equation or the ABE bit.
    DynamicLights = 1 << 6, // Colour summed from SetDynamicLights on the VU, not taken from the vertex.
    Warped        = 1 << 7, // Run the warp block on VU1: UVs arrive in raw texels and animate there.
    WarpFlowing   = 1 << 8, // With Warped: also drift the surface along S (SURF_FLOWING).
};

constexpr DrawFlags operator|(DrawFlags a, DrawFlags b)
{
    return static_cast<DrawFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

constexpr bool HasDrawFlag(DrawFlags flags, DrawFlags test)
{
    return (static_cast<u32>(flags) & static_cast<u32>(test)) != 0;
}

// Which sign of a triangle's screen-space signed area the VU rejects. Which one faces away
// depends on the winding and the projection's Y orientation.
enum class FaceCull : u32
{
    None     = 0,
    Negative = 1,
    Positive = 2,
};

// The magnitude of the sign below. The microprogram does not see the screen area itself but the
// determinant of the triangle's three clip-space (x, y, w) corners, which is that area times
// w0*w1*w2 - it shrinks with the cube of the depth. A pixel-sized triangle at the view weapon's
// 0.25 near plane comes out around 1e-8, and the sign is read through an ftoi4 that resolves
// 1/16, so unscaled it would truncate to zero and never be culled. 2^40 resolves it with room to
// spare, and the largest the determinant can reach, around the far plane's 4096 cubed, still
// lands over ten orders of magnitude below the float range. Too small a scale fails safe:
// the triangle draws, and the z-buffer hides it.
constexpr float kCullSignScale = 1099511627776.0f; // 2^40

// What the microprogram actually receives: the sign it multiplies the determinant by, after
// which it only ever asks whether the result is negative. Zero never culls, because zero is not
// negative. Cheaper on the VU than the mode itself - a mask register, a compare target and a
// branch, all of which VI registers the microprogram does not have to spare.
constexpr float CullSignFor(const FaceCull cull)
{
    return (cull == FaceCull::Negative) ?  kCullSignScale
         : (cull == FaceCull::Positive) ? -kCullSignScale
                                        :  0.0f;
}

// ------------------------------------------------------------------------------------------------
// Debug draw stats trackers
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE
// What the renderer submitted this frame; what the view decided to submit is view::DrawStats.
// Counted by the streams and the draws below, so no caller adds to it. Cleared by BeginFrame.
struct DrawStats
{
    // VU1 clips, so what it cuts and drops is invisible from here: trisDrawn counts
    // what was *handed* to it, and the three below now describe only sky, the one
    // path that still cuts on the EE. They were every draw's numbers before the
    // clipper moved.
    int trisDrawn;   // Triangles handed to VU1.
    int trisClipped; // Of those, re-cut on the EE first.
    int trisCulled;  // Dropped whole on the EE, entirely outside the volume.

    // How 'trisClipped' splits by which planes the triangle straddled; the three
    // partition it. See CountClippedTriangle for what the split is for.
    int trisClipNearOnly;
    int trisClipNoNear;
    int trisClipMixed;

    // Straddled the far plane, on top of whatever else it straddled - so this
    // overlaps the three above rather than partitioning with them. Far is the
    // plane a VU1 clipper would most like to drop: skipping it costs one corner
    // off the worst-case fan, 8 instead of 9, which is a whole output triangle
    // off what a window has to reserve.
    int trisClipFar;

    // Most corners the clipper has handed back this frame. The worst case is 9
    // and what a window must reserve is driven by it, but what actually happens
    // is the number worth designing against.
    int clipMaxVerts;
    int drawBatches; // VU1 batches submitted (one or more per texture).
    int particles;   // Particle billboards submitted.
};

namespace detail {
extern DrawStats g_drawStats;
} // namespace detail

// Inline so the streams can bump the counters without a call.
Q_ALWAYS_INLINE DrawStats & GetStats()
{
    return detail::g_drawStats;
}

// Bins a clipped triangle by the planes it straddled, and counts it.
//
// The split is what says whether a VU1 clipper handling only the near plane could
// stand on its own: 'trisClipNearOnly' is what such a clipper would cut correctly
// and by itself, while the other two are triangles it would either not help with
// at all or cut only for the microprogram to reject the survivors whole - the
// guard band is judged again after the cut.
Q_ALWAYS_INLINE void CountClippedTriangle(const u32 planesCrossed, const int survivors)
{
    DrawStats & stats = GetStats();
    ++stats.trisClipped;

    if ((planesCrossed & clip::kPlaneFarBit) != 0)
    {
        ++stats.trisClipFar;
    }
    if (survivors > stats.clipMaxVerts)
    {
        stats.clipMaxVerts = survivors;
    }

    const bool crossesNear  = (planesCrossed &  clip::kPlaneNearBit) != 0;
    const bool crossesOther = (planesCrossed & ~clip::kPlaneNearBit) != 0;

    if (crossesNear && !crossesOther)
    {
        ++stats.trisClipNearOnly;
    }
    else if (crossesOther && !crossesNear)
    {
        ++stats.trisClipNoNear;
    }
    else
    {
        ++stats.trisClipMixed;
    }
}

// High-water of one GIF block, in qwords. Measured against the command buffer half it must fit
// inside (cmdbuf::kHalfBytes); shown as "Gif2DPk" in the draw-stats overlay.
int Gif2DPeakQwords();
#endif // PS2_QUAKE_PROFILE

// ------------------------------------------------------------------------------------------------
// Initialization / frame lifecycle
// ------------------------------------------------------------------------------------------------

// Brings up all the low-level rendering systems and hardware.
void Init(const gs::Config & gsConfig, void * memory, const u32 memorySizeBytes);

// Background colour the frame clear fills with.
void SetClearColor(u8 r, u8 g, u8 b);

// Opens the frame: shows the previous one if it was left drawing, rewinds the command buffer and
// writes the screen clear at the head of it. 2D and 3D may then be drawn in any order.
//
// 'dither' enables the GS's ordered dither, which hides 16-bit banding and does nothing to a
// 32-bit framebuffer.
void BeginFrame(bool dither);

// Closes the frame: flushes any pending 2D and submits the command buffer.
//
// 'deferPresent' leaves the frame drawing for the next BeginFrame to show, trading one frame of
// latency for the fence the EE would otherwise stand at. Clear it and this waits and flips here.
void EndFrame(bool deferPresent);

// Full sync/drain of the underlying cmdbuf. Kicks what has been recorded and waits for it.
void KickAndWait();

// Makes the texture's pixels resident in GS VRAM, uploading on a miss and evicting the
// least-recently-bound textures when the heap is full. A resident texture only has its LRU stamp
// refreshed, unless its pixels are dirty, which re-uploads in place.
//
// May fence the GS when an upload would land on VRAM queued draws still sample. That reopens any
// open GIF section, so a GifWriter taken before this call must not be used after it.
void EnsureTextureResident(const tex::Texture & texture);

// ------------------------------------------------------------------------------------------------
// VU1 bring-up
// ------------------------------------------------------------------------------------------------

// Built into the frame's chain like any other VIF1 transfer, so these run after cmdbuf::Init and
// before any drawing. vu1::Init is the only caller.
// References a microprogram into the chain as MPG transfers (chunked to the VIF's
// 256-instruction limit).
void AddVUMicroProgram(vu1::ProgramAddr dest, vu1::VUCode code);

// Programs the VIF1 BASE/OFFSET registers that split VU data memory into the two halves XTOP
// alternates between. Both in qwords.
void AddVUDoubleBufferSettings(u32 baseQw, u32 offsetQw);

// Unpacks a block of constants to an absolute VU data address, for blocks that are the same for
// the life of the process and so never need re-sending with a draw. 'data' is referenced, not
// copied, so it must outlive the kick - a static or a literal in .rodata, as the microprogram
// code above is. 16-byte aligned, size in qwords.
void AddVUDataUpload(u32 vuAddrQw, const void * data, u32 qwords);

// ------------------------------------------------------------------------------------------------
// Triangle Streams lifecycle
// ------------------------------------------------------------------------------------------------

// A stream gathers vertices into the command buffer and submits them when it goes out of scope's
// way: Begin<TriangleStream>(capacity) opens one, Submit(stream) sends what is in it. 'maxVerts'
// is what one gather cycle may hold; the stream flushes and re-claims on its own past that.
template<typename T>
T Begin(const int maxVerts);

template<typename T>
void Submit(T & stream);

// ------------------------------------------------------------------------------------------------
// 2D primitives
// ------------------------------------------------------------------------------------------------

// Closes the open 2D section, so what follows draws under it. Called at every 2D->3D boundary and
// at EndFrame; a no-op when nothing has accumulated.
void FlushPending2D();

// A solid rectangle. Alpha below 255 blends with the framebuffer.
void FillRect(int x, int y, int width, int height, u8 r, u8 g, u8 b, u8 a);

// A textured rectangle sampling 'texture' over texel range [u0,v0]..[u1,v1], made resident first
// if it is not already. 'brightness' modulates the texel colour per RGB channel: 128 leaves it
// unchanged. Texels with alpha 0 are cut out by the alpha test.
void DrawTexturedRect(const tex::Texture & texture, int x, int y, int width, int height,
                      int u0, int v0, int u1, int v1, const u8 brightness[3]);

// ------------------------------------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------------------------------------

// Room for 'particleCount' billboards in the command buffer, to fill in place. Closes the 2D
// section and reserves what the draw appends on top, as a stream's first BeginVerts does.
template<>
vu1::ParticleVertex * Begin<vu1::ParticleVertex *>(const int particleCount);

// Submits them, clearing 'particles'. Takes its draw state directly: the whole batch is written
// in one go, so there is no gather loop to re-set state per item.
void Submit(vu1::ParticleVertex * __restrict & particles, const math::Mat4 & mvp, const tex::Texture & texture,
            const math::Vec3 & quadOffset, DrawFlags flags = DrawFlags::Blended);

// ------------------------------------------------------------------------------------------------
// Chain budget
// ------------------------------------------------------------------------------------------------

// What a draw costs the command buffer besides its vertex data, so that a caller gathering into
// the buffer can reserve the pair together. It must reserve the pair: cmdbuf::Reserve rewinds when
// it comes up short, which would pull the buffer out from under the span the chunks reference.
//
// Nothing outside this module reserves anything - the streams and the particle path do it for
// themselves. It is in the header only because the stream constructors size their claim from it.

// Chain qwords one chunk appends. 12 in practice for the world path (inline header/parameters/GIF
// tags, vertex REF unpack, FLUSH + MSCAL), 13 lerped (two REF unpacks), 14 particles; declared
// with room to spare, since over-declaring only reserves a little more of the buffer than a chunk
// needs.
constexpr int kChunkChainQwords     = 17;
constexpr int kLerpChunkChainQwords = 22;
constexpr int kParticleChunkQwords  = 22;

// What a draw's opening costs: the transform and dynamic-light blocks (8 and 12 qwords of
// payload, each with cmdbuf::Alloc's skip tag and the REF tag that sends it), plus a VIF FLUSH.
//
// The FLUSH is load-bearing: both unpacks go to *absolute* VU addresses, which the double buffer
// does not protect, and the previous draw's last chunk is routinely still running.
constexpr int kDrawSetupQwords = 1 + (8 + 2) + (12 + 2);

constexpr int ChunkCount(const int items, const int perChunk)
{
    return (items + perChunk - 1) / perChunk;
}

// Chain qwords a draw of 'count' vertices / particles needs on top of the data itself.
//
// Two setup blocks, not one: every chunk reserves the setup alongside itself so that an
// overflowing reservation can re-emit it, so the last chunk asks for room the draw never uses.
// Plus the terminator, because EnsureTextureResident can fence the GS mid-draw and a kick writes
// FLUSH + END at the cursor - it cannot reserve that itself without risking a rewind.
constexpr int DrawTrianglesChainCost(const int vertCount)
{
    return cmdbuf::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(vertCount, vu1::kMaxVertsPerBatch) * kChunkChainQwords);
}

constexpr int DrawLerpedTrianglesChainCost(const int vertCount)
{
    return cmdbuf::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(vertCount, vu1::kMaxLerpVertsPerBatch) * kLerpChunkChainQwords);
}

constexpr int DrawParticlesChainCost(const int count)
{
    return cmdbuf::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(count, vu1::kMaxParticlesPerBatch) * kParticleChunkQwords);
}

// ------------------------------------------------------------------------------------------------
// Draws
// ------------------------------------------------------------------------------------------------

// **None is synchronous.** Each appends to the command buffer and returns; nothing is sent until
// EndFrame. The vertex data must therefore stay valid for the rest of the frame, which is why it
// is a span of the buffer itself, and the caller must have reserved the matching *ChainCost on
// top of it so that nothing here can rewind the buffer out from under it.

// A triangle list under 'mvp', sampling 'texture' (made resident on demand). Any whole-triangle
// count works: past vu1::kMaxVertsPerBatch it splits into chunks submitted back to back, which
// overlaps each chunk's upload with the previous one's transform.
void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const vu1::DrawVertex * verts, int vertCount,
                   DrawFlags flags = DrawFlags::None);

// 'vertCount' keyframe-lerped vertices: 'posChunks' is the gathered position stream, one
// vu1::LerpPosChunk per VU run, and 'attribs' a contiguous run of vertCount per-vertex attributes
// the chunks slice in the same order.
//
// The two come from different places on purpose: positions are an indexed gather and live in the
// command buffer, attributes are the model's own baked array and are referenced where they lie.
// Both must stay valid until the frame's kick, and 'attribs' must be qword aligned.
void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight, const vu1::LerpPosChunk * posChunks,
                         const vu1::LerpDrawAttrib * attribs, int vertCount,
                         FaceCull faceCull = FaceCull::None, DrawFlags flags = DrawFlags::None);

// Camera-facing billboards as GS sprites, expanded entirely on VU1 - the caller transforms
// nothing, and the billboard grows with distance the way ref_gl's particles do.
//
// 'quadOffset' is the world-space vector from a particle's anchor corner to its opposite one: the
// camera's (up + right), pre-scaled by the caller's blow-up (ref_gl uses 1.5). It must be
// orthogonal to the view axis - that is what lets every corner share the centre's depth and the
// billboard draw as one axis-aligned sprite.
void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const vu1::ParticleVertex * particles,
                   int count, DrawFlags flags = DrawFlags::Blended);

// The frame's lights, shared by every batch drawn with DrawFlags::DynamicLights until the next
// call. Fewer than vu1::kMaxDynamicLights is fine - unused slots are zeroed and contribute
// nothing; count 0 turns the lighting off without clearing the flag.
//
// Colours are pre-scaled to the GS 0-255 range and pre-divided by the radius squared here, which
// reduces the microprogram's attenuation to one multiply-add with no divide or square root.
void SetDynamicLights(const vu1::DynamicLight * lights, int count);

// The frame's turbulent surface animation, shared by every batch drawn with DrawFlags::Warped
// until the next call. 'phaseTurns' is the elapsed time as a fraction of a full sine period,
// wrapped into [0, 1) by the caller so it stays precise however long the session has run;
// 'scrollTexels' is SURF_FLOWING's whole-tile drift, applied only to batches that also carry
// DrawFlags::WarpFlowing. The rest of what the warp needs is per texture, and the chunk emitter
// takes it from the batch's own texture.
void SetWarpAnimation(float phaseTurns, float scrollTexels);

// ------------------------------------------------------------------------------------------------
// Vertex streams
// ------------------------------------------------------------------------------------------------

// A stream is a cursor into the command buffer, not storage it owns: it claims its worst case on
// the first BeginVerts of a cycle, fills what it needs and hands the rest back at Submit, so a
// gather 40 vertices long costs the frame 40 vertices rather than its whole capacity.
//
// Only one stream may hold a claim at a time - the earlier one's commit would cut the later one's
// data away - so a stream is scoped to a pass that finishes before the next starts, and its
// destructor asserts it went out submitted. Both are a handful of bytes; instances are locals.
//
// **Every member is defined here, and a stream's address must never be stored anywhere the
// compiler cannot see.** Load-bearing, not style: gcc keeps the gather cursor in registers only
// while it can prove nothing else reaches the object. An out-of-line member, or 'this' parked in
// a global, spills it to the stack and reloads it around every push - six memory ops per triangle
// in the renderer's hottest loop, measured at +6.7% on the MD2 gather. It is also why the draw
// state lives on the stream rather than in module state.

// Gathered triangles on their way to the world/lit microprogram.
class TriangleStream final
{
    // 'maxVerts' is a whole number of triangles, and at least one worst-case clipped triangle:
    // PushClippedTriangle fans a cut polygon in one go and cannot split it across two cycles.
    explicit TriangleStream(const int maxVerts)
        : m_maxVerts{ maxVerts }
        , m_claimQwords{ cmdbuf::CalcAllocCost<vu1::DrawVertex>(maxVerts)
                       + DrawTrianglesChainCost(maxVerts) }
    {
        PS2_AssertMsg((maxVerts % 3) == 0, "Stream capacity must be a whole number of triangles!");
        PS2_AssertMsg(maxVerts >= (clip::kMaxClippedVerts - 2) * 3,
                      "Stream capacity must hold one worst-case clipped triangle!");
    }

public:
    ~TriangleStream()
    {
        // Still holding a span means a leaked claim: the buffer's write cursor is above the
        // gathered vertices and nothing will ever reference them.
        PS2_AssertMsg(m_verts == nullptr, "TriangleStream destroyed without an rs::Submit!");
    }

    TriangleStream(const TriangleStream &) = delete;
    TriangleStream & operator=(const TriangleStream &) = delete;

    // --------------------------------------------------------------------------------------------
    // Draw state
    //
    // What the stream's contents are submitted under. Each setter flushes what was gathered under
    // the outgoing state first, so a run of vertices always draws with the state it was gathered
    // under.
    // --------------------------------------------------------------------------------------------

    // Held and compared by pointer: a Mat4 is four quadwords to copy per texture chain otherwise,
    // and every pass flushes before its matrix dies.
    Q_ALWAYS_INLINE void SetTransform(const math::Mat4 & mvp)
    {
        if (m_mvp != &mvp)
        {
            Flush();
            m_mvp = &mvp;
        }
    }

    // Residency is taken at the draw, not here, so a texture whose geometry all gets culled
    // uploads nothing.
    Q_ALWAYS_INLINE void SetTexture(const tex::Texture & texture)
    {
        if (m_texture != &texture)
        {
            Flush();
            m_texture = &texture;
        }
    }

    Q_ALWAYS_INLINE void SetDrawFlags(const DrawFlags flags)
    {
        if (m_drawFlags != flags)
        {
            Flush();
            m_drawFlags = flags;
        }
    }

    // What the clipper cuts against, and what the flush draws under.
    Q_ALWAYS_INLINE const math::Mat4 & Transform() const
    {
        PS2_AssertMsg(m_mvp != nullptr, "No transform set - call SetTransform first!");
        return *m_mvp;
    }

    // --------------------------------------------------------------------------------------------
    // Gathering
    // --------------------------------------------------------------------------------------------

    // "I am about to push 'verts' vertices." Flushes and re-claims when they will not fit, in
    // place of a capacity check at every push; hoist it out of a loop wherever the count is known.
    //
    // Also where the 2D section is closed, because taking the buffer *is* the 2D->3D boundary: a
    // pending 2D section holds an open DMA tag, and an allocation cannot land inside one.
    Q_ALWAYS_INLINE void BeginVerts(const int verts)
    {
        PS2_Assert(verts > 0 && verts <= m_maxVerts);

        if ((m_vertCount + verts) > m_maxVerts) [[unlikely]]
        {
            Flush();
        }
        if (m_verts == nullptr) [[unlikely]]
        {
            Claim();
        }
    }

    // Three consecutive slots, for a caller filling a whole triangle at once.
    // BeginVerts must have covered them.
    Q_ALWAYS_INLINE vu1::DrawVertex * PushTriangle()
    {
        PS2_AssertMsg((m_vertCount + 3) <= m_maxVerts, "TriangleStream overflow - BeginVerts undercounted!");

        vu1::DrawVertex * const tri = m_verts + m_vertCount;
        m_vertCount += 3;
        return tri;
    }

    // Room for up to 'verts' vertices, as BeginVerts, and where they start. For a caller gathering
    // a run whose most it knows up front - a polygon's triangles - which then writes through a
    // local cursor and hands back where it stopped with CommitVerts.
    //
    // A cursor of the caller's own is the point: pushed one triangle at a time, every triangle
    // reloads the stream's pointer and three of its members and stores the count back, because
    // under -fno-strict-aliasing the vertex stores in between could have changed any of them.
    // Nothing may push, flush or change state between the two calls.
    Q_ALWAYS_INLINE vu1::DrawVertex * ReserveVerts(const int verts)
    {
        BeginVerts(verts);
        return m_verts + m_vertCount;
    }

    // Takes what was written since ReserveVerts - everything below 'end' - and leaves the rest of
    // the reservation for whatever comes next.
    Q_ALWAYS_INLINE void CommitVerts(const vu1::DrawVertex * const end)
    {
        const int count = static_cast<int>(end - (m_verts + m_vertCount));
        PS2_AssertMsg(count >= 0 && (count % 3) == 0 && (m_vertCount + count) <= m_maxVerts,
                      "TriangleStream::CommitVerts past what ReserveVerts gave out!");
        m_vertCount += count;
    }

    // One slot. Same contract.
    Q_ALWAYS_INLINE vu1::DrawVertex & PushVertex()
    {
        PS2_AssertMsg(m_vertCount < m_maxVerts, "TriangleStream overflow - BeginVerts undercounted!");
        return m_verts[m_vertCount++];
    }

    // Clips one triangle against the volume the VU judges and appends the survivors, fanned. Calls
    // BeginVerts itself for the post-clip count, so a caller gathering only through this never
    // calls it at all, and counts the culled/clipped/drawn triangles for the caller.
    //
    // The corners arrive with position, UVs and colour payload set; the clipper fills in their
    // distances. 'vertexColor' packs one survivor's final GS colour.
    void PushClippedTriangle(clip::ClipVertex (&corners)[3], const u32 vertexColor)
    {
        const clip::ClipVertex * verts = nullptr;
        u32 planesCrossed = 0;
        const int count = clip::ClipTriangle(corners, Transform(),
                                             clip::SharedScratch(),
                                             &verts, &planesCrossed);

        if (count == 0)
        {
            PS2_PROFILE_ONLY(++GetStats().trisCulled);
            return;
        }

#if PS2_QUAKE_PROFILE
        if (planesCrossed != 0)
        {
            CountClippedTriangle(planesCrossed, count);
        }
#endif // PS2_QUAKE_PROFILE

        // The survivors fan-triangulate.
        const int numTriangles = count - 2;
        BeginVerts(numTriangles * 3);

        for (int v = 1; v < count - 1; ++v)
        {
            EmitVertex(verts[0],     vertexColor);
            EmitVertex(verts[v],     vertexColor);
            EmitVertex(verts[v + 1], vertexColor);
        }
    }

    Q_ALWAYS_INLINE bool IsEmpty() const { return m_vertCount == 0; }

private:
    template<typename T>
    friend T Begin(const int maxVerts);

    template<typename T>
    friend void Submit(T & stream);

    // Sends what has been gathered under the current draw state and empties the stream. Free when
    // it is already empty.
    void Flush()
    {
        if (m_verts == nullptr)
        {
            return; // Nothing claimed, so nothing to hand back.
        }

        if (m_vertCount > 0)
        {
            PS2_AssertMsg(m_mvp != nullptr && m_texture != nullptr,
                          "TriangleStream::Flush with no transform or texture set!");

            // Cut the claim back *before* the draw appends the chunks referencing it: they have
            // to land after the data, and the rest of the claim is what makes room for them.
            cmdbuf::Commit(m_verts, m_vertCount);

#if PS2_QUAKE_PROFILE
            DrawStats & stats = GetStats();
            ++stats.drawBatches;
            stats.trisDrawn += m_vertCount / 3;
#endif // PS2_QUAKE_PROFILE

            DrawTriangles(*m_mvp, *m_texture, m_verts, m_vertCount, m_drawFlags);

            m_vertCount = 0;
        }
        else
        {
            // Claimed and never filled: BeginVerts takes the span before the first push, so a
            // flush can land in between. Committing nothing hands the whole capacity back;
            // dropping the pointer would strand it in the buffer.
            cmdbuf::Commit(m_verts, 0);
        }

        m_verts = nullptr;
    }

    // Closes the 2D section, reserves and claims the span. Cold: once per flush cycle.
    void Claim()
    {
        // Reserving is separate from claiming because cmdbuf::Reserve may rewind the buffer,
        // which is safe here and only here - nothing of this stream's is live yet.
        FlushPending2D();
        cmdbuf::Reserve(m_claimQwords);
        m_verts = cmdbuf::AllocMax<vu1::DrawVertex>(m_maxVerts);
    }

    Q_ALWAYS_INLINE void EmitVertex(const clip::ClipVertex & v, const u32 rgba)
    {
        vu1::DrawVertex & dst = m_verts[m_vertCount++];
        dst.position = { v.pos.x, v.pos.y, v.pos.z };
        dst.rgba     = rgba;
        dst.s        = v.st.x;
        dst.t        = v.st.y;
        // The two lightmap lanes go unwritten: no microprogram reads them, and a clipped
        // vertex has no second UV set to put there - the pass that wants one feeds it
        // through 'st' instead (see SurfaceDrawState::lightmapUVs).
    }

private:
    vu1::DrawVertex * m_verts     = nullptr; // into the command buffer; null between flush cycles
    int               m_vertCount = 0;

    const math::Mat4 *   m_mvp       = nullptr;
    const tex::Texture * m_texture   = nullptr;
    DrawFlags            m_drawFlags = DrawFlags::None;

    const int m_maxVerts;
    const int m_claimQwords; // what one flush cycle reserves: the span plus the draw's tags
};

template<>
Q_ALWAYS_INLINE TriangleStream Begin<TriangleStream>(const int maxVerts)
{
    return TriangleStream{ maxVerts };
}

template<>
Q_ALWAYS_INLINE void Submit<TriangleStream>(TriangleStream & stream)
{
    stream.Flush();
}

// The keyframe-lerped equivalent, for MD2 alias models. It gathers *only* positions, in
// vu1::LerpPosChunk groups of one VU run each; the per-vertex attributes are the model's own baked
// array, named once by SetAttribSource and sliced to match each flush.
class LerpStream final
{
    // 'maxVerts' is a whole number of triangles. The claim covers two draws' worth of tags
    // because rs::Resubmit emits a second set over the same data and must not be what overflows.
    explicit LerpStream(const int maxVerts)
        : m_maxVerts{ maxVerts }
        , m_maxChunks{ ChunkCount(maxVerts, vu1::kMaxLerpVertsPerBatch) }
        , m_claimQwords{ cmdbuf::CalcAllocCost<vu1::LerpPosChunk>(ChunkCount(maxVerts, vu1::kMaxLerpVertsPerBatch))
                       + (2 * DrawLerpedTrianglesChainCost(maxVerts)) }
    {
        PS2_AssertMsg((maxVerts % 3) == 0, "Stream capacity must be a whole number of triangles!");
    }

public:
    ~LerpStream()
    {
        PS2_AssertMsg(m_chunks == nullptr, "LerpStream destroyed without an rs::Submit!");
    }

    LerpStream(const LerpStream &) = delete;
    LerpStream & operator=(const LerpStream &) = delete;

    // Draw state, as TriangleStream's:

    Q_ALWAYS_INLINE void SetTransform(const math::Mat4 & mvp)
    {
        if (m_mvp != &mvp)
        {
            Flush();
            m_mvp = &mvp;
        }
    }

    Q_ALWAYS_INLINE void SetTexture(const tex::Texture & texture)
    {
        if (m_texture != &texture)
        {
            Flush();
            m_texture = &texture;
        }
    }

    Q_ALWAYS_INLINE void SetDrawFlags(const DrawFlags flags)
    {
        if (m_drawFlags != flags)
        {
            Flush();
            m_drawFlags = flags;
        }
    }

    Q_ALWAYS_INLINE void SetFaceCull(const FaceCull cull)
    {
        if (m_faceCull != cull)
        {
            Flush();
            m_faceCull = cull;
        }
    }

    // The keyframe interpolation the contents draw under: the two frame scales and the entity's
    // light (vertex alpha in .w). See vu1::LerpConstants. No equality test - these change per
    // entity, and comparing seven floats costs more than the flush it would save.
    void SetLerpParams(const math::Vec3 & frontv, const math::Vec3 & backv, const math::Vec4 & shadeLight)
    {
        Flush();
        m_frontv     = frontv;
        m_backv      = backv;
        m_shadeLight = shadeLight;
    }

    // Names the attribute array the gather about to start reads alongside its positions - the
    // model's baked vertices, in draw order. Every push consumes one entry and each flush steps
    // past the run it covered, so a model too large for one cycle splits both streams alike.
    void SetAttribSource(const vu1::LerpDrawAttrib * const attribs)
    {
        PS2_AssertMsg(m_vertCount == 0, "SetAttribSource in the middle of a gather!");
        m_attribs = attribs;
    }

    // --------------------------------------------------------------------------------------------
    // Gathering
    // --------------------------------------------------------------------------------------------

    // As TriangleStream::BeginVerts. The *group* boundary is PushTriangle's business - a group is
    // one VU run, and a triangle may not straddle two.
    Q_ALWAYS_INLINE void BeginVerts(const int verts)
    {
        PS2_Assert(verts > 0 && verts <= m_maxVerts);

        if ((m_vertCount + verts) > m_maxVerts) [[unlikely]]
        {
            Flush();
        }
    }

    // Three consecutive position slots. Advances to the next group when the current one fills,
    // which is safe here because a group holds a whole number of triangles. Only positions come
    // back - the attributes were named once by SetAttribSource and sit at the same index.
    Q_ALWAYS_INLINE vu1::LerpVertexBytes * PushTriangle()
    {
        PS2_AssertMsg((m_vertCount + 3) <= m_maxVerts, "LerpStream overflow - BeginVerts undercounted!");

        // One test covers both the first push of a cycle and a group boundary: m_chunkVerts starts
        // out saying the (non-existent) current group is full, so the claim and the advance are the
        // same branch.
        if (m_chunkVerts == vu1::kMaxLerpVertsPerBatch) [[unlikely]]
        {
            NextChunk();
        }

        vu1::LerpVertexBytes * const pos = m_pos;
        m_pos        += 3;
        m_chunkVerts += 3;
        m_vertCount  += 3;
        return pos;
    }

    Q_ALWAYS_INLINE bool IsEmpty() const { return m_vertCount == 0; }

private:
    template<typename T>
    friend T Begin(const int maxVerts);

    template<typename T>
    friend void Submit(T & stream);

    friend void Resubmit(LerpStream & stream);

    // Sends the gathered groups under the stream's current draw state and empties it.
    void Flush()
    {
        // Nothing claimed and nothing gathered: return without touching the redraw record. Load
        // bearing - the MD2 shadow sets its own state between the model's submit and its
        // rs::Resubmit, and those setters flush, which would otherwise wipe the record.
        if (m_vertCount == 0 && m_chunks == nullptr)
        {
            return;
        }

        // Otherwise recorded even with nothing to send, so a resubmit after an empty flush draws
        // nothing rather than whatever came before.
        m_lastFlushed        = m_chunks;
        m_lastFlushedAttribs = m_attribs;
        m_lastFlushedCount   = m_vertCount;

        if (m_vertCount > 0)
        {
            PS2_AssertMsg(m_attribs != nullptr, "LerpStream::Flush with no attribute source!");
            PS2_AssertMsg(m_mvp != nullptr && m_texture != nullptr, "LerpStream::Flush with no transform or texture set!");

            // Whole groups: a partly filled last group is all a cycle wastes.
            cmdbuf::Commit(m_chunks, ChunkCount(m_vertCount, vu1::kMaxLerpVertsPerBatch));

#if PS2_QUAKE_PROFILE
            DrawStats & stats = GetStats();
            ++stats.drawBatches;
            stats.trisDrawn += m_vertCount / 3;
#endif // PS2_QUAKE_PROFILE

            // Through locals: DrawLerpedTriangles takes these by const reference, and handing it
            // &m_frontv would make the whole stream address-taken. See the note on this class.
            const math::Vec3 frontv = m_frontv;
            const math::Vec3 backv  = m_backv;
            const math::Vec4 shade  = m_shadeLight;
            DrawLerpedTriangles(*m_mvp, *m_texture, frontv, backv, shade,
                                m_chunks, m_attribs, m_vertCount, m_faceCull, m_drawFlags);

            // Past what this cycle submitted, so a multi-cycle model carries on where it left off.
            m_attribs  += m_vertCount;
            m_vertCount = 0;
        }
        else if (m_chunks != nullptr)
        {
            // As TriangleStream::Flush, though NextChunk only claims on an actual push - belt to
            // that brace rather than a path anything takes today.
            cmdbuf::Commit(m_chunks, 0);
        }

        m_chunks     = nullptr;
        m_chunkVerts = vu1::kMaxLerpVertsPerBatch; // next push starts a group
    }

    // Draws the most recent flush's vertices again under whatever draw state the stream carries
    // now, without rebuilding them: the groups are still in the command buffer, so this is only a
    // second set of chunk tags over them. The MD2 shadow is the caller.
    //
    // Valid only while nothing has been pushed since that flush, and only useful if the geometry
    // went out in one cycle - a model that filled the buffer mid-way left just its tail behind.
    void ResubmitLastFlush()
    {
        PS2_AssertMsg(m_vertCount == 0, "ResubmitLastFlush after pushing new vertices!");

        if (m_lastFlushedCount > 0)
        {
            // A batch, but not new geometry, so trisDrawn is deliberately left alone.
            PS2_PROFILE_ONLY(++GetStats().drawBatches);

            const math::Vec3 frontv = m_frontv; // as Flush, see there
            const math::Vec3 backv  = m_backv;
            const math::Vec4 shade  = m_shadeLight;
            DrawLerpedTriangles(*m_mvp, *m_texture, frontv, backv, shade,
                                m_lastFlushed, m_lastFlushedAttribs, m_lastFlushedCount,
                                m_faceCull, m_drawFlags);
        }
    }

    // Claims the span on the first push of a cycle, and steps to the next group after that.
    void NextChunk()
    {
        if (m_chunks == nullptr) [[unlikely]]
        {
            // As TriangleStream::Claim.
            FlushPending2D();
            cmdbuf::Reserve(m_claimQwords);
            m_chunks = cmdbuf::AllocMax<vu1::LerpPosChunk>(m_maxChunks);
            m_chunk  = m_chunks;
        }
        else
        {
            ++m_chunk;
        }
        m_pos = m_chunk->pos;
        m_chunkVerts = 0;
    }

private:
    int m_vertCount = 0;

    // The model's baked attributes, at the vertex the next push will fill.
    const vu1::LerpDrawAttrib * m_attribs = nullptr;

    // What the last flush submitted, and where both its streams are; see ResubmitLastFlush.
    int                         m_lastFlushedCount   = 0;
    vu1::LerpPosChunk *         m_lastFlushed        = nullptr;
    const vu1::LerpDrawAttrib * m_lastFlushedAttribs = nullptr;

    // The claim, the group being filled and how much of it is spoken for.
    vu1::LerpPosChunk * m_chunks = nullptr; // into the command buffer; null between flush cycles
    vu1::LerpPosChunk * m_chunk  = nullptr;
    int m_chunkVerts = vu1::kMaxLerpVertsPerBatch;

    // A cursor rather than an index off m_chunk: under -fno-strict-aliasing the compiler cannot
    // prove the gather's stores disjoint, so a base plus an index it must redo per push costs
    // more than a pointer it can bump.
    vu1::LerpVertexBytes * m_pos = nullptr;

    const math::Mat4 *   m_mvp        = nullptr;
    const tex::Texture * m_texture    = nullptr;
    DrawFlags            m_drawFlags  = DrawFlags::None;
    FaceCull             m_faceCull   = FaceCull::None;
    math::Vec3           m_frontv     = {};
    math::Vec3           m_backv      = {};
    math::Vec4           m_shadeLight = {};

    const int m_maxVerts;
    const int m_maxChunks;
    const int m_claimQwords;
};

template<>
Q_ALWAYS_INLINE LerpStream Begin<LerpStream>(const int maxVerts)
{
    return LerpStream{ maxVerts };
}

template<>
Q_ALWAYS_INLINE void Submit<LerpStream>(LerpStream & stream)
{
    stream.Flush();
}

// For alias MD2 shadows.
Q_ALWAYS_INLINE void Resubmit(LerpStream & stream)
{
    stream.ResubmitLastFlush();
}

} // namespace ps2::rs
