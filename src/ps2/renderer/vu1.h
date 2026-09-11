#pragma once
/* ================================================================================================
 * File: vu1.h
 * Brief: VU1-accelerated 3D drawing: microprogram upload and triangle batch submission.
 *        The microprogram transforms vertices by the caller's MVP matrix, performs
 *        guard-band clipping (whole triangles) and XGKICKs the result to the GS.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/math/vec_mat.h"
#include "ps2/renderer/frame_chain.h"
#include "ps2/renderer/vif_packet.h"

namespace ps2::tex { struct Texture; }

namespace ps2::vu1 {

// ------------------------------------------------------------------------------------------------
// Utilities
// ------------------------------------------------------------------------------------------------

// Declares the linker symbols bracketing an assembled VU microprogram in the
// ELF's .vudata section. 'progName' must match the #vuprog name in the .vcl.
// The instruction count is in 64-bit VU instructions - the unit MPG upload
// destinations and MSCAL entry points use - NOT qwords (half as many).
#define PS2_DECLARE_VU_MICROPROGRAM(progName) \
    extern "C" u32 progName##_CodeStart __attribute__((section(".vudata"))); \
    extern "C" u32 progName##_CodeEnd   __attribute__((section(".vudata"))); \
    inline u32 progName##_InstructionCount()  { return u32((&progName##_CodeEnd - &progName##_CodeStart) / 2); } \
    inline ps2::vu1::VUCode progName##_Code() { return { &progName##_CodeStart, &progName##_CodeEnd }; }

// The NDC guard band the microprogram accepts: a triangle with any vertex at
// |x/w| or |y/w| beyond this (or outside the exact [-1, +1] z range) is
// rejected whole, not clipped - bounded by the reach of the GS 12.4 window
// coordinates, so it cannot simply be raised. Callers submitting geometry
// that can cross these planes (the world renderer) must pre-clip against
// them on the EE; the visible screen only spans |ndc| = screenW/4096, so the
// band still leaves several screens of margin fully to the VU.
constexpr float kGuardBandNdcLimit = 0.8f;

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

// Packs 0-255 channels into DrawVertex::rgba, the GS native RGBAQ byte order
// (r in the low byte). Alpha 0x80 = 1.0 on the GS.
constexpr u32 PackColorRGBA(u32 r, u32 g, u32 b, u32 a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// ------------------------------------------------------------------------------------------------
// Chain budget
// ------------------------------------------------------------------------------------------------

// What a draw costs the frame chain besides its vertex data, so a caller whose vertex data is
// *itself* in the chain can reserve the pair together.
//
// It has to reserve the pair. The chunk loop reserves as it goes, and chain::Reserve rewinds when
// it comes up short - which would pull the chain out from under the very span the chunks being
// emitted reference. Reserving the whole draw up front means that reservation can never fire half
// way through one.

// Vertices one VU1 run carries. Draws longer than this are split into chunks of this size,
// submitted back to back in the same chain.
constexpr int kMaxVertsPerBatch = 96;

// Chain qwords one chunk appends: the header/GIF-tag inline unpack (1 tag qword plus 8 of
// payload), the vertex REF unpack (1 - its VIFcodes ride in the tag's upper half) and the
// FLUSH + MSCAL (1). 11 in practice, declared with room to spare; over-declaring only reserves
// slightly more of the chain than a chunk needs.
constexpr int kChunkChainQwords = 16;

// Chain qwords a draw's opening costs: the transform block and the dynamic-light block, both
// built in the chain rather than referenced out of a static. 8 and 12 qwords of payload, each
// fronted by the skip tag chain::Alloc needs and followed by the REF tag that sends it.
constexpr int kDrawSetupQwords = (8 + 2) + (12 + 2);

// Vertices one lerped VU run carries, and what one of its chunks costs: the 3-qword-per-vertex
// batch (2 position qwords + 1 attribute) fits fewer than the world path's 96. Whole triangles,
// and even - so every full chunk's slice of the 8-byte position stream is whole source qwords
// starting 16-byte aligned. The chunk is the header/frontv/backv/shadeLight/tags inline unpack
// (1 tag + 11 payload), two REF unpacks and the FLUSH + MSCAL: 15 in practice.
constexpr int kMaxLerpVertsPerBatch  = 78;
constexpr int kLerpChunkChainQwords  = 22;

// Vertices one particle VU run carries, and what one of its chunks costs: the same shape, with
// an 11-qword header/constants/tag payload instead of 8.
constexpr int kMaxParticlesPerBatch = 78;
constexpr int kParticleChunkQwords  = 22;

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
// only what is written would let that final ask rewind the chain mid-draw.
//
// Plus the terminator, because every draw ends in a kick and the kick writes its FLUSH + END
// into the chain at the cursor. Small, but it belongs to the draw that caused it - left out, it
// would silently come out of whatever the next caller reserved.
constexpr int DrawTrianglesChainCost(const int vertCount)
{
    return chain::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(vertCount, kMaxVertsPerBatch) * kChunkChainQwords);
}

constexpr int DrawLerpedTrianglesChainCost(const int vertCount)
{
    return chain::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(vertCount, kMaxLerpVertsPerBatch) * kLerpChunkChainQwords);
}

constexpr int DrawParticlesChainCost(const int count)
{
    return chain::kTerminatorQwords + (2 * kDrawSetupQwords)
         + (ChunkCount(count, kMaxParticlesPerBatch) * kParticleChunkQwords);
}

// ------------------------------------------------------------------------------------------------
// Generic VU1 triangles (static world geometry)
// ------------------------------------------------------------------------------------------------

// One triangle vertex, 2 qwords, matching the microprogram's input layout.
// The packed color must sit in the first word of its qword: the microprogram
// raw-copies it into a GS A+D RGBAQ qword with a single masked store, and
// only word 0 is reachable that way (swizzling the raw bits through the FMAC
// instead would flush denormal color patterns to zero).
struct alignas(16) DrawVertex
{
    float x, y, z, w; // model-space position; w must be 1.0f
    u32   rgba;       // packed color, use PackColorRGBA()
    float s, t, q;    // texture coords; q must be 1.0f
};
static_assert(sizeof(DrawVertex)  == 32, "DrawVertex must be exactly 2 qwords");
static_assert(alignof(DrawVertex) == 16, "CopyDrawVertex's lq/sq require qword alignment");

// Copies one gathered vertex whole, in two of the R5900's 128-bit moves.
//
// A plain struct assignment would be correct, but gcc lowers it to four ld/sd
// pairs - it never forms lq/sq of its own accord, and neither __int128 nor a
// 16-byte-aligned aggregate persuades it to. That doubles the memory ops in the
// innermost step of the world gather, which is hot enough to care.
//
// The integer lq/sq rather than the VU0 lqc2/sqc2 the math helpers use: this
// moves a packed colour whose bit pattern is a float denormal (see the note
// above), and the integer path provably never reaches an FMAC to flush it.
//
// Constrained rather than clobbering "memory", so a caller copying several
// vertices in a row keeps its own state in registers across them.
Q_ALWAYS_INLINE void CopyDrawVertex(DrawVertex & dst, const DrawVertex & src)
{
    asm volatile (
        "lq $8, 0x00(%1) \n\t"
        "lq $9, 0x10(%1) \n\t"
        "sq $8, 0x00(%2) \n\t"
        "sq $9, 0x10(%2) \n\t"
        : "=m" (dst)
        : "r" (&src), "r" (&dst), "m" (src)
        : "$8", "$9");
}

// Draws a batch of triangles (3 verts each, triangle list) through VU1 with
// the given transform and texture (uploaded to GS VRAM on demand). Any whole-
// triangle count works: draws beyond kMaxVertsPerBatch are split into chunks
// submitted back to back in the same DMA chain, overlapping each chunk's
// upload with the previous one's transform. Synchronous for now: returns once
// the GS has consumed the batch, so the vertex data only needs to stay valid
// for the duration of the call. Call between gs::Begin/EndFrame.
//
// 'verts' is normally a span of the frame chain itself (chain::Alloc), which is
// how the gather buffers stopped being statics. Such a caller must have reserved
// DrawTrianglesChainCost(vertCount) on top of the span - see the chain budget
// above - so that nothing here can rewind the chain out from under it. A drain
// is harmless; a rewind would leave the REF tags below pointing at memory the
// next gather is about to write.
void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const DrawVertex * verts, int vertCount,
                   DrawFlags flags = DrawFlags::None);

// ------------------------------------------------------------------------------------------------
// Keyframe-lerped triangles (MD2 alias models)
// ------------------------------------------------------------------------------------------------

// The two keyframes' quantized positions of one vertex, interleaved: the
// current frame's dtrivertx_t bytes, then the old frame's, both copied
// verbatim from the MD2 frame data (the VIF widens each byte into an integer
// lane; the microprogram converts and lerps them). The 4th byte of each word
// is that frame's lightnormalindex, which rides along unread - the EE indexes
// its color LUT with it instead.
struct LerpVertexBytes
{
    u32 cur;
    u32 old;
};

// Per-vertex attributes for DrawLerpedTriangles - everything but the
// position. One qword, matching the microprogram's input layout.
//
// The color is not packed here: 'shade' is the vertex's shade term and the
// microprogram multiplies the batch's shadeLight by it, clamps and converts,
// which is the whole of what the EE used to do by building a 162-entry lookup
// table per entity per frame. It also means this qword holds no packed color,
// so the denormal rule that governs DrawVertex does not apply - every lane
// here is a real float meant for the FMAC.
struct alignas(16) LerpDrawAttrib
{
    float shade;   // per-vertex shade term, multiplied by the batch's shadeLight
    float s, t, q; // texture coords; q must be 1.0f
};
static_assert(sizeof(LerpDrawAttrib) == 16, "LerpDrawAttrib must be exactly 1 qword");

// The one-qword sibling of CopyDrawVertex, for a caller whose source is already
// laid out as a LerpDrawAttrib - mod::AliasVertex deliberately is, keeping the
// keyframe index where the shade goes so a model's baked attributes reach the
// batch in one move and the shade is written over lane 0 afterwards.
//
// The reason is gcc, not the FMAC: it never forms lq/sq of its own accord, so a
// plain struct assignment becomes four ld/sd pairs in the innermost step of the
// entity gather. Templated on the source only to avoid a dependency on the model
// headers here; the layout is asserted rather than assumed.
template<typename SrcT>
Q_ALWAYS_INLINE void CopyLerpAttrib(LerpDrawAttrib & dst, const SrcT & src)
{
    static_assert(sizeof(SrcT) == sizeof(LerpDrawAttrib) && alignof(SrcT) == 16,
                  "CopyLerpAttrib's lq/sq need one qword-aligned qword");

    asm volatile (
        "lq $8, 0x00(%1) \n\t"
        "sq $8, 0x00(%2) \n\t"
        : "=m" (dst)
        : "r" (&src), "r" (&dst), "m" (src)
        : "$8");
}

// One VU run's input, both streams together: the geometry is handed over a chunk
// at a time rather than as two long parallel arrays.
//
// The layout is what the chain forced and what the chain wanted anyway. A gather
// writing into the frame chain claims one block and gives back the tail it did
// not use (chain::AllocMax / Commit), and a block is cut back from its end - so
// two streams that both have to shrink cannot be two allocations. Grouped per
// chunk they are one, the two REF tags of a chunk point at neighbouring qwords
// instead of half a batch apart, and a short final chunk wastes at most one
// group's tail instead of the whole of both streams' slack.
//
// 'pos' is sized for the maximum chunk, which is also what supplies the pad the
// byte stream needs at an odd vertex count: the DMA carries whole source qwords,
// so an odd chunk transfers one element past its count (transferred, never read).
// An odd chunk is always shorter than the maximum, so that element is in here.
struct alignas(16) LerpChunk
{
    LerpVertexBytes pos[kMaxLerpVertsPerBatch];    // 2 keyframe words per vertex
    LerpDrawAttrib  attrib[kMaxLerpVertsPerBatch]; // 1 qword per vertex
};
static_assert((sizeof(LerpChunk) % 16) == 0, "LerpChunk must be a whole number of qwords");
static_assert((sizeof(LerpVertexBytes) * kMaxLerpVertsPerBatch % 16) == 0,
              "The attribute stream must start qword aligned - the unpack REFs it directly");

// Draws textured triangles whose positions VU1 interpolates from the two
// keyframe streams: position = cur * frontv + old * backv, plus the MVP's
// row 3 - fold the MD2 lerp's uniform 'move' translation in there (see
// render_md2.cpp). 'chunks' must be 16-byte aligned and hold
// ChunkCount(vertCount, kMaxLerpVertsPerBatch) groups, filled front to back -
// every group but the last one full. Chunking, texture residency and
// synchronicity as DrawTriangles.
//
// 'shadeLight' is the batch's light color in GS units (0-128 per channel, the
// entity's shade times the modulate identity) with the vertex alpha in .w. The
// microprogram builds each vertex's color as clamp(shade * shadeLight), so an
// all-zero .xyz gives flat black at whatever alpha .w carries - which is how the
// projected shadow draws over the model's own attribute stream, untouched.
void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const math::Vec4 & shadeLight,
                         const LerpChunk * chunks, int vertCount,
                         FaceCull faceCull = FaceCull::None,
                         DrawFlags flags = DrawFlags::None);

// ------------------------------------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------------------------------------

// One particle billboard, 1 qword. The packed color must sit in the first word
// for the same reason DrawVertex's does: the microprogram raw-copies it into an
// A+D RGBAQ qword, and only word 0 is reachable that way. The position takes the
// remaining three words and the microprogram reads it from .yzw.
struct alignas(16) ParticleVertex
{
    u32   rgba;    // packed color, use PackColorRGBA()
    float x, y, z; // world-space position of the billboard's anchor corner
};
static_assert(sizeof(ParticleVertex) == 16, "ParticleVertex must be exactly 1 qword");

// Draws camera-facing particle billboards as GS sprites, expanded entirely on
// VU1 - the caller transforms nothing.
//
// 'quadOffset' is the world-space vector from a particle's anchor corner to its
// opposite corner: the camera's (up + right), pre-scaled by whatever blow-up the
// caller wants (ref_gl uses 1.5). It must be orthogonal to the view axis, which
// is what makes every corner share the centre's depth and lets the billboard
// draw as a single axis-aligned sprite; DrawParticles transforms it once per
// call as a direction.
//
// The billboard also grows with distance the way ref_gl's particles do. The
// texture is sampled corner to corner through UV (no perspective correction,
// which a screen-aligned sprite does not need).
//
// Chunking, texture residency and synchronicity as DrawTriangles.
void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const ParticleVertex * particles,
                   int count, DrawFlags flags = DrawFlags::Blended);

// ------------------------------------------------------------------------------------------------
// Dynamic point lights
// ------------------------------------------------------------------------------------------------

// How many lights the microprogram evaluates at once. Four is not arbitrary: the
// VU's SIMD lanes are four wide, so packing one light per lane costs the same as
// packing one, and the FMAC's four-cycle latency is covered exactly.
constexpr int kMaxDynamicLights = 4;

// One point light, in world space. Attenuation is distance-only (no N.L term)
// and reaches zero at 'radius' - Quake 2's dlight intensity is exactly that
// radius in world units.
struct DynamicLight
{
    math::Vec3 origin;
    math::Vec3 color;  // 0..1
    float      radius; // world units; attenuation is zero beyond it.
};

// Sets the frame's lights, shared by every batch drawn with
// DrawFlags::DynamicLights until the next call. Fewer than kMaxDynamicLights is
// fine - unused slots are zeroed and contribute nothing. Pass count 0 to turn
// the lighting off without clearing the flag.
//
// Colours are pre-scaled here into the GS 0-255 range and pre-divided by the
// radius squared, which is what lets the microprogram attenuate with a single
// multiply-add and no divide or square root.
void SetDynamicLights(const DynamicLight * lights, int count);

// ------------------------------------------------------------------------------------------------
// VU1 initialization
// ------------------------------------------------------------------------------------------------

// Brings up the VIF1 DMA channel, uploads the microprograms to VU1 micro memory
// and programs the double-buffer registers. Call once, after gs::Init().
void Init();

} // namespace ps2::vu1
