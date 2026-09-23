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

#include <cstddef> // offsetof
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
    Textured,  // world geometry: transform, clip, gouraud triangles. Its batch header picks the
               // per-vertex work: how the colour arrives, and whether the texture coordinates
               // animate. The lit and warped programs used to be their own; they share this
               // one's clipper now, which is the only way a second copy would ever have fit.
    Lerped,    // MD2 alias models: two keyframes lerped on the VU ahead of the transform
    Particles, // camera-facing billboards expanded to GS sprites

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
//      8-955     the two XTOP double buffers (VIF1 BASE=8, OFFSET=474)
//      956-1009  the clipper's ping-pong scratch
//      1010-1021 the dynamic light block
//      1022-1023 the turbulent surface constants
//
// The regions tile the address space exactly - see the static_asserts below. Anything new has to
// take its qwords from one of them, and the only one with any to give is the double buffer, at
// the cost of the per-batch vertex ceilings further down.
// ------------------------------------------------------------------------------------------------

// Frame constants at fixed low VU addresses (below kDoubleBufferBase).
constexpr int kFrameConstantsAddr = 0;

// VIF1 double-buffer registers: two 474-qword buffers above the constants.
constexpr int kDoubleBufferBase   = 8;
constexpr int kDoubleBufferOffset = 474;

// The clipper's Sutherland-Hodgman ping-pong, above the double buffers rather than inside them:
// only one microprogram runs at a time, so one shared region costs half what a copy in each
// buffer half would.
//
// Three qwords per corner: the clip-space position, then s/t with the raw packed colour word,
// then the computed vertex colour. The third exists for BatchColorMode::DynamicLights, whose
// colour is a function of the *world* position - which a cut vertex does not have, having only
// two endpoints to interpolate between - so the light sum has to travel as an ordinary
// attribute. It will not fit in the first two: the transform's last madd writes all four lanes
// of the position, and a corner's own colour lane is the packed u32 the other mode needs.
//
// It costs the clipper alone. The input vertex stays at two qwords, so the diffuse pass still
// hands the loader's memory straight to DMA - which is what made the first attempt at this, in
// the vertex format, cost 49% of TexChains.
//
// Two unequal buffers, since a pass reads one and writes the other and Sutherland-Hodgman can
// add a corner per plane: A holds the 7 corners pass 4 can leave and B the 8 of pass 5, each
// plus the repeated first corner the edge walk needs. 24 + 27 qwords, two spill qwords above
// them, and one qword of slack that the double buffer cannot use either (474 is the largest
// half that leaves room for this block, and 2 * 474 is one short of filling it).
constexpr int kClipScratchAddr   = 956;
constexpr int kClipScratchQwords = 54;

// Everything in the map above, in order, with nothing left over.
static_assert(kClipScratchAddr == kDoubleBufferBase + (2 * kDoubleBufferOffset),
              "The clip scratch starts where the second double buffer ends");
static_assert(kClipScratchAddr + kClipScratchQwords == 1010, "...and ends where the light block begins");

// The top two qwords of that scratch are not buffer space. They are a spill area for integer
// state the microprograms deliberately do not keep in VI registers: 1008 the output window's own
// address and step (see vu_common.i), 1009 the clipper's survivor pointer (see vu_clip.i).
//
// openvcl allocates the 13 usable VI registers by live interval, so a value that is live across
// a whole loop but read in only one place is dearer held than parked - it holds a register for
// the loop's whole length and squeezes everything the loop actually needs. It is also what
// openvcl gets wrong: a value carried from one loop into a sibling loop can have its register
// handed to a temporary inside the second one, silently.
constexpr int kWindowSpillAddr = 1008;
constexpr int kClipSpillAddr   = 1009;
static_assert(kWindowSpillAddr >= kClipScratchAddr &&
              kClipSpillAddr < kClipScratchAddr + kClipScratchQwords,
              "The spill qwords live at the top of the clip scratch");

