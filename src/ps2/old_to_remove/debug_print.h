
/* ================================================================================================
 * -*- C -*-
 * File: debug_print.h
 * Author: Guilherme R. Lampert
 * Created on: 14/10/15
 * Brief: Very crude debug printing to screen (PS2). Only used for fatal error reporting and dev.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_DEBUG_PRINT_H
#define PS2_DEBUG_PRINT_H

#include "common/q_common.h"

// Char printing invariants:
enum
{
    DBG_SCR_CHAR_SIZE = 8,
    DBG_SCR_MAX_X     = 80,
    DBG_SCR_MAX_Y     = 40
};

// Lazily initialized by first print if not done explicitly.
void Dbg_ScrInit(void);

// Print char to specified position using provided text color.
void Dbg_ScrPrintChar(int x, int y, unsigned int color, int ch);

// Print at current position moving the cursor and handling newlines.
// Uses the currently set text and background colors.
void Dbg_ScrPrintf(const char * format, ...) Q_PRINTF_FUNC(1, 2);

// Get/set the current text position used by Dbg_ScrPrintf.
int Dbg_ScrGetPrintPosX(void);
int Dbg_ScrGetPrintPosY(void);
void Dbg_ScrSetPrintPos(int x, int y);

// Get/set the current BACKGROUND color for Dbg_ScrPrintf and Dbg_ScrPrintChar.
void Dbg_ScrSetBgColor(unsigned int color);
unsigned int Dbg_ScrGetBgColor(void);

// Get/set the current TEXT color for Dbg_ScrPrintf only.
void Dbg_ScrSetTextColor(unsigned int color);
unsigned int Dbg_ScrGetTextColor(void);

// Clear all previous prints or just a given line.
// Dbg_ScrClear resets the text position to (0,0),
// Dbg_ScrClearLine doesn't change the cursor position.
void Dbg_ScrClear(void);
void Dbg_ScrClearLine(int y);

#endif // PS2_DEBUG_PRINT_H
