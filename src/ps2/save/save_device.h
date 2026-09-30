#pragma once
/* ================================================================================================
 * File: save_device.h
 * Brief: The host-file save Device (<gamedir>/save/), and the devices' shared error reporting.
 *        See save_system.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

namespace ps2::save {

// Plain files under <gamedir>/save/, through stdio. Only offered when running from the
// emulator's host: filesystem (ps2_savedevice "host"); saves go to the memory card otherwise.
Device & GetHostDevice();

// Deleting on host: through the ROM's FILEIO leaves a directory where the file was, unless
// iop_boot managed to patch it (ps2::sys::FileIoRemovePatched). This removes that directory.
void SetHostDeleteLeavesDirectory(bool leavesDirectory);

} // namespace ps2::save
