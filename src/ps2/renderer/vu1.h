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
#include "ps2/renderer/vif_packet.h"

namespace ps2::tex { struct Texture; }

namespace ps2::vu1 {

// Declares the linker symbols bracketing an assembled VU microprogram in the
// ELF's .vudata section. 'progName' must match the #vuprog name in the .vcl.
// The instruction count is in 64-bit VU instructions - the unit MPG upload
// destinations and MSCAL entry points use - NOT qwords (half as many).
#define PS2_DECLARE_VU_MICROPROGRAM(progName) \
    extern "C" u32 progName##_CodeStart __attribute__((section(".vudata"))); \
    extern "C" u32 progName##_CodeEnd   __attribute__((section(".vudata"))); \
    inline u32 progName##_InstructionCount() { return u32((&progName##_CodeEnd - &progName##_CodeStart) / 2); } \
    inline ps2::vu1::VUCode progName##_Code() { return { &progName##_CodeStart, &progName##_CodeEnd }; }

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
static_assert(sizeof(DrawVertex) == 32, "DrawVertex must be exactly 2 qwords");

// Packs 0-255 channels into DrawVertex::rgba, the GS native RGBAQ byte order
// (r in the low byte). Alpha 0x80 = 1.0 on the GS.
constexpr u32 PackColorRGBA(u32 r, u32 g, u32 b, u32 a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

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
enum class DrawFlags : u32
{
    None         = 0,
    Blended      = 1 << 0,
    Untextured   = 1 << 1,
    Additive     = 1 << 2,
    Modulate     = 1 << 3,
    DepthHack    = 1 << 4,
    NoDepthWrite = 1 << 5,
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

// Brings up the VIF1 DMA channel, uploads the microprograms to VU1 micro memory
// and programs the double-buffer registers. Call once, after gs::Init().
void Init();

// Draws a batch of triangles (3 verts each, triangle list) through VU1 with
// the given transform and texture (uploaded to GS VRAM on demand). Any whole-
// triangle count works: draws beyond kMaxVertsPerBatch are split into chunks
// submitted back to back in the same DMA chain, overlapping each chunk's
// upload with the previous one's transform. Synchronous for now: returns once
// the GS has consumed the batch, so the vertex data only needs to stay valid
// for the duration of the call. Call between gs::Begin/EndFrame.
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
// position. One qword, matching the microprogram's input layout; the
// same packed-color placement rules as DrawVertex apply.
struct alignas(16) LerpDrawAttrib
{
    u32 rgba;      // packed color, use PackColorRGBA()
    float s, t, q; // texture coords; q must be 1.0f
};
static_assert(sizeof(LerpDrawAttrib) == 16, "LerpDrawAttrib must be exactly 1 qword");

// Draws textured triangles whose positions VU1 interpolates from the two
// keyframe streams: position = cur * frontv + old * backv, plus the MVP's
// row 3 - fold the MD2 lerp's uniform 'move' translation in there (see
// render_md2.cpp). Both arrays must be 16-byte aligned, and 'positions'
// needs one readable element past vertCount when the count is odd: the byte
// stream is DMA'd in whole qwords and the pad element fills the last one
// (transferred, never read). Chunking, texture residency and synchronicity
// as DrawTriangles.
void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const LerpVertexBytes * positions, const LerpDrawAttrib * attribs,
                         int vertCount, FaceCull faceCull = FaceCull::None, DrawFlags flags = DrawFlags::None);

// ------------------------------------------------------------------------------------------------
// Per-frame submission size, for sizing the frame arena
// ------------------------------------------------------------------------------------------------

// Rolls the per-frame byte counter into the high-water mark and clears it.
// Called from gs::BeginFrame().
void BeginFrame();

// Most bytes any single frame has ever handed to the DMA as REF'd payload:
// the vertex streams of every batch plus one FrameConstants block per draw
// call. Both draw paths funnel through here, so unlike DrawStats::trisDrawn
// (which the MD2 PushVertex paths bypass) this counts everything.
//
// This is exactly the per-frame demand a frame arena would have to satisfy,
// measured before building one. Reported as "ArenaHi" by ps2_show_drawstats.
int PeakFrameSubmittedBytes();

} // namespace ps2::vu1
