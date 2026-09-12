#pragma once
/* ================================================================================================
 * File: batch.h
 * Brief: The triangle gather buffer every 3D path fills, then hands to VU1 as one batch.
 *
 *  vu1::DrawTriangles wants whole triangle lists, and a draw call costs a DMA
 *  chain of its own, so nothing submits a triangle at a time: each path gathers
 *  into a scratch buffer and flushes it when the texture changes, when the batch
 *  state changes or when it fills up. The buffer is referenced in place by the
 *  chain rather than copied into it.
 *
 *  That buffer is a span of the frame chain itself (chain::Alloc), not storage
 *  the batch owns. A batch claims its worst case on its first push, fills what
 *  it needs, hands the rest back at Flush and is then referenced where it lies -
 *  so a gather that is 40 vertices long costs the frame 40 vertices instead of
 *  the 2304 the old file-level array reserved whether or not anything used them.
 *
 *  Gathering goes through the EE clipper (clip.h): the microprogram rejects a
 *  triangle that straddles its clip volume whole rather than cutting it, so the
 *  survivors of a cut arrive here as a convex polygon and fan-triangulate into
 *  the buffer. That, the capacity check, the flush and the draw statistics are
 *  the same work for the world, the sky and alias models; only the per-vertex
 *  colour differs between them, which is what the 'vertexColor' callable is for.
 *
 *  A batch is 8 bytes - a cursor into the chain and a count - so instances are
 *  locals, passed to whatever does the gathering. MaxVerts is the only thing that
 *  varies between them: it says how much of the chain one flush cycle may claim.
 *  The clipper's ping-pong buffers are not in here; they are one shared instance
 *  (clip::SharedScratch), EE-only, and never reach the DMAC.
 *
 *  Two invariants hold over a span, and both come from it being part of the
 *  chain: while it is claimed nothing else may allocate from the chain, and
 *  until the frame ends nothing may rewind it. chain::Commit asserts the first.
 *  The second is chain::Reserve's overflow path, which is why a batch reserves
 *  the whole of what it is about to build - the data *and* every tag that will
 *  reference it - before it claims anything (see vu1.h's chain budget). Drains
 *  are not part of this: a drain empties the pipeline but leaves the chain
 *  where it is, so a span outlives the draw that submitted it.
 *
 *  The first invariant is what decides where a batch may live. Two of them
 *  holding claimed spans at once is not a thing the chain can represent - the
 *  earlier one's commit would cut the later one's data away - so a batch is
 *  scoped to a pass that finishes before the next one starts. The destructor
 *  asserts it went out flushed.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/clip.h"
#include "ps2/renderer/frame_chain.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_view.h"
#include "ps2/renderer/vu1.h"

namespace ps2::batch {

// A gather buffer of at most 'MaxVerts' vertices, i.e. MaxVerts/3 triangles.
//
// The colour a gathered vertex draws with is the caller's business - a flat
// batch colour for the sky, the luxel tint or gouraud alpha for world surfaces,
// the shade LUT for alias models - so GatherTriangle takes it as a callable
// rather than a policy of its own. Everything else a vertex carries comes
// straight off the clip::ClipVertex the caller filled in.
template<int MaxVerts>
class TriangleBatch final
{
public:
    TriangleBatch() = default;

    // A batch that goes out of scope still holding a span has leaked its claim:
    // the chain's write cursor is still above the gathered vertices and nothing
    // will ever reference them. Debug only, but it is the check that makes these
    // safe as locals.
    ~TriangleBatch()
    {
        PS2_AssertMsg(m_verts == nullptr, "TriangleBatch destroyed without a Flush!");
    }

    // Non-copyable.
    TriangleBatch(const TriangleBatch &) = delete;
    TriangleBatch & operator=(const TriangleBatch &) = delete;

    // Capacity has to be a whole number of triangles, and has to hold at least
    // one worst-case clipped triangle: the polygon a cut against all six planes
    // leaves behind fans out to kMaxClippedVerts - 2 triangles, and GatherTriangle
    // flushes to make room for them but cannot split them across two batches.
    static_assert((MaxVerts % 3) == 0, "Batch capacity must be a whole number of triangles!");
    static_assert(MaxVerts >= (clip::kMaxClippedVerts - 2) * 3, "Batch capacity must hold one worst-case clipped triangle!");

    // What one flush cycle claims from the chain: the vertices, plus everything
    // the chunks that reference them will append afterwards. Both together,
    // because the chunk loop reserves as it goes and a reservation that overflowed
    // half way through would rewind the chain out from under the span.
    static constexpr int kClaimQwords = chain::CalcAllocCost<vu1::DrawVertex>(MaxVerts)
                                      + vu1::DrawTrianglesChainCost(MaxVerts);

    Q_ALWAYS_INLINE bool IsFull()  const { return m_vertCount == MaxVerts; }
    Q_ALWAYS_INLINE bool IsEmpty() const { return m_vertCount == 0; }

    // Sends the gathered triangles as one batch and empties the buffer. The
    // transform, texture and flags are the batch's; a caller that changes any
    // of them must flush with the *outgoing* ones first. Does nothing when the
    // buffer is empty, so flushing an already-flushed batch is free.
    void Flush(const math::Mat4 & mvp, const tex::Texture & texture,
               const vu1::DrawFlags flags = vu1::DrawFlags::None)
    {
        if (m_vertCount > 0)
        {
            // Cut the claim back to what was gathered *before* the draw appends the
            // chunks that reference it - they have to land after the data, and the
            // rest of the claim is what makes room for them.
            chain::Commit(m_verts, m_vertCount);

            ++view::GetDrawStats().drawBatches;
            vu1::DrawTriangles(mvp, texture, m_verts, m_vertCount, flags);

            m_vertCount = 0;
            m_verts     = nullptr;
        }
        PS2_AssertMsg(m_verts == nullptr, "A claimed span with no vertices in it!");
    }

    // Hands out the next slot for a path that fills a vertex itself - the ones
    // submitting geometry the VU can be trusted to judge whole, so it never
    // meets the clipper. The caller must check IsFull() (and flush) first;
    // capacity is a triangle multiple, so that only ever fires between triangles.
    Q_ALWAYS_INLINE vu1::DrawVertex & PushVertex()
    {
        PS2_AssertMsg(m_vertCount < MaxVerts, "TriangleBatch is full!");
        return Verts()[m_vertCount++];
    }

    // Hands out three consecutive slots for a path filling a whole triangle at
    // once, so the count moves once instead of three times. Same contract as
    // PushVertex - check IsFull() (and flush) first - and since capacity is a
    // triangle multiple, a buffer that is not full always has room for three.
    Q_ALWAYS_INLINE vu1::DrawVertex * PushTriangle()
    {
        PS2_AssertMsg((m_vertCount + 3) <= MaxVerts, "TriangleBatch is full!");
        vu1::DrawVertex * const tri = Verts() + m_vertCount;
        m_vertCount += 3;
        return tri;
    }

    // Clips one triangle against the volume the VU judges and appends the
    // survivors, flushing first if they cannot fit. The corners arrive with
    // their position, UVs and colour payload set; their clip distances are
    // computed by the clipper. 'vertexColor' packs one surviving vertex's final
    // GS colour: u32 (const clip::ClipVertex &).
    //
    // What was culled, cut and drawn goes into the frame's draw statistics here,
    // so callers count nothing of their own for the triangles they hand over.
    template<typename ColorFn>
    void GatherTriangle(clip::ClipVertex (&corners)[3], const math::Mat4 & mvp,
                        const tex::Texture & texture, const vu1::DrawFlags flags,
                        ColorFn && vertexColor)
    {
        const clip::ClipVertex * verts = nullptr;
        bool wasClipped = false;
        const int count = clip::ClipTriangle(corners, mvp, clip::SharedScratch(), &verts, &wasClipped);

        if (count == 0)
        {
            ++view::GetDrawStats().trisCulled;
            return;
        }

        if (wasClipped)
        {
            ++view::GetDrawStats().trisClipped;
        }

        // The survivors fan-triangulate.
        const int numTriangles = count - 2;
        if (m_vertCount + (numTriangles * 3) > MaxVerts)
        {
            Flush(mvp, texture, flags);
        }

        for (int v = 1; v < count - 1; ++v)
        {
            EmitVertex(verts[0],     vertexColor(verts[0]));
            EmitVertex(verts[v],     vertexColor(verts[v]));
            EmitVertex(verts[v + 1], vertexColor(verts[v + 1]));
        }

        view::GetDrawStats().trisDrawn += numTriangles;
    }

private:
    // The batch's vertices, claimed from the frame chain on the first push of a
    // flush cycle and given back at Flush. Reserving is separate from claiming on
    // purpose: chain::Reserve may rewind the chain, which is safe here and only
    // here, because nothing of this batch's is live yet.
    //
    // **Claiming the chain is the 2D->3D boundary, not the draw.** A pending 2D
    // batch holds an open DMA tag in the chain, and an allocation cannot land
    // inside one - its own tag has to be part of the tag stream, not of somebody
    // else's payload. vu1::Draw* closes the section too, but by then this has
    // already run, so the flush has to happen here, where the chain is taken.
    Q_ALWAYS_INLINE vu1::DrawVertex * Verts()
    {
        if (m_verts == nullptr) [[unlikely]]
        {
            gs::FlushPending2D();
            chain::Reserve(kClaimQwords);
            m_verts = chain::AllocMax<vu1::DrawVertex>(MaxVerts);
        }
        return m_verts;
    }

    Q_ALWAYS_INLINE void EmitVertex(const clip::ClipVertex & v, const u32 rgba)
    {
        PS2_AssertMsg(m_vertCount < MaxVerts, "TriangleBatch is full!");
        vu1::DrawVertex & dst = Verts()[m_vertCount++];
        dst.x    = v.pos.x;
        dst.y    = v.pos.y;
        dst.z    = v.pos.z;
        dst.w    = 1.0f;
        dst.rgba = rgba;
        dst.s    = v.st.x;
        dst.t    = v.st.y;
        dst.q    = 1.0f;
    }

    int               m_vertCount = 0;
    vu1::DrawVertex * m_verts     = nullptr; // into the frame chain; null between flush cycles
};

// Triangle batch specialized for the interpolated MD2 models.
//
// Same shape as TriangleBatch - a cursor into a span of the frame chain, claimed
// on the first push and handed back at Flush - gathering the keyframe position
// stream in vu1::LerpPosChunk groups, one per VU run.
//
// It gathers *only* positions. The other half of what the microprogram reads, the
// per-vertex attributes, is the model's own baked array in draw order, so the
// batch carries a cursor into it rather than a copy of it: SetAttribSource names
// the array, PushTriangle advances nothing of it, and Flush hands the draw the
// slice matching the positions it just submitted. That is what this class used to
// spend a whole second stream on.
//
// The span outlives its own Flush on purpose - that is what RedrawLastFlush
// draws from. It stays good until the chain is rewound, which is why the claim
// reserves two draws' worth of tags rather than one.
template<int MaxVerts>
class VULerpTriangleBatch final
{
public:
    VULerpTriangleBatch() = default;

    // As TriangleBatch: a batch that goes out of scope still holding a span left
    // the chain's cursor above gathered vertices nothing will ever reference.
    ~VULerpTriangleBatch()
    {
        PS2_AssertMsg(m_chunks == nullptr, "VULerpTriangleBatch destroyed without a Flush!");
    }

    // Non-copyable.
    VULerpTriangleBatch(const VULerpTriangleBatch &) = delete;
    VULerpTriangleBatch & operator=(const VULerpTriangleBatch &) = delete;

    static_assert((MaxVerts % 3) == 0, "Batch capacity must be a whole number of triangles!");

    // Groups the capacity needs. A group holds a whole number of triangles
    // (vu1.cpp asserts kMaxLerpVertsPerBatch is one), so a triangle never
    // straddles two of them and PushTriangle only ever has to notice that the
    // current group is full.
    static constexpr int kMaxChunks = vu1::ChunkCount(MaxVerts, vu1::kMaxLerpVertsPerBatch);

    // What one flush cycle claims from the chain: the groups, the tags of the
    // draw that sends them - and a second draw's worth of tags, because
    // RedrawLastFlush emits another set over the same data and must not be the
    // thing that overflows. An overflow between the two would rewind the chain
    // out from under the span the redraw exists to reference.
    static constexpr int kClaimQwords = chain::CalcAllocCost<vu1::LerpPosChunk>(kMaxChunks)
                                      + (2 * vu1::DrawLerpedTrianglesChainCost(MaxVerts));

    Q_ALWAYS_INLINE bool IsFull()  const { return m_vertCount == MaxVerts; }
    Q_ALWAYS_INLINE bool IsEmpty() const { return m_vertCount == 0; }

    // Names the per-vertex attribute array the gather about to start reads its
    // positions out of - the model's baked vertices, in draw order. Every push
    // from here on consumes one entry of it, and each Flush hands the draw the
    // run it just covered and steps past it, so a model too large for one batch
    // splits its attributes at exactly the same place as its positions.
    //
    // Call once before the first push of a model. It is the only thing the batch
    // needs to know about where the attributes live, because it never writes them.
    void SetAttribSource(const vu1::LerpDrawAttrib * const attribs)
    {
        PS2_AssertMsg(m_vertCount == 0, "SetAttribSource in the middle of a gather!");
        m_attribs = attribs;
    }

    // The VU-lerp equivalent of TriangleBatch::Flush, submitting the gathered
    // groups. Does nothing when the buffer is empty.
    void Flush(const math::Mat4 & mvp, const tex::Texture & texture,
               const math::Vec3 & frontv, const math::Vec3 & backv,
               const math::Vec4 & shadeLight,
               const vu1::FaceCull faceCull, const vu1::DrawFlags flags)
    {
        // Recorded even when there is nothing to send, so RedrawLastFlush after
        // an empty flush draws nothing rather than the previous caller's model.
        m_lastFlushed        = m_chunks;
        m_lastFlushedAttribs = m_attribs;
        m_lastFlushedCount   = m_vertCount;

        if (m_vertCount > 0)
        {
            PS2_AssertMsg(m_attribs != nullptr, "VULerpTriangleBatch::Flush with no attribute source!");

            // Whole groups: the tail of a partly filled last group is the only
            // thing a flush cycle wastes, and it is bounded by one group.
            chain::Commit(m_chunks, vu1::ChunkCount(m_vertCount, vu1::kMaxLerpVertsPerBatch));

            ++view::GetDrawStats().drawBatches;
            vu1::DrawLerpedTriangles(mvp, texture, frontv, backv, shadeLight,
                                     m_chunks, m_attribs, m_vertCount, faceCull, flags);

            // Past what this cycle submitted, so a model that needed more than one
            // batch carries on where it left off.
            m_attribs  += m_vertCount;
            m_vertCount = 0;
        }

        m_chunks     = nullptr;
        m_chunkVerts = vu1::kMaxLerpVertsPerBatch; // next push starts a group
    }

    // Draws the vertices of the most recent Flush again, under a different
    // transform, without rebuilding them.
    //
    // The groups the last Flush submitted are still sitting in the chain: a
    // Flush commits the span and moves on, and nothing rewinds the chain until
    // the frame ends. So the redraw is a second set of chunk tags over data that
    // is already there. The MD2 shadow is exactly this - the model's own
    // keyframe bytes under a squashed matrix, with an all-zero shadeLight that
    // multiplies every vertex's shade term out to black, so even the attribute
    // stream can be the model's own.
    //
    // Only valid while nothing has been pushed since that Flush, and only worth
    // anything if the geometry went out in a single batch - a caller that filled
    // the buffer mid-model left only its tail behind.
    void RedrawLastFlush(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight,
                         const vu1::FaceCull faceCull, const vu1::DrawFlags flags)
    {
        PS2_AssertMsg(m_vertCount == 0, "RedrawLastFlush after pushing new vertices!");

        if (m_lastFlushedCount > 0)
        {
            ++view::GetDrawStats().drawBatches;
            vu1::DrawLerpedTriangles(mvp, texture, frontv, backv, shadeLight,
                                     m_lastFlushed, m_lastFlushedAttribs, m_lastFlushedCount,
                                     faceCull, flags);
        }
    }

    // Three consecutive slots of each stream, for a caller filling a whole
    // triangle at once - the count then moves once instead of three times, and
    // IsFull() is answered once instead of three times. The caller must check
    // IsFull() (and flush) first, which is enough because capacity is a
    // triangle multiple.
    // Three consecutive position slots, for a caller filling a whole triangle at
    // once - the count then moves once instead of three times, and IsFull() is
    // answered once instead of three times. The caller must check IsFull() (and
    // flush) first, which is enough because capacity is a triangle multiple.
    //
    // Only positions come back. There is nothing to hand out for the attributes:
    // the caller named them once with SetAttribSource and the triangle it is
    // filling reads from them at the same index, which is the whole point.
    Q_ALWAYS_INLINE vu1::LerpVertexBytes * PushTriangle()
    {
        PS2_AssertMsg((m_vertCount + 3) <= MaxVerts, "VULerpTriangleBatch is full!");

        // One test covers both the first push of a cycle and a group boundary:
        // m_chunkVerts starts out saying the (non-existent) current group is
        // full, so the claim and the advance are the same branch.
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

private:
    // Claims the span on the first push of a flush cycle, and steps to the next
    // group after that. Reserving is separate from claiming on purpose:
    // chain::Reserve may rewind the chain, which is safe here and only here,
    // because nothing of this batch's is live yet. The 2D flush is the 2D->3D
    // boundary - see TriangleBatch::Verts for why it belongs here.
    void NextChunk()
    {
        if (m_chunks == nullptr) [[unlikely]]
        {
            gs::FlushPending2D();
            chain::Reserve(kClaimQwords);
            m_chunks = chain::AllocMax<vu1::LerpPosChunk>(kMaxChunks);
            m_chunk  = m_chunks;
        }
        else
        {
            ++m_chunk;
        }
        m_pos        = m_chunk->pos;
        m_chunkVerts = 0;
    }

    int m_vertCount = 0;

    // The model's baked attributes, at the vertex the next push will fill.
    const vu1::LerpDrawAttrib * m_attribs = nullptr;

    // Vertices the last Flush submitted, and where both of its streams are; see
    // RedrawLastFlush.
    int                         m_lastFlushedCount   = 0;
    vu1::LerpPosChunk *         m_lastFlushed        = nullptr;
    const vu1::LerpDrawAttrib * m_lastFlushedAttribs = nullptr;

    // The claim, the group being filled and how much of it is spoken for.
    vu1::LerpPosChunk * m_chunks = nullptr; // into the frame chain; null between flush cycles
    vu1::LerpPosChunk * m_chunk  = nullptr;
    int m_chunkVerts = vu1::kMaxLerpVertsPerBatch;

    // Cursors rather than an index off m_chunk: the gather loop's stores are ones
    // the compiler cannot prove disjoint from anything under -fno-strict-aliasing,
    // so a base plus an index it has to redo per push costs more than two pointers
    // it can bump.
    vu1::LerpVertexBytes * m_pos = nullptr;
};

} // namespace ps2::batch
