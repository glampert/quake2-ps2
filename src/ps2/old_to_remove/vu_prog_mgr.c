
/* ================================================================================================
 * -*- C -*-
 * File: vu_prog_mgr.c
 * Author: Guilherme R. Lampert
 * Created on: 01/02/16
 * Brief: Vector Unit microprogram management.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/vu_prog_mgr.h"
#include "common/q_common.h"

/*
================
VU_ProgManagerInit
================
*/
void VU_ProgManagerInit(ps2_vu_prog_manager_t * mgr)
{
    VU_ProgManagerClearActiveProgs(mgr);
    // Nothing else to initialize right now...
}

/*
================
VU_ProgManagerClearActiveProgs
================
*/
void VU_ProgManagerClearActiveProgs(ps2_vu_prog_manager_t * mgr)
{
    int p;
    for (p = 0; p < MAX_ACTIVE_VU1_PROGS; ++p)
    {
        mgr->active_vu1_progs[p] = NULL;
    }

    mgr->dw_vu1_progmem_used = 0;
}

/*
================
VU_InitMicroprogram
================
*/
void VU_InitMicroprogram(ps2_vif_dma_obj_t dma_obj, ps2_vu_prog_t * prog,
                         ps2_vu_prog_type_t type, int dw_start_offset)
{
    if (prog == NULL || prog->code_start_ptr == NULL || prog->code_end_ptr == NULL)
    {
        Sys_Error("VU_InitMicroprogram: Null/invalid microprogram!");
    }
    if (type != VU1_MICROPROGRAM)
    {
        Sys_Error("VU_InitMicroprogram: Only VU1 microprograms supported right now!");
    }
    if (dw_start_offset < 0)
    {
        Sys_Error("VU_InitMicroprogram: Bad start offset!");
    }

    prog->dw_code_size     = prog->code_end_ptr - prog->code_start_ptr;
    prog->dw_vu_mem_offset = dw_start_offset;
    prog->prog_type        = type;

    // Create a static DMA segment that has an MPG VIF code
    // followed by the actual microcode data.

    prog->upload_ptr = VIFDMA_GetPointer(dma_obj);
    VIFDMA_StartMPG(dma_obj, prog->dw_vu_mem_offset);

    int i;
    for (i = 0; i < prog->dw_code_size; ++i)
    {
        VIFDMA_AddMPG(dma_obj, prog->code_start_ptr[i]);
    }

    VIFDMA_EndMPG(dma_obj);
    VIFDMA_DMARet(dma_obj);
}

/*
================
VU_UploadMicroprogram
================
*/
void VU_UploadMicroprogram(ps2_vu_prog_manager_t * mgr, ps2_vif_dma_obj_t dma_obj,
                           const ps2_vu_prog_t * prog, int index, int force)
{
    if (index < 0 || index >= MAX_ACTIVE_VU1_PROGS)
    {
        Sys_Error("VU_UploadMicroprogram: Invalid index: %d", index);
    }
    if (prog->prog_type != VU1_MICROPROGRAM)
    {
        Sys_Error("VU_UploadMicroprogram: Only VU1 microprograms supported right now!");
    }

    if (!force)
    {
        int p;
        for (p = 0; p < MAX_ACTIVE_VU1_PROGS; ++p)
        {
            if (mgr->active_vu1_progs[p] == prog)
            {
                return; // Already uploaded; do nothing.
            }
        }
    }

    if (mgr->active_vu1_progs[index] != NULL)
    {
        mgr->dw_vu1_progmem_used -= mgr->active_vu1_progs[index]->dw_code_size;
    }

    mgr->active_vu1_progs[index] = prog;
    mgr->dw_vu1_progmem_used += prog->dw_code_size;

    if (mgr->dw_vu1_progmem_used > MAX_VU1_PROGMEM_DWORDS)
    {
        Sys_Error("VU_UploadMicroprogram: MAX_VU1_PROGMEM_DWORDS overflowed!");
    }

    VIFDMA_AddU32(dma_obj, VIF_FLUSH_E);
    VIFDMA_DMACall(dma_obj, prog->upload_ptr);
}
