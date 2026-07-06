
/* ================================================================================================
 * -*- C -*-
 * File: vu1.c
 * Author: Guilherme R. Lampert
 * Created on: 17/01/16
 * Brief: Vector Unit 1 (VU1) microcode upload and VU management.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/vu1.h"
#include "ps2/mem_alloc.h"
#include "ps2/defs_ps2.h"
#include "game/q_shared.h" // For qboolean and stuff...

// PS2DEV SDK:
#include <kernel.h>
#include <dma_tags.h>
#include <dma.h>

//=============================================================================
//
// Following code is based on lib PDK by Jesper Svennevid, Daniel Collin.
//
//=============================================================================

// DMA hardware defines:
#define VU1_DMA_END_TAG(COUNT) (((u64)(0x7) << 28) | COUNT)
#define VU1_DMA_CNT_TAG(COUNT) (((u64)(0x1) << 28) | COUNT)
#define VU1_DMA_REF_TAG(ADDR, COUNT) ((((u64)ADDR) << 32) | (0x3 << 28) | COUNT)

// VIF hardware defines:
#define VU1_VIF_NOP    0x00
#define VU1_VIF_MPG    0x4A
#define VU1_VIF_MSCAL  0x14
#define VU1_VIF_STCYL  0x01
#define VU1_VIF_FLUSH  0x11
#define VU1_VIF_UNPACK 0x60
#define VU1_VIF_UNPACK_V4_32 (VU1_VIF_UNPACK | 0x0C)
#define VU1_VIF_CODE(CMD, NUM, IMMEDIATE) ((((u32)(CMD)) << 24) | (((u32)(NUM)) << 16) | ((u32)(IMMEDIATE)))

//=============================================================================

typedef struct
{
    void *   offset;
    void *   kickbuffer;
    int      dma_size;
    int      cnt_dma_dest;
    qboolean is_buiding_dma; // True when between VU1_ListAddBegin/VU1_ListAddEnd
} vu1_context_t;

// Allowed to be accessed externally.
u32 vu1_buffer_index = 0;

// Locals:
static byte * vu1_current_buffer;
static byte * vu1_dma_buffers[2] PS2_ALIGN(16);
static vu1_context_t vu1_local_context PS2_ALIGN(16);

// Wait time for the VIF DMAs: -1 no time out. Wait till finished.
static const int VU1_DMA_CHAN_TIMEOUT = -1;

// We have two buffers of this size for the VIF DMAs.
static const int VU1_DMA_BUFFER_SIZE_BYTES = 512 * 1024; // 1MB for both

//=============================================================================

/*
================
VU1_CodeSize

Local helper function.
================
*/
static int VU1_CodeSize(u32 * start, u32 * end)
{
    int size = (end - start) / 2;

    // If size is odd we have make it even in order for the transfer to work
    // (quadwords, because of that its VERY important to have an extra nop nop
    // at the end of each VU program.
    if (size & 1)
    {
        ++size;
    }
    return size;
}

/*
================
VU1_Init
================
*/
void VU1_Init(void)
{
    dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);

    int i;
    for (i = 0; i < sizeof(vu1_dma_buffers) / sizeof(vu1_dma_buffers[0]); ++i)
    {
        vu1_dma_buffers[i] = PS2_MemAllocAligned(16, VU1_DMA_BUFFER_SIZE_BYTES, MEMTAG_RENDERER);
    }

    vu1_buffer_index   = 0;
    vu1_current_buffer = vu1_dma_buffers[0];
}

/*
================
VU1_Shutdown
================
*/
void VU1_Shutdown(void)
{
    int i;
    for (i = 0; i < sizeof(vu1_dma_buffers) / sizeof(vu1_dma_buffers[0]); ++i)
    {
        PS2_MemFree(vu1_dma_buffers[i], VU1_DMA_BUFFER_SIZE_BYTES, MEMTAG_RENDERER);
        vu1_dma_buffers[i] = NULL;
    }

    vu1_buffer_index   = 0;
    vu1_current_buffer = NULL;
    memset(&vu1_local_context, 0, sizeof(vu1_local_context));
}

/*
================
VU1_UploadProg
================
*/
void VU1_UploadProg(int dest, void * start, void * end)
{
    if (vu1_dma_buffers[0] == NULL)
    {
        Sys_Error("Call VU1_Init() before uploading a microprogram!");
    }

    // We can use one of the DMA buffers for the program upload,
    // since we synchronize immediately after the upload.
    byte * chain = vu1_dma_buffers[0];
    dma_channel_wait(DMA_CHANNEL_VIF1, VU1_DMA_CHAN_TIMEOUT); // Finish any pending transfers first.

    // Get the size of the code as we can only send 256 instructions in each MPG tag.
    int count = VU1_CodeSize(start, end);
    while (count > 0)
    {
        u32 current_count = (count > 256) ? 256 : count;

        *((u64 *)chain)++ = VU1_DMA_REF_TAG((u32)start, current_count / 2);
        *((u32 *)chain)++ = VU1_VIF_CODE(VU1_VIF_NOP, 0, 0);
        *((u32 *)chain)++ = VU1_VIF_CODE(VU1_VIF_MPG, current_count & 0xFF, dest);

        start += current_count * 2;
        count -= current_count;
        dest  += current_count;
    }

    *((u64 *)chain)++ = VU1_DMA_END_TAG(0);
    *((u32 *)chain)++ = VU1_VIF_CODE(VU1_VIF_NOP, 0, 0);
    *((u32 *)chain)++ = VU1_VIF_CODE(VU1_VIF_NOP, 0, 0);

    // Send it to VIF1:
    FlushCache(0);
    dma_channel_send_chain(DMA_CHANNEL_VIF1, vu1_dma_buffers[0], 0, DMA_FLAG_TRANSFERTAG, 0);
    dma_channel_wait(DMA_CHANNEL_VIF1, VU1_DMA_CHAN_TIMEOUT); // Synchronize immediately.
}

