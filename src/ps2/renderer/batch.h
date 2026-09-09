#pragma once
/* ================================================================================================
 * File: batch.h
 * Brief: The triangle gather buffer every 3D path fills, then hands to VU1 as one batch.
 *
 *  vu1::DrawTriangles wants whole triangle lists, and a draw call costs a DMA
 *  chain of its own, so nothing submits a triangle at a time: each path gathers
 *  into a scratch buffer and flushes it when the texture changes, when the batch
 *  state changes or when it fills up. Since DrawTriangles is synchronous - it
 *  returns once the GS has consumed the data - one buffer can serve every batch
 *  in turn, referenced in place by the chain rather than copied.
 *
 *  Gathering goes through the EE clipper (clip.h): the microprogram rejects a
 *  triangle that straddles its clip volume whole rather than cutting it, so the
 *  survivors of a cut arrive here as a convex polygon and fan-triangulate into
 *  the buffer. That, the capacity check, the flush and the draw statistics are
 *  the same work for the world, the sky and alias models; only the per-vertex
 *  colour differs between them, which is what the 'vertexColor' callable is for.
 *
 *  A batch owns its storage, so each module keeps its own instance sized to what
 *  it actually gathers, and the clipper's ping-pong buffers ride along inside it.
 *  Instances are file-level statics for that reason - the arrays are far too
 *  large for the stack, and draws are synchronous, so a single one per caller
 *  serves every triangle in turn.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/clip.h"
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
class alignas(16) TriangleBatch final
{
public:
    TriangleBatch() = default;

    // Non-copyable.
    TriangleBatch(const TriangleBatch &) = delete;
    TriangleBatch & operator=(const TriangleBatch &) = delete;

    // Capacity has to be a whole number of triangles, and has to hold at least
    // one worst-case clipped triangle: the polygon a cut against all six planes
    // leaves behind fans out to kMaxClippedVerts - 2 triangles, and GatherTriangle
    // flushes to make room for them but cannot split them across two batches.
    static_assert((MaxVerts % 3) == 0, "Batch capacity must be a whole number of triangles!");
    static_assert(MaxVerts >= (clip::kMaxClippedVerts - 2) * 3, "Batch capacity must hold one worst-case clipped triangle!");

    bool IsFull()  const { return m_vertCount == MaxVerts; }
    bool IsEmpty() const { return m_vertCount == 0; }

    // Sends the gathered triangles as one batch and empties the buffer. The
    // transform, texture and flags are the batch's; a caller that changes any
    // of them must flush with the *outgoing* ones first. Does nothing when the
    // buffer is empty, so flushing an already-flushed batch is free.
    void Flush(const math::Mat4 & mvp, const tex::Texture & texture,
               const vu1::DrawFlags flags = vu1::DrawFlags::None)
    {
        if (m_vertCount > 0)
        {
            ++view::GetDrawStats().drawBatches;
            vu1::DrawTriangles(mvp, texture, m_verts, m_vertCount, flags);
            m_vertCount = 0;
        }
    }

    // Hands out the next slot for a path that fills a vertex itself - the ones
    // submitting geometry the VU can be trusted to judge whole, so it never
    // meets the clipper. The caller must check IsFull() (and flush) first;
    // capacity is a triangle multiple, so that only ever fires between triangles.
    vu1::DrawVertex & PushVertex()
    {
        PS2_AssertMsg(m_vertCount < MaxVerts, "TriangleBatch is full!");
        return m_verts[m_vertCount++];
    }

    // Hands out three consecutive slots for a path filling a whole triangle at
    // once, so the count moves once instead of three times. Same contract as
    // PushVertex - check IsFull() (and flush) first - and since capacity is a
    // triangle multiple, a buffer that is not full always has room for three.
    vu1::DrawVertex * PushTriangle()
    {
        PS2_AssertMsg((m_vertCount + 3) <= MaxVerts, "TriangleBatch is full!");
        vu1::DrawVertex * const tri = &m_verts[m_vertCount];
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
        const int count = clip::ClipTriangle(corners, mvp, m_clipScratch, &verts, &wasClipped);

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
    void EmitVertex(const clip::ClipVertex & v, const u32 rgba)
    {
        PS2_AssertMsg(m_vertCount < MaxVerts, "TriangleBatch is full!");
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

    int             m_vertCount = 0;
    vu1::DrawVertex m_verts[static_cast<size_t>(MaxVerts)];
    clip::Scratch   m_clipScratch;
};

// Triangle batch specialized for the interpolated MD2 models.
template<int MaxVerts>
class alignas(16) VULerpTriangleBatch final
{
public:
    VULerpTriangleBatch() = default;

    // Non-copyable.
    VULerpTriangleBatch(const VULerpTriangleBatch &) = delete;
    VULerpTriangleBatch & operator=(const VULerpTriangleBatch &) = delete;

    static_assert((MaxVerts % 3) == 0, "Batch capacity must be a whole number of triangles!");

    bool IsFull()  const { return m_vertCount == MaxVerts; }
    bool IsEmpty() const { return m_vertCount == 0; }

    // The VU-lerp equivalent of TriangleBatch::Flush, submitting the two SoA streams.
    //
    // 'attribsOverride' replaces m_attribs, and is taken as a repeating block
    // rather than a per-vertex stream: every caller that overrides does so
    // because its attributes are the same for every vertex, so it only has to
    // supply vu1::kMaxLerpVertsPerBatch of them however long the batch is.
    void Flush(const math::Mat4 & mvp, const tex::Texture & texture,
               const math::Vec3 & frontv, const math::Vec3 & backv,
               const vu1::FaceCull faceCull, const vu1::DrawFlags flags,
               const vu1::LerpDrawAttrib * attribsOverride = nullptr)
    {
        // Recorded even when there is nothing to send, so RedrawLastFlush after
        // an empty flush draws nothing rather than the previous caller's model.
        m_lastFlushedCount = m_vertCount;

        if (m_vertCount > 0)
        {
            ++view::GetDrawStats().drawBatches;
            vu1::DrawLerpedTriangles(mvp, texture, frontv, backv,
                                     m_vertBytes, (attribsOverride != nullptr) ? attribsOverride : m_attribs,
                                     m_vertCount, faceCull, flags, (attribsOverride != nullptr));
            m_vertCount = 0;
        }
    }

    // Draws the vertices of the most recent Flush again, under a different
    // transform and attribute stream, without rebuilding them.
    //
    // Flush leaves both streams where they are and only resets the count, and
    // DrawLerpedTriangles is synchronous - it returns once the GS has consumed
    // the batch - so what the last submission referenced is still sitting there
    // intact. The MD2 shadow is exactly this: the model's own keyframe bytes
    // under a squashed matrix and a flat attribute stream, which the caller would
    // otherwise walk the whole glcmds list a second time to rebuild identically.
    //
    // Only valid while nothing has been pushed since that Flush, and only worth
    // anything if the geometry went out in a single batch - a caller that filled
    // the buffer mid-model left only its tail behind.
    //
    // NOTE: this is one of the places that rests on draws being synchronous. If
    // submission ever goes asynchronous, the stream has to stay owned until the
    // frame's fence, like every other buffer the DMA references in place.
    void RedrawLastFlush(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const vu1::FaceCull faceCull, const vu1::DrawFlags flags,
                         const vu1::LerpDrawAttrib * attribsOverride = nullptr)
    {
        PS2_AssertMsg(m_vertCount == 0, "RedrawLastFlush after pushing new vertices!");

        if (m_lastFlushedCount > 0)
        {
            ++view::GetDrawStats().drawBatches;
            vu1::DrawLerpedTriangles(mvp, texture, frontv, backv,
                                     m_vertBytes, (attribsOverride != nullptr) ? attribsOverride : m_attribs,
                                     m_lastFlushedCount, faceCull, flags, (attribsOverride != nullptr));
        }
    }

    struct Vert
    {
        vu1::LerpVertexBytes & pos;
        vu1::LerpDrawAttrib  & attrib;
    };

    Vert PushVertex()
    {
        PS2_AssertMsg(m_vertCount < MaxVerts, "VULerpTriangleBatch is full!");
        const Vert v = { m_vertBytes[m_vertCount], m_attribs[m_vertCount] };
        ++m_vertCount;
        return v;
    }

    // Three consecutive slots of each stream, for a caller filling a whole
    // triangle at once - the count then moves once instead of three times, and
    // IsFull() is answered once instead of three times. Same contract as
    // PushVertex: check IsFull() (and flush) first, which is enough because
    // capacity is a triangle multiple.
    struct Tri
    {
        vu1::LerpVertexBytes * pos;    // [3]
        vu1::LerpDrawAttrib  * attrib; // [3]
    };

    Tri PushTriangle()
    {
        PS2_AssertMsg((m_vertCount + 3) <= MaxVerts, "VULerpTriangleBatch is full!");
        const Tri t = { &m_vertBytes[m_vertCount], &m_attribs[m_vertCount] };
        m_vertCount += 3;
        return t;
    }

private:
    int m_vertCount = 0;

    // Vertices the last Flush submitted; see RedrawLastFlush.
    int m_lastFlushedCount = 0;

    // The +1 on the positions is the DrawLerpedTriangles pad element for odd flush counts (transferred, never read).
    alignas(16) vu1::LerpVertexBytes m_vertBytes[static_cast<size_t>(MaxVerts + 1)];
    alignas(16) vu1::LerpDrawAttrib  m_attribs[static_cast<size_t>(MaxVerts)];
};

} // namespace ps2::batch
