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
 *  qword (vertex count in .w), 5 GIF/AD tag qwords, then 2 qwords per vertex;
 *  the microprogram builds the GS packet in the same buffer after the input.
 *  The A+D block programs TEST as well as TEX0/TEX1, so a batch draws with the
 *  proper z-test no matter what state the surrounding 2D packets left behind.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/texture.h"

#include <dma.h>
#include <draw.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>

namespace ps2::vu1 {

PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_TexturedTriangles);

namespace {

// Vertices per VU run: DrawTriangles splits larger draws into chunks of this
// size. Bounded by the VU double buffer: input (6 + 2n) plus output (5 + 3n)
// qwords must fit in one 496-qword buffer half, so n <= 97 - and chunks are
// whole triangles, hence 96.
constexpr int kMaxVertsPerBatch = 96;

// VIF1 double-buffer registers: two 496-qword buffers above the constants.
constexpr int kDoubleBufferBase   = 8;
constexpr int kDoubleBufferOffset = 496;

// Frame constants at fixed low VU addresses (below kDoubleBufferBase).
constexpr int kFrameConstantsAddr = 0;

// Batch layout, relative to the current double buffer (XTOP).
constexpr int kBatchHeaderAddr = 0; // vertex count in .w
constexpr int kGifTagsAddr     = 1; // 5 qwords: GIF set tag, TEST, TEX1, TEX0, prim tag
constexpr int kVertexDataAddr  = kGifTagsAddr + 5;

// The chain is tags plus small per-chunk inline unpacks; constants and
// vertices are referenced in place. Sized so a DrawTriangles call fits ~30
// chunks (~2900 verts) before it must flush the chain mid-call.
constexpr int kDrawPacketQwords = 512;

// Conservative chain footprint of one chunk segment (header/tags inline
// unpack, vertex REF unpack, FLUSH + MSCAL; ~11 qwords in practice) and of
// the chain tail (trailing FLUSH + END tag). DrawTriangles flushes the packet
// when the next chunk plus the tail might not fit.
constexpr int kChunkChainQwords = 16;
constexpr int kChainTailQwords  = 4;

// Depth scale: the microprogram's ftoi4 multiplies by 16, so scale + offset of
// 0xFFFF/32 maps z/w [-1 (far), +1 (near)] onto [0, 0xFFFF] in the 16-bit z-buffer.
constexpr float kGsDepthScale = static_cast<float>(0xFFFF) / 32.0f;

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
// them against |w|, so triangles survive out to |ndc| = 0.8 - about 5x the
// half-screen (the visible screen ends at ndc 640/4096 = 0.15) while staying
// inside the representable 12.4 coordinate range. The GS scissor does the
// actual on-screen cut; only triangles beyond the band (or crossing the
// near/far planes, z scale 1) are dropped whole via the ADC bit.
constexpr float kGuardBandScale = 1.25f;

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

// TEX0/TEX1 register qwords for the batch's texture bind, sent A+D over PATH1.
// Built here rather than with the packet2_utils helpers because those hardcode
// GS context 0 and this renderer alternates contexts per frame.
u64 MakeTex0Data(const tex::Texture & texture)
{
    // Palettized textures sample through the global-palette CLUT; reloading
    // the on-chip CLUT cache on every bind is cheap (1 KB). Everything else
    // leaves the CLUT fields zero (as gs::SetTextureFor2D).
    const bool palettized = (texture.format == tex::PixelFormat::Palette8);

    return GS_SET_TEX0(texture.texbuf.address >> 6,
                       texture.texbuf.width >> 6,
                       texture.texbuf.psm,
                       texture.texbuf.info.width,
                       texture.texbuf.info.height,
                       texture.texbuf.info.components,
                       texture.texbuf.info.function,
                       palettized ? ((int)gs::GlobalClutAddress() >> 6) : 0,
                       GS_PSM_32, // CPSM; only read for palettized PSMs (and == 0 anyway)
                       CLUT_STORAGE_MODE1, 0,
                       palettized ? CLUT_LOAD : CLUT_NO_LOAD);
}

u64 MakeTex1Data(const tex::Texture & texture)
{
    return GS_SET_TEX1(LOD_USE_K, 0,
                       tex::GsMagFilter(texture.magFilter),
                       tex::GsMinFilter(texture.minFilter),
                       LOD_MIPMAP_REGISTER, 0, 0);
}

// Pixel tests for the batch: the environment's alpha test plus the real
// z-test (mirrors libdraw's draw_enable_tests).
u64 MakeTestData()
{
    return GS_SET_TEST(DRAW_ENABLE, ATEST_METHOD_NOTEQUAL, 0x00, ATEST_KEEP_FRAMEBUFFER,
                       DRAW_DISABLE, DRAW_DISABLE,
                       DRAW_ENABLE, gs::DepthTestMethod());
}

// Emits one chunk into the chain: batch header and GIF tags unpacked inline
// to the current double buffer, the vertex data referenced in place, and the
// MSCAL that runs the microprogram over it.
void AddBatchChunk(VifPacket & pkt, const tex::Texture & texture, int ctx,
                   const DrawVertex * verts, int vertCount)
{
    PS2_Assert(vertCount > 0 && vertCount <= kMaxVertsPerBatch && (vertCount % 3) == 0);
    pkt.EnsureSpace(kChunkChainQwords + kChainTailQwords);

    pkt.OpenInlineUnpack(kBatchHeaderAddr, true);
    {
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(static_cast<u32>(vertCount));

        // Three A+D register writes: pixel tests and the texture bind for
        // this context...
        pkt.AddQword(GIF_SET_TAG(3, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        pkt.AddQword(MakeTestData(), static_cast<u64>(GS_REG_TEST + ctx));
        pkt.AddQword(MakeTex1Data(texture), static_cast<u64>(GS_REG_TEX1 + ctx));
        pkt.AddQword(MakeTex0Data(texture), static_cast<u64>(GS_REG_TEX0 + ctx));

        // ...then the drawing tag: gouraud textured triangle list, STQ mapping,
        // with the per-vertex registers of kVertexRegList.
        const u128 prim = VU_GS_PRIM(PRIM_TRIANGLE, 1, 1, 0, 0, 0, 0, ctx, 0);
        pkt.AddQword(VU_GS_GIFTAG(static_cast<u64>(vertCount), 1, 1, prim, 0, 3),
                     kVertexRegList);
    }
    pkt.CloseInlineUnpack();

    pkt.AddUnpackData(kVertexDataAddr, verts, static_cast<u32>(vertCount * 2), true);

    pkt.AddStartProgram(0);
}

// FLUSH so a DMA wait covers the VU runs and their XGKICKs, then terminate
// and send the chain, blocking until it is fully consumed.
void SendChainAndWait(VifPacket & pkt)
{
    pkt.AddFlush();
    pkt.AddEndTag();
    pkt.Send();
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

    const auto instructions = VU1Prog_TexturedTriangles_InstructionQwordCount();
    PS2_AssertMsg(instructions <= 2048, "Microprogram overflows VU1 micro memory!");

    s_drawPacket.Init(kDrawPacketQwords);

    // Upload the microprogram to micro address 0 and set up the double buffer.
    // Synchronous; VU1 is ready once this returns.
    VifPacket & pkt = s_drawPacket;
    pkt.AddMicroProgram(0, VU1Prog_TexturedTriangles_Code());
    pkt.AddDoubleBufferSettings(kDoubleBufferBase, kDoubleBufferOffset);
    pkt.AddEndTag();
    pkt.Send();
    pkt.Wait();
}

void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const DrawVertex * verts, int vertCount)
{
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    PS2_AssertMsg(!gs::In2DMode(), "No 3D drawing inside the 2D section!");
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawTriangles wants whole triangles!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(verts) & 15u) == 0, "Vertex data must be 16-byte aligned!");

