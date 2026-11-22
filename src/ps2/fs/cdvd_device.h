#pragma once
/* ================================================================================================
 * File: cdvd_device.h
 * Brief: The disc build's game data device (PS2_QUAKE_ISO): a read-only ISO9660 file system on
 *        the CD/DVD drive, read sector by sector through libcdvd's sceCdRead and installed as the
 *        "cdrom0:" device of newlib's file calls. With it, fopen/open/read/lseek/stat on
 *        "cdrom0:/baseq2/..." work as they do on any other drive, so the engine's file system,
 *        the compressed packs, the background loader and the music reader run unchanged.
 *
 *        Why not one of the SDK's or the ROM's CD file systems: ps2sdk's cdfs.irx serves 4 open
 *        files at most and 16 KB per read, where the game holds more than 4 open at once (a pak
 *        twice, the music, a cinematic or demo stream, config.cfg), and the ROM's cdrom0: driver
 *        wants upper-case 8.3-style names with ";1" on the end. Reading sectors is also what
 *        Open PS2 Loader emulates for every game: it swaps the IOP's CD drivers for its own,
 *        which answer sceCdRead out of the disc image on a USB drive, hard disk or network share.
 *        So here the disc's directories are read once, at the mount, into an index; names match
 *        without regard to case or a ";1"; any number of files can be open; and reads go
 *        straight into the caller's memory when it is aligned for DMA, through a 64 KB buffer
 *        otherwise. See README.md, "Disc images".
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::cdvd {

// The device the disc's files are found under, and the base path of the game data on it.
constexpr const char * kBasePath = "cdrom0:";

// Brings the drive up, waits for a disc (up to ~15 s while it is still being detected), reads its
// ISO9660 directories into an index, and puts the device in front of newlib's file calls. False,
// with the reason in *failure, when there is no disc, or none with an ISO9660 file system.
// The IOP must have its CD drivers (CDVDMAN, CDVDFSV) running: the boot ROM's, or OPL's.
bool Mount(const char ** failure);

// For the boot report: the disc's media type, files and directories, the index's size and how
// long the mount took. Empty before Mount.
const char * MountSummary();

// Every read the device has made, for the I/O log: sceCdRead calls, sectors, the ones that failed
// and were tried again, and the drive's last error code (sceCdGetError; -1 for a drive that
// stayed not ready), 0 for none.
struct ReadStats
{
    u32 reads;
    u32 sectors;
    u32 retries;
    int lastError;
};

ReadStats GetReadStats();

} // namespace ps2::cdvd
