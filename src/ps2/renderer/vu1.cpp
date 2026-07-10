/* ================================================================================================
 * File: vu1.cpp
 * Brief: VU1-accelerated 3D drawing. See vu1.h.
 *
 *  Modelled on the ps2sdk "draw/vu1" sample. Each DrawTriangles call builds one VIF1
 *  source chain: frame constants (MVP + GS screen mapping) unpacked to fixed low VU
 *  addresses, then the batch (header, GIF tags, vertices) unpacked at the current
 *  double buffer, then FLUSH + MSCAL to run the microprogram, which transforms,
 *  clips and XGKICKs the triangles to the GS over PATH1.
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
 *  qword (vertex count in .w), 5 GIF/AD tag qwords, then 3 qwords per vertex;
 *  the microprogram builds the GS packet in the same buffer after the input.
 *  The A+D block programs TEST as well as TEX0/TEX1, so a batch draws with the
 *  proper z-test no matter what state the surrounding 2D packets left behind.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/vif_packet.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/texture.h"

#include <dma.h>
#include <draw.h>
#include <gif_tags.h>
#include <gs_gp.h>

namespace ps2::vu1 {

PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_TexturedTriangles);

namespace {

// VIF1 double-buffer registers: two 496-qword buffers above the constants.
constexpr int kDoubleBufferBase   = 8;
constexpr int kDoubleBufferOffset = 496;

// Frame constants at fixed low VU addresses (below kDoubleBufferBase).
constexpr int kFrameConstantsAddr = 0;

// Batch layout, relative to the current double buffer (XTOP).
constexpr int kBatchHeaderAddr = 0; // vertex count in .w
constexpr int kGifTagsAddr     = 1; // 5 qwords: GIF set tag, TEST, TEX1, TEX0, prim tag
constexpr int kVertexDataAddr  = kGifTagsAddr + 5;

// The chain is tags plus one small inline unpack; constants and vertices are
// referenced in place.
constexpr int kDrawPacketQwords = 64;

// Depth scale: the microprogram's ftoi4 multiplies by 16, so scale + offset of
// 0xFFFFFF/32 maps z/w [-1 (far), +1 (near)] onto [0, 0xFFFFFF] in the z-buffer.
constexpr float kGsDepthScale = static_cast<float>(0xFFFFFF) / 32.0f;

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
std::uint64_t MakeTex0Data(const tex::Texture & texture)
{
    // No palettized formats in use, so the CLUT fields stay zero (as gs::SetTexture).
    return GS_SET_TEX0(texture.texbuf.address >> 6,
                       texture.texbuf.width >> 6,
                       texture.texbuf.psm,
                       texture.texbuf.info.width,
                       texture.texbuf.info.height,
                       texture.texbuf.info.components,
                       texture.texbuf.info.function,
                       0, 0, CLUT_STORAGE_MODE1, 0, CLUT_NO_LOAD);
}

std::uint64_t MakeTex1Data(const tex::Texture & texture)
{
    return GS_SET_TEX1(LOD_USE_K, 0,
                       tex::GsMagFilter(texture.magFilter),
                       tex::GsMinFilter(texture.minFilter),
                       LOD_MIPMAP_REGISTER, 0, 0);
}

// Pixel tests for the batch: the environment's alpha test plus the real
// z-test (mirrors libdraw's draw_enable_tests).
std::uint64_t MakeTestData()
{
    return GS_SET_TEST(DRAW_ENABLE, ATEST_METHOD_NOTEQUAL, 0x00, ATEST_KEEP_FRAMEBUFFER,
                       DRAW_DISABLE, DRAW_DISABLE,
                       DRAW_ENABLE, gs::DepthTestMethod());
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

    const auto instructions = (&VU1Prog_TexturedTriangles_CodeEnd - &VU1Prog_TexturedTriangles_CodeStart) / 2;
    PS2_AssertMsg(instructions <= 2048, "Microprogram overflows VU1 micro memory!");

    s_drawPacket.Init(kDrawPacketQwords);

    // Upload the microprogram to micro address 0 and set up the double buffer.
    // Synchronous; VU1 is ready once this returns.
    VifPacket & pkt = s_drawPacket;
    pkt.AddMicroProgram(0, &VU1Prog_TexturedTriangles_CodeStart, &VU1Prog_TexturedTriangles_CodeEnd);
    pkt.AddDoubleBufferSettings(kDoubleBufferBase, kDoubleBufferOffset);
    pkt.AddEndTag();
    pkt.Send();
    pkt.Wait();
}

void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const DrawVertex * verts, int vertCount)
{
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    PS2_AssertMsg(vertCount > 0 && (vertCount % 3) == 0, "DrawTriangles wants whole triangles!");
    PS2_AssertMsg(vertCount <= kMaxVertsPerBatch, "Batch too large for the VU double buffer!");
    PS2_AssertMsg(texture.vramAddr != tex::Texture::kNotResident, "Texture not resident in VRAM!");
    PS2_AssertMsg((reinterpret_cast<std::uintptr_t>(verts) & 15u) == 0, "Vertex data must be 16-byte aligned!");

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

    // Batch header and the GIF tags the microprogram prepends to its output.
    pkt.OpenInlineUnpack(kBatchHeaderAddr, true);
    {
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(0);
        pkt.AddU32(static_cast<std::uint32_t>(vertCount));

        // Three A+D register writes: pixel tests and the texture bind for
        // this context...
        pkt.AddQword(GIF_SET_TAG(3, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        pkt.AddQword(MakeTestData(), static_cast<std::uint64_t>(GS_REG_TEST + ctx));
        pkt.AddQword(MakeTex1Data(texture), static_cast<std::uint64_t>(GS_REG_TEX1 + ctx));
        pkt.AddQword(MakeTex0Data(texture), static_cast<std::uint64_t>(GS_REG_TEX0 + ctx));

        // ...then the drawing tag: gouraud textured triangle list, STQ mapping,
        // ST before RGBAQ so the GS latches Q for perspective-correct texturing.
        const u128 prim = VU_GS_PRIM(PRIM_TRIANGLE, 1, 1, 0, 0, 0, 0, ctx, 0);
        pkt.AddQword(VU_GS_GIFTAG(static_cast<std::uint64_t>(vertCount), 1, 1, prim, 0, 3),
                     DRAW_STQ2_REGLIST);
    }
    pkt.CloseInlineUnpack();

    pkt.AddUnpackData(kVertexDataAddr, verts, vertCount * 3, true);

    pkt.AddStartProgram(0);
    pkt.AddFlush(); // so Wait() covers the VU run and its XGKICKs, not just the DMA
    pkt.AddEndTag();

    pkt.Send();
    pkt.Wait();
}

} // namespace ps2::vu1
