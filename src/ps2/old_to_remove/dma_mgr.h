
/* ================================================================================================
 * -*- C -*-
 * File: dma_mgr.h
 * Author: Guilherme R. Lampert
 * Created on: 01/02/16
 * Brief: DMA memory management and VIF DMA helpers.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_DMA_MGR_H
#define PS2_DMA_MGR_H

#include <tamtypes.h>
#include "ps2/defs_ps2.h"

/*
==============================================================

DMA/VIF helper structures:

==============================================================
*/

// Basic info about a memory page/block.
typedef struct
{
    u8 * start_ptr; // Pointer to the start of this block.
    int  qw_size;   // Number of quadwords in this block.
} ps2_dma_mem_page_t;

// Direct + TTE has alignment issues, MPG, and unpack do not.
typedef enum
{
    VIF_STATE_BASE,
    VIF_STATE_DIRECT
} ps2_vif_state_t;

// Dynamic or static flags.
typedef enum
{
    VIF_DYNAMIC_DMA,
    VIF_STATIC_DMA
} ps2_vif_dma_type_t;

// Common DMA chain data.
typedef struct
{
    ps2_dma_mem_page_t * mem_pages; // Array of available pages.
    int num_pages;                  // Size of mem_pages[].

    u8  * start_ptr;                // Head of the current page.
    u32 * end_ptr;                  // The next page starts here.
    u32 * write_ptr;                // The data write pointer.

    u64 * ad_gif_tag;               // The currently open AD GIF tag.
    u64 * dma_tag;                  // The currently open DMA tag.
    u32 * vif_code;                 // The currently open VIF code.

    u32 mpg_count;                  // How many MPG instructions we have written.
    u32 mpg_addr;                   // The VU micro mem address we are writing to for MPG.

    ps2_vif_state_t vif_state;      // The VIF state at this point in the chain.
    ps2_vif_dma_type_t dma_type;    // ps2_vif_dynamic_dma_t or ps2_vif_static_dma_t.
} ps2_dma_chain_t;

// Used for data that changes every frame or even many times in a frame.
// This chain is double-buffered to allow building a new chain while the
// previous is still in-flight.
typedef struct
{
    ps2_dma_chain_t base;
    int curr_page;   // The current page we are writing to.
    int curr_buffer; // The current buffer we are writing to (0 or 1).
} ps2_vif_dynamic_dma_t;

// Used for pre-built DMA chains that don't change often
// and get re-submitted several times. Single-buffered.
typedef struct
{
    ps2_dma_chain_t base;
    int curr_page; // The current page we are writing to.
} ps2_vif_static_dma_t;

/*
==============================================================

ps2_vif_dma_obj_t common functions:

==============================================================
*/

// Opaque pointer so we can pass static or dynamic DMAs to the following functions.
typedef void * ps2_vif_dma_obj_t;

// Initialize/terminate the DMA object.
// Pointer should be either a ps2_vif_dynamic_dma_t or a ps2_vif_static_dma_t.
void VIFDMA_Initialize(ps2_vif_dma_obj_t obj, int num_pages, ps2_vif_dma_type_t dma_type);
void VIFDMA_Shutdown(ps2_vif_dma_obj_t obj);

// Starts a new DMA chain.
void VIFDMA_Begin(ps2_vif_dma_obj_t obj);

// Append scalar data to the chain:
void VIFDMA_AddU32(ps2_vif_dma_obj_t obj, u32 data);
void VIFDMA_AddU64(ps2_vif_dma_obj_t obj, u64 data);
void VIFDMA_AddU128(ps2_vif_dma_obj_t obj, u128 data);
void VIFDMA_AddFloat(ps2_vif_dma_obj_t obj, float data);

// Explicitly sized vectors/matrix:
void VIFDMA_AddVector4I(ps2_vif_dma_obj_t obj, int x, int y, int z, int w);
void VIFDMA_AddVector4F(ps2_vif_dma_obj_t obj, float x, float y, float z, float w);
void VIFDMA_AddMatrix4F(ps2_vif_dma_obj_t obj, const float * m4x4);

// Int/float arrays, for generic size vectors and matrices:
void VIFDMA_AddInts(ps2_vif_dma_obj_t obj, const int * data, int num_elements);
void VIFDMA_AddFloats(ps2_vif_dma_obj_t obj, const float * data, int num_elements);

// Aligns the write pointer to a multiple of align * 32 bits.
// The value can be offset by offset * 32 bits.
void VIFDMA_Align(ps2_vif_dma_obj_t obj, int align, int offset);

// Makes sure that we are writing a multiple of 1 quadword (pad with NO-OPS if we aren't)
// and also makes sure that the next tag isn't on a stitching boundary.
void VIFDMA_PrepForDMATag(ps2_vif_dma_obj_t obj);

// Functions to add different DMA Tags:
void VIFDMA_DMAEnd(ps2_vif_dma_obj_t obj);

// Use this function at the end of any static blocks to tell the DMAC to
// return to the dynamic buffer just after the DMACall tag.
void VIFDMA_DMARet(ps2_vif_dma_obj_t obj);

// Use this to call little chunks inside the static DMA buffer.
// Use the physical address returned by VIFDMA_GetPointer().
void VIFDMA_DMACall(ps2_vif_dma_obj_t obj, u32 addr);

// Start a section of data that should be sent directly to the GS (i.e. GIF path 2)
// These functions can go over a stitching boundary, so there is no need to worry about that.
// They also handle calculating the QWC for the DIRECT VIF Code and will automatically align
// to the last word of a quadword if needed!
void VIFDMA_StartDirect(ps2_vif_dma_obj_t obj);
void VIFDMA_EndDirect(ps2_vif_dma_obj_t obj);

// Use these functions to add data that will used in A+D mode, and these functions
// will calculate the QWC for you. (You must be in DIRECT mode to use these)
void VIFDMA_StartAD(ps2_vif_dma_obj_t obj);
void VIFDMA_EndAD(ps2_vif_dma_obj_t obj);
void VIFDMA_AddAD(ps2_vif_dma_obj_t obj, u64 data, u64 addr);

// Use these functions to transfer microcode across to VU1 via the DMAC.
void VIFDMA_StartMPG(ps2_vif_dma_obj_t obj, u32 addr);
void VIFDMA_EndMPG(ps2_vif_dma_obj_t obj);
void VIFDMA_AddMPG(ps2_vif_dma_obj_t obj, u64 instruction);

// Add a VIF unpack code. You must know the number of quadwords you are adding for this function.
void VIFDMA_AddUnpackEx(ps2_vif_dma_obj_t obj, int format, int addr, int num_qwords, int use_tops, int no_sign, int masking);
static inline void VIFDMA_AddUnpack(ps2_vif_dma_obj_t obj, int format, int addr, int num_qwords) { VIFDMA_AddUnpackEx(obj, format, addr, num_qwords, 0, 1, 0); }

// Stitches across the page boundary with a next tag.
void VIFDMA_Stitch(ps2_vif_dma_obj_t obj);

// Reserves space in the chain for a DMA tag.
void VIFDMA_NewTag(ps2_vif_dma_obj_t obj);

// Starts writing to a new memory page.
u8 * VIFDMA_NewPage(ps2_vif_dma_obj_t obj);

// Retrieve the current pointer's physical address (ps2_vif_static_dma_t only).
u32 VIFDMA_GetPointer(ps2_vif_dma_obj_t obj);

// This fires the DMA packet via path 1 (ps2_vif_dynamic_dma_t only).
void VIFDMA_Fire(ps2_vif_dma_obj_t obj);

#endif // PS2_DMA_MGR_H
