
/* ================================================================================================
 * -*- C -*-
 * File: vu1.h
 * Author: Guilherme R. Lampert
 * Created on: 17/01/16
 * Brief: Vector Unit 1 (VU1) microcode upload and VU management.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_VU1_H
#define PS2_VU1_H

#include "ps2/vu_prog_mgr.h"

// Initialize local VU1 library data. Call it at renderer startup.
void VU1_Init(void);
void VU1_Shutdown(void);

// Send program microcode to the VU1.
void VU1_UploadProg(int dest, void * start, void * end);

// Begin a new program run;
// End the current list and start the VU1 program (located in micromem 'start' address)
void VU1_Begin(void);
void VU1_End(int start);

// Begin a new primitive list:
void VU1_ListAddBegin(int address);
void VU1_ListAddEnd(void);

// Add data to the current list:
void VU1_ListData(int dest_address, void * data, int quad_size);
void VU1_ListAdd128(u64 v1, u64 v2);
void VU1_ListAdd64(u64 v);
void VU1_ListAdd32(u32 v);
void VU1_ListAddFloat(float v);

// Adds an empty 128-bytes GIF tag + reglist to the draw list.
// Returns the pointer to the first of the two quadwords.
// You can then fill it with the tag data anytime before VU1_End().
u64 * VU1_ListAddGIFTag(void);

#endif // PS2_VU1_H