// Per-batch work parameters, whatever the header's mode fields called for: the warp path's
// texel-to-image divide in .xy and its SURF_FLOWING scroll in .z. Zero otherwise. It is a
// quadword of its own because the header's four lanes were already spoken for, and because the
// lerp path will want somewhere to put its keyframe scales when it merges in too.
constexpr int kBatchParamsAddr = 1;

constexpr int kGifTagsAddr     = 2; // 7 qwords: GIF set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag
constexpr int kNumGifTagQwords = 7; // must match the microprograms' tag-copy loops

// Batch header .x: where a vertex's colour comes from.
//
// This and BatchWarp below are separate fields rather than one mode enum so the microprogram can
// test each with a bare compare against zero - no scratch register, no arithmetic. They are
// independent in principle; no caller currently sets both.
enum class BatchColorMode : u32
{
    PackedU32 = 0, // A+D write to RGBAQ, straight out of the vertex - world diffuse, sprites, beams
    DynamicLights, // four point lights summed on the VU, emitted as PACKED RGBAQ - the lightmap pass
};

// Batch header .y: whether the texture coordinates animate.
//
// Warped surfaces arrive in raw texel units and ref_gl's ripple is evaluated on the VU, at the
// emit rather than the transform - so a vertex the clipper cut is interpolated raw and warped
// afterwards, which is the vertex that actually exists. See the note in DrawAnimatedWaterPolys.
enum class BatchWarp : u32
{
    Off = 0,
    On,            // reads the parameters at kBatchParamsAddr
};

// Depth scale: the microprogram's ftoi4 multiplies by 16, so scale + offset of
// 0xFFFF/32 maps z/w [-1 (far), +1 (near)] onto [0, 0xFFFF] in the 16-bit z-buffer.
constexpr float kGsDepthScale = static_cast<float>(0xFFFF) / 32.0f;

// rs::DrawFlags::DepthHack: the fraction of the z-buffer a hacked batch keeps at the near end.
// ref_gl's glDepthRange(0, 0.3) over the inverted range this projection produces.
constexpr float kDepthHackScale = 0.15f;

// The NDC guard band the microprogram accepts: a triangle with any vertex beyond it in |x/w| or
// |y/w|, or outside the exact [-1, +1] z range, is rejected whole rather than clipped. Bounded by
// the reach of the GS 12.4 window coordinates, so it cannot simply be raised - callers whose
// geometry can cross these planes must pre-clip on the EE.
constexpr float kGuardBandNdcLimit = 0.8f;

// The clip judgement multiplies x/y by this before clipw tests them against |w|, so triangles
// survive out to |ndc| = kGuardBandNdcLimit - about 5x the half-screen (the visible screen ends at
// ndc 640/4096 = 0.15) while staying inside the representable 12.4 coordinate range. The GS
// scissor does the actual on-screen cut; only triangles beyond the band (or crossing the near/far
// planes, z scale 1) are dropped whole via the ADC bit.
constexpr float kGuardBandScale = 1.0f / kGuardBandNdcLimit;

// How far inside the clip planes the VU1 clipper actually cuts, as a fraction of w.
//
// A cut lands the vertex exactly on the plane, and the guard-band judgement that runs on the
// survivors tests the very same quantity - so whether it reads as inside comes down to which way
// the divide rounded. That is a coin flip on every cut vertex, and a lost toss takes the whole
// triangle with it through the ADC bit. Clipping a hair early means a survivor is strictly
// inside and the judgement can only agree.
//
// clip::kClipEpsilon is the EE clipper's version of this and exists for the same reason. This one
// is relative rather than absolute, so the margin holds at any depth; 0.1% of the guard band is
// some five hundred times the rounding it covers and still far below anything visible, the band
// being about five times the half-screen.
constexpr float kVuClipShrink = 0.001f;

// What the clipper multiplies a plane distance by before reading its sign.
//
// The sign comes off an ftoi4, which resolves 1/16 of a unit, and the crossing test
// multiplies two distances together - so the headroom has to cover the product, not just
// the distance. 2048 squared was previously applied as two multiplies by fGSScale.x; as its
// own constant it is one, which takes four cycles of FMAC latency off a chain that the
// clipper walks twice per edge and five times per triangle.
constexpr float kVuClipDistScale = 2048.0f * 2048.0f;