    gs::EnsureTextureResident(texture);
    PS2_Assert(texture.vramAddr != tex::Texture::kNotResident);

    const int ctx = gs::CurrentContext();

    s_constants.mvp       = mvp;
    s_constants.gsScale   = { 2048.0f, 2048.0f, kGsDepthScale, 0.0f };
    s_constants.gsOffset  = { 2048.0f + static_cast<float>(gs::Width())  * 0.5f,
                              2048.0f + static_cast<float>(gs::Height()) * 0.5f,
                              kGsDepthScale, 0.0f };
    s_constants.clipScale = { kGuardBandScale, kGuardBandScale, 1.0f, 0.0f };

    VifPacket & pkt = s_drawPacket;
    pkt.Reset();

    pkt.AddUnpackData(kFrameConstantsAddr, &s_constants, sizeof(FrameConstants) / 16, false);

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
            pkt.Reset();
            pkt.AddUnpackData(kFrameConstantsAddr, &s_constants, sizeof(FrameConstants) / 16, false);
        }

        const int remaining  = vertCount - firstVert;
        const int chunkVerts = (remaining < kMaxVertsPerBatch) ? remaining : kMaxVertsPerBatch;
        AddBatchChunk(pkt, texture, ctx, verts + firstVert, chunkVerts);
    }

    SendChainAndWait(pkt);
}

} // namespace ps2::vu1
