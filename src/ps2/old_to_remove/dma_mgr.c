
/* ================================================================================================
 * -*- C -*-
 * File: dma_mgr.c
 * Author: Guilherme R. Lampert
 * Created on: 01/02/16
 * Brief: DMA memory management and VIF DMA helpers.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

//
// Code on this file was based on the DMA helper classes used on the
// PS2 Linux samples available at http://www.hsfortuna.pwp.blueyonder.co.uk
//

#include "ps2/dma_mgr.h"
#include "ps2/mem_alloc.h"
#include "ps2/vu_prog_mgr.h"
#include "common/q_common.h"

// PS2DEV SDK:
#include <kernel.h>
#include <vif_registers.h> // For VIF1_ERR

// Helper macros to typecast the DMA object:
#define DMA_BASE(obj)    ((ps2_dma_chain_t       *)(obj))
#define DMA_DYNAMIC(obj) ((ps2_vif_dynamic_dma_t *)(obj))
#define DMA_STATIC(obj)  ((ps2_vif_static_dma_t  *)(obj))

// Memory-mapped DMA registers we need (channel 1 = VIF)
#define DMA_VIF1_CHAN_CHCR *((volatile u32 *)0x10009000) /* Channel Control Register */
#define DMA_VIF1_CHAN_MADR *((volatile u32 *)0x10009010) /* Memory Address Register  */
#define DMA_VIF1_CHAN_QWC  *((volatile u32 *)0x10009020) /* Quadword Count Register  */
#define DMA_VIF1_CHAN_TADR *((volatile u32 *)0x10009030) /* Tag Address Register     */

// Tag helpers:
#define DMA_END_TAG(COUNT) ((((u64)0x7) << 28) | (COUNT))
#define DMA_RET_TAG(COUNT) ((((u64)0x6) << 28) | (COUNT))
#define DMA_CALL_TAG(ADDR, COUNT) ((((u64)(ADDR)) << 32) | (0x5 << 28) | (COUNT))
#define DMA_SET_CHCR(DIR, MOD, ASP, TTE, TIE, STR) ((((u32)DIR) << 0) | (((u32)MOD) << 2) | (((u32)ASP) << 4) | (((u32)TTE) << 6) | (((u32)TIE) << 7) | (((u32)STR) << 8))
#define DMA_SET_TADR(ADDR, SPR) ((u32)((ADDR) & 0x7FFFFFFF) << 0 | (u32)((SPR) & 0x00000001) << 31)

/*
================
VIFDMA_Initialize
================
*/
void VIFDMA_Initialize(ps2_vif_dma_obj_t obj, int num_pages, ps2_vif_dma_type_t dma_type)
{
    // Clear out the object first, just to be safe:
    const int obj_size_bytes = (dma_type == VIF_DYNAMIC_DMA) ? sizeof(ps2_vif_dynamic_dma_t) : sizeof(ps2_vif_static_dma_t);
    memset(obj, 0, obj_size_bytes);

    // Init according to usage type:
    if (dma_type == VIF_DYNAMIC_DMA)
    {
        ps2_vif_dynamic_dma_t * dyn_dma = DMA_DYNAMIC(obj);
        dyn_dma->base.dma_type  = dma_type;
        dyn_dma->base.num_pages = num_pages;
        dyn_dma->base.mem_pages = calloc(num_pages * 2, sizeof(ps2_dma_mem_page_t)); // We're using a double buffer so allocate 2x the pages

        const int total_bytes = (num_pages * 2) * 4096; // 4K pages
        u8 * chunk = PS2_MemAllocAligned(16, total_bytes, MEMTAG_RENDERER);
        memset(chunk, 0, total_bytes);

        int i; // Alloc the individual pages:
        for (i = 0; i < num_pages * 2; ++i)
        {
            ps2_dma_mem_page_t * page = &dyn_dma->base.mem_pages[i];
            page->start_ptr = chunk;
            page->qw_size   = 256;
            chunk          += 4096;
        }

        VIF1_ERR = 2; // Ignore DMA mismatch errors.
        VIFDMA_Begin(dyn_dma);
    }
    else if (dma_type == VIF_STATIC_DMA)
    {
        ps2_vif_static_dma_t * static_dma = DMA_STATIC(obj);
        static_dma->base.dma_type  = dma_type;
        static_dma->base.num_pages = num_pages;
        static_dma->base.mem_pages = calloc(num_pages, sizeof(ps2_dma_mem_page_t));

        const int total_bytes = num_pages * 4096; // 4K pages
        u8 * chunk = PS2_MemAllocAligned(16, total_bytes, MEMTAG_RENDERER);
        memset(chunk, 0, total_bytes);

        int i; // Alloc the individual pages:
        for (i = 0; i < num_pages; ++i)
        {
            ps2_dma_mem_page_t * page = &static_dma->base.mem_pages[i];
            page->start_ptr = chunk;
            page->qw_size   = 256;
            chunk          += 4096;
        }

        VIF1_ERR = 2; // Ignore DMA mismatch errors.
        VIFDMA_Begin(static_dma);
    }
    else
    {
        Sys_Error("VIFDMA_Initialize: Invalid dma_type!");
    }
}