// Values FrameConstants::clipScale and ::colorClamp are always set to. clipScale's .w is the
// exception: no program reads it as part of the clip judgement, so the turbulent animation phase
// rides there instead and BeginDrawChain fills it per frame (see rs::SetWarpAnimation).
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

    // .xyz is the guard band scale (kClipScale); .w is the frame's turbulent animation phase,
    // in turns, which only a warped batch reads. A spare lane rather than a ninth qword
    // because this block cannot grow without moving the double buffers.
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

// Vertices one VU1 run carries. Two windows' worth exactly, so a full chunk is two kicks.
// Draws longer than this are split into chunks of this size.
constexpr int kMaxVertsPerBatch = 90;

// Batch layout, relative to the current double buffer (XTOP).
constexpr int kBatchHeaderAddr = 0; // colour mode .x, warp flag .y, vertex count .w
constexpr int kVertexDataAddr  = kGifTagsAddr + kNumGifTagQwords;

// The GS packet is built in one of two fixed output windows rather than immediately after the
// input vertices. A clipping microprogram does not know its output count until it has run, so
// the packet can have neither a count-dependent address nor a count-dependent size; the
// microprogram fills a window, patches the drawing tag's NLOOP with what it actually wrote,
// kicks it, and moves to the other one.
//
// Two windows and not one because a window may not be rewritten while the GIF is still reading
// it. Alternating gives that for free: the kick that starts window B is issued before window A
// is ever touched again, and an XGKICK issued while a transfer is in flight stalls until that
// transfer has drained.
//
// A window holds whole triangles only - a triangle split across two packets would be re-primed
// by the second packet's tag and drawn wrong - so the capacity is a multiple of three.
constexpr int kMaxVertsPerWindow  = 45;
constexpr int kOutputWindowQwords = kNumGifTagQwords + (3 * kMaxVertsPerWindow);
constexpr int kOutputWindowAAddr  = kVertexDataAddr + (2 * kMaxVertsPerBatch);
constexpr int kOutputWindowBAddr  = kOutputWindowAAddr + kOutputWindowQwords;

static_assert((kMaxVertsPerWindow % 3) == 0, "A window holds whole triangles");
static_assert((kMaxVertsPerBatch % 3) == 0, "World chunks are whole triangles");
static_assert(kMaxVertsPerBatch == 2 * kMaxVertsPerWindow, "A full chunk should be exactly two kicks");
static_assert(kOutputWindowBAddr + kOutputWindowQwords <= kDoubleBufferOffset,
              "World batch input + both output windows must fit one double-buffer half");

// The microprogram addresses all of these with immediates, so they are #defines over there and
// nothing but this line ties the two together. The lerp and particle layouts have had their
// equivalent since they were written; this path went without one until the warp merge moved
// every address in it at once.
static_assert(kBatchHeaderAddr == 0 && kBatchParamsAddr == 1 && kGifTagsAddr == 2 &&
              kVertexDataAddr == 9 && kOutputWindowAAddr == 189 && kOutputWindowBAddr == 331 &&
              kOutputWindowQwords == 142 && kMaxVertsPerWindow == 45,
              "Batch layout must match the #defines in textured_triangles.vcl");

// Per-vertex GIF registers the microprogram outputs. RGBAQ goes through an A+D qword so the VU
// can raw-copy the packed colour instead of spreading one byte per word as the PACKED RGBAQ
// descriptor wants; Q rides in the A+D data. XYZ2 last - it kicks the vertex with whatever
// ST/RGBAQ hold.
constexpr u64 kVertexRegList = (u64(GIF_REG_ST)   << 0) |
                               (u64(GIF_REG_AD)   << 4) |
                               (u64(GIF_REG_XYZ2) << 8);

