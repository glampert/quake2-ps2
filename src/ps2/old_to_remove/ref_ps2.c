
/* ================================================================================================
 * -*- C -*-
 * File: ref_ps2.c
 * Author: Guilherme R. Lampert
 * Created on: 13/10/15
 * Brief: Implementation of Quake2 "refresh" methods (renderer module) for the PS2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "common/q_common.h"
#include "ps2/ref_ps2.h"
#include "ps2/mem_alloc.h"
#include "ps2/model_load.h"
#include "ps2/vu1.h"

// PS2DEV SDK:
#include <kernel.h>
#include <dma_tags.h>
#include <gif_tags.h>
#include <gs_psm.h>
#include <dma.h>
#include <graph.h>
#include <draw.h>

//TODO:
// All these static buffers we use in here and in teximage.c/model.c should
// be allocated dynamically on the heap, so we can keep track of the used memory.
// they are resident blocks, so the end result is the same as if we had
// static memory, the difference is that we'd increment the mem tags.

//=============================================================================

// Renderer globals (alignment not strictly required, but shouldn't harm either...)
ps2_refresh_t ps2ref PS2_ALIGN(16) = {0};

// Renderer perf counters for debugging and profiling:
static int ps2_draws2d      = 0;
static int ps2_tex_uploads  = 0;
static int ps2_pipe_flushes = 0;

// Config vars:
static cvar_t * r_ps2_vid_width         = NULL; // Horizontal video resolution in pixels.
static cvar_t * r_ps2_vid_height        = NULL; // Vertical video resolution in pixels.
static cvar_t * r_ps2_ui_brightness     = NULL; // UI brightness adjustment from 0 to 255. Default to 128.
static cvar_t * r_ps2_fade_scr_alpha    = NULL; // Fade screen opacity from 0 to 255. Defaults to 100.
static cvar_t * r_ps2_show_fps          = NULL; // Show FPS counter and frame times on screen; "1" by default.
static cvar_t * r_ps2_show_mem_tags     = NULL; // Show memory usage on screen; "1" by default.
static cvar_t * r_ps2_show_render_stats = NULL; // Show renderer statistics, like models/textures loaded; "1" by default.
static cvar_t * r_ps2_skip_render_frame = NULL; // Skips PS2_RenderFrame() entirely; "0" by default.

// Average multiple frames together to smooth changes out a bit.
enum { MAX_FPS_HIST = 4 };
static struct
{
    int index;
    int count;
    int previous_time;
    int times_hist[MAX_FPS_HIST];
} ps2_fps_counter;

//
// All 2D draw calls are batched and flushed at the end of each frame.
// Uses uint16s since we need quite a few of these. See PS2_Flush2DBatch().
//
typedef struct
{
    s16 tex_index;
    u16 z_index;
    u16 x0, y0;
    u16 x1, y1;
    u16 u0, v0;
    u16 u1, v1;
    byte r, g, b, a;
} ps2_screen_quad_t;

enum
{
    DRAW2D_BATCH_SIZE         =  8000, // size of ps2_draw2d_batch[] in elements
    DRAW2D_TEX_INDEX_NO_TEX   = -1,    // used by quad->tex_index for non-textured quads
    DRAW2D_TEX_INDEX_FADE_SCR = -2     // dummy value for DrawFadeScreen tex_index
};

// Sequential z-index for 2D primitives added to the ps2_draw2d_batch[].
static u16 ps2_next_z_index_2d = 0;
#define DRAW2D_NEXT_Z_INDEX() (ps2_next_z_index_2d++)

// Set if there's a full screen overlay for this frame (index into ps2_draw2d_batch).
// We only allow one per frame, and that's all Quake2 needs!
// -1 if no fade screen/full screen wipe this frame.
static int ps2_fade_scr_index = -1;

// 2D draw calls are always batched to try avoiding unnecessary texture switches.
static int ps2_next_in_2d_batch = 0;
static ps2_screen_quad_t ps2_draw2d_batch[DRAW2D_BATCH_SIZE] PS2_ALIGN(16);
#define DRAW2D_NEXT_BATCH_ELEMENT() (&ps2_draw2d_batch[ps2_next_in_2d_batch++])

//
// Quake2 cinematics.
// One cinematic frame per rendered frame at most, if active.
//
static struct
{
    int x, y;
    int w, h;
    ps2_teximage_t * teximage;
    qboolean draw_pending;
} ps2_cinematic_frame;

// Palette provided by the game to expand 8bit cinematic frames.
static u32 ps2_cinematic_palette[256] PS2_ALIGN(16);

// Cinematic frames are rendered into this RGB16 texture
// and blitted to screen using a full-screen quadrilateral
// that applies this buffer as texture.
static u16 ps2_cinematic_buffer[MAX_TEXIMAGE_SIZE * MAX_TEXIMAGE_SIZE] PS2_ALIGN(16);

// Built-in texture images that are always available (defined in tex_image.c):
extern ps2_teximage_t * ps2_builtin_tex_conchars;
extern ps2_teximage_t * ps2_builtin_tex_conback;
extern ps2_teximage_t * ps2_builtin_tex_inventory;
extern ps2_teximage_t * ps2_builtin_tex_help;
extern ps2_teximage_t * ps2_builtin_tex_backtile;
extern ps2_teximage_t * ps2_builtin_tex_debug;

//=============================================================================
//
// Local helpers:
//
//=============================================================================

// This assumes the pointer is a member of ps2ref.teximages[]!
#define TEXIMAGE_INDEX(teximage_ptr) ((int)((teximage_ptr) - ps2ref.teximages))

// Test if the texture pointer is from the scrap atlas.
#define TEXIMAGE_IS_SCRAP(teximage_ptr) ((teximage_ptr)->u1 != 0 && (teximage_ptr)->v1 != 0)

//
// Error checks:
//
#define CHECK_FRAME_STARTED()                                          \
    do                                                                 \
    {                                                                  \
        if (!ps2ref.frame_started)                                     \
        {                                                              \
            Sys_Error("%s called outside begin/end frame!", __func__); \
        }                                                              \
    } while (0)

//
// DMA GIF tag helpers:
//
#define BEGIN_DMA_TAG(qword_ptr) \
    qword_t * _dma_tag_ = (qword_ptr)++

#define END_DMA_TAG(qword_ptr) \
    DMATAG_CNT(_dma_tag_, (qword_ptr) - _dma_tag_ - 1, 0, 0, 0)

#define END_DMA_TAG_AND_CHAIN(qword_ptr) \
    DMATAG_END(_dma_tag_, (qword_ptr) - _dma_tag_ - 1, 0, 0, 0)

#define BEGIN_DMA_TAG_NAMED(tag_name, qword_ptr) \
    tag_name = (qword_ptr)++

#define END_DMA_TAG_NAMED(tag_name, qword_ptr) \
    DMATAG_CNT((tag_name), (qword_ptr) - (tag_name) - 1, 0, 0, 0)

//=============================================================================
//
// GS Packet handling:
//
//=============================================================================

/*
================
PS2_PacketAlloc
================
*/
void PS2_PacketAlloc(ps2_gs_packet_t * packet, int qwords, int type)
{
    int byte_size = 0;

    if (type == GS_PACKET_SPR) // Scratch Pad
    {
		if (qwords > PS2_SPR_SIZE_QWORDS)
		{
			Sys_Error("Scratch Pad memory can only fit up to %d quadwords!", PS2_SPR_SIZE_QWORDS);
		}

        (u32 *)packet->data = (u32 *)PS2_SPR_MEM_BEGIN;
        qwords = PS2_SPR_SIZE_QWORDS;
        byte_size = qwords << 4;
    }
    else
    {
        byte_size = qwords << 4;
        packet->data = PS2_MemAllocAligned(64, byte_size, MEMTAG_RENDERER);

        // Optionally set the pointer attribute to UCAB space.
        if (type == GS_PACKET_UCAB)
        {
            (u32) packet->data |= (u32)PS2_UCAB_MEM_MASK;
        }
    }

    packet->type   = type;
    packet->qwords = qwords;
    memset(packet->data, 0, byte_size);
}