/*
================
VIFDMA_Shutdown
================
*/
void VIFDMA_Shutdown(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    if (base == NULL)
    {
        return;
    }

    if (base->mem_pages != NULL)
    {
        free(base->mem_pages[0].start_ptr); // First page has the start pointer for the block.
        free(base->mem_pages);              // The array was also heap allocated.
    }

    memset(base, 0, sizeof(*base));
}

/*
================
VIFDMA_Begin
================
*/
void VIFDMA_Begin(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);
    base->vif_state = VIF_STATE_BASE;
    base->start_ptr = VIFDMA_NewPage(obj);
    VIFDMA_NewTag(obj); // Add the first tag of the chain
}

/*
================
VIFDMA_AddU32
================
*/
void VIFDMA_AddU32(ps2_vif_dma_obj_t obj, u32 data)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    // Would we be writing to the new page?
    if (base->write_ptr >= base->end_ptr)
    {
        VIFDMA_Stitch(obj); // If so then stitch.
    }

    *base->write_ptr++ = data;
}

/*
================
VIFDMA_AddU64
================
*/
void VIFDMA_AddU64(ps2_vif_dma_obj_t obj, u64 data)
{
    VIFDMA_AddU32(obj, (u32)data);
    VIFDMA_AddU32(obj, (u32)(data >> 32));
}

/*
================
VIFDMA_AddU128
================
*/
void VIFDMA_AddU128(ps2_vif_dma_obj_t obj, u128 data)
{
    VIFDMA_AddU64(obj, (u64)data);
    VIFDMA_AddU64(obj, (u64)(data >> 64));
}

/*
================
VIFDMA_AddFloat
================
*/
void VIFDMA_AddFloat(ps2_vif_dma_obj_t obj, float data)
{
    FU32_t f2i;
    f2i.asFloat = data;
    VIFDMA_AddU32(obj, f2i.asU32);
}

/*
================
VIFDMA_AddVector4I
================
*/
void VIFDMA_AddVector4I(ps2_vif_dma_obj_t obj, int x, int y, int z, int w)
{
    VIFDMA_AddU32(obj, x);
    VIFDMA_AddU32(obj, y);
    VIFDMA_AddU32(obj, z);
    VIFDMA_AddU32(obj, w);
}

/*
================
VIFDMA_AddVector4F
================
*/
void VIFDMA_AddVector4F(ps2_vif_dma_obj_t obj, float x, float y, float z, float w)
{
    VIFDMA_AddFloat(obj, x);
    VIFDMA_AddFloat(obj, y);
    VIFDMA_AddFloat(obj, z);
    VIFDMA_AddFloat(obj, w);
}

/*
================
VIFDMA_AddMatrix4F
================
*/
void VIFDMA_AddMatrix4F(ps2_vif_dma_obj_t obj, const float * m4x4)
{
    int i;
    for (i = 0; i < 4 * 4; ++i)
    {
        VIFDMA_AddFloat(obj, m4x4[i]);
    }
}

/*
================
VIFDMA_AddInts
================
*/
void VIFDMA_AddInts(ps2_vif_dma_obj_t obj, const int * data, int num_elements)
{
    int i;
    for (i = 0; i < num_elements; ++i)
    {
        VIFDMA_AddU32(obj, data[i]);
    }
}

