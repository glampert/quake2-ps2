#pragma once
/* ================================================================================================
 * File: drive.h
 * Brief: The save Device on the drive the game booted from - the emulator's host: filesystem, a
 *        USB drive or the hard disk - holding the same slot archives as the memory card, as files
 *        under <gamedir>/save/ (baseq2/save/). See save_system.h and drive.cpp.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

namespace ps2::save {

// The drive the game data was found on (FS_Gamedir's device): its Kind() is HostFiles, UsbDrive
// or HardDisk.
Device & GetDriveDevice();

} // namespace ps2::save