// One triangle vertex, 2 qwords, matching the microprogram's input layout. The packed colour must
// sit in the first word of its qword: the microprogram raw-copies it with a single masked store
// and only word 0 is reachable that way (going through the FMAC would flush denormal colour
// patterns to zero).
//
// Two of the eight lanes are free, and the world renderer's second UV set is what they carry -
// which is why this is also mod::PolyVertex (see model.h), and why a world polygon goes to the
// DMA as the loader baked it rather than being rebuilt a vertex at a time.
//
// They are free because no microprogram reads either one: the position's translation row is
// scaled by vf00's hardwired 1.0 rather than by the vertex, and Q reaches the GS from the
// reciprocal the perspective divide already computed - through the A+D RGBAQ write for the
// programs that raw-copy a packed colour, and through an explicit store into the ST qword for
// the ones that compute their colour. So a producer with no lightmap coordinates to park here
// simply leaves both alone.
struct alignas(16) DrawVertex
{
    math::Vec3 position;   // model space
    float      lightmap_s; // free lane; the world's lightmap S
    u32        rgba;       // packed color, use PackColorRGBA()
    float      s, t;       // diffuse texture coords
    float      lightmap_t; // free lane; the world's lightmap T
};
static_assert(sizeof(DrawVertex)  == 32, "DrawVertex must be exactly 2 qwords");
static_assert(alignof(DrawVertex) == 16, "CopyDrawVertex's lq/sq require qword alignment");

// The microprograms address these by word rather than by name - the colour is raw-copied out of
// word 4 with sq.x and the coords are read as lanes .y/.z of the same qword - so moving a field
// silently mis-renders instead of failing to build. Hence pinning them here.
static_assert(offsetof(DrawVertex, rgba) == 16, "The microprograms raw-copy rgba out of word 4");
static_assert(offsetof(DrawVertex, s)    == 20, "s must be word 5; the microprograms read it there");
static_assert(offsetof(DrawVertex, t)    == 24, "t must be word 6; the microprograms read it there");

// Copies one gathered vertex whole, in two of the R5900's 128-bit moves.
//
// A plain struct assignment is correct but gcc lowers it to four ld/sd pairs - it never forms
// lq/sq on its own, and neither __int128 nor a 16-byte-aligned aggregate persuades it to. That
// doubles the memory ops in the innermost step of the world gather.
//
// Integer lq/sq rather than the VU0 lqc2/sqc2 the math helpers use: this moves a packed colour
// whose bit pattern is a float denormal (see above), and the integer path never reaches an FMAC
// to flush it. Constrained rather than clobbering "memory", so a caller copying several vertices
// in a row keeps its own state in registers across them.
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
// Turbulent (warped) surfaces: the warp block of textured_triangles.vcl
// ------------------------------------------------------------------------------------------------

// The warp program takes the *same* batch layout as the textured one - same header, same GIF tag
// block, same kMaxVertsPerBatch - so it shares the chunk emitter and the chain budget. It differs
// only in what it reads: the header's three spare lanes carry this batch's texture size and scroll
// (AddBatchChunk fills them), FrameConstants::clipScale.w the frame's phase, and the block below
// the shape of the sine itself.

// ref_gl's r_turbsin amplitude (gl_warp.c, values in warpsin.h): 8*sin(i*2pi/256), halved once at
// startup by R_Init, so the effective amplitude is 4 texels. Folded into the polynomial terms
// below rather than multiplied on separately.
constexpr float kTurbSinAmplitude = 4.0f;

// ref_gl divides a vertex's texel coordinate by 8 before taking its sine. The microprogram carries
// phase in turns rather than radians - the range reduction wants a fraction anyway - so the 2pi
// goes in here, where it costs nothing, instead of in the VU.
constexpr float kTurbTurnsPerTexel = 0.125f / (2.0f * math::kPI);

// The bias that makes the microprogram's ftoi0 a floor *and* a round-to-nearest in one step: it
// truncates toward zero, so the phase is lifted clear of zero first and the half lands the
// rounding. Correct while |phase| stays under 1024 turns, which texel coordinates would have to
// pass ~51k to break (the frame's own phase is pre-wrapped into [0, 1) by rs::SetWarpAnimation).
constexpr float kTurbPhaseBias = 1024.5f;

