/* ================================================================================================
 * File: gs.cpp
 * Brief: Double-buffered Graphics Synthesizer front-end. See gs.h.
 *
 *  Modelled on the ps2sdk libdraw "font"/"cube" samples: two 32-bit framebuffers
 *  in VRAM, one displayed while the other is drawn, using the two GS drawing
 *  contexts. draw_setup_environment programs each context so screen coordinates
 *  are direct top-left pixels. The z-buffer is disabled for the 2D-only path.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/gs.h"

#include <kernel.h>
#include <dma.h>
#include <dma_tags.h>
#include <gs_psm.h>
#include <graph.h>
#include <draw.h>
#include <draw2d.h>
#include <draw_buffers.h>
#include <packet.h>

namespace ps2::gs {
namespace {

constexpr int kWidth        = 640;
constexpr int kHeight       = 448;
constexpr int kPacketQwords = 8192; // headroom for a full 2D screen (clear + rects/sprites)

static framebuffer_t s_frame[2];
static zbuffer_t     s_zbuffer;
static packet_t *    s_packet[2] = { nullptr, nullptr };
static qword_t *     s_q = nullptr;

static int s_drawCtx   = 1; // which framebuffer/context we render into this frame
static int s_packetIdx = 0; // which DMA packet buffer is being filled

static std::uint8_t s_clear[3] = { 0x20, 0x20, 0x38 }; // distinctive dark blue

inline int QwordCount(const packet_t * pkt)
{
    return static_cast<int>(s_q - pkt->data);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

int Width()  { return kWidth; }
int Height() { return kHeight; }

void SetClearColor(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    s_clear[0] = r;
    s_clear[1] = g;
    s_clear[2] = b;
}

void Init()
{
    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // Two 32-bit framebuffers.
    s_frame[0].width   = kWidth;
    s_frame[0].height  = kHeight;
    s_frame[0].mask    = 0;
    s_frame[0].psm     = GS_PSM_32;
    s_frame[0].address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_PSM_32, GRAPH_ALIGN_PAGE));

    s_frame[1]         = s_frame[0];
    s_frame[1].address = static_cast<unsigned int>(graph_vram_allocate(kWidth, kHeight, GS_PSM_32, GRAPH_ALIGN_PAGE));

    // Z-buffer disabled for the 2D path (frees VRAM; re-enable for the 3D world).
    s_zbuffer.enable  = 0;
    s_zbuffer.method  = ZTEST_METHOD_GREATER;
    s_zbuffer.address = 0;
    s_zbuffer.mask    = 1;
    s_zbuffer.zsm     = 0;

    // Display framebuffer 0 first; auto-detects NTSC/PAL.
    graph_initialize(static_cast<int>(s_frame[0].address), kWidth, kHeight, GS_PSM_32, 0, 0);

    s_packet[0] = packet_init(kPacketQwords, PACKET_NORMAL);
    s_packet[1] = packet_init(kPacketQwords, PACKET_NORMAL);

    // Program both drawing contexts: context 0 -> frame 0, context 1 -> frame 1.
    packet_t * pkt = s_packet[0];
    s_q = pkt->data;
    s_q = draw_setup_environment(s_q, 0, &s_frame[0], &s_zbuffer);
    s_q = draw_setup_environment(s_q, 1, &s_frame[1], &s_zbuffer);
    // Alpha blending on, so translucent 2D (fade screen) works; a==255 stays opaque.
    draw_enable_blending();
    s_q = draw_finish(s_q);

    dma_channel_send_normal(DMA_CHANNEL_GIF, pkt->data, QwordCount(pkt), 0, 0);
    dma_wait_fast();
    draw_wait_finish();

    s_drawCtx   = 1;
    s_packetIdx = 0;
}

void BeginFrame()
{
    s_packetIdx ^= 1;
    packet_t * pkt = s_packet[s_packetIdx];
    s_q = pkt->data;

    s_q = draw_clear(s_q, s_drawCtx,
                     0.0f, 0.0f,
                     static_cast<float>(kWidth), static_cast<float>(kHeight),
                     static_cast<int>(s_clear[0]), static_cast<int>(s_clear[1]), static_cast<int>(s_clear[2]));
}

void FillRect(int x, int y, int w, int h,
              std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    rect_t rect;
    rect.v0.x = static_cast<float>(x);
    rect.v0.y = static_cast<float>(y);
    rect.v0.z = 0u;
    rect.v1.x = static_cast<float>(x + w);
    rect.v1.y = static_cast<float>(y + h);
    rect.v1.z = 0u;
    rect.color.r = r;
    rect.color.g = g;
    rect.color.b = b;
    rect.color.a = a;
    rect.color.q = 1.0f;

    s_q = draw_rect_filled(s_q, s_drawCtx, &rect);
}

void EndFrame()
{
    s_q = draw_finish(s_q);

    packet_t * pkt = s_packet[s_packetIdx];
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, pkt->data, QwordCount(pkt), 0, 0);
    draw_wait_finish();

    graph_wait_vsync();
    graph_set_framebuffer_filtered(static_cast<int>(s_frame[s_drawCtx].address),
                                   static_cast<int>(s_frame[s_drawCtx].width),
                                   static_cast<int>(s_frame[s_drawCtx].psm), 0, 0);

    s_drawCtx ^= 1; // draw into the other buffer next frame
}

} // namespace ps2::gs