/*
================
VIFDMA_AddFloats
================
*/
void VIFDMA_AddFloats(ps2_vif_dma_obj_t obj, const float * data, int num_elements)
{
    int i;
    for (i = 0; i < num_elements; ++i)
    {
        VIFDMA_AddFloat(obj, data[i]);
    }
}

/*
================
VIFDMA_Align
================
*/
void VIFDMA_Align(ps2_vif_dma_obj_t obj, int align, int offset)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    s32 p = (((s32)base->write_ptr >> 2) - offset) & (align - 1);
    if (p == 0)
    {
        return;
    }

    for (p = align - p; p > 0; --p)
    {
        VIFDMA_AddU32(obj, 0);
    }
}

/*
================
VIFDMA_PrepForDMATag
================
*/
void VIFDMA_PrepForDMATag(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);
    if (base->vif_state != VIF_STATE_BASE)
    {
        Sys_Error("VIFDMA_PrepForDMATag: Invalid call state!");
    }

    // Make sure we are aligned on a quadword boundary.
    VIFDMA_Align(obj, 4, 0);

    // We can't add a new tag at the very end of a packet, so
    // lets just add a NOP and let the stitching handle the new tag.
    if (base->write_ptr == base->end_ptr)
    {
        VIFDMA_AddU64(obj, 0);
    }
}

/*
================
VIFDMA_DMAEnd
================
*/
void VIFDMA_DMAEnd(ps2_vif_dma_obj_t obj)
{
    // This function sets the previous open DMA tag to an end tag so that the DMA chain ends.
    // Called automatically by VIFDMA_Fire().

    ps2_dma_chain_t * base = DMA_BASE(obj);
    if (base->vif_state != VIF_STATE_BASE)
    {
        Sys_Error("VIFDMA_DMAEnd: Invalid call state!");
    }

    // Pad if we are trying to transfer less than a whole number of quadwords:
    VIFDMA_Align(obj, 4, 0);

    // And finish the open DMA tag:
    *base->dma_tag = DMA_END_TAG((base->write_ptr - (u32 *)base->dma_tag) / 4 - 1);
}

/*
================
VIFDMA_DMARet
================
*/
void VIFDMA_DMARet(ps2_vif_dma_obj_t obj)
{
    // Call this when you want to return from a DMA chain in a static buffer.

    ps2_dma_chain_t * base = DMA_BASE(obj);
    VIFDMA_PrepForDMATag(obj);
    *base->dma_tag = DMA_RET_TAG((base->write_ptr - (u32 *)base->dma_tag) / 4 - 1);
    VIFDMA_NewTag(obj);
}

/*
================
VIFDMA_DMACall
================
*/
void VIFDMA_DMACall(ps2_vif_dma_obj_t obj, u32 addr)
{
    // The function will cause the DMAC to branch to the address at addr
    // and run until it hits a DMARet tag, when it will return control
    // back to the tag after this one.

    ps2_dma_chain_t * base = DMA_BASE(obj);
    VIFDMA_PrepForDMATag(obj);
    *base->dma_tag = DMA_CALL_TAG(addr, (base->write_ptr - (u32 *)base->dma_tag) / 4 - 1);
    VIFDMA_NewTag(obj);
}

/*
================
VIFDMA_StartDirect
================
*/
void VIFDMA_StartDirect(ps2_vif_dma_obj_t obj)
{
    // Start Direct (GS Path 2) mode.

    ps2_dma_chain_t * base = DMA_BASE(obj);
    if (base->vif_state != VIF_STATE_BASE)
    {
        Sys_Error("VIFDMA_StartDirect: Invalid call state!");
    }

    // The DIRECT VIF code has to go into the very last (i.e. 3rd) word of a quadword.
    VIFDMA_Align(obj, 4, 3);

    // Set the VIF state so that the stitching knows if it has to stop and start direct mode.
    base->vif_state = VIF_STATE_DIRECT;
    base->vif_code  = base->write_ptr;

    // Add the space for the DIRECT VIF code (it won't be written until
    // VIFDMA_EndDirect() is called and we know how many quad words were written).
    VIFDMA_AddU32(obj, 0);
}