/*
================
PS2_PacketFree
================
*/
void PS2_PacketFree(ps2_gs_packet_t * packet)
{
    // Free the allocated data buffer (if any).
    if (packet->type == GS_PACKET_SPR)
    {
        packet->data = NULL;
    }
    else
    {
        if (packet->type == GS_PACKET_UCAB)
        {
            (u32)packet->data ^= (u32)PS2_UCAB_MEM_MASK;
        }

        PS2_MemFree(packet->data, packet->qwords << 4, MEMTAG_RENDERER);
        packet->data = NULL;
    }

    packet->type   = 0;
    packet->qwords = 0;
}

/*
================
PS2_PacketReset
================
*/
void PS2_PacketReset(ps2_gs_packet_t * packet)
{
    if (packet->type == GS_PACKET_SPR)
    {
        (u32 *)packet->data = (u32 *)PS2_SPR_MEM_BEGIN;
        return;
    }

    if (packet->data != NULL)
    {
        memset(packet->data, 0, packet->qwords << 4);
    }
}

//=============================================================================
//
// Renderer locals:
//
//=============================================================================

/*
================
PS2_VRamAlloc

Remarks: Local function.
================
*/
static int PS2_VRamAlloc(int width, int height, int psm, int alignment)
{
    int addr = graph_vram_allocate(width, height, psm, alignment);
    if (addr < 0)
    {
        Sys_Error("Failed to allocate VRam space! Requested: %d, %d, %d\n",
                  width, height, alignment);
    }

    int size = graph_vram_size(width, height, psm, alignment);
    ps2ref.vram_used_bytes += size * 4; // Size is in 32bit VRam words.
    return addr;
}

/*
================
PS2_AllocRenderPackets

Remarks: Local function.
================
*/
static void PS2_AllocRenderPackets(void)
{
    // Create double buffer render packets:
    //
    // FRAME_PACKET_SIZE is the number of quadwords per render packet
    // in our double buffer. We have two of them, so this adds up to
    // ~2 Megabytes of memory.
    //
    // NOTE: Currently no overflow checking is done, so drawing a
    // very big mesh could potentially crash the renderer!
    //

    //FIXME Temp: will settle on a definite size later, but we
    // don't need a lot since now the 3D drawing happens on the VU1.
    // this is for the 2D drawing and a few render-states only...
    enum { FRAME_PACKET_SIZE = 65535/2 };
    //enum { FRAME_PACKET_SIZE = 65535 };

    PS2_PacketAlloc(&ps2ref.frame_packets[0], FRAME_PACKET_SIZE, GS_PACKET_NORMAL);
    PS2_PacketAlloc(&ps2ref.frame_packets[1], FRAME_PACKET_SIZE, GS_PACKET_NORMAL);

    // Extra packets for texture uploads:
    PS2_PacketAlloc(&ps2ref.tex_upload_packet[0], 128, GS_PACKET_NORMAL);
    PS2_PacketAlloc(&ps2ref.tex_upload_packet[1], 128, GS_PACKET_NORMAL);

    // One small UCAB packet used to send the flip buffer command:
    PS2_PacketAlloc(&ps2ref.flip_fb_packet, 8, GS_PACKET_UCAB);
}

/*
================
PS2_InitGSBuffers

Remarks: Local function.
================
*/
static void PS2_InitGSBuffers(int vidMode, int fbPsm, int zPsm, qboolean interlaced)
{
    // Init GIF DMA channel:
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // FB 0:
    ps2ref.frame_buffers[0].width   = viddef.width;
    ps2ref.frame_buffers[0].height  = viddef.height;
    ps2ref.frame_buffers[0].mask    = 0;
    ps2ref.frame_buffers[0].psm     = fbPsm;
    ps2ref.frame_buffers[0].address = PS2_VRamAlloc(ps2ref.frame_buffers[0].width,
                                                    ps2ref.frame_buffers[0].height,
                                                    ps2ref.frame_buffers[0].psm,
                                                    GRAPH_ALIGN_PAGE);

    // FB 1:
    ps2ref.frame_buffers[1].width   = viddef.width;
    ps2ref.frame_buffers[1].height  = viddef.height;
    ps2ref.frame_buffers[1].mask    = 0;
    ps2ref.frame_buffers[1].psm     = fbPsm;
    ps2ref.frame_buffers[1].address = PS2_VRamAlloc(ps2ref.frame_buffers[1].width,
                                                    ps2ref.frame_buffers[1].height,
                                                    ps2ref.frame_buffers[1].psm,
                                                    GRAPH_ALIGN_PAGE);

    // Z-buffer/depth-buffer (same size as the frame-buffers):
    ps2ref.z_buffer.enable  = DRAW_ENABLE;
    ps2ref.z_buffer.mask    = 0;
    ps2ref.z_buffer.method  = ZTEST_METHOD_GREATER_EQUAL;
    ps2ref.z_buffer.zsm     = zPsm;
    ps2ref.z_buffer.address = PS2_VRamAlloc(ps2ref.frame_buffers[0].width,
                                            ps2ref.frame_buffers[0].height,
                                            ps2ref.z_buffer.zsm,
                                            GRAPH_ALIGN_PAGE);

    // User textures start after the z-buffer.
    // Allocate space for a single 256x256 large texture
    // (pretty much all the space we have left):
    ps2ref.vram_texture_start = PS2_VRamAlloc(MAX_TEXIMAGE_SIZE,
                                              MAX_TEXIMAGE_SIZE,
                                              GS_PSM_32,
                                              GRAPH_ALIGN_BLOCK);

    //
    // Initialize the screen and tie the first framebuffer to the read circuits:
    //

    // Select between NTSC or PAL, based on region:
    if (vidMode == GRAPH_MODE_AUTO)
    {
        vidMode = graph_get_region();
    }

    // Set video mode with flicker filter:
    int graphMode = interlaced ? GRAPH_MODE_INTERLACED : GRAPH_MODE_NONINTERLACED;
    graph_set_mode(graphMode, vidMode, GRAPH_MODE_FIELD, GRAPH_ENABLE);

    // Set screen dimensions and framebuffer:
    graph_set_screen(0, 0,
                     ps2ref.frame_buffers[0].width,
                     ps2ref.frame_buffers[0].height);
    graph_set_bgcolor(0, 0, 0);

    graph_set_framebuffer_filtered(ps2ref.frame_buffers[0].address,
                                   ps2ref.frame_buffers[0].width,
                                   ps2ref.frame_buffers[0].psm,
                                   0, 0);
    graph_enable_output();
}

/*
================
PS2_InitDrawingEnvironment

Remarks: Local function.
================
*/
static void PS2_InitDrawingEnvironment(void)
{
    // We can grab one of the frame_packets to this temp.
    // It is not in use yet.
    ps2_gs_packet_t * packet = &ps2ref.frame_packets[1];

    // Set framebuffer and virtual screen offsets:
    qword_t * q = packet->data;
    q = draw_setup_environment(q, 0, &ps2ref.frame_buffers[0], &ps2ref.z_buffer);
    q = draw_primitive_xyoffset(q, 0, 2048 - (viddef.width / 2), 2048 - (viddef.height / 2));

    // Default texture addressing mode: REPEAT
    texwrap_t wrap;
    wrap.horizontal = WRAP_REPEAT;
    wrap.vertical   = WRAP_REPEAT;
    wrap.minu = 0;
    wrap.minv = 0;
    wrap.maxu = MAX_TEXIMAGE_SIZE;
    wrap.maxv = MAX_TEXIMAGE_SIZE;
    q = draw_texture_wrapping(q, 0, &wrap);

    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, (q - packet->data), 0, 0);
    dma_wait_fast(); // -- Synchronize immediately.
}

/*
================
PS2_ClearScreen

Remarks: Local function.
================
*/
static void PS2_ClearScreen(void)
{
    static qword_t scrap_dma_buffer[64] PS2_ALIGN(16);

    qword_t * qwptr  = scrap_dma_buffer;
    const int width  = viddef.width;
    const int height = viddef.height;

    BEGIN_DMA_TAG(qwptr);
    qwptr = draw_disable_tests(qwptr, 0, &ps2ref.z_buffer);

    qwptr = draw_clear(qwptr, 0,
                   2048 - (width  / 2),
                   2048 - (height / 2),
                   width, height,
                   ps2ref.screen_color.r,
                   ps2ref.screen_color.g,
                   ps2ref.screen_color.b);

    qwptr = draw_enable_tests(qwptr, 0, &ps2ref.z_buffer);
    END_DMA_TAG(qwptr);

    dma_channel_send_chain(DMA_CHANNEL_GIF, scrap_dma_buffer, 0, DMA_FLAG_TRANSFERTAG, 0);
    dma_wait_fast(); // -- Synchronize immediately.
}