/*
================
VU1_Begin
================
*/
void VU1_Begin(void)
{
    // Switch context:
    //  1 XOR 1 = 0
    //  0 XOR 1 = 1
    vu1_buffer_index ^= 1;
    vu1_current_buffer = vu1_dma_buffers[vu1_buffer_index];

    // Rest frame context:
    vu1_local_context.dma_size       = 0;
    vu1_local_context.cnt_dma_dest   = 0;
    vu1_local_context.is_buiding_dma = false;
    vu1_local_context.offset         = NULL;
    vu1_local_context.kickbuffer     = vu1_current_buffer;
}

/*
================
VU1_End
================
*/
void VU1_End(int start)
{
    *((u64 *)vu1_current_buffer)++ = VU1_DMA_END_TAG(0);

    if (start >= 0)
    {
        *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_FLUSH, 0, 0);
        *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_MSCAL, 0, start);
    }
    else
    {
        *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_NOP, 0, 0);
    }

    // Wait for previous transfer to complete if not yet:
    dma_channel_wait(DMA_CHANNEL_VIF1, VU1_DMA_CHAN_TIMEOUT);

    // Start new one.
    dma_channel_send_chain(DMA_CHANNEL_VIF1, vu1_local_context.kickbuffer, 0, DMA_FLAG_TRANSFERTAG, 0);
}

/*
================
VU1_ListAddBegin
================
*/
void VU1_ListAddBegin(int address)
{
    if (vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAddBegin: Already building a DMA list!");
    }

    vu1_local_context.offset         = vu1_current_buffer;
    vu1_local_context.cnt_dma_dest   = address;
    vu1_local_context.is_buiding_dma = true;

    // These are placeholders filled later by VU1_ListAddEnd().
    *((u64 *)vu1_current_buffer)++ = VU1_DMA_CNT_TAG(0);
    *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_STCYL, 0, 0x0101);
    *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_UNPACK_V4_32, 0, 0);
}

/*
================
VU1_ListAddEnd
================
*/
void VU1_ListAddEnd(void)
{
    if (!vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAddEnd: Missing a DMA list begin!");
    }

    // Pad to qword alignment if necessary:
    while (vu1_local_context.dma_size & 0xF)
    {
        *((u32 *)vu1_current_buffer)++ = 0;
        vu1_local_context.dma_size += sizeof(u32);
    }

    const int dma_size_qwords = vu1_local_context.dma_size >> 4;
    *((u64 *)vu1_local_context.offset)++ = VU1_DMA_CNT_TAG(dma_size_qwords);
    *((u32 *)vu1_local_context.offset)++ = VU1_VIF_CODE(VU1_VIF_STCYL, 0, 0x0101);
    *((u32 *)vu1_local_context.offset)++ = VU1_VIF_CODE(VU1_VIF_UNPACK_V4_32, dma_size_qwords, vu1_local_context.cnt_dma_dest);

    vu1_local_context.is_buiding_dma = false;
}

/*
================
VU1_ListData
================
*/
void VU1_ListData(int dest_address, void * data, int quad_size)
{
    if ((u32)data & 0xF)
    {
        Sys_Error("VU1_ListData: Pointer is not 16-bytes aligned!");
    }

    *((u64 *)vu1_current_buffer)++ = VU1_DMA_REF_TAG((u32)data, quad_size);
    *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_STCYL, 0, 0x0101);
    *((u32 *)vu1_current_buffer)++ = VU1_VIF_CODE(VU1_VIF_UNPACK_V4_32, quad_size, dest_address);
}

/*
================
VU1_ListAdd128
================
*/
void VU1_ListAdd128(u64 v1, u64 v2)
{
    if (!vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAdd128: Missing a DMA list begin!");
    }

    *((u64 *)vu1_current_buffer)++ = v1;
    *((u64 *)vu1_current_buffer)++ = v2;
    vu1_local_context.dma_size += sizeof(u64) * 2;
}

/*
================
VU1_ListAddGIFTag
================
*/
u64 * VU1_ListAddGIFTag(void)
{
    if (!vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAddGIFTag: Missing a DMA list begin!");
    }

    // Empty tag that the caller can fill up.
    u64 * tag_ptr = (u64 *)vu1_current_buffer;
    *((u64 *)vu1_current_buffer)++ = 0;
    *((u64 *)vu1_current_buffer)++ = 0;
    vu1_local_context.dma_size += sizeof(u64) * 2;
    return tag_ptr;
}

/*
================
VU1_ListAdd64
================
*/
void VU1_ListAdd64(u64 v)
{
    if (!vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAdd64: Missing a DMA list begin!");
    }

    *((u64 *)vu1_current_buffer)++ = v;
    vu1_local_context.dma_size += sizeof(u64);
}

/*
================
VU1_ListAdd32
================
*/
void VU1_ListAdd32(u32 v)
{
    if (!vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAdd32: Missing a DMA list begin!");
    }

    *((u32 *)vu1_current_buffer)++ = v;
    vu1_local_context.dma_size += sizeof(u32);
}

/*
================
VU1_ListAddFloat
================
*/
void VU1_ListAddFloat(float v)
{
    if (!vu1_local_context.is_buiding_dma)
    {
        Sys_Error("VU1_ListAddFloat: Missing a DMA list begin!");
    }

    *((float *)vu1_current_buffer)++ = v;
    vu1_local_context.dma_size += sizeof(float);
}
