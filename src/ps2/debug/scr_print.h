#pragma once
/* ================================================================================================
 * File: scr_print.h
 * Brief: Very crude debug printing to screen (PS2). Only used for fatal error reporting and dev.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include <tamtypes.h>

namespace ps2::debug {

// Char printing invariants.
constexpr int ScrCharSize = 8;
constexpr int ScrMaxX = 80;
constexpr int ScrMaxY = 40;

// Lazily initialized by first print if not done explicitly.
void ScrInit();

// Print char to specified position using provided text color.
void ScrPrintChar(int x, int y, u32 color, int ch);

// Print at current position moving the cursor and handling newlines.
// Uses the currently set text and background colors.
void ScrPrintf(const char * format, ...) Q_PRINTF_FUNC(1, 2);

// Get/set the current text position used by ScrPrintf.
int ScrGetPrintPosX();
int ScrGetPrintPosY();
void ScrSetPrintPos(int x, int y);

// Get/set the current BACKGROUND color for ScrPrintf and ScrPrintChar.
void ScrSetBgColor(u32 color);
u32 ScrGetBgColor();

// Get/set the current TEXT color for ScrPrintf only.
void ScrSetTextColor(u32 color);
u32 ScrGetTextColor();

// Clear all previous prints or just a given line.
// ScrClear resets the text position to (0,0),
// ScrClearLine doesn't change the cursor position.
void ScrClear();
void ScrClearLine(int y);

} // namespace ps2::debug