/*
================
PS2_Draw2DBegin

Remarks: Local function.
================
*/
static void PS2_Draw2DBegin(void)
{
    if (ps2ref.dmatag_draw2d != NULL)
    {
        Sys_Error("PS2_Draw2DBegin: Already in 2D mode!");
    }

    // Reference the external tag 'dmatag_draw2d':
    BEGIN_DMA_TAG_NAMED(ps2ref.dmatag_draw2d, ps2ref.current_frame_qwptr);
    ps2ref.current_frame_qwptr = draw_primitive_xyoffset(ps2ref.current_frame_qwptr, 0, 2048, 2048);
}

/*
================
PS2_Draw2DEnd

Remarks: Local function.
================
*/
static void PS2_Draw2DEnd(void)
{
    ps2ref.current_frame_qwptr = draw_primitive_xyoffset(ps2ref.current_frame_qwptr, 0,
                                                         2048 - (viddef.width  / 2),
                                                         2048 - (viddef.height / 2));

    // Close 'dmatag_draw2d'.
    END_DMA_TAG_NAMED(ps2ref.dmatag_draw2d, ps2ref.current_frame_qwptr);
    ps2ref.dmatag_draw2d = NULL;
}

/*
================
PS2_FlushPipeline

Remarks: Local function.
================
*/
static void PS2_FlushPipeline(void)
{
    // Add a finish command to the DMA chain:
    BEGIN_DMA_TAG(ps2ref.current_frame_qwptr);
    ps2ref.current_frame_qwptr = draw_finish(ps2ref.current_frame_qwptr);
    END_DMA_TAG_AND_CHAIN(ps2ref.current_frame_qwptr);

    dma_channel_send_chain(DMA_CHANNEL_GIF, ps2ref.current_frame_packet->data,
                           (ps2ref.current_frame_qwptr - ps2ref.current_frame_packet->data),
                           0, 0);

    // Reset packet data pointers:
    ps2ref.current_frame_packet = &ps2ref.frame_packets[ps2ref.frame_index];
    ps2ref.current_frame_qwptr  = ps2ref.current_frame_packet->data;

    // Synchronize.
    dma_wait_fast();
    ps2_pipe_flushes++;
}

/*
================
PS2_Draw2DAddToPacket

Remarks: Local function.
================
*/
enum elem2d_type
{
    ELEM_2D_TEXTURED,
    ELEM_2D_COLOR_ONLY,
    ELEM_2D_FADE_SCR
};
static qword_t * PS2_Draw2DAddToPacket(qword_t * qwptr, const ps2_screen_quad_t * quad, enum elem2d_type type)
{
    rect_t rect;
    texrect_t texrect;

    switch (type)
    {
    case ELEM_2D_TEXTURED :
        texrect.v0.x = quad->x0;
        texrect.v0.y = quad->y0;
        texrect.v0.z = 0xFFFFFFFF;
        texrect.t0.u = quad->u0;
        texrect.t0.v = quad->v0;
        texrect.v1.x = quad->x1;
        texrect.v1.y = quad->y1;
        texrect.v1.z = 0xFFFFFFFF;
        texrect.t1.u = quad->u1;
        texrect.t1.v = quad->v1;
        texrect.color.r = quad->r;
        texrect.color.g = quad->g;
        texrect.color.b = quad->b;
        texrect.color.a = quad->a;
        texrect.color.q = 1.0f;
        qwptr = draw_rect_textured(qwptr, 0, &texrect);
        break;

    case ELEM_2D_COLOR_ONLY :
        rect.v0.x = quad->x0;
        rect.v0.y = quad->y0;
        rect.v0.z = 0xFFFFFFFF;
        rect.v1.x = quad->x1;
        rect.v1.y = quad->y1;
        rect.v1.z = 0xFFFFFFFF;
        rect.color.r = quad->r;
        rect.color.g = quad->g;
        rect.color.b = quad->b;
        rect.color.a = quad->a;
        rect.color.q = 1.0f;
        qwptr = draw_rect_filled(qwptr, 0, &rect);
        break;

    case ELEM_2D_FADE_SCR :
        rect.v0.x = quad->x0;
        rect.v0.y = quad->y0;
        rect.v0.z = 0xFFFFFFFF;
        rect.v1.x = quad->x1;
        rect.v1.y = quad->y1;
        rect.v1.z = 0xFFFFFFFF;
        rect.color.r = quad->r;
        rect.color.g = quad->g;
        rect.color.b = quad->b;
        rect.color.a = quad->a;
        rect.color.q = 1.0f;
        // Was experiencing gaps in the emulator,
        // so added these offsets...
        rect.v0.x -= 2;
        rect.v0.y -= 2;
        rect.v1.x += 2;
        rect.v1.y += 2;
        draw_enable_blending();
        // libdraw recommends filled strips for full screen draws.
        // It seems like the GS is slower when dealing with very large polygons.
        qwptr = draw_rect_filled_strips(qwptr, 0, &rect);
        draw_disable_blending();
        break;

    default :
        Sys_Error("Bad elem2d_type enum!!!");
    } // switch (type)

    return qwptr;
}

/*
================
PS2_Draw2DTexChange

Only called by PS2_SortAndDraw2DElements.
Remarks: Local function.
================
*/
static void PS2_Draw2DTexChange(u32 tex_index)
{
    if (tex_index > MAX_TEXIMAGES)
    {
        Sys_Error("PS2_Draw2DTexChange: Invalid tex_index %d!!!", (int)tex_index);
    }

    ps2_teximage_t * teximage = &ps2ref.teximages[tex_index];
    if (teximage != ps2ref.current_tex)
    {
        // Note: also check the pic pointer because of the shared scrap atlas!
        qboolean need_tex_switch = (ps2ref.current_tex == NULL);
        if (!need_tex_switch)
        {
            need_tex_switch = (teximage->pic != ps2ref.current_tex->pic);
        }

        if (need_tex_switch)
        {
            // Need to close the current 2D tag, flush, then reopen it.
            END_DMA_TAG_NAMED(ps2ref.dmatag_draw2d, ps2ref.current_frame_qwptr);

            PS2_FlushPipeline();
            PS2_TexImageVRamUpload(teximage);

            BEGIN_DMA_TAG_NAMED(ps2ref.dmatag_draw2d, ps2ref.current_frame_qwptr);
            PS2_TexImageBindCurrent();
        }
    }
}

/*
================
PS2_Draw2DBatchSortPredicate

Passed as callback to qsort().
Remarks: Local function.
================
*/
static int PS2_Draw2DBatchSortPredicate(const void * a, const void * b)
{
    const ps2_screen_quad_t * q1 = (const ps2_screen_quad_t *)a;
    const ps2_screen_quad_t * q2 = (const ps2_screen_quad_t *)b;

    // Group by texture:
    int result = q1->tex_index - q2->tex_index;
    if (result == 0)
    {
        // Within each texture group, order by z-index.
        return (int)q1->z_index - (int)q2->z_index;
    }
    else
    {
        return result;
    }
}

/*
================
PS2_SortAndDraw2DElements

Remarks: Local function.
================
*/
static void PS2_SortAndDraw2DElements(ps2_screen_quad_t * batch, int batch_size)
{
    if (batch_size <= 0)
    {
        return;
    }

    // Sort by texture, so we only switch once per texture image:
    qsort(batch, batch_size, sizeof(ps2_screen_quad_t), &PS2_Draw2DBatchSortPredicate);

    // Non-textured elements draw first. Since those have negative
    // tex_index, they are at the front of the sorted batch.
    int curr_tex_idx = DRAW2D_TEX_INDEX_NO_TEX;

    int i;
    enum elem2d_type quad_type;
    const ps2_screen_quad_t * quad;

    for (i = 0; i < batch_size; ++i)
    {
        quad = &batch[i];

        if (quad->tex_index > DRAW2D_TEX_INDEX_NO_TEX)
        {
            quad_type = ELEM_2D_TEXTURED;
        }
        else
        {
            quad_type = ELEM_2D_COLOR_ONLY;
        }

        // Might have jumped to another texture group...
        if (quad->tex_index != curr_tex_idx)
        {
            if (quad->tex_index > DRAW2D_TEX_INDEX_NO_TEX)
            {
                PS2_Draw2DTexChange((u32)quad->tex_index);
            }
            curr_tex_idx = quad->tex_index;
        }

        ps2ref.current_frame_qwptr =
            PS2_Draw2DAddToPacket(ps2ref.current_frame_qwptr, quad, quad_type);
    }
}

