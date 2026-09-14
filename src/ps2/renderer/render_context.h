#pragma once
/* ================================================================================================
 * File: render_context.h
 * Brief: The renderer's command recorder: the frame lifecycle, the 2D primitives, the VU1 draws
 *        and the vertex streams that gather for them. Everything a frame tells the GS to do is
 *        recorded into ps2::cmdbuf's chain here and sent in one kick at EndFrame.
 *
 *        A module, not an object: there is one recorder per frame and its state lives in
 *        detail::State, so the stream code below can reach it inline.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"
#include "ps2/renderer/clip.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/cmd_buffer.h"

namespace ps2::tex { struct Texture; }

namespace ps2::rc {

// ------------------------------------------------------------------------------------------------
// Draw flags
// ------------------------------------------------------------------------------------------------

// Optional batch draw flags (OR-able). Blended batches combine with the
// framebuffer through the A+D block's source-alpha blend and mask their
// depth writes - the z-test still reads, so they sort against opaque
// geometry but never occlude. Untextured batches draw pure gouraud colour
// (the texture argument is still bound, just not sampled).
//
// Additive replaces Blended's equation with Cs * As + Cd - the source colour
// added to the framebuffer instead of interpolated with it - and implies
// everything else Blended does, including the depth-write mask. Note the alpha
// still scales the contribution, so a fully opaque (As = 0x80) additive batch
// is exactly OpenGL's glBlendFunc(GL_ONE, GL_ONE) while a fading one needs no
// second blend mode. Additive results saturate rather than wrap: gs::Init
// leaves the GS COLCLAMP register enabled.
//
// Modulate is the third equation, Cd * As: it scales what is already in the
// framebuffer by the batch's alpha and contributes no colour of its own. This
// is how the lightmap pass darkens the diffuse pass under it. The GS blend unit
// multiplies by a scalar alpha and never by a second colour, so this is as close
// to OpenGL's glBlendFunc(GL_ZERO, GL_SRC_COLOR) as the hardware gets - what it
// modulates by is the source's *alpha*, not its RGB. The colour half of a luxel
// therefore cannot come through here at all; it reaches the screen through the
// diffuse pass's vertex colour instead (see lm::AtlasColors).
//
// The three are mutually exclusive; passing more than one asserts. Each implies
// the ABE bit and the depth-write mask.
//
// DepthHack is orthogonal to all of the above and touches no GS register: it
// compresses the batch's depth into the slice of the z-buffer nearest the
// camera, so the view weapon can never poke through a wall it is visually in
// front of (Quake 2's RF_DEPTHHACK, ref_gl's glDepthRange(0, 0.3)). It applies
// where OpenGL's depth range does - to the window coordinate, *after* the clip
// judgement - by scaling the microprogram's NDC-to-GS z conversion. Folding an
// equivalent remap into the caller's projection instead would run it before the
// judgement, which is not the same thing: clipw tests |z| against |w|, so a
// remapped z stops being rejected once w goes negative and the geometry behind
// the camera reaches the GS mirrored through the origin.
//
// NoDepthWrite is likewise orthogonal: it masks the batch's depth writes on
// their own, without the blend equation and ABE bit the three modes above drag
// along with theirs. The z-test still reads, so the batch sorts against what is
// already in the buffer but leaves nothing behind for later ones to sort
// against - what an opaque primitive standing in for infinity wants. The
// skybox is the caller: it draws at a finite 2300 units so the world can
// occlude it, and must not occlude anything drawn after it out there in return.
//
// DynamicLights run the lit microprogram: the batch's vertex colour is computed
// from the frame's dynamic point lights (see SetDynamicLights) instead of taken
// from the input vertices, and Modulate batches add it on top of what they
// modulate. Only meaningful for geometry submitted in world space.
enum class DrawFlags : u32
{
    None          = 0,
    Blended       = 1 << 0,
    Untextured    = 1 << 1,
    Additive      = 1 << 2,
    Modulate      = 1 << 3,
    DepthHack     = 1 << 4,
    NoDepthWrite  = 1 << 5,
    DynamicLights = 1 << 6,
};

constexpr DrawFlags operator|(DrawFlags a, DrawFlags b)
{
    return static_cast<DrawFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

constexpr bool HasDrawFlag(DrawFlags flags, DrawFlags test)
{
    return (static_cast<u32>(flags) & static_cast<u32>(test)) != 0;
}

// Backface culling mode for DrawLerpedTriangles: which sign of a triangle's
// screen-space signed area gets rejected on the VU. Which of the two is
// facing away depends on the winding and the projection's Y orientation.
enum class FaceCull : u32
{
    None     = 0,
    Negative = 1,
    Positive = 2,
};

// ------------------------------------------------------------------------------------------------
// Debug draw stats trackers
// ------------------------------------------------------------------------------------------------

// What the renderer submitted this frame, as opposed to what the view decided to submit - that
// half is view::DrawStats. Everything here is counted by the streams and the draw paths below, so
// no caller adds to it. Cleared by BeginFrame.
struct DrawStats
{
    int trisDrawn;   // Triangles handed to VU1, after EE clipping.
    int trisClipped; // Triangles re-cut against the VU clip volume.
    int trisCulled;  // Triangles dropped whole, entirely outside it.
    int drawBatches; // VU1 batches submitted (one or more per texture).
    int particles;   // Particle billboards submitted.
};

namespace detail {
extern DrawStats g_drawStats;
} // namespace detail

// What this frame has submitted so far. Inline so the streams can bump it without a call.
Q_ALWAYS_INLINE DrawStats & GetStats()
{
    return detail::g_drawStats;
}

// The most qwords one GIF block has held. Shown as "Gif2DPk" in the draw-stats overlay; what it
// measures against is the command buffer half it has to fit inside (cmdbuf::kHalfBytes).
int Gif2DPeakQwords();

// ------------------------------------------------------------------------------------------------
// Frame lifecycle
// ------------------------------------------------------------------------------------------------

// Background colour the frame clear fills with.
void SetClearColor(u8 r, u8 g, u8 b);

// Opens the frame: shows the previous one if it was left drawing, rewinds the command buffer
// and writes the screen clear at the head of it. 2D and 3D may then be drawn in any order, and
// both record into the same buffer.
//
// 'dither' enables the GS's ordered dither, which hides the banding a 16-bit framebuffer shows on
// smooth gradients and does nothing to a 32-bit one. Passed per frame so it can be flipped live.
void BeginFrame(bool dither);

// Closes the frame: flushes any pending 2D and submits the command buffer.
//
// 'deferPresent' leaves the frame drawing for the next BeginFrame to show, so the GS rasterises it
// while the EE builds the frame after - one frame of latency bought for the fence the EE would
// otherwise stand at. Clear it and this waits for the GS and flips before returning.
void EndFrame(bool deferPresent);

// Full sync/drain of the underlying cmdbuf. Kicks what has been recorded and waits for it.
void KickAndWait();

// Makes the texture's pixels resident in GS VRAM, uploading them on a miss and evicting the
// least-recently-bound textures when the heap is full. Already-resident textures only have their
// LRU stamp refreshed, unless their pixels were marked dirty, which re-uploads in place.
//
// May fence the GS - submitting the frame so far and waiting for it - when an upload would land
// on VRAM that queued draws still sample. That closes and reopens any open GIF section, so a
// GifWriter taken before this call must not be used after it.
void EnsureTextureResident(const tex::Texture & texture);

// ------------------------------------------------------------------------------------------------
// VU1 bring-up
// ------------------------------------------------------------------------------------------------

// Built into the frame's chain like any other VIF1 transfer, so these run after cmdbuf::Init and
// before any drawing. vu1::Init is the only caller.

// References a microprogram into the chain as MPG transfers (chunked to the VIF's
// 256-instruction limit).
void AddMicroProgram(vu1::ProgramAddr dest, vu1::VUCode code);

// Programs the VIF1 BASE/OFFSET registers that split VU data memory into the two halves XTOP
// alternates between. Both in qwords.
void AddDoubleBufferSettings(u32 baseQw, u32 offsetQw);

// ------------------------------------------------------------------------------------------------
// Triangle Streams lifecycle
// ------------------------------------------------------------------------------------------------

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

// Room for 'count' particle billboards in the command buffer, to fill in place. Closes the 2D
// section and reserves what the draw will append on top, as a stream's first BeginVerts does.
template<>
vu1::ParticleVertex * Begin<vu1::ParticleVertex *>(const int particleCount);

// Submits particles. Takes its draw state directly rather than carrying any: there is one caller, and a
// stream's reason for holding state - a gather loop that would otherwise re-set it per triangle -
// does not apply when the whole batch is written in one go.
void Submit(vu1::ParticleVertex * __restrict & particles, const math::Mat4 & mvp, const tex::Texture & texture,
            const math::Vec3 & quadOffset, DrawFlags flags = DrawFlags::Blended);

// ------------------------------------------------------------------------------------------------
// Chain budget
// ------------------------------------------------------------------------------------------------

// What a draw costs the command buffer besides its vertex data, so a caller whose vertex data is
// *itself* in the buffer can reserve the pair together.
//
// It has to reserve the pair. The chunk loop reserves as it goes, and cmdbuf::Reserve rewinds when
// it comes up short - which would pull the buffer out from under the very span the chunks being
// emitted reference. Reserving the whole draw up front means that reservation can never fire half
// way through one.
//
// Nothing outside this module reserves anything: the streams below reserve for themselves and
// BeginParticles reserves for the particle draw. This is in the header only because the stream
// constructors compute their claim from it.

// Chain qwords one chunk of each draw appends. The world path: the header/GIF-tag inline unpack
// (1 tag qword plus 8 of payload), the vertex REF unpack (1 - its VIFcodes ride in the tag's upper
// half) and the FLUSH + MSCAL (1). 11 in practice, declared with room to spare; over-declaring
// only reserves slightly more of the buffer than a chunk needs. The lerped path adds a second REF
// unpack and a longer header (15 in practice), the particle path an 11-qword header (14).
constexpr int kChunkChainQwords     = 16;
constexpr int kLerpChunkChainQwords = 22;
constexpr int kParticleChunkQwords  = 22;

// Chain qwords a draw's opening costs: the transform block and the dynamic-light block, both
// built in the buffer rather than referenced out of a static. 8 and 12 qwords of payload, each
// fronted by the skip tag cmdbuf::Alloc needs and followed by the REF tag that sends it, plus
// the VIF FLUSH in front of the pair.
//
// That FLUSH is what stops the two unpacks landing on VU memory a microprogram is still
// reading. Both go to *absolute* addresses - the constants sit below the double buffer and the
// light block above it - so unlike a chunk's data they get no protection from the buffer swap,
// and since draws stopped being kicked and waited on one at a time the previous draw's last
// chunk is routinely still running when the next draw's setup arrives.
constexpr int kDrawSetupQwords = 1 + (8 + 2) + (12 + 2);

constexpr int ChunkCount(const int items, const int perChunk)
{
    return (items + perChunk - 1) / perChunk;
}

// Chain qwords the three draws need for 'count' vertices / particles, not counting the data
// itself.
//
// The peak the chunk loop *demands*, which is one setup block more than the draw ever
// appends: every chunk reserves the setup alongside itself, because a reservation that
// overflowed would rewind the setup with everything else and the next chunk has to be able to
// re-emit it. So the last chunk asks for room the draw will not end up using, and reserving
// only what is written would let that final ask rewind the buffer mid-draw.
//
// Plus the terminator. A draw no longer ends in a kick, but it can still contain one: the GS
// fence EnsureTextureResident takes when an upload is about to land on VRAM that queued
// draws may still sample has to send the buffer now that nothing else will until EndFrame, and
// a kick writes its FLUSH + END into the buffer at the cursor. Budgeted here rather than
// reserved where it fires, because reserving there is exactly what must not happen - a
// reservation that overflowed would rewind the buffer out from under the span the draw is
// about to reference.
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

// **None is synchronous.** Each appends to the frame's command buffer and returns; nothing is sent
// until EndFrame kicks it. So the vertex data has to stay valid for the rest of the frame, not for
// the duration of the call - which is why it is a span of the buffer itself (cmdbuf::Alloc).
//
// A caller must have reserved the matching *ChainCost on top of that span, so that nothing here
// can rewind the buffer out from under it. A kick is harmless (it leaves the buffer where it is);
// a rewind would leave the REF tags pointing at memory the next gather is about to write.

// Draws a batch of triangles (3 verts each, triangle list) with the given transform and texture
// (made resident on demand). Any whole-triangle count works: draws beyond vu1::kMaxVertsPerBatch
// are split into chunks submitted back to back, overlapping each chunk's upload with the previous
// one's transform.
void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const vu1::DrawVertex * verts, int vertCount,
                   DrawFlags flags = DrawFlags::None);

// Draws 'vertCount' keyframe-lerped vertices: 'posChunks' is the gathered position stream, one
// vu1::LerpPosChunk per VU run, and 'attribs' is a contiguous run of vertCount per-vertex
// attributes the chunks slice in the same order.
//
// The two come from different places on purpose. Positions are an indexed gather and have to be
// built, so they live in the command buffer like every other gather; attributes are the model's
// own baked array in draw order, so they are referenced where they lie and never copied. Both must
// stay valid until the frame's kick - the buffer does by construction, the model hunk because
// nothing unloads a model mid-frame - and 'attribs' must be qword aligned, which mod::AliasVertex is.
void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight, const vu1::LerpPosChunk * posChunks,
                         const vu1::LerpDrawAttrib * attribs, int vertCount,
                         FaceCull faceCull = FaceCull::None, DrawFlags flags = DrawFlags::None);

// Draws camera-facing particle billboards as GS sprites, expanded entirely on VU1 - the caller
// transforms nothing.
//
// 'quadOffset' is the world-space vector from a particle's anchor corner to its opposite corner:
// the camera's (up + right), pre-scaled by whatever blow-up the caller wants (ref_gl uses 1.5). It
// must be orthogonal to the view axis, which is what makes every corner share the centre's depth
// and lets the billboard draw as a single axis-aligned sprite; it is transformed once per call as
// a direction.
//
// The billboard also grows with distance the way ref_gl's particles do. The texture is sampled
// corner to corner through UV (no perspective correction, which a screen-aligned sprite does not
// need).
void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const vu1::ParticleVertex * particles,
                   int count, DrawFlags flags = DrawFlags::Blended);

// Sets the frame's lights, shared by every batch drawn with DrawFlags::DynamicLights until the
// next call. Fewer than vu1::kMaxDynamicLights is fine - unused slots are zeroed and contribute
// nothing. Pass count 0 to turn the lighting off without clearing the flag.
//
// Colours are pre-scaled here into the GS 0-255 range and pre-divided by the radius squared, which
// is what lets the microprogram attenuate with a single multiply-add and no divide or square root.
void SetDynamicLights(const vu1::DynamicLight * lights, int count);

// ------------------------------------------------------------------------------------------------
// Vertex streams
// ------------------------------------------------------------------------------------------------

// A stream is a cursor into the command buffer, not storage it owns: it claims its worst case on
// the first BeginVerts of a flush cycle, fills what it needs, hands the rest back at Flush and is then
// referenced where it lies - so a gather 40 vertices long costs the frame 40 vertices, not its
// whole capacity.
//
// Two streams cannot hold claims at once: the earlier one's commit would cut the later one's data
// away. So a stream is scoped to a pass that finishes before the next starts, and its destructor
// asserts it went out flushed. Both are a handful of bytes - instances are locals.
//
// **Every member is defined here, and the stream's address must never be stored anywhere the
// compiler cannot see.** Both are load-bearing rather than style. The gather cursor lives in
// registers only while gcc can prove nothing else reaches the object: give it an out-of-line
// member, or park 'this' in a global, and it spills m_vertCount/m_pos/m_chunkVerts to the stack
// and reloads them around every push - six memory ops per triangle in the hottest loop in the
// renderer, measured at +6.7% on the MD2 gather. That is also why the draw state lives on the
// stream rather than in the module's own state: the flush had to be able to read it without the
// stream registering itself anywhere.

// Gathered triangles on their way to the world/lit microprogram.
class TriangleStream final
{
    // 'maxVerts' is what one flush cycle may claim: a whole number of triangles, and at least one
    // worst-case clipped triangle, since PushClippedTriangle fans a cut polygon in one go and
    // cannot split it across two cycles.
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
        // A stream that goes out of scope still holding a span has leaked its claim: the buffer's
        // write cursor is above the gathered vertices and nothing will ever reference them.
        PS2_AssertMsg(m_verts == nullptr, "TriangleStream destroyed without a Flush!");
    }

    TriangleStream(const TriangleStream &) = delete;
    TriangleStream & operator=(const TriangleStream &) = delete;

    // --------------------------------------------------------------------------------------------
    // Draw state
    //
    // What the stream's contents are submitted under. Each setter flushes what was gathered under
    // the outgoing set first, so a run of vertices always draws with the state it was gathered
    // under - the rule that used to be the caller's to remember (flush with the *outgoing*
    // transform and texture, then change them).
    // --------------------------------------------------------------------------------------------

    // Held by pointer: callers keep their matrices alive across the flush, and a Mat4 is four
    // quadwords to copy per texture chain otherwise. Compared by address, which is sound because
    // every pass flushes before its matrix dies.
    Q_ALWAYS_INLINE void SetTransform(const math::Mat4 & mvp)
    {
        if (m_mvp != &mvp)
        {
            Flush();
            m_mvp = &mvp;
        }
    }

    // Residency is not taken here - it happens at the draw, so a texture whose geometry all gets
    // culled still uploads nothing.
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

    // The transform the clipper cuts against, which is also what the flush draws under.
    Q_ALWAYS_INLINE const math::Mat4 & Transform() const
    {
        PS2_AssertMsg(m_mvp != nullptr, "No transform set - call SetTransform first!");
        return *m_mvp;
    }

    // --------------------------------------------------------------------------------------------
    // Gathering
    // --------------------------------------------------------------------------------------------

    // "I am about to push 'verts' vertices." Flushes and re-claims when they will not fit, which
    // is what replaces a capacity check at every push. Hoist it out of a loop wherever the count
    // is known - a polygon fan, a whole mesh that fits.
    //
    // Also where the 2D section is closed and the command buffer reserved, because taking the
    // buffer *is* the 2D->3D boundary: a pending 2D section holds an open DMA tag, and an
    // allocation cannot land inside one.
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

    // One slot. Same contract.
    Q_ALWAYS_INLINE vu1::DrawVertex & PushVertex()
    {
        PS2_AssertMsg(m_vertCount < m_maxVerts, "TriangleStream overflow - BeginVerts undercounted!");
        return m_verts[m_vertCount++];
    }

    // Clips one triangle against the volume the VU judges and appends the survivors, fanned. Calls
    // BeginVerts for the post-clip count itself, so a caller that only gathers through this never calls
    // BeginVerts at all.
    //
    // The corners arrive with their position, UVs and colour payload set; their clip distances are
    // computed by the clipper. 'vertexColor' packs one surviving vertex's final GS colour:
    // u32 (const clip::ClipVertex &). What was culled, cut and drawn goes into the frame's draw
    // statistics here, so callers count nothing of their own.
    template<typename ColorFn>
    void PushClippedTriangle(clip::ClipVertex (&corners)[3], ColorFn && vertexColor)
    {
        const clip::ClipVertex * verts = nullptr;
        bool wasClipped = false;
        const int count = clip::ClipTriangle(corners, Transform(),
                                             clip::SharedScratch(),
                                             &verts, &wasClipped);

        if (count == 0)
        {
            ++GetStats().trisCulled;
            return;
        }

        if (wasClipped)
        {
            ++GetStats().trisClipped;
        }

        // The survivors fan-triangulate.
        const int numTriangles = count - 2;
        BeginVerts(numTriangles * 3);

        for (int v = 1; v < count - 1; ++v)
        {
            EmitVertex(verts[0],     vertexColor(verts[0]));
            EmitVertex(verts[v],     vertexColor(verts[v]));
            EmitVertex(verts[v + 1], vertexColor(verts[v + 1]));
        }
    }

    Q_ALWAYS_INLINE bool IsEmpty() const { return m_vertCount == 0; }

private:
    template<typename T>
    friend T Begin(const int maxVerts);

    template<typename T>
    friend void Submit(T & stream);

    // Sends what has been gathered, under the stream's current draw state, and empties it. Does
    // nothing when it is empty, so flushing twice is free.
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

            // Cut the claim back to what was gathered *before* the draw appends the chunks that
            // reference it - they have to land after the data, and the rest of the claim is what
            // makes room for them.
            cmdbuf::Commit(m_verts, m_vertCount);

            DrawStats & stats = GetStats();
            ++stats.drawBatches;
            stats.trisDrawn += m_vertCount / 3;

            DrawTriangles(*m_mvp, *m_texture, m_verts, m_vertCount, m_drawFlags);

            m_vertCount = 0;
        }
        else
        {
            // Claimed and never filled - BeginVerts takes the span before the first push, so a flush
            // can land in between. Commit nothing rather than drop it: the claim is the stream's
            // whole capacity, and letting go of the pointer would leave all of it in the buffer.
            cmdbuf::Commit(m_verts, 0);
        }

        m_verts = nullptr;
    }

    // Closes the 2D section, reserves and claims the span. Cold: once per flush cycle.
    void Claim()
    {
        // Reserving is separate from claiming on purpose: cmdbuf::Reserve may rewind the buffer,
        // which is safe here and only here, because nothing of this stream's is live yet.
        FlushPending2D();
        cmdbuf::Reserve(m_claimQwords);
        m_verts = cmdbuf::AllocMax<vu1::DrawVertex>(m_maxVerts);
    }

    Q_ALWAYS_INLINE void EmitVertex(const clip::ClipVertex & v, const u32 rgba)
    {
        vu1::DrawVertex & dst = m_verts[m_vertCount++];
        dst.x    = v.pos.x;
        dst.y    = v.pos.y;
        dst.z    = v.pos.z;
        dst.w    = 1.0f;
        dst.rgba = rgba;
        dst.s    = v.st.x;
        dst.t    = v.st.y;
        dst.q    = 1.0f;
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

// The keyframe-lerped equivalent, for MD2 alias models.
//
// It gathers *only* positions, in vu1::LerpPosChunk groups, one per VU run. The other half of what
// the microprogram reads - the per-vertex attributes - is the model's own baked array in draw
// order, so the stream carries a cursor into it rather than a copy: SetAttribSource names the
// array, and each Flush hands the draw the slice matching the positions it just submitted.
class LerpStream final
{
    // 'maxVerts' is a whole number of triangles. The claim covers two draws' worth of tags, not
    // one, because ResubmitLastFlush emits a second set over the same data and must not be the thing
    // that overflows.
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
        PS2_AssertMsg(m_chunks == nullptr, "LerpStream destroyed without a Flush!");
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

    // The keyframe interpolation this stream's contents draw under: the two frame scales and the
    // entity's light (vertex alpha in .w). See vu1::kLerpFrontVAddr.
    //
    // No equality test: these change per entity, and comparing seven floats costs more than the
    // flush it would save on the rare repeat.
    void SetLerpParams(const math::Vec3 & frontv, const math::Vec3 & backv, const math::Vec4 & shadeLight)
    {
        Flush();
        m_frontv     = frontv;
        m_backv      = backv;
        m_shadeLight = shadeLight;
    }

    // Names the per-vertex attribute array the gather about to start reads alongside its
    // positions - the model's baked vertices, in draw order. Every push consumes one entry, and
    // each Flush hands the draw the run it covered and steps past it, so a model too large for one
    // cycle splits its attributes exactly where its positions split.
    void SetAttribSource(const vu1::LerpDrawAttrib * const attribs)
    {
        PS2_AssertMsg(m_vertCount == 0, "SetAttribSource in the middle of a gather!");
        m_attribs = attribs;
    }

    // --------------------------------------------------------------------------------------------
    // Gathering
    // --------------------------------------------------------------------------------------------

    // As TriangleStream::BeginVerts, for the flush cycle's capacity. The *group* boundary is
    // PushTriangle's business - a group is one VU run, and a triangle may not straddle two.
    Q_ALWAYS_INLINE void BeginVerts(const int verts)
    {
        PS2_Assert(verts > 0 && verts <= m_maxVerts);

        if ((m_vertCount + verts) > m_maxVerts) [[unlikely]]
        {
            Flush();
        }
    }

    // Three consecutive position slots. Advances to the next group when the current one is full,
    // which is safe at a triangle boundary because a group holds a whole number of triangles.
    //
    // Only positions come back: the caller named the attributes once with SetAttribSource and the
    // triangle it is filling reads them at the same index, which is the whole point.
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
        // Nothing claimed and nothing gathered: return without touching the redraw record.
        //
        // This is load-bearing, not an optimisation. The state setters flush, and the MD2 shadow
        // sets its own transform, flags and lerp params between the model's Flush and
        // ResubmitLastFlush - so without this, those setters would overwrite the record with the
        // empty state and the shadow would draw nothing at all.
        if (m_vertCount == 0 && m_chunks == nullptr)
        {
            return;
        }

        // Otherwise recorded even when there is nothing to send, so ResubmitLastFlush after a flush
        // that had nothing in it draws nothing rather than whatever came before.
        m_lastFlushed        = m_chunks;
        m_lastFlushedAttribs = m_attribs;
        m_lastFlushedCount   = m_vertCount;

        if (m_vertCount > 0)
        {
            PS2_AssertMsg(m_attribs != nullptr, "LerpStream::Flush with no attribute source!");
            PS2_AssertMsg(m_mvp != nullptr && m_texture != nullptr, "LerpStream::Flush with no transform or texture set!");

            // Whole groups: the tail of a partly filled last group is the only thing a flush cycle
            // wastes, and it is bounded by one group.
            cmdbuf::Commit(m_chunks, ChunkCount(m_vertCount, vu1::kMaxLerpVertsPerBatch));

            DrawStats & stats = GetStats();
            ++stats.drawBatches;
            stats.trisDrawn += m_vertCount / 3;

            // Through locals, not the members directly. DrawLerpedTriangles takes these by const
            // reference, so handing it &m_frontv would make the whole stream address-taken - and
            // then gcc spills the gather cursor to the stack and reloads it around every push. See
            // the note on this class.
            const math::Vec3 frontv = m_frontv;
            const math::Vec3 backv  = m_backv;
            const math::Vec4 shade  = m_shadeLight;
            DrawLerpedTriangles(*m_mvp, *m_texture, frontv, backv, shade,
                                m_chunks, m_attribs, m_vertCount, m_faceCull, m_drawFlags);

            // Past what this cycle submitted, so a model that needed more than one cycle carries
            // on where it left off.
            m_attribs  += m_vertCount;
            m_vertCount = 0;
        }
        else if (m_chunks != nullptr)
        {
            // As TriangleStream::Flush, though NextChunk only claims on an actual push, so this is
            // the belt to that brace rather than a path anything takes today.
            cmdbuf::Commit(m_chunks, 0);
        }

        m_chunks     = nullptr;
        m_chunkVerts = vu1::kMaxLerpVertsPerBatch; // next push starts a group
    }

    // Draws the vertices of the most recent Flush again, under whatever draw state the stream
    // carries now, without rebuilding them.
    //
    // The groups that Flush submitted are still in the command buffer: a Flush commits the span
    // and moves on, and nothing rewinds the buffer until the frame ends. So this is a second set
    // of chunk tags over data already there. The MD2 shadow is exactly that - the model's own
    // keyframe bytes under a squashed matrix, with an all-zero shadeLight that multiplies every
    // vertex's shade term out to black.
    //
    // Only valid while nothing has been pushed since that Flush, and only worth anything if the
    // geometry went out in a single cycle - a caller that filled the buffer mid-model left only
    // its tail behind.
    void ResubmitLastFlush()
    {
        PS2_AssertMsg(m_vertCount == 0, "ResubmitLastFlush after pushing new vertices!");

        if (m_lastFlushedCount > 0)
        {
            // A batch, but not new geometry: this re-submits the span the last Flush already
            // counted, so trisDrawn is deliberately left alone.
            ++GetStats().drawBatches;

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
            // As TriangleStream::Claim - the 2D flush is the 2D->3D boundary, and it belongs where
            // the buffer is taken.
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

    // Vertices the last Flush submitted, and where both of its streams are; see ResubmitLastFlush.
    int                         m_lastFlushedCount   = 0;
    vu1::LerpPosChunk *         m_lastFlushed        = nullptr;
    const vu1::LerpDrawAttrib * m_lastFlushedAttribs = nullptr;

    // The claim, the group being filled and how much of it is spoken for.
    vu1::LerpPosChunk * m_chunks = nullptr; // into the command buffer; null between flush cycles
    vu1::LerpPosChunk * m_chunk  = nullptr;
    int m_chunkVerts = vu1::kMaxLerpVertsPerBatch;

    // A cursor rather than an index off m_chunk: the gather loop's stores are ones the compiler
    // cannot prove disjoint from anything under -fno-strict-aliasing, so a base plus an index it
    // has to redo per push costs more than a pointer it can bump.
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

} // namespace ps2::rc
