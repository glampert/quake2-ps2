/* ================================================================================================
 * File: vu1.cpp
 * Brief: VU1-accelerated 3D drawing. See vu1.h.
 *
 *  Modelled on the ps2sdk "draw/vu1" sample. Each DrawTriangles call builds one VIF1
 *  source chain: frame constants (MVP + GS screen mapping) unpacked to fixed low VU
 *  addresses, then, per chunk of up to kMaxVertsPerBatch vertices, the batch (header,
 *  GIF tags, vertices) unpacked at the current double buffer plus FLUSH + MSCAL to
 *  run the microprogram, which transforms, clips and XGKICKs the triangles to the GS
 *  over PATH1. XTOP flips on every MSCAL, so the VIF unpacks one chunk into a buffer
 *  half while the VU still transforms the previous one. No extra syncs are needed
 *  between chunks: MSCAL stalls the VIF while a program runs, and each program's
 *  XGKICK stalls until the previous kick drained, which keeps a half's output area
 *  safe from the next-but-one program until the GS is done reading it.
 *
 *  VU1 data memory layout (1024 qwords; addresses in qwords):
 *      0-3    MVP matrix rows
 *      4      GS scale  (2048, 2048, zScale)
 *      5      GS offset (2048 + w/2, 2048 + h/2, zScale)
 *      6      clip-judgement scale (guard band)
 *      7      reserved
 *      8-999  the two XTOP double buffers (VIF1 BASE=8, OFFSET=496)
 *
 *  Batch layout inside a double buffer (relative to XTOP): input is one header
 *  qword (vertex count in .w), 7 GIF/AD tag qwords, then 2 qwords per vertex;
 *  the microprogram builds the GS packet in the same buffer after the input.
 *  The A+D block programs TEST, ALPHA and ZBUF as well as TEX0/TEX1, so a
 *  batch draws with the proper z-test, blend function and depth-write mask no
 *  matter what state the surrounding 2D packets (or an earlier blended batch)
 *  left behind.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/render_profile.h"

#include <dma.h>
#include <draw.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>

namespace ps2::vu1 {

PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_TexturedTriangles);
PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_LerpedTriangles);
PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_Particles);

// ------------------------------------------------------------------------------------------------
// Shared local helpers
// ------------------------------------------------------------------------------------------------

namespace {

// Frame constants at fixed low VU addresses (below kDoubleBufferBase).
constexpr int kFrameConstantsAddr = 0;

// VIF1 double-buffer registers: two 496-qword buffers above the constants.
constexpr int kDoubleBufferBase   = 8;
constexpr int kDoubleBufferOffset = 496;

constexpr int kGifTagsAddr     = 1; // 7 qwords: GIF set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag
constexpr int kNumGifTagQwords = 7; // must match the microprograms' tag-copy loops

// The chain is tags plus small per-chunk inline unpacks; constants and
// vertices are referenced in place. Sized so a DrawTriangles call fits ~30
// chunks (~2900 verts) before it must flush the chain mid-call.
constexpr int kDrawPacketQwords = 512;

// Conservative chain footprint of one chunk segment (header/tags inline
// unpack, vertex REF unpack, FLUSH + MSCAL; ~13 qwords in practice) and of
// the chain tail (trailing FLUSH + END tag). DrawTriangles flushes the packet
// when the next chunk plus the tail might not fit.
constexpr int kChunkChainQwords = 16;
constexpr int kChainTailQwords  = 4;

// Depth scale: the microprogram's ftoi4 multiplies by 16, so scale + offset of
// 0xFFFF/32 maps z/w [-1 (far), +1 (near)] onto [0, 0xFFFF] in the 16-bit z-buffer.
constexpr float kGsDepthScale = static_cast<float>(0xFFFF) / 32.0f;

// DrawFlags::DepthHack: the fraction of the z-buffer a hacked batch keeps, up
// against the near end. ref_gl's glDepthRange(0, 0.3) over the same inverted
// range this projection produces.
constexpr float kDepthHackScale = 0.15f;

// Per-vertex GIF registers the microprogram outputs. RGBAQ goes through an
// A+D qword because the native RGBAQ layout is the vertex's packed color u32
// with Q in the word above - the VU raw-copies the color instead of spreading
// one byte per word as the PACKED RGBAQ descriptor would want. Q rides in the
// A+D data, so nothing relies on the ST-latched Q. XYZ2 last: it kicks the
// vertex with whatever ST/RGBAQ hold.
constexpr u64 kVertexRegList = (u64(GIF_REG_ST)   << 0) |
                               (u64(GIF_REG_AD)   << 4) |
                               (u64(GIF_REG_XYZ2) << 8);

// Guard band: the clip judgement multiplies x/y by this before clipw tests
// them against |w|, so triangles survive out to |ndc| = kGuardBandNdcLimit -
// about 5x the half-screen (the visible screen ends at ndc 640/4096 = 0.15)
// while staying inside the representable 12.4 coordinate range. The GS
// scissor does the actual on-screen cut; only triangles beyond the band (or
// crossing the near/far planes, z scale 1) are dropped whole via the ADC bit.
constexpr float kGuardBandScale = 1.0f / kGuardBandNdcLimit;

// Unpacked to kFrameConstantsAddr before every batch. Static so the DMA REF
// source stays valid; rebuilt per draw.
struct alignas(16) FrameConstants
{
    math::Mat4 mvp;
    math::Vec4 gsScale;
    math::Vec4 gsOffset;
    math::Vec4 clipScale;
};
static_assert(sizeof(FrameConstants) == 7 * 16, "Must match the VU memory layout");

static FrameConstants s_constants;
static VifPacket s_drawPacket;
static bool s_initialized = false;

// REF'd payload bytes submitted this frame, and the high-water across the run.
// Sizes the frame arena that will replace the per-module gather statics; see
// vu1::PeakFrameSubmittedBytes.
static int s_frameSubmittedBytes = 0;
static int s_peakSubmittedBytes  = 0;

// Micro memory entry point of the VU1 programs (64-bit
// instruction units; the textured program sits at 0).
// Set by Init().
static u32 s_texturedTrisProgAddr = 0;
static u32 s_lerpedProgAddr = 0;
static u32 s_particlesProgAddr = 0;

// ------------------------------------------------------------------------------------------------
// Helper functions
// ------------------------------------------------------------------------------------------------

// PRIM Register Bits:
//   PRI  - Primitive type
//   IIP  - Shading method (0=flat, 1=gouraud)
//   TME  - Texture mapping (0=off, 1=on)
//   FGE  - Fog (0=off, 1=on)
//   ABE  - Alpha Blending (0=off, 1=on)
//   AA1  - Anti-aliasing (0=off,1=on)
//   FST  - Texture coordinate specification (0=use ST/RGBAQ register, 1=use UV register) (UV means no perspective correction, good for 2D)
//   CTXT - Drawing context (0=1, 1=2)
//   FIX  - ?? Fragment value control (use 0)

// TEX0/TEX1 register qwords for the batch's texture bind, sent A+D over PATH1.
// Built here rather than with the packet2_utils helpers because those hardcode
// GS context 0 and this renderer alternates contexts per frame.
inline u64 MakeTex0Data(const tex::Texture & texture)
{
    // Indexed textures sample through one of the two fixed CLUTs (the global
    // palette or the alpha ramp, by format); reloading the on-chip CLUT cache
    // on every bind is cheap (1 KB). Everything else leaves the CLUT fields
    // zero (as gs::SetTextureFor2D).
    const vram::Address clutAddr = gs::ClutAddressFor(texture);
    const bool palettized = (clutAddr != vram::Address::Invalid);

    const int psm    = tex::GsPsm(texture.format);
    const int stride = tex::TextureStridePixels(texture, psm);

    texbuffer_t texbuf;
    texbuf.address         = static_cast<unsigned int>(texture.vramAddr);
    texbuf.width           = static_cast<unsigned int>(stride);
    texbuf.psm             = static_cast<unsigned int>(psm);
    texbuf.info.width      = tex::Log2(static_cast<u32>(texture.width));
    texbuf.info.height     = tex::Log2(static_cast<u32>(texture.height));
    texbuf.info.components = static_cast<unsigned char>(tex::GsComponents(texture.components));
    texbuf.info.function   = static_cast<unsigned char>(tex::GsFunction(texture.function));

    return GS_SET_TEX0(texbuf.address >> 6,
                       texbuf.width >> 6,
                       texbuf.psm,
                       texbuf.info.width,
                       texbuf.info.height,
                       texbuf.info.components,
                       texbuf.info.function,
                       palettized ? ((int)clutAddr >> 6) : 0,
                       GS_PSM_32, // CPSM; only read for palettized PSMs (and == 0 anyway)
                       CLUT_STORAGE_MODE1, 0,
                       palettized ? CLUT_LOAD : CLUT_NO_LOAD);
}

inline u64 MakeTex1Data(const tex::Texture & texture)
{
    return GS_SET_TEX1(LOD_USE_K, 0,
                       tex::GsMagFilter(texture.magFilter),
                       tex::GsMinFilter(texture.minFilter),
                       LOD_MIPMAP_REGISTER, 0, 0);
}

// Pixel tests for the batch: the environment's alpha test plus the real
// z-test (mirrors libdraw's draw_enable_tests).
inline u64 MakeTestData()
{
    return GS_SET_TEST(DRAW_ENABLE, ATEST_METHOD_NOTEQUAL, 0x00, ATEST_KEEP_FRAMEBUFFER,
                       DRAW_DISABLE, DRAW_DISABLE,
                       DRAW_ENABLE, gs::DepthTestMethod());
}

// Blend function for batches drawn with the PRIM ABE bit on. Opaque batches
// write it too - deterministic register state, ignored while ABE is off.
//
// The additive form keeps C = As rather than the fixed 0x80 that would spell
// GL_ONE literally: at As = 0x80 the two are identical, and routing the
// source alpha through the equation lets a caller fade an additive primitive
// per vertex (the dynamic light flares rely on it) without a third mode.
inline u64 MakeAlphaData(DrawFlags flags)
{
    if (HasDrawFlag(flags, DrawFlags::Additive))
    {
        // (Cs - 0) * As / 128 + Cd
        return GS_SET_ALPHA(BLEND_COLOR_SOURCE, BLEND_COLOR_ZERO,
                            BLEND_ALPHA_SOURCE, BLEND_COLOR_DEST, 0x80);
    }

    if (HasDrawFlag(flags, DrawFlags::Modulate))
    {
        // (Cd - 0) * As / 128 + 0: scales the framebuffer by the source alpha
        // and adds nothing, so the batch's own colour never reaches the pixel.
        return GS_SET_ALPHA(BLEND_COLOR_DEST, BLEND_COLOR_ZERO,
                            BLEND_ALPHA_SOURCE, BLEND_COLOR_ZERO, 0x80);
    }

    // (Cs - Cd) * As / 128 + Cd
    return GS_SET_ALPHA(BLEND_COLOR_SOURCE, BLEND_COLOR_DEST,
                        BLEND_ALPHA_SOURCE, BLEND_COLOR_DEST, 0x80);
}

// The GS z conversion for a batch, as the (offset, scale) pair the microprogram
// applies to NDC z. The unhacked pair is the mapping described on kGsDepthScale
// above; a hacked one squeezes NDC z into [1 - 2s, 1] before it, i.e.
//
//     Z = 16 * kGsDepthScale * (1 + (s * ndcZ + (1 - s)))
//       = 16 * (kGsDepthScale * (2 - s) + ndcZ * kGsDepthScale * s)
//
// leaving the nearest s of the z-buffer to the batch and costing the
// microprogram nothing - it multiplies and adds these either way.
inline void DepthRangeFor(DrawFlags flags, float * outScale, float * outOffset)
{
    const float s = HasDrawFlag(flags, DrawFlags::DepthHack) ? kDepthHackScale : 1.0f;
    *outScale  = kGsDepthScale * s;
    *outOffset = kGsDepthScale * (2.0f - s);
}

// Emits the 6 qwords of state every batch opens with: the GIF tag announcing
// five A+D register writes, then TEST, TEX1, TEX0, ALPHA and ZBUF for this
// context. Shared by the triangle and particle paths, which differ only in the
// seventh qword - the drawing tag - that each appends afterwards.
//
// Returns whether the batch blends, since the drawing tag needs it for the
// prim's ABE bit and it is decided here.
bool AddBatchStateBlock(VifPacket & pkt, const tex::Texture & texture, int ctx, DrawFlags flags)
{
    // The blend flags select alternative equations, they are not switches to
    // combine: each one brings the ABE bit and the depth-write mask with it.
    const int blendModes = static_cast<int>(HasDrawFlag(flags, DrawFlags::Blended))
                         + static_cast<int>(HasDrawFlag(flags, DrawFlags::Additive))
                         + static_cast<int>(HasDrawFlag(flags, DrawFlags::Modulate));
    PS2_AssertMsg(blendModes <= 1,
                  "Pick one blend mode - Blended, Additive and Modulate are exclusive!");

    const bool blended = (blendModes != 0);

    // Five A+D register writes: pixel tests, the texture bind, the blend
    // function and the depth-write mask for this context...
    pkt.AddQword(GIF_SET_TAG(5, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    pkt.AddQword(MakeTestData(), static_cast<u64>(GS_REG_TEST + ctx));
    pkt.AddQword(MakeTex1Data(texture), static_cast<u64>(GS_REG_TEX1  + ctx));
    pkt.AddQword(MakeTex0Data(texture), static_cast<u64>(GS_REG_TEX0  + ctx));
    pkt.AddQword(MakeAlphaData(flags),  static_cast<u64>(GS_REG_ALPHA + ctx));
    pkt.AddQword(gs::ZBufData(blended || HasDrawFlag(flags, DrawFlags::NoDepthWrite)),
                 static_cast<u64>(GS_REG_ZBUF + ctx));

    return blended;
}

// Emits the batch's 7 GIF tag qwords into an open inline unpack: the A+D
// state block and the drawing tag for 'vertCount' vertices. Blended batches
// turn the prim's ABE bit on and mask depth writes; NoDepthWrite masks them
// without the ABE bit; untextured ones clear the TME bit (the texture
// registers are still written, just not sampled).
void AddBatchGifTags(VifPacket & pkt, const tex::Texture & texture, int ctx,
                     int vertCount, DrawFlags flags)
{
    const bool blended = AddBatchStateBlock(pkt, texture, ctx, flags);
    const int  tme     = HasDrawFlag(flags, DrawFlags::Untextured) ? 0 : 1;
    const int  abe     = blended ? 1 : 0;

    // ...then the drawing tag: gouraud triangle list, STQ mapping, with the
    // per-vertex registers of kVertexRegList.
    //
    // Built with the gif_tags.h macros, not packet2_utils.h's VU_GS_PRIM /
    // VU_GS_GIFTAG: those do not parenthesize their parameters, so an
    // argument that is an expression silently mis-assembles. Passing
    // 'blended ? 1 : 0' for ABE expanded to '(blended ? 1 : 0 << 6)', which
    // parses as 'blended ? 1 : (0 << 6)' and drops the bit at position 0 -
    // inside the PRIM field, where PRIM_TRIANGLE (3) already has that bit
    // set. Nothing warned and the primitive still drew, just never blended.
    const u64 prim = GIF_SET_PRIM(PRIM_TRIANGLE, 1, tme, 0, abe, 0, 0, ctx, 0);
    pkt.AddQword(GIF_SET_TAG(vertCount, 1, 1, prim, GIF_FLG_PACKED, 3), kVertexRegList);
}

// Rebuilds s_constants for a draw and opens the chain with its unpack to the
// fixed low VU addresses (shared by both draw paths).
void BeginDrawChain(VifPacket & pkt, const math::Mat4 & mvp, DrawFlags flags)
{
    // Every chunk of a draw shares one flags value, so the batch's depth range
    // is a property of the whole chain and rides with the other constants.
    float depthScale, depthOffset;
    DepthRangeFor(flags, &depthScale, &depthOffset);

    s_constants.mvp       = mvp;
    s_constants.gsScale   = { 2048.0f, 2048.0f, depthScale, 0.0f };
    s_constants.gsOffset  = { 2048.0f + static_cast<float>(gs::Width())  * 0.5f,
                              2048.0f + static_cast<float>(gs::Height()) * 0.5f,
                              depthOffset, 0.0f };
    s_constants.clipScale = { kGuardBandScale, kGuardBandScale, 1.0f, 0.0f };

    pkt.Reset();
    pkt.AddUnpackData(kFrameConstantsAddr, &s_constants, sizeof(FrameConstants) / 16, false);

    // Every chain opens with its own constants block, so an arena would need one
    // per draw call (and one more per mid-call overflow flush) - count them here
    // rather than at the Draw* entry points, which would miss the reopens.
    s_frameSubmittedBytes += static_cast<int>(sizeof(FrameConstants));
}

// FLUSH so a DMA wait covers the VU runs and their XGKICKs, then terminate
// and send the chain, blocking until it is fully consumed.
void SendChainAndWait(VifPacket & pkt)
{
    pkt.AddFlush();
    pkt.AddEndTag();
    pkt.Send();

    // The stall this whole batch exists to pay for: one per drawBatches, and the
    // single largest recoverable cost in the renderer. Charged to the shared
    // GSWait total (render_profile.h) alongside the GIF-side waits in gs.cpp.
    PS2_PROFILE_SCOPED_EVENT(prof_evt::GsWait);
    pkt.Wait();
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    PS2_AssertMsg(!s_initialized, "vu1::Init called twice!");
    s_initialized = true;

    dma_channel_initialize(DMA_CHANNEL_VIF1, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);

    // All three microprograms stay resident: the textured one at micro address
    // 0, then the lerped one, then the particle one. MPG uploads round an odd
    // instruction count up to even, so each base rounds up too.
    const u32 texturedInstructions  = VU1Prog_TexturedTriangles_InstructionCount();
    const u32 lerpedInstructions    = VU1Prog_LerpedTriangles_InstructionCount();
    const u32 particlesInstructions = VU1Prog_Particles_InstructionCount();

    s_texturedTrisProgAddr = 0;
    s_lerpedProgAddr       = (texturedInstructions + 1u) & ~1u;
    s_particlesProgAddr    = (s_lerpedProgAddr + lerpedInstructions + 1u) & ~1u;

    PS2_AssertMsg(s_particlesProgAddr + particlesInstructions <= 2048,
                  "Microprograms overflow VU1 micro memory!");

    s_drawPacket.Init(kDrawPacketQwords);

    // Upload the microprograms and set up the double buffer. Synchronous;
    // VU1 is ready once this returns.
    VifPacket & pkt = s_drawPacket;
    pkt.AddMicroProgram(s_texturedTrisProgAddr, VU1Prog_TexturedTriangles_Code());
    pkt.AddMicroProgram(s_lerpedProgAddr, VU1Prog_LerpedTriangles_Code());
    pkt.AddMicroProgram(s_particlesProgAddr, VU1Prog_Particles_Code());
    pkt.AddDoubleBufferSettings(kDoubleBufferBase, kDoubleBufferOffset);
    pkt.AddEndTag();
    pkt.Send();
    pkt.Wait();
}

void BeginFrame()
{
    if (s_frameSubmittedBytes > s_peakSubmittedBytes)
    {
        s_peakSubmittedBytes = s_frameSubmittedBytes;
    }
    s_frameSubmittedBytes = 0;
}

int PeakFrameSubmittedBytes()
{
    return s_peakSubmittedBytes;
}

int FrameSubmittedBytes()
{
    return s_frameSubmittedBytes;
}

// ------------------------------------------------------------------------------------------------
// Generic VU1 triangles
// ------------------------------------------------------------------------------------------------

// Vertices per VU run: DrawTriangles splits larger draws into chunks of this
// size. Bounded by the VU double buffer: input (8 + 2n) plus output (7 + 3n)
// qwords must fit in one 496-qword buffer half, so n <= 96 - and chunks are
// whole triangles, hence 96.
constexpr int kMaxVertsPerBatch = 96;

// Batch layout, relative to the current double buffer (XTOP).
constexpr int kBatchHeaderAddr = 0; // vertex count in .w
constexpr int kVertexDataAddr  = kGifTagsAddr + kNumGifTagQwords;

// Emits one chunk into the chain: batch header and GIF tags unpacked inline
// to the current double buffer, the vertex data referenced in place, and the
// MSCAL that runs the microprogram over it.
static void AddBatchChunk(VifPacket & pkt, const tex::Texture & texture, int ctx,
                          const DrawVertex * verts, int vertCount, DrawFlags flags)
{
    PS2_Assert(vertCount > 0 && vertCount <= kMaxVertsPerBatch && (vertCount % 3) == 0);
    pkt.EnsureSpace(kChunkChainQwords + kChainTailQwords);

    pkt.OpenInlineUnpack(kBatchHeaderAddr, true);
    {
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(static_cast<u32>(vertCount));

        AddBatchGifTags(pkt, texture, ctx, vertCount, flags);
    }
    pkt.CloseInlineUnpack();

    pkt.AddUnpackData(kVertexDataAddr, verts, static_cast<u32>(vertCount * 2), true);

    pkt.AddStartProgram(s_texturedTrisProgAddr);
}

void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const DrawVertex * verts, int vertCount, DrawFlags flags)
{
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(verts) & 15u) == 0, "Vertex data must be 16-byte aligned!");

    // Send any 2D accumulated before this 3D burst so it draws underneath (and
    // its textures are consumed before our uploads can evict them). A no-op once
    // the batch is already flushed - only the first 3D draw after 2D pays it.
    gs::FlushPending2D();

    gs::EnsureTextureResident(texture);
    PS2_Assert(texture.vramAddr != tex::Texture::kNotResident);

    const int ctx = gs::CurrentContext();

    s_frameSubmittedBytes += vertCount * static_cast<int>(sizeof(DrawVertex));

    VifPacket & pkt = s_drawPacket;
    BeginDrawChain(pkt, mvp, flags);

    // One chunk per VU run; the double buffer overlaps each chunk's unpack
    // with the previous chunk's transform.
    for (int firstVert = 0; firstVert < vertCount; firstVert += kMaxVertsPerBatch)
    {
        // If the next chunk plus the chain tail might not fit the packet,
        // send what we have and open a fresh, self-contained chain. The
        // Wait() makes this safe: everything referenced so far was consumed.
        if (pkt.QwordCount() + kChunkChainQwords + kChainTailQwords > kDrawPacketQwords)
        {
            SendChainAndWait(pkt);
            BeginDrawChain(pkt, mvp, flags);
        }

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < kMaxVertsPerBatch) ? remaining : kMaxVertsPerBatch;
        AddBatchChunk(pkt, texture, ctx, verts + firstVert, chunkVerts, flags);
    }

    SendChainAndWait(pkt);
}

// ------------------------------------------------------------------------------------------------
// Keyframe-lerped triangles
// ------------------------------------------------------------------------------------------------

// Lerped-triangles batch layout (must match lerped_triangles.vcl)

// Vertices per lerped VU run: the 3-qword-per-vertex batch (2 position
// qwords + 1 attribute) fits fewer than the world path's 96. Whole
// triangles, and even - so every full chunk's slice of the 8-byte position
// stream is whole source qwords starting 16-byte aligned.
constexpr int kMaxLerpVertsPerBatch = 78;

// Chain footprint of one lerped chunk: header/frontv/backv/tags inline
// unpack (1 + 10 qwords), two REF unpacks, FLUSH + MSCAL; ~15 in practice.
constexpr int kLerpChunkChainQwords = 20;

// The regions sit at fixed offsets sized for the maximum chunk (short
// chunks leave gaps), so the microprogram addresses them with immediates.
constexpr int kLerpBatchHeaderAddr = 0; // vertex count in .w
constexpr int kLerpFrontVAddr      = 1; // current frame scale * (1 - backlerp)
constexpr int kLerpBackVAddr       = 2; // old frame scale * backlerp
constexpr int kLerpGifTagsAddr     = 3; // the same 7-qword block as the world path
constexpr int kLerpPositionsAddr   = kLerpGifTagsAddr + kNumGifTagQwords;              // 2 qwords per vertex
constexpr int kLerpAttribsAddr     = kLerpPositionsAddr + (2 * kMaxLerpVertsPerBatch); // 1 qword per vertex
constexpr int kLerpOutputAddr      = kLerpAttribsAddr + kMaxLerpVertsPerBatch;         // the GS packet

static_assert(kLerpFrontVAddr == 1 && kLerpBackVAddr == 2 && kLerpPositionsAddr == 10 && kLerpAttribsAddr == 166 && kLerpOutputAddr == 244, "Batch layout must match the #defines in lerped_triangles.vcl");
static_assert(kLerpOutputAddr + kNumGifTagQwords + (3 * kMaxLerpVertsPerBatch) <= kDoubleBufferOffset, "Lerp batch input + GS packet must fit one double-buffer half");
static_assert((kMaxLerpVertsPerBatch % 3) == 0, "Lerp chunks are whole triangles");
static_assert((kMaxLerpVertsPerBatch % 2) == 0, "Lerp chunk position slices must be whole qwords");

// The lerped equivalent: header (count + the two lerp scale vectors) and GIF
// tags inline, then the two vertex streams, then the MSCAL. The byte-position
// DMA must be whole source qwords, so an odd count transfers one pad vertex
// the VU never reads (the fixed region has room: odd counts are < the even maximum).
static void AddLerpBatchChunk(VifPacket & pkt, const tex::Texture & texture, int ctx,
                              const math::Vec3 & frontv, const math::Vec3 & backv,
                              const LerpVertexBytes * positions, const LerpDrawAttrib * attribs,
                              int vertCount, FaceCull faceCull, DrawFlags flags)
{
    PS2_Assert(vertCount > 0 && vertCount <= kMaxLerpVertsPerBatch && (vertCount % 3) == 0);
    pkt.EnsureSpace(kLerpChunkChainQwords + kChainTailQwords);

    pkt.OpenInlineUnpack(kLerpBatchHeaderAddr, true);
    {
        pkt.AddU32(static_cast<u32>(faceCull)); // backface cull mode in .x
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(static_cast<u32>(vertCount));

        pkt.AddFloat(frontv.x);
        pkt.AddFloat(frontv.y);
        pkt.AddFloat(frontv.z);
        pkt.AddFloat(0.0f); // .w rides through the lerp; keep it finite

        pkt.AddFloat(backv.x);
        pkt.AddFloat(backv.y);
        pkt.AddFloat(backv.z);
        pkt.AddFloat(0.0f);

        AddBatchGifTags(pkt, texture, ctx, vertCount, flags);
    }
    pkt.CloseInlineUnpack();

    // The keyframe bytes: V4_8 elements, one source word and two destination
    // qwords per vertex, padded to an even vertex count so the transfer is
    // whole qwords (every word the DMA carries must be unpack payload).
    const int srcVerts = vertCount + (vertCount & 1);
    pkt.AddUnpackDataFmt(kLerpPositionsAddr, positions,
                         static_cast<u32>(srcVerts / 2), // qwords: 8 bytes per vertex
                         static_cast<u32>(srcVerts * 2), // elements: 2 per vertex
                         P2_UNPACK_V4_8, true);

    pkt.AddUnpackData(kLerpAttribsAddr, attribs, static_cast<u32>(vertCount), true);

    pkt.AddStartProgram(s_lerpedProgAddr);
}

void DrawLerpedTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                         const math::Vec3 & frontv, const math::Vec3 & backv,
                         const LerpVertexBytes * positions, const LerpDrawAttrib * attribs,
                         int vertCount, FaceCull faceCull, DrawFlags flags)
{
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawLerpedTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(positions) & 15u) == 0, "Position data must be 16-byte aligned!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(attribs) & 15u) == 0, "Attribute data must be 16-byte aligned!");

    gs::FlushPending2D();

    gs::EnsureTextureResident(texture);
    PS2_Assert(texture.vramAddr != tex::Texture::kNotResident);

    const int ctx = gs::CurrentContext();

    // Both streams, plus the odd-count pad element the position DMA carries.
    s_frameSubmittedBytes += (vertCount + (vertCount & 1)) * static_cast<int>(sizeof(LerpVertexBytes))
                          + vertCount * static_cast<int>(sizeof(LerpDrawAttrib));

    VifPacket & pkt = s_drawPacket;
    BeginDrawChain(pkt, mvp, flags);

    // Chunking as in DrawTriangles. Full chunks are even, so every chunk's
    // slice of the 8-byte position stream starts 16-byte aligned; only a
    // final odd chunk pads its transfer (see AddLerpBatchChunk).
    for (int firstVert = 0; firstVert < vertCount; firstVert += kMaxLerpVertsPerBatch)
    {
        if (pkt.QwordCount() + kLerpChunkChainQwords + kChainTailQwords > kDrawPacketQwords)
        {
            SendChainAndWait(pkt);
            BeginDrawChain(pkt, mvp, flags);
        }

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < kMaxLerpVertsPerBatch) ? remaining : kMaxLerpVertsPerBatch;
        AddLerpBatchChunk(pkt, texture, ctx, frontv, backv,
                          positions + firstVert, attribs + firstVert, chunkVerts, faceCull, flags);
    }

    SendChainAndWait(pkt);
}

// ------------------------------------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------------------------------------

// Particle batch layout (must match particles.vcl)

constexpr int kPrtBatchHeaderAddr = 0;  // particle count in .w
constexpr int kPrtQuadOffsetAddr  = 1;  // clip-space corner offset in .xyz, blow-up rate in .w
constexpr int kPrtUV0Addr         = 2;  // anchor corner UV
constexpr int kPrtUV1Addr         = 3;  // opposite corner UV
constexpr int kPrtGifTagsAddr     = 4;  // the same 7-qword block as the world path
constexpr int kPrtDataAddr        = kPrtGifTagsAddr + kNumGifTagQwords; // 1 qword per particle

// Particles per VU run. Input is 1 qword each and the sprite output 5, so a
// chunk occupies kPrtDataAddr + 6n qwords of a double-buffer half; 78 leaves a
// little room under the 496 the halves have.
constexpr int kMaxParticlesPerBatch = 78;

static_assert(kPrtQuadOffsetAddr == 1 && kPrtUV0Addr == 2 && kPrtUV1Addr == 3 && kPrtGifTagsAddr == 4 && kPrtDataAddr == 11, "Batch layout must match the #defines in particles.vcl");
static_assert(kPrtDataAddr + (6 * kMaxParticlesPerBatch) <= kDoubleBufferOffset, "Particle batch input + GS packet must fit one double-buffer half");

// Chain footprint of one particle chunk: the 11-qword header/constants/tags
// inline unpack, one REF unpack, FLUSH + MSCAL; ~16 in practice.
constexpr int kPrtChunkChainQwords = 22;

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

// Emits one particle chunk: the header, the batch constants and the GIF tags
// unpacked inline, the particles referenced in place, and the MSCAL.
//
// 'clipOffset' is the corner offset already transformed to clip space; the UVs
// are in the GS 12.4 fixed point the PACKED UV descriptor wants.
static void AddParticleChunk(VifPacket & pkt, const tex::Texture & texture, int ctx,
                             const math::Vec4 & clipOffset, u32 uvMaxU, u32 uvMaxV,
                             const ParticleVertex * particles, int count, DrawFlags flags)
{
    PS2_Assert(count > 0 && count <= kMaxParticlesPerBatch);
    pkt.EnsureSpace(kPrtChunkChainQwords + kChainTailQwords);

    pkt.OpenInlineUnpack(kPrtBatchHeaderAddr, true);
    {
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(static_cast<u32>(count));

        // The corner offset, with the distance blow-up rate riding in the .w the
        // offset itself has no use for (it is a direction, so its w is zero).
        pkt.AddFloat(clipOffset.x);
        pkt.AddFloat(clipOffset.y);
        pkt.AddFloat(clipOffset.z);
        pkt.AddFloat(kParticleBlowUpRate);

        // The two corner UVs. PACKED UV takes U in word 0 and V in word 1; the
        // upper half of the qword is not part of the descriptor.
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(0);

        pkt.AddU32(uvMaxU);
        pkt.AddU32(uvMaxV);
        pkt.AddU32(0);
        pkt.AddU32(0);

        const bool blended = AddBatchStateBlock(pkt, texture, ctx, flags);
        const int  abe     = blended ? 1 : 0; // Hoisted: see the note in AddBatchGifTags.

        // The drawing tag: one sprite per particle, five registers each - the
        // A+D that sets its colour, then a UV/XYZ2 pair per corner. FST selects
        // UV over ST: a screen-aligned sprite needs no perspective correction.
        const u64 prim = GIF_SET_PRIM(PRIM_SPRITE, 0, 1, 0, abe, 0, 1, ctx, 0);
        pkt.AddQword(GIF_SET_TAG(count, 1, 1, prim, GIF_FLG_PACKED, 5), kParticleRegList);
    }
    pkt.CloseInlineUnpack();

    pkt.AddUnpackData(kPrtDataAddr, particles, static_cast<u32>(count), true);

    pkt.AddStartProgram(s_particlesProgAddr);
}

void DrawParticles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const math::Vec3 & quadOffset, const ParticleVertex * particles,
                   int count, DrawFlags flags)
{
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    PS2_AssertMsg(count > 0, "DrawParticles wants at least one particle!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(particles) & 15u) == 0, "Particle data must be 16-byte aligned!");

    gs::FlushPending2D();

    gs::EnsureTextureResident(texture);
    PS2_Assert(texture.vramAddr != tex::Texture::kNotResident);

    const int ctx = gs::CurrentContext();

    s_frameSubmittedBytes += count * static_cast<int>(sizeof(ParticleVertex));

    // The corner offset transforms once for the whole call, as a direction
    // (w = 0). Because it is orthogonal to the view axis its clip z and w both
    // come out zero, which is what lets the microprogram reuse the centre's
    // depth and 1/w for both corners - see particles.vcl.
    const math::Vec4 clipOffset = math::Transform(
        math::Vec4{ quadOffset.x, quadOffset.y, quadOffset.z, 0.0f }, mvp);

    // Corner UVs in the GS 12.4 fixed point, spanning the whole image.
    // Particle images are power-of-two, so no ST rescale applies here.
    const u32 uvMaxU = static_cast<u32>(texture.width)  << 4;
    const u32 uvMaxV = static_cast<u32>(texture.height) << 4;

    VifPacket & pkt = s_drawPacket;
    BeginDrawChain(pkt, mvp, flags);

    for (int first = 0; first < count; first += kMaxParticlesPerBatch)
    {
        if (pkt.QwordCount() + kPrtChunkChainQwords + kChainTailQwords > kDrawPacketQwords)
        {
            SendChainAndWait(pkt);
            BeginDrawChain(pkt, mvp, flags);
        }

        const int remaining  = count - first;
        const int chunkCount = (remaining < kMaxParticlesPerBatch) ? remaining : kMaxParticlesPerBatch;
        AddParticleChunk(pkt, texture, ctx, clipOffset, uvMaxU, uvMaxV,
                         particles + first, chunkCount, flags);
    }

    SendChainAndWait(pkt);
}

} // namespace ps2::vu1