/*
================
PS2_Flush2DBatch

Remarks: Local function.
================
*/
static void PS2_Flush2DBatch(void)
{
    if (ps2_next_in_2d_batch == 0)
    {
        return; // Nothing this frame.
    }

    if (ps2_next_in_2d_batch > DRAW2D_BATCH_SIZE)
    {
        Sys_Error("PS2_Flush2DBatch: Bad ps2_next_in_2d_batch value!!!");
    }

    //
    // We want to sort all elements by texture to avoid unnecessary texture switches.
    // But by doing that, we'll break z-order. This is mostly fine for Quake, but it does
    // requires a few workarounds. Fade screens and full screen wipes are the annoying cases.
    //
    // - DrawFill (colored only quads) will draw first, thanks to our sorting predicate,
    //   with preserved z-order.
    //
    // - Textured draws will be next. Depth order withing each texture group is preserved
    //   but order is not guaranteed to remain correct across different textures. We can
    //   get away with this because there's little overlapping of 2D UI elements in Quake,
    //   plus we group small things into the scrap atlas, so depth order errors should be
    //   rare, if at all present.
    //
    // - Fade screens and full screen wipes are not textured, but they cannot be mixed
    //   with the rest of the untextured quads, because for these draw order does matter.
    //   Things can be drawn on top of a fade screen, so we split each sorted batch by
    //   fade screen draw, creating two sub-batches for each fade/wipe, one drawn under
    //   it and one above.
    //

    if (ps2_fade_scr_index < 0)
    {
        PS2_SortAndDraw2DElements(ps2_draw2d_batch, ps2_next_in_2d_batch);
    }
    else // Has one screen fade/wipe, split the batch:
    {
        // Things under the fade overlay:
        PS2_SortAndDraw2DElements(ps2_draw2d_batch, ps2_fade_scr_index);

        // Draw the overlay:
        ps2ref.current_frame_qwptr = PS2_Draw2DAddToPacket(
                                        ps2ref.current_frame_qwptr,
                                        &ps2_draw2d_batch[ps2_fade_scr_index],
                                        ELEM_2D_FADE_SCR);

        // Elements on top of the fade overlay:
        PS2_SortAndDraw2DElements(ps2_draw2d_batch + ps2_fade_scr_index + 1,
                                  ps2_next_in_2d_batch - (ps2_fade_scr_index + 1));
    }

    // Done! Reset for next frame:
    ps2_next_z_index_2d  =  0;
    ps2_next_in_2d_batch =  0;
    ps2_fade_scr_index   = -1;
}

/*
================
PS2_DrawFullScreenCinematic

Remarks: Local function.
================
*/
static void PS2_DrawFullScreenCinematic(void)
{
    texrect_t texrect;

    // Cinematic are not filling the screen properly on my tests...
    // Adding these hackish offsets for now.
    texrect.v0.x = ps2_cinematic_frame.x - 1;
    texrect.v0.y = ps2_cinematic_frame.y - 1;
    texrect.v1.x = ps2_cinematic_frame.x + ps2_cinematic_frame.w + 1;
    texrect.v1.y = ps2_cinematic_frame.y + ps2_cinematic_frame.h + 45;
    texrect.v0.z = 0xFFFFFFFF;
    texrect.v1.z = 0xFFFFFFFF;

    texrect.t0.u = 0;
    texrect.t0.v = 0;
    texrect.t1.u = ps2_cinematic_frame.teximage->width;
    texrect.t1.v = ps2_cinematic_frame.teximage->height;

    texrect.color.r = (byte)ps2ref.ui_brightness;
    texrect.color.g = (byte)ps2ref.ui_brightness;
    texrect.color.b = (byte)ps2ref.ui_brightness;
    texrect.color.a = (byte)ps2ref.ui_brightness;
    texrect.color.q = 1.0f;

    PS2_TexImageVRamUpload(ps2_cinematic_frame.teximage);
    PS2_TexImageBindCurrent();

    ps2ref.current_frame_qwptr = draw_rect_textured(
                    ps2ref.current_frame_qwptr, 0, &texrect);

    // Since the cinematic uses a special texture buffer, we need to flush
    // immediately after the thing is drawn. Disable and re-enable 2D mode for that.
    END_DMA_TAG_NAMED(ps2ref.dmatag_draw2d, ps2ref.current_frame_qwptr);
    PS2_FlushPipeline();
    BEGIN_DMA_TAG_NAMED(ps2ref.dmatag_draw2d, ps2ref.current_frame_qwptr);

    ps2_cinematic_frame.draw_pending = false;
}

/*
================
PS2_DrawFPSCounter

Frames-Per-Second counter at the top-right corner of the screen.
Remarks: Local function.
================
*/
static void PS2_DrawFPSCounter(void)
{
    int time_millisec = Sys_Milliseconds(); // Real time clock
    int frame_time = time_millisec - ps2_fps_counter.previous_time;

    ps2_fps_counter.times_hist[ps2_fps_counter.index++] = frame_time;
    ps2_fps_counter.previous_time = time_millisec;

    // Average our time history when we get enough samples:
    if (ps2_fps_counter.index == MAX_FPS_HIST)
    {
        int i, total = 0;
        for (i = 0; i < MAX_FPS_HIST; ++i)
        {
            total += ps2_fps_counter.times_hist[i];
        }

        if (total == 0)
        {
            total = 1;
        }

        ps2_fps_counter.count = (10000 * MAX_FPS_HIST / total);
        ps2_fps_counter.count = (ps2_fps_counter.count + 5) / 10;
        ps2_fps_counter.index = 0;
    }

    // Draw it at the top-right corner of the screen:
    PS2_DrawFill(viddef.width - 65, 3, viddef.width - 10, 15, 0); // A black bg to give the text more contrast.
    PS2_DrawString(viddef.width - 60, 6, va("FPS %u", ps2_fps_counter.count));
}

//===============================================

// Pretty hackish, but we need to share 'em with PS2_DrawMemTags/PS2_DrawRenderStats...
static int draw_stats_curr_y  = 0;
static int draw_stats_old_y   = 0;
static int draw_stats_start_y = 35;

static inline void Stats_Print(const char * str)
{
    PS2_DrawString(viddef.width - 170, draw_stats_curr_y, str);
    draw_stats_curr_y += 12;
}

static inline void Stats_DrawBackground(void)
{
    PS2_DrawTileClear(viddef.width - 180, draw_stats_old_y - 5,
                      viddef.width - 10, draw_stats_curr_y - draw_stats_old_y + 5, "backtile");
}

//===============================================

/*
================
PS2_DrawMemTags

Prints current allocation totals for the memory tags.
Remarks: Local function.
================
*/
static void PS2_DrawMemTags(void)
{
    draw_stats_old_y = draw_stats_curr_y;

    int i;
    u32 total = 0;
    for (i = 0; i < MEMTAG_COUNT; ++i)
    {
        total += ps2_mem_tag_counts[i].total_bytes;
        const char * count_str = PS2_FormatMemoryUnit(ps2_mem_tag_counts[i].total_bytes, true);
        Stats_Print(va("%-10s %s", ps2_mem_tag_names[i], count_str));
    }

    // Total memory currently allocated (approximately):
    draw_stats_curr_y += 5;
    Stats_Print(va("TOTAL: %s", PS2_FormatMemoryUnit(total, true)));

    // A darker background to give the text more contrast.
    Stats_DrawBackground();
}