// Odd Taylor terms of sin(2pi*t) with the amplitude folded in, for |t| <= 0.25 turns - the range
// the microprogram's triangle fold leaves. Degree 7 is good to ~0.002 texels against a true sine,
// where the 256-entry table this replaces was only good to 0.098.
//
// t is in turns, so every power of the radian argument carries its own power of 2pi.
constexpr float kTurbTau = 2.0f * math::kPI;
constexpr float kTurbPoly1 =  kTurbSinAmplitude * kTurbTau;
constexpr float kTurbPoly3 = -kTurbSinAmplitude * (kTurbTau * kTurbTau * kTurbTau) / 6.0f;
constexpr float kTurbPoly5 =  kTurbSinAmplitude * (kTurbTau * kTurbTau * kTurbTau * kTurbTau * kTurbTau) / 120.0f;
constexpr float kTurbPoly7 = -kTurbSinAmplitude * (kTurbTau * kTurbTau * kTurbTau * kTurbTau * kTurbTau * kTurbTau * kTurbTau) / 5040.0f;

// VU data address of the warp constant block: the last two qwords of VU1 data memory.
constexpr int kWarpConstBlockAddr = 1022;

// The warp constants as the microprogram reads them. Uploaded once by Init and never rewritten -
// everything here is a compile-time constant; what varies per frame or per batch travels in
// FrameConstants::clipScale.w and the batch header.
struct alignas(16) WarpConstants
{
    math::Vec4 fold; // (phase bias, 0.5, turns per texel, unused)
    math::Vec4 poly; // the four odd sine terms, amplitude folded in
};
static_assert(sizeof(WarpConstants) == 2 * 16, "Must match the VU memory layout");
static_assert(kWarpConstBlockAddr + 2 == 1024, "Warp constants must be the last two qwords of VU1 data memory");

constexpr WarpConstants kWarpConstants = {
    { kTurbPhaseBias, 0.5f, kTurbTurnsPerTexel, 0.0f },
    { kTurbPoly1, kTurbPoly3, kTurbPoly5, kTurbPoly7 }
};

// ------------------------------------------------------------------------------------------------
// Keyframe-lerped triangles (MD2 alias models), must match lerped_triangles.vcl
// ------------------------------------------------------------------------------------------------

// Vertices one lerped VU run carries: the 3-qword-per-vertex batch (2 position qwords + 1
// attribute) fits fewer than the world path's 93. Whole triangles, and even - so every full
// chunk's slice of the 8-byte position stream is whole source qwords starting 16-byte aligned,
// which together make it a multiple of six.
constexpr int kMaxLerpVertsPerBatch = 72;

// Fixed offsets sized for the maximum chunk (short chunks leave gaps), so the microprogram
// addresses them with immediates.
constexpr int kLerpBatchHeaderAddr = 0; // vertex count in .w
constexpr int kLerpFrontVAddr      = 1; // current frame scale * (1 - backlerp)
constexpr int kLerpBackVAddr       = 2; // old frame scale * backlerp
constexpr int kLerpShadeLightAddr  = 3; // entity light in GS units, vertex alpha in .w
constexpr int kLerpGifTagsAddr     = 4; // the same 7-qword block as the world path
constexpr int kLerpPositionsAddr   = kLerpGifTagsAddr + kNumGifTagQwords;              // 2 qwords per vertex
constexpr int kLerpAttribsAddr     = kLerpPositionsAddr + (2 * kMaxLerpVertsPerBatch); // 1 qword per vertex
constexpr int kLerpOutputAddr      = kLerpAttribsAddr + kMaxLerpVertsPerBatch;         // window A

// The lerp path's own pair of output windows, working exactly as the world path's above: a full
// chunk is two kicks. The capacity is lower only because the input regions ahead of them are
// bigger - two position qwords and an attribute qword per vertex, against the world's two.
constexpr int kLerpMaxVertsPerWindow  = 36;
constexpr int kLerpOutputWindowQwords = kNumGifTagQwords + (3 * kLerpMaxVertsPerWindow);
constexpr int kLerpWindowAAddr        = kLerpOutputAddr;
constexpr int kLerpWindowBAddr        = kLerpWindowAAddr + kLerpOutputWindowQwords;

