#pragma once
/* ================================================================================================
 * File: iop_boot.h
 * Brief: Boot-time IOP bring-up and game-data location. Finds where the baseq2/ data
 *        lives - host:, an APA/PFS HDD, or USB mass storage - and returns the filesystem
 *        base path to hand to FS_SetDefaultBasePath. Every IOP module the game runs is
 *        loaded here, the drivers started after the boot included: a module the launcher
 *        left resident is used when one doing that job is there, and the game's own embedded
 *        or ROM module is loaded otherwise.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::sys {

// Probes host: for the game data first (PCSX2 exposes the ELF's directory as host: and
// services it without any IOP involvement - and the probe fails instantly on hardware).
// On a miss, resets the IOP and brings up iomanX/fileXio and the drives' drivers, all of
// them the game's own. -DPS2_QUAKE_ALWAYS_RESET_IOP=0 takes the IOP as the launcher left
// it instead: each module is taken from the resident ones when a module doing its job is
// there, and loaded otherwise, with the reset only for something resident that can't work
// alongside the game's drivers, or as the fallback when a module won't start, when the
// drives show no game data through the launcher's drivers, or when too little IOP memory
// is left for the drivers that load later.
// HDD comes next, unless the ELF was launched from USB (mass:), whose boot leaves the HDD
// alone: optional DEV9/ATA/APA/PFS modules, the ELF's launch partition first,
// then other main PFS partitions. Within the launch partition, its folder precedes
// its root; other partitions' folders are all searched before their roots. If HDD
// misses, starts USB/BDM and waits up to 10 seconds, searching the ELF folder on all
// FAT volumes, then roots. elfPath is the loader's argv[0], or null if absent.
// Returns "host:", "host:.", "pfs0:" or "mass0:"-"mass9:", plus the selected
// folder. A successful HDD mount stays live; missing HDD hardware falls back to USB.
// Sys_Errors when no game data can be found anywhere.
//
// Must run from main() BEFORE Qcommon_Init: FS_InitFilesystem opens pak files
// during Qcommon_Init (before Sys_Init), and every driver started after this - sound,
// pad, memory card, keyboard, see below - goes on top of the IOP it leaves.
const char * DetectBasePathAndBootIop(const char * elfPath);

// Prints what DetectBasePathAndBootIop did, with times from main(): what the launcher
// left running, whether the IOP was reset and why, each module reused or loaded and how
// long it took, the HDD probe, the USB wait, and the IOP memory left. Called from Sys_Init,
// where it reaches the console and the log.
void PrintIopBootReport(void (*printer)(const char *, ...));

// Flushes cached data/metadata on the HDD mount selected by boot, after the caller closes
// its write handle. Returns 0 without any RPC on host:/USB boots; HDD sync blocks and
// returns 0 on success or the negative driver error code on failure.
int SyncGameDataDevice();

// Whether the game data is on the HDD mount (the one SyncGameDataDevice syncs). On the HDD a
// file call out on the IOP holds up other RPCs, the audio submit's among them (audio.md).
bool GameDataOnHdd();

// The IOP buffer fileXio serves every HDD/USB read through, in bytes, which caps how much one
// device read asks for (see PS2_QUAKE_FILEXIO_BUFFER_KB in iop_boot.cpp). 0 on host: boots,
// which read through the ROM's FILEIO instead.
int FileXioBufferBytes();

// Whether the ROM FILEIO module's remove() has been patched (sbv_patch_fileio, applied on the
// host: fast path). Unpatched, every remove() falls through into a mkdir() of the same path,
// so a deleted file comes back as an empty directory; callers deleting on host: undo that.
bool FileIoRemovePatched();

// The largest block of IOP memory that can be allocated now, to 4 KB. The EE has no RPC that
// says how much is free, so this halves its way between allocations that work and ones that
// don't: about 20 RPCs.
int LargestFreeIopBlockBytes();

// Debug helper - lists all currently loaded IOP modules.
void PrintLoadedIopModules(int maxModules, void (*printer)(const char *, ...));

// ------------------------------------------------------------------------------------------------
// The drivers that start after the boot
// ------------------------------------------------------------------------------------------------
// Every IOP module the game loads is loaded here. Each driver goes on top of the IOP the boot
// settled on: a module doing a job that is already running - started by the boot, or left by
// the launcher - is used instead of the game's own, since a second copy would fail to start
// next to it. Best-effort, unlike the boot: false leaves the caller to run without that driver.
// Each is brought up once, and later calls return the first one's result. On a host: boot the
// first of them also applies the sbv patches that let a module load out of the ELF, which a
// console boot applied already.
//
// These start the IOP side only. The EE library's own set-up (audsrv_init, mcInit, PS2KbdInit)
// stays with its caller, except libpad's: see StartPadDriver.

// The ROM's SIO2MAN and PADMAN, then libpad's padInit. When the PADMAN was left by the launcher,
// both ports are closed here: a port the launcher left open is fed into the EE buffer its
// program opened it with, every field, and that memory is the game's now. The boot calls it as
// soon as it finds such a PADMAN, IN_Init when it opens the pad.
bool StartPadDriver();

// The ROM's SIO2MAN (shared with the pad), MCMAN and MCSERV, for libmc.
bool StartMemoryCardDriver();

// The embedded libsd.irx, then audsrv.irx, which imports it, for libaudsrv.
bool StartSoundDriver();

// The embedded usbd.irx (running already on a USB boot), then ps2kbd.irx, for libkbd.
bool StartKeyboardDriver();

} // namespace ps2::sys