/*
================
PS2_DrawRenderStats

Displays miscellaneous debug stats about the renderer and texture and model managers.
Remarks: Local function.
================
*/
static void PS2_DrawRenderStats(void)
{
    extern int ps2_model_pool_used;
    extern int ps2_model_cache_hits;
    extern int ps2_unused_models_freed;
    extern int ps2_inline_models_used;
    extern int ps2_models_failed;

    extern int ps2_model_load_fs_time;
    extern int ps2_model_load_world_time;
    extern int ps2_model_load_ents_time;

    extern int ps2_teximages_used;
    extern int ps2_teximage_cache_hits;
    extern int ps2_unused_teximages_freed;
    extern int ps2_teximages_failed;
    extern int ps2_teximage_load_time;

    draw_stats_old_y = draw_stats_curr_y;

    Stats_Print("--------------------");
    Stats_Print(va("MDL loaded     %d", ps2_model_pool_used));
    Stats_Print(va("MDL inline     %d", ps2_inline_models_used));
    Stats_Print(va("MDL cache hit  %d", ps2_model_cache_hits));
    Stats_Print(va("MDL freed      %d", ps2_unused_models_freed));
    Stats_Print(va("MDL failed     %d", ps2_models_failed));
    Stats_Print(va("TEX loaded     %d", ps2_teximages_used));
    Stats_Print(va("TEX cache hits %d", ps2_teximage_cache_hits));
    Stats_Print(va("TEX freed      %d", ps2_unused_teximages_freed));
    Stats_Print(va("TEX failed     %d", ps2_teximages_failed));
    Stats_Print("--------------------");
    Stats_Print(va("Load MDL FS %.2f s", ps2_msec_to_sec(ps2_model_load_fs_time)));
    Stats_Print(va("Load WORLD  %.2f s", ps2_msec_to_sec(ps2_model_load_world_time)));
    Stats_Print(va("Load ENTS   %.2f s", ps2_msec_to_sec(ps2_model_load_ents_time)));
    Stats_Print(va("Load TEX    %.2f s", ps2_msec_to_sec(ps2_teximage_load_time)));
    Stats_Print("--------------------");

    // A darker background to give the text more contrast.
    Stats_DrawBackground();
}

//=============================================================================
//
// Rendering methods exported to the game and engine (refresh exports):
//
//=============================================================================

/*
================
PS2_RendererInit
================
*/
qboolean PS2_RendererInit(void * unused1, void * unused2)
{
    // These were used by the original GL renderer
    // to pass the HWND and GLRC Windows handles.
    (void)unused1;
    (void)unused2;

    Com_DPrintf("---- PS2_RendererInit ----\n");

    // These can be set from a config file:
    r_ps2_vid_width          = Cvar_Get("r_ps2_vid_width",  va("%d", DEFAULT_VID_WIDTH),  0);
    r_ps2_vid_height         = Cvar_Get("r_ps2_vid_height", va("%d", DEFAULT_VID_HEIGHT), 0);
    r_ps2_ui_brightness      = Cvar_Get("r_ps2_ui_brightness",     "128", 0);
    r_ps2_fade_scr_alpha     = Cvar_Get("r_ps2_fade_scr_alpha",    "100", 0);
    r_ps2_show_fps           = Cvar_Get("r_ps2_show_fps",          "1",   0);
    r_ps2_show_mem_tags      = Cvar_Get("r_ps2_show_mem_tags",     "1",   0);
    r_ps2_show_render_stats  = Cvar_Get("r_ps2_show_render_stats", "1",   0);
    r_ps2_skip_render_frame  = Cvar_Get("r_ps2_skip_render_frame", "0",   0);

    // Cache these, since on the PS2 we don't have a way of interacting with the console.
    viddef.width             = (int)r_ps2_vid_width->value;
    viddef.height            = (int)r_ps2_vid_height->value;
    ps2ref.ui_brightness     = (u32)r_ps2_ui_brightness->value;
    ps2ref.fade_scr_alpha    = (u32)r_ps2_fade_scr_alpha->value;
    ps2ref.show_fps_count    = (qboolean)r_ps2_show_fps->value;
    ps2ref.show_mem_tags     = (qboolean)r_ps2_show_mem_tags->value;
    ps2ref.show_render_stats = (qboolean)r_ps2_show_render_stats->value;

    // Renderer id. Used in a couple places by the game.
    vidref_val = VIDREF_OTHER;

    // Reset the VRam pointer to the first address, just to be sure.
    graph_vram_clear();
    ps2ref.vram_used_bytes = 0;

    // Main renderer and video initialization:
    PS2_AllocRenderPackets();
    PS2_InitGSBuffers(GRAPH_MODE_AUTO, GS_PSM_32, GS_PSMZ_32, true);
    PS2_InitDrawingEnvironment();

    // Reset these, to be sure...
    ps2ref.frame_started         = false;
    ps2ref.registration_started  = false;
    ps2ref.registration_sequence = 0;
    ps2ref.frame_index           = 0;
    ps2ref.current_frame_packet  = NULL;
    ps2ref.current_frame_qwptr   = NULL;
    ps2ref.dmatag_draw2d         = NULL;
    ps2ref.current_tex           = NULL;
    memset(&ps2_fps_counter, 0, sizeof(ps2_fps_counter));

    // Default clear color is black.
    ps2ref.screen_color.r = 0;
    ps2ref.screen_color.g = 0;
    ps2ref.screen_color.b = 0;
    ps2ref.screen_color.a = 255;
    ps2ref.screen_color.q = 1.0f;

    // Fire up the Vector Unit...
    VU1_Init();

    // Gen the default images.
    PS2_TexImageInit();

    // Make sure it's initially set to a valid palette (ps2_global_palette).
    PS2_CinematicSetPalette(NULL);

    // And reserve a texture slot for the cinematic frame.
    ps2_cinematic_frame.teximage = PS2_TexImageAlloc();

    // 3D mesh loading/rendering setup:
    PS2_ModelInit();

    Com_DPrintf("---- PS2_RendererInit completed! ( %d, %d ) ----\n", viddef.width, viddef.height);
    ps2ref.initialized = true;
    return true;
}

/*
================
PS2_RendererShutdown
================
*/
void PS2_RendererShutdown(void)
{
    int i;

    if (!ps2ref.initialized)
    {
        return;
    }

    // This is necessary to ensure the crash screen works if
    // called between BeginFrame/EndFrame, not quite sure why...
    if (ps2ref.flip_fb_packet.data != NULL)
    {
        dma_wait_fast();

        framebuffer_t * fb = &ps2ref.frame_buffers[0];
        qword_t * q = ps2ref.flip_fb_packet.data;

        q = draw_framebuffer(q, 0, fb);
        q = draw_finish(q);

        dma_channel_send_normal_ucab(DMA_CHANNEL_GIF, ps2ref.flip_fb_packet.data,
                                     (q - ps2ref.flip_fb_packet.data), 0);
        dma_wait_fast();
    }

    draw_wait_finish();
    graph_wait_vsync();
    graph_shutdown();

    for (i = 0; i < 2; ++i)
    {
        PS2_PacketFree(&ps2ref.frame_packets[i]);
    }
    for (i = 0; i < 2; ++i)
    {
        PS2_PacketFree(&ps2ref.tex_upload_packet[i]);
    }
    PS2_PacketFree(&ps2ref.flip_fb_packet);

    VU1_Shutdown();
    PS2_ModelShutdown();
    PS2_TexImageShutdown();
    PS2_MemClearObj(&ps2ref);
}

/*
================
PS2_WaitGSDrawFinish
================
*/
void PS2_WaitGSDrawFinish(void)
{
    static qword_t scrap_dma_buffer[8] PS2_ALIGN(16);

    qword_t * qwptr = scrap_dma_buffer;
    qwptr = draw_finish(qwptr);

    dma_channel_send_normal(DMA_CHANNEL_GIF, scrap_dma_buffer, (qwptr - scrap_dma_buffer), 0, 0);
    dma_wait_fast(); // -- Synchronize immediately.

    draw_wait_finish();
}

/*
================
PS2_SetClearColor
================
*/
void PS2_SetClearColor(byte r, byte g, byte b)
{
    ps2ref.screen_color.r = r;
    ps2ref.screen_color.g = g;
    ps2ref.screen_color.b = b;
}

