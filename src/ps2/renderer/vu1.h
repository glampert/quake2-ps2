#pragma once
/* ================================================================================================
 * File: vu1.h
 * Brief: The VU1 microprograms: their upload and entry points, the VU1 data memory layout they
 *        read, and the vertex formats and constants they consume. The draw paths that feed them
 *        belong to ps2::rs.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"

#include <tamtypes.h>
#include <gif_tags.h>

namespace ps2::vu1 {

// ------------------------------------------------------------------------------------------------
// Microprograms
// ------------------------------------------------------------------------------------------------

// An assembled microprogram's extent in the ELF's .vudata section.
struct VUCode
{
    u32 * start;
    u32 * end;
};

// Declares the linker symbols bracketing an assembled VU microprogram in the
// ELF's .vudata section. 'progName' must match the #vuprog name in the .vcl.
// The instruction count is in 64-bit VU instructions - the unit MPG upload
// destinations and MSCAL entry points use - NOT qwords (half as many).
#define PS2_DECLARE_VU_MICROPROGRAM(progName) \
    extern "C" u32 progName##_CodeStart __attribute__((section(".vudata"))); \
    extern "C" u32 progName##_CodeEnd   __attribute__((section(".vudata"))); \
    inline u32 progName##_InstructionCount()  { return u32((&progName##_CodeEnd - &progName##_CodeStart) / 2); } \
    inline ps2::vu1::VUCode progName##_Code() { return { &progName##_CodeStart, &progName##_CodeEnd }; }

// An entry point in VU1 micro memory, in 64-bit instruction units - what MPG destinations and
// MSCAL take, not qwords.
enum struct ProgramAddr : u32 {};

enum class Program
{
    Textured,  // world geometry: transform, guard-band clip, textured gouraud triangles
    Lerped,    // MD2 alias models: two keyframes lerped on the VU ahead of the transform
    Particles, // camera-facing billboards expanded to GS sprites
    Lit,       // world geometry, with the vertex colour computed from the frame's dynamic lights

    Count      // Number of VU1 programs - not valid for ProgramAddress.
};

// Where 'prog' was uploaded. Init must have run.
ProgramAddr ProgramAddress(Program prog);

// Uploads the microprograms to VU1 micro memory and programs the double-buffer registers.
// Call once, after gs::Init() and cmdbuf::Init() - the upload rides the command buffer.
void Init();

// ------------------------------------------------------------------------------------------------
// VU1 data memory layout (1024 qwords; addresses in qwords)
//
//      0-7       the frame constants below
//      8-999     the two XTOP double buffers (VIF1 BASE=8, OFFSET=496)
//      1000-1011 the dynamic light block
// ------------------------------------------------------------------------------------------------

// Frame constants at fixed low VU addresses (below kDoubleBufferBase).
constexpr int kFrameConstantsAddr = 0;

// VIF1 double-buffer registers: two 496-qword buffers above the constants.
constexpr int kDoubleBufferBase   = 8;
constexpr int kDoubleBufferOffset = 496;

constexpr int kGifTagsAddr     = 1; // 7 qwords: GIF set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag
constexpr int kNumGifTagQwords = 7; // must match the microprograms' tag-copy loops

// Depth scale: the microprogram's ftoi4 multiplies by 16, so scale + offset of
// 0xFFFF/32 maps z/w [-1 (far), +1 (near)] onto [0, 0xFFFF] in the 16-bit z-buffer.
constexpr float kGsDepthScale = static_cast<float>(0xFFFF) / 32.0f;

// rs::DrawFlags::DepthHack: the fraction of the z-buffer a hacked batch keeps, up
// against the near end. ref_gl's glDepthRange(0, 0.3) over the same inverted
// range this projection produces.
constexpr float kDepthHackScale = 0.15f;

// The NDC guard band the microprogram accepts: a triangle with any vertex at
// |x/w| or |y/w| beyond this (or outside the exact [-1, +1] z range) is
// rejected whole, not clipped - bounded by the reach of the GS 12.4 window
// coordinates, so it cannot simply be raised. Callers submitting geometry
// that can cross these planes (the world renderer) must pre-clip against
// them on the EE; the visible screen only spans |ndc| = screenW/4096, so the
// band still leaves several screens of margin fully to the VU.
constexpr float kGuardBandNdcLimit = 0.8f;

// The clip judgement multiplies x/y by this before clipw tests them against |w|, so triangles
// survive out to |ndc| = kGuardBandNdcLimit - about 5x the half-screen (the visible screen ends at
// ndc 640/4096 = 0.15) while staying inside the representable 12.4 coordinate range. The GS
// scissor does the actual on-screen cut; only triangles beyond the band (or crossing the near/far
// planes, z scale 1) are dropped whole via the ADC bit.
constexpr float kGuardBandScale = 1.0f / kGuardBandNdcLimit;

// Values FrameConstants::clipScale and ::colorClamp are always set to.
constexpr math::Vec4 kClipScale  = { kGuardBandScale, kGuardBandScale, 1.0f, 0.0f };
constexpr math::Vec4 kColorClamp = { 255.0f, 255.0f, 255.0f, 255.0f };

// Unpacked to kFrameConstantsAddr when a draw opens its chain. Built in a span of the
// command buffer itself rather than in a static the REF tag points at: once submission is one
// kick per frame, a static would be rewritten by the next draw long before the DMAC had read it
// for this one. It is the same bytes on the wire either way - the block was always REF'd.
struct alignas(16) FrameConstants
{
    math::Mat4 mvp;
    math::Vec4 gsScale;
    math::Vec4 gsOffset;
    math::Vec4 clipScale;

    // The ceiling a computed vertex color is clamped to before ftoi0 packs it
    // into GS bytes. A frame constant rather than a batch one because it is the
    // same 255 for everybody, and qword 7 was reserved anyway.
    math::Vec4 colorClamp;
};
// Exactly the 8 qwords below kDoubleBufferBase, so this block cannot grow again
// without moving the buffers.
static_assert(sizeof(FrameConstants) == 8 * 16, "Must match the VU memory layout");

// Packs 0-255 channels into DrawVertex::rgba, the GS native RGBAQ byte order
// (r in the low byte). Alpha 0x80 = 1.0 on the GS.
constexpr u32 PackColorRGBA(u32 r, u32 g, u32 b, u32 a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// ------------------------------------------------------------------------------------------------
// Generic VU1 triangles (static world geometry)
// ------------------------------------------------------------------------------------------------

// Vertices one VU1 run carries, bounded by the VU double buffer: input (8 + 2n) plus output
// (7 + 3n) qwords must fit in one 496-qword buffer half, so n <= 96 - and chunks are whole
// triangles, hence 96. Draws longer than this are split into chunks of this size.
constexpr int kMaxVertsPerBatch = 96;

// Batch layout, relative to the current double buffer (XTOP).
constexpr int kBatchHeaderAddr = 0; // vertex count in .w
constexpr int kVertexDataAddr  = kGifTagsAddr + kNumGifTagQwords;

// Per-vertex GIF registers the microprogram outputs. RGBAQ goes through an
// A+D qword because the native RGBAQ layout is the vertex's packed color u32
// with Q in the word above - the VU raw-copies the color instead of spreading
// one byte per word as the PACKED RGBAQ descriptor would want. Q rides in the
// A+D data, so nothing relies on the ST-latched Q. XYZ2 last: it kicks the
// vertex with whatever ST/RGBAQ hold.
constexpr u64 kVertexRegList = (u64(GIF_REG_ST)   << 0) |
                               (u64(GIF_REG_AD)   << 4) |
                               (u64(GIF_REG_XYZ2) << 8);

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

// ------------------------------------------------------------------------------------------------
// Keyframe-lerped triangles (MD2 alias models), must match lerped_triangles.vcl
// ------------------------------------------------------------------------------------------------

// Vertices one lerped VU run carries: the 3-qword-per-vertex batch (2 position qwords + 1
// attribute) fits fewer than the world path's 96. Whole triangles, and even - so every full
// chunk's slice of the 8-byte position stream is whole source qwords starting 16-byte aligned.
constexpr int kMaxLerpVertsPerBatch = 78;

// The regions sit at fixed offsets sized for the maximum chunk (short
// chunks leave gaps), so the microprogram addresses them with immediates.
constexpr int kLerpBatchHeaderAddr = 0; // vertex count in .w
constexpr int kLerpFrontVAddr      = 1; // current frame scale * (1 - backlerp)
constexpr int kLerpBackVAddr       = 2; // old frame scale * backlerp
constexpr int kLerpShadeLightAddr  = 3; // entity light in GS units, vertex alpha in .w
constexpr int kLerpGifTagsAddr     = 4; // the same 7-qword block as the world path
constexpr int kLerpPositionsAddr   = kLerpGifTagsAddr + kNumGifTagQwords;              // 2 qwords per vertex
constexpr int kLerpAttribsAddr     = kLerpPositionsAddr + (2 * kMaxLerpVertsPerBatch); // 1 qword per vertex
constexpr int kLerpOutputAddr      = kLerpAttribsAddr + kMaxLerpVertsPerBatch;         // the GS packet

static_assert(kLerpFrontVAddr == 1 && kLerpBackVAddr == 2 && kLerpShadeLightAddr == 3 && kLerpPositionsAddr == 11 && kLerpAttribsAddr == 167 && kLerpOutputAddr == 245, "Batch layout must match the #defines in lerped_triangles.vcl");
static_assert(kLerpOutputAddr + kNumGifTagQwords + (3 * kMaxLerpVertsPerBatch) <= kDoubleBufferOffset, "Lerp batch input + GS packet must fit one double-buffer half");
static_assert((kMaxLerpVertsPerBatch % 3) == 0, "Lerp chunks are whole triangles");
static_assert((kMaxLerpVertsPerBatch % 2) == 0, "Lerp chunk position slices must be whole qwords");

// The two keyframes' quantized positions of one vertex, interleaved: the
// current frame's dtrivertx_t bytes, then the old frame's, both copied
// verbatim from the MD2 frame data (the VIF widens each byte into an integer
// lane; the microprogram converts and lerps them).
//
// The 4th byte of each word is that frame's lightnormalindex. 'cur' keeps its
// copy - the EE indexes the shade table with it - and 'old' does not: that byte
// is where the vertex's **quantized shade term** rides instead, shade * 128 in
// 0..255, which the microprogram reads out of the lerped .w lane.
//
// That is the whole reason the attribute stream can be the model's own baked
// vertices, untouched: the one per-vertex value the EE still has to compute goes
// in a byte nothing was using, in a word it was storing anyway. shade runs
// [0.70, 1.99] (see kMaxShadeDot), so *128 lands inside a byte exactly, and the
// quantization step is 1/128 of a shade unit - against a 5-bit framebuffer
// channel, eight times finer than anything that can be displayed.
struct LerpVertexBytes
{
    u32 cur;
    u32 old;
};

// One VU run's worth of keyframe bytes, which is what a lerp chunk gathers.
//
// The attributes are not beside them any more: they are the model's own baked
// vertices, referenced where they lie in the model hunk, so there is only one
// stream left to gather.
struct alignas(16) LerpPosChunk
{
    LerpVertexBytes pos[kMaxLerpVertsPerBatch];
};
static_assert((sizeof(LerpPosChunk) % 16) == 0, "LerpPosChunk must be a whole number of qwords");

// Per-vertex attributes for a lerped draw - everything but the position and the
// shade. One qword, matching the microprogram's input layout.
//
// **Nothing writes one of these any more.** mod::AliasVertex has exactly this
// shape, so a model's baked attributes are handed to the DMA where they lie and
// the per-vertex attribute gather is gone - which is what this type is for now:
// naming the layout the microprogram reads, not a buffer anybody fills.
//
// Lane 0 is whatever the source left there (the model's keyframe index, an
// integer bit pattern) and the microprogram never reads it. It used to be the
// shade; that moved into the position stream's spare byte, which is what freed
// the rest of the qword to come straight from the model.
struct alignas(16) LerpDrawAttrib
{
    u32   unused;  // the source's own business; the microprogram does not read it
    float s, t, q; // texture coords; q must be 1.0f
};
static_assert(sizeof(LerpDrawAttrib) == 16, "LerpDrawAttrib must be exactly 1 qword");

// ------------------------------------------------------------------------------------------------
// Particles, must match particles.vcl
// ------------------------------------------------------------------------------------------------

// Particles one VU run carries. Input is 1 qword each and the sprite output 5, so a chunk
// occupies kPrtDataAddr + 6n qwords of a double-buffer half; 78 leaves a little room under 496.
constexpr int kMaxParticlesPerBatch = 78;

constexpr int kPrtBatchHeaderAddr = 0;  // particle count in .w
constexpr int kPrtQuadOffsetAddr  = 1;  // clip-space corner offset in .xyz, blow-up rate in .w
constexpr int kPrtUV0Addr         = 2;  // anchor corner UV
constexpr int kPrtUV1Addr         = 3;  // opposite corner UV
constexpr int kPrtGifTagsAddr     = 4;  // the same 7-qword block as the world path
constexpr int kPrtDataAddr        = kPrtGifTagsAddr + kNumGifTagQwords; // 1 qword per particle

static_assert(kPrtQuadOffsetAddr == 1 && kPrtUV0Addr == 2 && kPrtUV1Addr == 3 && kPrtGifTagsAddr == 4 && kPrtDataAddr == 11, "Batch layout must match the #defines in particles.vcl");
static_assert(kPrtDataAddr + (6 * kMaxParticlesPerBatch) <= kDoubleBufferOffset, "Particle batch input + GS packet must fit one double-buffer half");

// ref_gl's "hack a scale up to keep particles from disappearing": past 20 units
// the billboard grows with distance so it stays wide enough to cover a pixel.
// The microprogram applies 1 + rate * distance unconditionally rather than
// branching at 20 - below that the factor only reaches 1.08, and erring large is
// the direction the hack is pushing anyway.
constexpr float kParticleBlowUpRate = 0.004f;

// The five GIF registers one particle sprite emits: an A+D qword setting its
// RGBAQ (same raw-copy reasoning as kVertexRegList), then a UV/XYZ2 pair per
// corner. Two XYZ2 kicks complete one sprite.
constexpr u64 kParticleRegList = (u64(GIF_REG_AD)   <<  0) |
                                 (u64(GIF_REG_UV)   <<  4) |
                                 (u64(GIF_REG_XYZ2) <<  8) |
                                 (u64(GIF_REG_UV)   << 12) |
                                 (u64(GIF_REG_XYZ2) << 16);

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

// ------------------------------------------------------------------------------------------------
// Dynamic point lights, must match lit_triangles.vcl
// ------------------------------------------------------------------------------------------------

// How many lights the microprogram evaluates at once. Four is not arbitrary: the
// VU's SIMD lanes are four wide, so packing one light per lane costs the same as
// packing one, and the FMAC's four-cycle latency is covered exactly.
constexpr int kMaxDynamicLights = 4;

// VU data address of the light block. Deliberately above the double buffers
// (which end at 8 + 2*496 = 1000) rather than beside the frame constants, so
// adding it costs no vertex capacity: qwords 1000-1023 were unused.
constexpr int kLightBlockAddr = 1000;

// The GS alpha the lit colour carries. The lightmap pass needs its source alpha
// left at 1.0 so the blend still modulates by the luxel intensity; the lighting
// only ever touches .xyz, so this rides through untouched and ftoi0 turns it
// into the GS 0x80.
constexpr float kLitVertexAlpha = 128.0f;

// The lit program's per-vertex registers. Same three slots as kVertexRegList,
// but the colour goes through PACKED RGBAQ instead of an A+D write: the lit
// program *computes* its colour as four floats, and ftoi0 of a float vector
// lands one byte per word, which is exactly what the PACKED descriptor reads.
// The A+D route exists for the other programs because their colour arrives as a
// packed u32 that must be raw-copied; that does not apply here.
//
// ST must stay first: PACKED RGBAQ takes Q from the internal register the
// preceding ST write latches (word 2 of the ST qword carries it).
constexpr u64 kLitVertexRegList = (u64(GIF_REG_ST)    << 0) |
                                  (u64(GIF_REG_RGBAQ) << 4) |
                                  (u64(GIF_REG_XYZ2)  << 8);

// One point light, in world space. Attenuation is distance-only (no N.L term)
// and reaches zero at 'radius' - Quake 2's dlight intensity is exactly that
// radius in world units.
struct DynamicLight
{
    math::Vec3 origin;
    math::Vec3 color;  // 0..1
    float      radius; // world units; attenuation is zero beyond it.
};

// The light block as the microprogram reads it. Positions are transposed - all
// four lights' X in one quadword, all four Y in the next - so one SIMD lane
// carries one light and the whole four-light distance calculation is three
// subtracts and three multiply-accumulates. That transposition is the whole
// trick; see the header comment in lit_triangles.vcl.
struct alignas(16) LightConstants
{
    math::Vec4 posX;
    math::Vec4 posY;
    math::Vec4 posZ;
    math::Vec4 negColorDivR2[kMaxDynamicLights]; // -(color / radius^2), GS units
    math::Vec4 color[kMaxDynamicLights];         // color, GS units
    math::Vec4 clamp;                            // (255, 255, 255, 128)
};
static_assert(sizeof(LightConstants) == 12 * 16, "Must match the VU memory layout");
static_assert(kLightBlockAddr + 12 <= 1024, "Light block overruns VU1 data memory");

} // namespace ps2::vu1