/*
================
VIFDMA_EndDirect
================
*/
void VIFDMA_EndDirect(ps2_vif_dma_obj_t obj)
{
    // End Direct (GS Path 2) mode.

    ps2_dma_chain_t * base = DMA_BASE(obj);
    if (base->vif_state != VIF_STATE_DIRECT)
    {
        Sys_Error("VIFDMA_EndDirect: Invalid call state!");
    }

    // We can only transfer an integer number of quadwords,
    // so pad with NOPs if we haven't got that much.
    VIFDMA_Align(obj, 4, 0);

    const int size = (base->write_ptr - base->vif_code - 1) / 4;
    if (size != 0) // If the size isn't 0, write the DIRECT VIF code.
    {
        *base->vif_code = VIF_DIRECT(size);
    }

    base->vif_state = VIF_STATE_BASE;
}

/*
================
VIFDMA_StartAD
================
*/
void VIFDMA_StartAD(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);
    if (base->vif_state != VIF_STATE_DIRECT)
    {
        Sys_Error("VIFDMA_StartAD: Invalid call state!");
    }

    if (base->write_ptr >= base->end_ptr)
    {
        VIFDMA_EndDirect(obj);
        VIFDMA_StartDirect(obj);
    }

    // Store the location so we can add the A+D amount later.
    base->ad_gif_tag = (u64 *)base->write_ptr;

    // Append the A+D GIF tag:
    VIFDMA_AddU64(obj, (1 << 15) | (1ULL << 60));
    VIFDMA_AddU64(obj, 0xE);
}

/*
================
VIFDMA_EndAD
================
*/
void VIFDMA_EndAD(ps2_vif_dma_obj_t obj)
{
    // Nothing to be done right now, just here
    // for orthogonality with the rest of the API.
    (void)obj;
}

/*
================
VIFDMA_AddAD
================
*/
void VIFDMA_AddAD(ps2_vif_dma_obj_t obj, u64 data, u64 addr)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    // Add an A+D mode piece, data is the data to add,
    // addr is the register number to send it to.
    VIFDMA_AddU64(obj, data);
    VIFDMA_AddU64(obj, addr);

    // Increment the NLOOP in the GIF tag:
    (*base->ad_gif_tag)++;
}

/*
================
VIFDMA_StartMPG
================
*/
void VIFDMA_StartMPG(ps2_vif_dma_obj_t obj, u32 addr)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);
    if (base->vif_state != VIF_STATE_BASE)
    {
        Sys_Error("VIFDMA_StartMPG: Invalid call state!");
    }

    // MPG VIF code must be in either word 1 or 3 of a quadword.
    VIFDMA_Align(obj, 2, 1);
    base->vif_code = base->write_ptr;

    // Add the VIF code:
    VIFDMA_AddU32(obj, (0x4A << 24) + addr);
    base->mpg_count = 0;
    base->mpg_addr  = addr;
}

/*
================
VIFDMA_EndMPG
================
*/
void VIFDMA_EndMPG(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    // Write the number of double-word chunks of VU code that were written:
    *base->vif_code += (base->mpg_count & 0xFF) << 16;
}

/*
================
VIFDMA_AddMPG
================
*/
void VIFDMA_AddMPG(ps2_vif_dma_obj_t obj, u64 instruction)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    // We are only allowed to transfer 256 microcode instructions at once,
    // so if we are trying to send more we will need to use a new MPG VIF code.
    if (base->mpg_count >= 256)
    {
        VIFDMA_EndMPG(obj);
        VIFDMA_StartMPG(obj, base->mpg_addr + base->mpg_count);
    }

    // Add the instruction and increment the microcode count.
    VIFDMA_AddU64(obj, instruction);
    base->mpg_count++;
}

/*
================
VIFDMA_AddUnpackEx
================
*/
void VIFDMA_AddUnpackEx(ps2_vif_dma_obj_t obj, int format, int addr, int num_qwords, int use_tops, int no_sign, int masking)
{
    VIFDMA_AddU32(obj, (0x60 << 24) + (format << 24) + (masking << 28) + (use_tops << 15) + (no_sign << 14) + (num_qwords << 16) + addr);
}