/*
================
PS2_BeginRegistration
================
*/
void PS2_BeginRegistration(const char * map_name)
{
    // View cluster stuff:
    extern int ps2_view_cluster;
    extern int ps2_old_view_cluster;

    // Stats counters for debug printing:
    extern int ps2_teximage_cache_hits;
    extern int ps2_unused_teximages_freed;
    extern int ps2_teximages_failed;
    extern int ps2_teximage_load_time;

    Com_DPrintf("*** PS2_BeginRegistration: '%s' ***\n", map_name);

    ps2_view_cluster     = -1;
    ps2_old_view_cluster = -1;

    ps2_teximage_cache_hits    = 0;
    ps2_unused_teximages_freed = 0;
    ps2_teximages_failed       = 0;
    ps2_teximage_load_time     = 0;

    ps2ref.registration_started = true;
    ps2ref.registration_sequence++;

    PS2_ModelLoadWorld(map_name);
}

/*
================
PS2_EndRegistration
================
*/
void PS2_EndRegistration(void)
{
    Com_DPrintf("*** PS2_EndRegistration ***\n");

    // Let go what was not referenced by the current registration sequence.
    PS2_ModelFreeUnused();
    PS2_TexImageFreeUnused();

    ps2ref.registration_started = false;
}

/*
================
PS2_RegisterModel
================
*/
struct model_s * PS2_RegisterModel(const char * name)
{
    return (struct model_s *)PS2_ModelFindOrLoad(name, MDL_BRUSH | MDL_SPRITE | MDL_ALIAS);
}

/*
================
PS2_RegisterSkin
================
*/
struct image_s * PS2_RegisterSkin(const char * name)
{
    return (struct image_s *)PS2_TexImageFindOrLoad(name, IT_SKIN | IT_BUILTIN);
}

/*
================
PS2_RegisterPic
================
*/
struct image_s * PS2_RegisterPic(const char * name)
{
    return (struct image_s *)PS2_TexImageFindOrLoad(name, IT_PIC | IT_BUILTIN);
}

/*
================
PS2_SetSky
================
*/
void PS2_SetSky(const char * name, float rotate, vec3_t axis)
{
    //TODO
    //
    // Looking at ref_gl, it seems that the renderer can decide
    // between using TGA or PCX for the skyboxes. TGA should be
    // a good choice for the PS2.
    //
    Com_DPrintf("PS2_SetSky: '%s'\n", name);
}

/*
================
PS2_BeginFrame
================
*/
void PS2_BeginFrame(float camera_separation)
{
    // This was for GL stereo rendering, which we don't support.
    (void)camera_separation;

    // Make sure we didn't screw ourselves since the last frame:
    if (ps2ref.frame_index > 1 || ps2ref.frame_started == true)
    {
        Sys_Error("PS2_BeginFrame: Inconsistent frame states!!!");
    }

    // Reset these perf counters for the new frame:
    ps2_draws2d      = 0;
    ps2_tex_uploads  = 0;
    ps2_pipe_flushes = 0;

    ps2ref.current_frame_packet = &ps2ref.frame_packets[ps2ref.frame_index];
    ps2ref.current_frame_qwptr  = ps2ref.current_frame_packet->data;
    ps2ref.frame_started = true;

    // Start the frame with a wiped screen.
    PS2_ClearScreen();
}

/*
================
PS2_EndFrame
================
*/
void PS2_EndFrame(void)
{
    if (ps2ref.frame_started == false)
    {
        Sys_Error("PS2_EndFrame: Inconsistent frame states!!!");
    }

    //
    // Finalize pending 2D draws.
    //
    // All of our 2D drawings are batched, so we concentrate
    // 2D draw call here to ensure only one texture switch per
    // image being used by the 2D elements.
    //
    PS2_Draw2DBegin();
    PS2_DrawFullScreenCinematic();

    draw_stats_curr_y = draw_stats_start_y;

    // Extra debug overlays:
    if (ps2ref.show_fps_count)
    {
        PS2_DrawFPSCounter();
    }
    if (ps2ref.show_mem_tags)
    {
        PS2_DrawMemTags();
    }
    if (ps2ref.show_render_stats)
    {
        PS2_DrawRenderStats();
    }

    PS2_Flush2DBatch();
    PS2_Draw2DEnd();

    // Add a finish command to the DMA chain:
    BEGIN_DMA_TAG(ps2ref.current_frame_qwptr);
    ps2ref.current_frame_qwptr = draw_finish(ps2ref.current_frame_qwptr);
    END_DMA_TAG_AND_CHAIN(ps2ref.current_frame_qwptr);

    dma_wait_fast();
    dma_channel_send_chain(DMA_CHANNEL_GIF, ps2ref.current_frame_packet->data,
                           (ps2ref.current_frame_qwptr - ps2ref.current_frame_packet->data),
                           0, 0);

    // V-Sync wait:
    graph_wait_vsync();
    draw_wait_finish();

    graph_set_framebuffer_filtered(ps2ref.frame_buffers[ps2ref.frame_index].address,
                                   ps2ref.frame_buffers[ps2ref.frame_index].width,
                                   ps2ref.frame_buffers[ps2ref.frame_index].psm, 0, 0);

    // Switch context:
    //  1 XOR 1 = 0
    //  0 XOR 1 = 1
    ps2ref.frame_index ^= 1;

    // Swap render framebuffer:
    framebuffer_t * fb = &ps2ref.frame_buffers[ps2ref.frame_index];
    qword_t * q = ps2ref.flip_fb_packet.data;

    q = draw_framebuffer(q, 0, fb);
    q = draw_finish(q);

    dma_wait_fast();
    dma_channel_send_normal_ucab(DMA_CHANNEL_GIF, ps2ref.flip_fb_packet.data,
                                 (q - ps2ref.flip_fb_packet.data), 0);
    draw_wait_finish();
    ps2ref.frame_started = false;
}

/*
================
PS2_RenderFrame
================
*/
void PS2_RenderFrame(refdef_t * view_def)
{
    CHECK_FRAME_STARTED();
    if (view_def == NULL)
    {
        Sys_Error("PS2_RenderFrame: Null view def!");
    }

    // You can skip 3D frame rendering for profiling/testing.
    if (r_ps2_skip_render_frame->value)
    {
        return;
    }

    // A world map should have been loaded already by BeginRegistration().
    if (PS2_ModelGetWorld() == NULL && !(view_def->rdflags & RDF_NOWORLDMODEL))
    {
        Sys_Error("PS2_RenderFrame: Null world model!");
    }

    //
    // Trivia:
    // Quake 2 default render mode was 2D, only switching to 3D in here
    // (probably in account of the software renderer).
    //

    PS2_DrawFrameSetup(view_def);
    PS2_DrawWorldModel(view_def);
    PS2_DrawViewEntities(view_def);
}

/*
================
PS2_IsFrameStarted
================
*/
qboolean PS2_IsFrameStarted(void)
{
    return ps2ref.frame_started;
}

/*
================
PS2_TexImageVRamUpload
================
*/
void PS2_TexImageVRamUpload(ps2_teximage_t * teximage)
{
    // Can be called outside Begin/End frame.

    if (teximage == NULL)
    {
        teximage = ps2_builtin_tex_debug;
    }

    if (ps2ref.current_tex == teximage) // Already in VRam.
    {
        return;
    }

    //
    // Upload the texture to GS VRam.
    // Since we are using almost all of our video memory with framebuffers
    // and z-buffer, the rest of it can only fit a single large RGBA texture.
    //

    int width;
    int height;
    if (!TEXIMAGE_IS_SCRAP(teximage))
    {
        width  = teximage->width;
        height = teximage->height;
    }
    else
    {
        width  = MAX_TEXIMAGE_SIZE;
        height = MAX_TEXIMAGE_SIZE;
    }

    ps2_gs_packet_t * packet = &ps2ref.tex_upload_packet[ps2ref.frame_index];
    qword_t * q = packet->data;

    q = draw_texture_transfer(q, teximage->pic, width, height,
                              teximage->texbuf.psm, teximage->texbuf.address, width);
    q = draw_texture_flush(q);

    dma_channel_send_chain(DMA_CHANNEL_GIF, packet->data, (q - packet->data), 0, 0);
    dma_wait_fast();

    ps2ref.current_tex = teximage;
    ps2_tex_uploads++;
}

