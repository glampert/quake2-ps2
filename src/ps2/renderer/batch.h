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
    // if attribsOverride != null, overrides m_attribs.
    void Flush(const math::Mat4 & mvp, const tex::Texture & texture,
               const math::Vec3 & frontv, const math::Vec3 & backv,
               const vu1::FaceCull faceCull, const vu1::DrawFlags flags,
               const vu1::LerpDrawAttrib * attribsOverride = nullptr)
    {
        if (m_vertCount > 0)
        {
            ++view::GetDrawStats().drawBatches;
            vu1::DrawLerpedTriangles(mvp, texture, frontv, backv,
                                     m_vertBytes, (attribsOverride != nullptr) ? attribsOverride : m_attribs,
                                     m_vertCount, faceCull, flags);
            m_vertCount = 0;
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

private:
    int m_vertCount = 0;

    // The +1 on the positions is the DrawLerpedTriangles pad element for odd flush counts (transferred, never read).
    alignas(16) vu1::LerpVertexBytes m_vertBytes[static_cast<size_t>(MaxVerts + 1)];
    alignas(16) vu1::LerpDrawAttrib  m_attribs[static_cast<size_t>(MaxVerts)];
};

} // namespace ps2::batch
