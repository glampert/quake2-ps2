#pragma once
/* ================================================================================================
 * File: mc_icon.h
 * Brief: The files that make the save directory show up properly in the PS2 browser: icon.sys
 *        (title, background, lighting) and the 3D icon it names. See mc_icon.cpp.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

namespace ps2::save {

constexpr const char * kIconSysFile   = "icon.sys";
constexpr const char * kIconModelFile = "icon.ico";

// Builds both files and writes them into the device's save directory.
// False (SetError) if they couldn't be written.
bool WriteSaveIcons(Device & device);

} // namespace ps2::save