/*
================
PS2_TexImageBindCurrent()
Sets the current texture in VRam as the
one to sample from on subsequent draw calls.
================
*/
void PS2_TexImageBindCurrent(void)
{
    CHECK_FRAME_STARTED();

    if (ps2ref.current_tex == NULL)
    {
        return;
    }

    lod_t         lod;
    clutbuffer_t  clut;
    texbuffer_t   tmp_texbuf;
    texbuffer_t * p_texbuf;

    // Use defaults for most of the parameters:
    lod.mag_filter    = ps2ref.current_tex->mag_filter;
    lod.min_filter    = ps2ref.current_tex->min_filter;
    lod.calculation   = LOD_USE_K;
    lod.max_level     = 0;
    lod.l             = 0;
    lod.k             = 0;
    clut.address      = 0;
    clut.psm          = 0;
    clut.start        = 0;
    clut.storage_mode = CLUT_STORAGE_MODE1;
    clut.load_method  = CLUT_NO_LOAD;

    if (!TEXIMAGE_IS_SCRAP(ps2ref.current_tex))
    {
        p_texbuf = &ps2ref.current_tex->texbuf;
    }
    else
    {
        // Need a custom texbuf for the scrap, since the width/height are not
        // the size of the backing atlas texture, but the size of the tile.
        byte size_log2 = draw_log2(MAX_TEXIMAGE_SIZE);

        tmp_texbuf.address         = ps2ref.vram_texture_start;
        tmp_texbuf.width           = MAX_TEXIMAGE_SIZE;
        tmp_texbuf.psm             = GS_PSM_32;
        tmp_texbuf.info.width      = size_log2;
        tmp_texbuf.info.height     = size_log2;
        tmp_texbuf.info.components = TEXTURE_COMPONENTS_RGBA;
        tmp_texbuf.info.function   = TEXTURE_FUNCTION_MODULATE;

        p_texbuf = &tmp_texbuf;
    }

    ps2ref.current_frame_qwptr = draw_texture_sampling(ps2ref.current_frame_qwptr, 0, &lod);
    ps2ref.current_frame_qwptr = draw_texturebuffer(ps2ref.current_frame_qwptr, 0, p_texbuf, &clut);
}

/*
================
PS2_DrawGetPicSize
================
*/
void PS2_DrawGetPicSize(int * w, int * h, const char * name)
{
    // This one can be called outside Begin/End frame.
    ps2_teximage_t * teximage = PS2_TexImageFindOrLoad(name, IT_PIC | IT_BUILTIN);
    if (teximage == NULL)
    {
        *w = *h = -1;
        return;
    }
    *w = teximage->width;
    *h = teximage->height;
}

/*
================
PS2_DrawPic
================
*/
void PS2_DrawPic(int x, int y, const char * name)
{
    CHECK_FRAME_STARTED();

    ps2_teximage_t * teximage = PS2_TexImageFindOrLoad(name, IT_PIC | IT_BUILTIN);
    if (teximage == NULL)
    {
        Com_DPrintf("Can't find or load pic: %s\n", name);
        return;
    }
    PS2_DrawTexImage(x, y, teximage);
}

/*
================
PS2_DrawStretchPic
================
*/
void PS2_DrawStretchPic(int x, int y, int w, int h, const char * name)
{
    CHECK_FRAME_STARTED();

    ps2_teximage_t * teximage = PS2_TexImageFindOrLoad(name, IT_PIC | IT_BUILTIN);
    if (teximage == NULL)
    {
        Com_DPrintf("Can't find or load pic: %s\n", name);
        return;
    }
    PS2_DrawStretchTexImage(x, y, w, h, teximage);
}

/*
================
PS2_DrawChar
================
*/
enum { GLYPH_SIZE = 8 }; // size in pixels
void PS2_DrawChar(int x, int y, int c)
{
    // Draws one 8*8 graphics character with 0 being transparent.
    // It can be clipped to the top of the screen to allow the console
    // to be smoothly scrolled off. Based on Draw_Char() from ref_gl.

    CHECK_FRAME_STARTED();

    c &= 255;

    if ((c & 127) == ' ')
    {
        return; // Whitespace
    }
    if (y <= -8)
    {
        return; // Totally off screen
    }
    if (ps2_next_in_2d_batch == DRAW2D_BATCH_SIZE)
    {
        return; // No more space this frame!
    }

    int row, col;
    row = (c >> 4) * GLYPH_SIZE;
    col = (c & 15) * GLYPH_SIZE;

    ps2_screen_quad_t * quad = DRAW2D_NEXT_BATCH_ELEMENT();

    quad->z_index   = DRAW2D_NEXT_Z_INDEX();
    quad->tex_index = TEXIMAGE_INDEX(ps2_builtin_tex_conchars);

    quad->x0 = x;
    quad->y0 = y;
    quad->x1 = x + GLYPH_SIZE;
    quad->y1 = y + GLYPH_SIZE;

    quad->u0 = col;
    quad->v0 = row;
    quad->u1 = col + GLYPH_SIZE;
    quad->v1 = row + GLYPH_SIZE;

    // This will behave differently if texture func is
    // TEXTURE_FUNCTION_MODULATE or TEXTURE_FUNCTION_DECAL!
    quad->r = (byte)ps2ref.ui_brightness;
    quad->g = (byte)ps2ref.ui_brightness;
    quad->b = (byte)ps2ref.ui_brightness;
    quad->a = 255;

    ps2_draws2d++;
}

/*
================
PS2_DrawString
================
*/
void PS2_DrawString(int x, int y, const char * s)
{
    int initial_x = x;
    while (*s != '\0')
    {
        PS2_DrawChar(x, y, *s);
        x += GLYPH_SIZE;
        if (*s == '\n')
        {
            y += GLYPH_SIZE + 2; // 2 pixels of spacing between lines.
            x = initial_x;
        }
        ++s;
    }
}

/*
================
PS2_DrawAltString
================
*/
void PS2_DrawAltString(int x, int y, const char * s)
{
    int initial_x = x;
    while (*s != '\0')
    {
        PS2_DrawChar(x, y, *s ^ 0x80); // With this it'll hit the index of a green char (in conchars.pcx).
        x += GLYPH_SIZE;
        if (*s == '\n')
        {
            y += GLYPH_SIZE + 2; // 2 pixels of spacing between lines.
            x = initial_x;
        }
        ++s;
    }
}

/*
================
PS2_DrawTileClear
================
*/
void PS2_DrawTileClear(int x, int y, int w, int h, const char * name)
{
    CHECK_FRAME_STARTED();

    if (ps2_next_in_2d_batch == DRAW2D_BATCH_SIZE)
    {
        return; // No more space this frame!
    }

    ps2_teximage_t * teximage = PS2_TexImageFindOrLoad(name, IT_PIC | IT_BUILTIN);
    if (teximage == NULL)
    {
        Com_DPrintf("Can't find or load pic: %s\n", name);
        return;
    }

    ps2_screen_quad_t * quad = DRAW2D_NEXT_BATCH_ELEMENT();

    quad->z_index   = DRAW2D_NEXT_Z_INDEX();
    quad->tex_index = TEXIMAGE_INDEX(teximage);

    quad->x0 = x;
    quad->y0 = y;
    quad->x1 = x + w;
    quad->y1 = y + h;

    quad->u0 = x;
    quad->v0 = y;
    quad->u1 = x + w;
    quad->v1 = y + h;

    // This will behave differently if texture func is
    // TEXTURE_FUNCTION_MODULATE or TEXTURE_FUNCTION_DECAL!
    quad->r = (byte)ps2ref.ui_brightness;
    quad->g = (byte)ps2ref.ui_brightness;
    quad->b = (byte)ps2ref.ui_brightness;
    quad->a = 255;

    ps2_draws2d++;
}