/*
================
VIFDMA_Stitch
================
*/
void VIFDMA_Stitch(ps2_vif_dma_obj_t obj)
{
    // Stitches a DMA packet over the 4K page boundary.
    ps2_dma_chain_t * base = DMA_BASE(obj);
    const ps2_vif_state_t vif_state = base->vif_state;

    // If we are in direct mode we will have to end it,
    // stitch, and then start it again in the new page.
    if (vif_state == VIF_STATE_DIRECT)
    {
        VIFDMA_EndDirect(obj);
    }

    // Finish the previous DMA tag (i.e. set its QWC) and point it to the start of the next page
    u32 new_page = (u32)VIFDMA_NewPage(obj);
    *base->dma_tag = ((u64)new_page << 32) + (2 << 28) + (base->write_ptr - (u32 *)base->dma_tag) / 4 - 1;

    // And start a new one.
    VIFDMA_NewTag(obj);

    // Restore direct mode.
    if (vif_state == VIF_STATE_DIRECT)
    {
        VIFDMA_StartDirect(obj);
    }
}

/*
================
VIFDMA_NewTag
================
*/
void VIFDMA_NewTag(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);
    base->dma_tag = (u64 *)base->write_ptr;
    VIFDMA_AddU64(obj, 0);
}

/*
================
VIFDMA_NewPage
================
*/
u8 * VIFDMA_NewPage(ps2_vif_dma_obj_t obj)
{
    ps2_dma_chain_t * base = DMA_BASE(obj);

    if (base->dma_type == VIF_DYNAMIC_DMA)
    {
        ps2_vif_dynamic_dma_t * dyn_dma = DMA_DYNAMIC(obj);
        if (dyn_dma->curr_page >= base->num_pages)
        {
            Sys_Error("VIFDMA_NewPage: No more dynamic pages!");
        }

        const int page_idx = dyn_dma->curr_page + (dyn_dma->curr_buffer * base->num_pages);
        base->write_ptr = (u32 *)base->mem_pages[page_idx].start_ptr;
        base->end_ptr = base->write_ptr + 1024; // Advance to the end of the page (1024 4-byte uints)
        dyn_dma->curr_page++;

        return (u8 *)base->write_ptr;
    }

    if (base->dma_type == VIF_STATIC_DMA)
    {
        ps2_vif_static_dma_t * static_dma = DMA_STATIC(obj);
        if (static_dma->curr_page >= base->num_pages)
        {
            Sys_Error("VIFDMA_NewPage: No more static pages!");
        }

        base->write_ptr = (u32 *)base->mem_pages[static_dma->curr_page].start_ptr;
        base->end_ptr = base->write_ptr + 1024; // Advance to the end of the page (1024 4-byte uints)
        static_dma->curr_page++;

        return (u8 *)base->write_ptr;
    }

    // Only hit if there's an internal inconsistency.
    Sys_Error("VIFDMA_NewPage: Invalid dma_type!");
}

/*
================
VIFDMA_GetPointer
================
*/
u32 VIFDMA_GetPointer(ps2_vif_dma_obj_t obj)
{
    if (DMA_BASE(obj)->dma_type != VIF_STATIC_DMA)
    {
        Sys_Error("VIFDMA_GetPointer can only be called from a static DMA object!");
    }

    ps2_vif_static_dma_t * static_dma = DMA_STATIC(obj);
    return (u32)static_dma->base.dma_tag;
}

/*
================
VIFDMA_Fire
================
*/
void VIFDMA_Fire(ps2_vif_dma_obj_t obj)
{
    if (DMA_BASE(obj)->dma_type != VIF_DYNAMIC_DMA)
    {
        Sys_Error("VIFDMA_Fire can only be called from a dynamic DMA object!");
    }

    ps2_vif_dynamic_dma_t * dyn_dma = DMA_DYNAMIC(obj);

    // End the chain before we send it.
    VIFDMA_DMAEnd(obj);

    // Wait for channel 1 to finish any previous packets:
    while (DMA_VIF1_CHAN_CHCR & 256) { }

    EE_SYNCL();
    FlushCache(0);

    // Set the data pointer:
    DMA_VIF1_CHAN_QWC  = 0;
    DMA_VIF1_CHAN_MADR = 0;
    DMA_VIF1_CHAN_TADR = DMA_SET_TADR((u32)dyn_dma->base.start_ptr, 0);

    // Send the packet!
    DMA_VIF1_CHAN_CHCR = DMA_SET_CHCR(1, 1, 0, 1, 0, 1);
    EE_SYNCL();

    // Swap the double buffers / reset the page:
    dyn_dma->curr_buffer ^= 1;
    dyn_dma->curr_page    = 0;

    // Start a new packet:
    VIFDMA_Begin(obj);
}
