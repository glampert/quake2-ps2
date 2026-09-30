#pragma once
/* ================================================================================================
 * File: memcard.h
 * Brief: The memory card save Device: the PS2 memory card in MEMORY CARD slot 1, through libmc
 *        and the console ROM's MCMAN/MCSERV drivers. See save_system.h and memcard.cpp.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

namespace ps2::save {

// The game's directory on the card, holding icon.sys, the icon and every save file.
// The PS2 browser shows it as one entry, titled from icon.sys.
constexpr const char * kCardSaveDir = "Q2PS2";

Device & GetMemoryCardDevice();

} // namespace ps2::save