/*
================
PS2_DrawFill
================
*/
void PS2_DrawFill(int x, int y, int w, int h, int c)
{
    CHECK_FRAME_STARTED();

    if ((u32)c > 255)
    {
        Sys_Error("PS2_DrawFill: Bad color index %d!", c);
    }

    ps2_screen_quad_t * quad;

    // Full screen wipe with black (happens on pause and menus), treat it as a screen fade.
    if (c == 0 && x == 0 && y == 0 && w == viddef.width && h == viddef.height)
    {
        if (ps2_fade_scr_index >= 0)
        {
            return; // Already issued one this frame!
        }

        quad = DRAW2D_NEXT_BATCH_ELEMENT();

        quad->z_index   = DRAW2D_NEXT_Z_INDEX();
        quad->tex_index = DRAW2D_TEX_INDEX_FADE_SCR;

        quad->x0 = x;
        quad->y0 = y;
        quad->x1 = x + w;
        quad->y1 = y + h;

        quad->r = 0;
        quad->g = 0;
        quad->b = 0;
        quad->a = 255;

        ps2_fade_scr_index = (ps2_next_in_2d_batch - 1);
    }
    else // Smaller screen element:
    {
        if (ps2_next_in_2d_batch == DRAW2D_BATCH_SIZE)
        {
            return; // No more space this frame!
        }

        quad = DRAW2D_NEXT_BATCH_ELEMENT();

        quad->z_index   = DRAW2D_NEXT_Z_INDEX();
        quad->tex_index = DRAW2D_TEX_INDEX_NO_TEX;

        quad->x0 = x;
        quad->y0 = y;
        quad->x1 = x + w;
        quad->y1 = y + h;

        u32 color = ps2_global_palette[c];
        quad->r = (color & 0xFF);
        quad->g = (color >>  8) & 0xFF;
        quad->b = (color >> 16) & 0xFF;
        quad->a = 255;
    }

    ps2_draws2d++;
}

/*
================
PS2_DrawFadeScreen
================
*/
void PS2_DrawFadeScreen(void)
{
    CHECK_FRAME_STARTED();

    if (ps2_fade_scr_index >= 0)
    {
        return; // Already issued one this frame!
    }

    ps2_screen_quad_t * quad = DRAW2D_NEXT_BATCH_ELEMENT();

    quad->z_index   = DRAW2D_NEXT_Z_INDEX();
    quad->tex_index = DRAW2D_TEX_INDEX_FADE_SCR;

    quad->x0 = 0;
    quad->y0 = 0;
    quad->x1 = viddef.width;
    quad->y1 = viddef.height;

    quad->r = 0;
    quad->g = 0;
    quad->b = 0;
    quad->a = (byte)ps2ref.fade_scr_alpha;

    ps2_fade_scr_index = (ps2_next_in_2d_batch - 1);
    ps2_draws2d++;
}

/*
================
PS2_DrawStretchRaw
================
*/
void PS2_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte * data)
{
    CHECK_FRAME_STARTED();

    //
    // This function is only used by Quake2 to draw the cinematic frames, nothing else,
    // so it could have a better name... We'll optimize for that and assume this is not
    // a generic "draw pixels" kind of function.
    //

    int i, j;
    int row, trows;
    int frac, fracstep;
    float hscale;

    const byte * source;
    u16 * dest;

    u32 color;
    byte r, g, b, a;

    if (rows <= MAX_TEXIMAGE_SIZE)
    {
        hscale = 1;
        trows = rows;
    }
    else
    {
        hscale = (float)rows / (float)MAX_TEXIMAGE_SIZE;
        trows = MAX_TEXIMAGE_SIZE;
    }

    // Good idea to clear the buffer first, in case the
    // next upsampling doesn't fill the whole thing.
    memset(ps2_cinematic_buffer, 0, sizeof(ps2_cinematic_buffer));

    // Upsample to fill our 256*256 cinematic buffer.
    // This is based on the algorithm applied by ref_gl.
    for (i = 0; i < trows; i++)
    {
        row = (int)(i * hscale);
        if (row > rows)
        {
            break;
        }

        source   = data + cols * row;
        dest     = &ps2_cinematic_buffer[i * MAX_TEXIMAGE_SIZE];
        fracstep = cols * 0x10000 / MAX_TEXIMAGE_SIZE;
        frac     = fracstep >> 1;

        for (j = 0; j < MAX_TEXIMAGE_SIZE; j++)
        {
            color = ps2_cinematic_palette[source[frac >> 16]];

            r = (color & 0xFF);
            g = (color >>  8) & 0xFF;
            b = (color >> 16) & 0xFF;
            a = (color >> 24) & 0xFF;

            dest[j] = ((a & 0x1) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3); // Pack RGBA-16
            frac += fracstep;
        }
    }

    // Reset the texture parameters (notice we use linear filtering for smoother sampling):
    PS2_TexImageSetup(ps2_cinematic_frame.teximage, "cinematic_frame", MAX_TEXIMAGE_SIZE,
                      MAX_TEXIMAGE_SIZE, TEXTURE_COMPONENTS_RGB, TEXTURE_FUNCTION_MODULATE,
                      GS_PSM_16, LOD_MAG_LINEAR, LOD_MIN_LINEAR, IT_BUILTIN, (byte *)ps2_cinematic_buffer);

    // Save these for drawing later.
    ps2_cinematic_frame.x = x;
    ps2_cinematic_frame.y = y;
    ps2_cinematic_frame.w = w;
    ps2_cinematic_frame.h = h;
    ps2_cinematic_frame.draw_pending = true;
}

/*
================
PS2_DrawTexImage
================
*/
void PS2_DrawTexImage(int x, int y, ps2_teximage_t * teximage)
{
    PS2_DrawStretchTexImage(x, y, teximage->width, teximage->height, teximage);
}

/*
================
PS2_DrawStretchTexImage
================
*/
void PS2_DrawStretchTexImage(int x, int y, int w, int h, ps2_teximage_t * teximage)
{
    CHECK_FRAME_STARTED();

    if (ps2_next_in_2d_batch == DRAW2D_BATCH_SIZE)
    {
        return; // No more space this frame!
    }

    ps2_screen_quad_t * quad = DRAW2D_NEXT_BATCH_ELEMENT();

    quad->z_index   = DRAW2D_NEXT_Z_INDEX();
    quad->tex_index = TEXIMAGE_INDEX(teximage);

    quad->x0 = x;
    quad->y0 = y;
    quad->x1 = x + w;
    quad->y1 = y + h;

    if (!TEXIMAGE_IS_SCRAP(teximage))
    {
        quad->u0 = 0;
        quad->v0 = 0;
        quad->u1 = teximage->width;
        quad->v1 = teximage->height;
    }
    else // The UVs will be non-zero for a scrap atlas texture.
    {
        quad->u0 = teximage->u0;
        quad->v0 = teximage->v0;
        quad->u1 = teximage->u1;
        quad->v1 = teximage->v1;
    }

    // This will behave differently if texture func is
    // TEXTURE_FUNCTION_MODULATE or TEXTURE_FUNCTION_DECAL!
    quad->r = (byte)ps2ref.ui_brightness;
    quad->g = (byte)ps2ref.ui_brightness;
    quad->b = (byte)ps2ref.ui_brightness;
    quad->a = 255;

    ps2_draws2d++;
}

/*
================
PS2_CinematicSetPalette
================
*/
void PS2_CinematicSetPalette(const byte * restrict palette)
{
    if (palette == NULL)
    {
        memcpy(ps2_cinematic_palette, ps2_global_palette, sizeof(ps2_cinematic_palette));
        return;
    }

    int i;
    byte * restrict dest = (byte *)ps2_cinematic_palette;

    for (i = 0; i < 256; i++)
    {
        dest[(i * 4) + 0] = palette[(i * 3) + 0];
        dest[(i * 4) + 1] = palette[(i * 3) + 1];
        dest[(i * 4) + 2] = palette[(i * 3) + 2];
        dest[(i * 4) + 3] = 0xFF;
    }
}

/*
================
PS2_AppActivate
================
*/
void PS2_AppActivate(qboolean activate)
{
    // I don't think there's really anything to be done here.
    // The app never goes inactive on the PS2.
    (void)activate;
}

// Cleanup the namespace:
#undef DRAW2D_NEXT_Z_INDEX
#undef DRAW2D_NEXT_BATCH_ELEMENT
#undef TEXIMAGE_INDEX
#undef TEXIMAGE_IS_SCRAP
#undef CHECK_FRAME_STARTED
#undef BEGIN_DMA_TAG
#undef END_DMA_TAG
#undef END_DMA_TAG_AND_CHAIN
#undef BEGIN_DMA_TAG_NAMED
#undef END_DMA_TAG_NAMED