static_assert(kLerpFrontVAddr == 1 && kLerpBackVAddr == 2 && kLerpShadeLightAddr == 3 && kLerpPositionsAddr == 11 && kLerpAttribsAddr == 155 && kLerpOutputAddr == 227, "Batch layout must match the #defines in lerped_triangles.vcl");
static_assert(kLerpWindowAAddr == 227 && kLerpWindowBAddr == 342 && kLerpOutputWindowQwords == 115, "Window layout must match the #defines in lerped_triangles.vcl");
static_assert(kLerpWindowBAddr + kLerpOutputWindowQwords <= kDoubleBufferOffset, "Lerp batch input + both output windows must fit one double-buffer half");
static_assert((kLerpMaxVertsPerWindow % 3) == 0, "A window holds whole triangles");
static_assert(kMaxLerpVertsPerBatch == 2 * kLerpMaxVertsPerWindow, "A full lerp chunk should be exactly two kicks");
static_assert((kMaxLerpVertsPerBatch % 3) == 0, "Lerp chunks are whole triangles");
static_assert((kMaxLerpVertsPerBatch % 2) == 0, "Lerp chunk position slices must be whole qwords");

// The two keyframes' quantized positions of one vertex, interleaved: the current frame's
// dtrivertx_t bytes then the old frame's, copied verbatim from the MD2 frame data (the VIF widens
// each byte into an integer lane; the microprogram converts and lerps them).
//
// The 4th byte of each word is that frame's lightnormalindex. 'cur' keeps its copy, which the EE
// indexes the shade table with; 'old' does not - that byte carries the vertex's **quantized shade
// term** instead, shade * 128 in 0..255, which the microprogram reads out of the lerped .w lane.
// Putting it there is what lets the attribute stream be the model's own baked vertices untouched.
//
// shade runs [0.70, 1.99] (see kMaxShadeDot), so *128 lands inside a byte exactly, at a step of
// 1/128 of a shade unit - eight times finer than a 5-bit framebuffer channel can show.
struct LerpVertexBytes
{
    u32 cur;
    u32 old;
};

// One VU run's worth of keyframe bytes - the only stream a lerp chunk gathers, since the
// attributes are referenced where they lie in the model hunk.
struct alignas(16) LerpPosChunk
{
    LerpVertexBytes pos[kMaxLerpVertsPerBatch];
};
static_assert((sizeof(LerpPosChunk) % 16) == 0, "LerpPosChunk must be a whole number of qwords");

// Per-vertex attributes for a lerped draw - everything but the position and the shade. One qword,
// matching the microprogram's input layout.
//
// This is mod::AliasVertex (see model.h): the loader bakes a model's attributes in exactly this
// shape, so they go to the DMA where they lie rather than being gathered first. Lane 0 is
// whatever the source left there and the microprogram never reads it.
struct alignas(16) LerpDrawAttrib
{
    u32 index;     // the source's own business; the microprogram does not read it
    float s, t, q; // texture coords; q must be 1.0f
};
static_assert(sizeof(LerpDrawAttrib) == 16, "LerpDrawAttrib must be exactly 1 qword");

// ------------------------------------------------------------------------------------------------
// Particles, must match particles.vcl
// ------------------------------------------------------------------------------------------------

// Particles one VU run carries. Input is 1 qword each and the sprite output 5, so a chunk
// occupies kPrtDataAddr + 6n qwords of a double-buffer half; 77 is what fits the 474 the clip
// scratch left, and this is the one batch ceiling that binds against it - the triangle and lerp
// layouts had slack to give.
constexpr int kMaxParticlesPerBatch = 77;

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

// VU data address of the light block, above the double buffers and the clip
// scratch rather than beside the frame constants - those low 8 qwords are what
// every batch layout indexes off.
constexpr int kLightBlockAddr = 1010;

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
static_assert(kLightBlockAddr + 12 == kWarpConstBlockAddr, "Light block must sit between the clip scratch and the warp constants");

} // namespace ps2::vu1
