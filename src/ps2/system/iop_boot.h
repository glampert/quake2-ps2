#pragma once
/* ================================================================================================
 * File: iop_boot.h
 * Brief: Boot-time IOP bring-up and game-data location. Finds where the baseq2/ data
 *        lives - host:, an APA/PFS HDD, or USB mass storage - and returns the filesystem
 *        base path to hand to FS_SetDefaultBasePath.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::sys {

// Probes host: for the game data first (PCSX2 exposes the ELF's directory as
// host: and services it without any IOP involvement - and the probe fails
// instantly on hardware). On a miss, resets/patches the IOP and starts iomanX/fileXio.
// HDD comes next: optional DEV9/ATA/APA/PFS modules, the ELF's launch partition first,
// then other main PFS partitions. Within the launch partition, its folder precedes
// its root; other partitions' folders are all searched before their roots. If HDD
// misses, starts USB/BDM and waits up to 10 seconds, searching the ELF folder on all
// FAT volumes, then roots. elfPath is the loader's argv[0], or null if absent.
// Returns "host:", "host:.", "pfs0:" or "mass0:"-"mass9:", plus the selected
// folder. A successful HDD mount stays live; missing HDD hardware falls back to USB.
// Sys_Errors when no game data can be found anywhere.
//
// Must run from main() BEFORE Qcommon_Init: FS_InitFilesystem opens pak files
// during Qcommon_Init (before Sys_Init), and the pad driver loads its rom0:
// modules later at IN_Init - after the IOP reset, which is the required order.
const char * DetectBasePathAndBootIop(const char * elfPath);

// Flushes cached data/metadata on the HDD mount selected by boot, after the caller closes
// its write handle. Returns 0 without any RPC on host:/USB boots; HDD sync blocks and
// returns 0 on success or the negative driver error code on failure.
int SyncGameDataDevice();

// True once the USB storage path has started usbd.irx. False on host: and HDD boots,
// so the keyboard must start usbd itself. Module-loader readiness is tracked
// separately: both HDD and USB boots already reset/patched the IOP.
bool UsbStackStarted();

// Starts an IRX image embedded in the ELF by the Makefile's bin2c rule. Unlike the
// boot-time module chain above this is best-effort: it returns false instead of
// halting, leaving the caller to run without that driver (see input/keyboard.cpp
// and audio/audsrv_device.cpp).
//
// Takes care of the prerequisites the "host:" fast path skipped - SIF RPC is brought
// up, and the sbv patch that permits loading a module out of an EE buffer is applied
// when the IOP was never reset. Both happen once, however many drivers call in.
bool StartIopModuleFromBuffer(const char * name, void * image, u32 sizeBytes);

// Loads a module from the console's ROM ("rom0:SIO2MAN", ...) the first time any caller asks
// for it, and returns that first result to every later caller - several drivers share the
// ROM's serial port manager, and a second copy of it must not be loaded. Best-effort, like
// StartIopModuleFromBuffer.
bool LoadRomModuleOnce(const char * path);

// Whether the ROM FILEIO module's remove() has been patched (sbv_patch_fileio, applied on the
// host: fast path). Unpatched, every remove() falls through into a mkdir() of the same path,
// so a deleted file comes back as an empty directory; callers deleting on host: undo that.
bool FileIoRemovePatched();

// Debug helper - lists all currently loaded IOP modules.
void PrintLoadedIopModules(int maxModules, void (*printer)(const char *, ...));

} // namespace ps2::sys
