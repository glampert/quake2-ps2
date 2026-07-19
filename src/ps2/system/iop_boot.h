#pragma once
/* ================================================================================================
 * File: iop_boot.h
 * Brief: Boot-time IOP bring-up and game-data location. Finds where the baseq2/ data
 *        lives - host: under PCSX2 (no IOP modules needed) or USB mass storage on a
 *        real console (full IOP reset + BDM driver stack) - and returns the filesystem
 *        base path to hand to FS_SetDefaultBasePath.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::sys {

// Probes host: for the game data first (PCSX2 exposes the ELF's directory as
// host: and services it without any IOP involvement - and the probe fails
// instantly on hardware). When that misses, performs the full IOP bring-up:
// reset, sbv patches, the embedded USB/BDM module chain, then waits for the
// USB drive to enumerate. Returns the base path ("host:.", "host:" or
// "mass:"); Sys_Errors when no game data can be found anywhere.
//
// Must run from main() BEFORE Qcommon_Init: FS_InitFilesystem opens pak files
// during Qcommon_Init (before Sys_Init), and the pad driver loads its rom0:
// modules later at IN_Init - after the IOP reset, which is the required order.
const char * DetectBasePathAndBootIop();

} // namespace ps2::sys
