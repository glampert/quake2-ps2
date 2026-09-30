/* ================================================================================================
 * File: iop_boot.cpp
 * Brief: Boot-time IOP bring-up and game-data location. See iop_boot.h.
 *
 *  The USB path boots the modern BDM stack: iomanX + fileXio (extended IO manager the
 *  block-device filesystem registers with), bdm + bdmfs_fatfs (block device manager and
 *  FAT driver providing mass:), usbd + usbmass_bd (USB core and mass-storage block
 *  device). fileXioInit() then swaps the newlib backend so plain fopen/fread - and with
 *  them the whole Quake filesystem - reach mass: transparently.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"

#include <cstdio>
#include <cstring>

#include <kernel.h>
#include <smod.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <loadfile.h>
#include <sbv_patches.h>

// The header refuses direct fio/fileXio use alongside newlib unless told the
// caller knows what it is doing. We only call fileXioInit() - which installs
// the newlib backend, the exact supported arrangement - never raw file ops.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

// IRX module images embedded by the Makefile's bin2c rule (IRX_FILES).
extern "C" {
extern unsigned char iomanX_irx[];
extern unsigned int  size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int  size_fileXio_irx;
extern unsigned char bdm_irx[];
extern unsigned int  size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[];
extern unsigned int  size_bdmfs_fatfs_irx;
extern unsigned char usbd_irx[];
extern unsigned int  size_usbd_irx;
extern unsigned char usbmass_bd_irx[];
extern unsigned int  size_usbmass_bd_irx;
}

namespace ps2::sys {
namespace {

// Set once the USB branch below has run. See UsbStackStarted().
static bool s_usbStackStarted = false;

// Set once SIF RPC is up and modules can be loaded out of an EE buffer.
// See StartIopModuleFromBuffer().
static bool s_moduleLoaderReady = false;

// See FileIoRemovePatched().
static bool s_fileIoRemovePatched = false;

// The ROM modules LoadRomModuleOnce has been asked for, and how loading them went.
struct RomModule
{
    const char * path;
    bool         loaded;
};

constexpr int kMaxRomModules = 8;
static RomModule s_romModules[kMaxRomModules];
static int       s_numRomModules = 0;

// The file probed for under "<base>/baseq2/" to decide a base path works.
constexpr const char * kProbeFile = "pak0.pak";

// How long to wait for the USB drive: enumeration + FAT mount happen
// asynchronously after usbmass_bd starts.
constexpr int kUsbWaitTotalMsec = 5000;
constexpr int kUsbWaitStepMsec  = 100;

bool CanOpen(const char * path)
{
    std::FILE * file = std::fopen(path, "rb");
    if (file != nullptr)
    {
        std::fclose(file);
        return true;
    }
    return false;
}

void ExecIopModule(const char * name, void * image, u32 sizeBytes)
{
    int moduleResult = 0;
    const int id = SifExecModuleBuffer(image, sizeBytes, 0, nullptr, &moduleResult);

    // Negative id = the load itself failed; result 1 = the module's _start
    // bailed out (NO_RESIDENT_END) - either way the driver is not running.
    if (id < 0 || moduleResult == 1) [[unlikely]]
    {
        Sys_Error("IOP boot: module '%s' failed (id %d, result %d)", name, id, moduleResult);
    }
    std::printf("IOP boot: started '%s' (id %d)\n", name, id);
}

// Crude millisecond wait; fine for boot-time polling.
void BusyWaitMsec(int msec)
{
    const int until = Sys_Milliseconds() + msec;
    while (Sys_Milliseconds() < until) {}
}

// Prerequisites for SifExecModuleBuffer, for drivers started after boot.
bool EnsureModuleLoaderReady()
{
    if (s_moduleLoaderReady)
    {
        return true;
    }

    SifInitRpc(0);

    // The USB branch of DetectBasePathAndBootIop() reset the IOP and patched it
    // already; the host: fast path skipped the whole bring-up, so do it here.
    if (!s_usbStackStarted && sbv_patch_enable_lmb() != 0)
    {
        Com_Printf("WARNING: sbv_patch_enable_lmb failed - no IOP module can be loaded!\n");
        return false;
    }

    s_moduleLoaderReady = true;
    return true;
}

} // namespace

const char * DetectBasePathAndBootIop()
{
    // host: fast path (PCSX2). Probe exactly the paths the Quake filesystem
    // will build from each base ("<base>/baseq2/..."): PCSX2 builds have
    // differed on whether "host:/" is ELF-relative or host-absolute, so try
    // the explicitly relative form too. Skips the IOP reset entirely.
    struct HostCandidate
    {
        const char * probePath;
        const char * basePath;
    };
    const HostCandidate hostCandidates[] = {
        { "host:/baseq2/pak0.pak",  "host:"  },
        { "host:./baseq2/pak0.pak", "host:." },
    };

    for (const HostCandidate & candidate : hostCandidates)
    {
        if (CanOpen(candidate.probePath))
        {
            std::printf("IOP boot: game data on %s/baseq2 (emulator host filesystem).\n", candidate.basePath);

            // host: is served by the ROM's FILEIO module here, whose remove() is missing a
            // break and runs a mkdir() of the same path after it. The save code deletes files.
            SifInitRpc(0);
            s_fileIoRemovePatched = (sbv_patch_fileio() == 0);
            std::printf("IOP boot: FILEIO remove() patch %s.\n", s_fileIoRemovePatched ? "applied" : "not applicable");
            return candidate.basePath;
        }
    }

    std::printf("IOP boot: no host: game data; bringing up USB mass storage...\n");

    // Reboot the IOP into a clean state and patch in support for loading
    // EE-embedded modules. The pad driver's rom0: modules load later (IN_Init),
    // safely after this reset.
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);

    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    ExecIopModule("iomanX",      iomanX_irx,      size_iomanX_irx);
    ExecIopModule("fileXio",     fileXio_irx,     size_fileXio_irx);
    ExecIopModule("bdm",         bdm_irx,         size_bdm_irx);
    ExecIopModule("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);
    ExecIopModule("usbd",        usbd_irx,        size_usbd_irx);
    ExecIopModule("usbmass_bd",  usbmass_bd_irx,  size_usbmass_bd_irx);

    s_usbStackStarted = true;

    // Route newlib file IO through fileXio -> iomanX: bdmfs registers mass:
    // with iomanX, which the plain kernel fio path cannot reach.
    fileXioInit();

    for (int waited = 0; waited <= kUsbWaitTotalMsec; waited += kUsbWaitStepMsec)
    {
        if (CanOpen("mass:/baseq2/pak0.pak"))
        {
            std::printf("IOP boot: game data on mass:/baseq2 (USB, ready after ~%d ms).\n", waited);
            return "mass:";
        }
        BusyWaitMsec(kUsbWaitStepMsec);
    }

    Sys_Error("No game data found!\n"
              "Emulator: enable the host filesystem and put baseq2/ next to the ELF.\n"
              "Console: USB drive with a baseq2/ folder (%s etc).", kProbeFile);
    return nullptr; // unreachable; Sys_Error halts
}

bool UsbStackStarted()
{
    return s_usbStackStarted;
}

bool LoadRomModuleOnce(const char * path)
{
    for (int i = 0; i < s_numRomModules; ++i)
    {
        if (std::strcmp(s_romModules[i].path, path) == 0)
        {
            return s_romModules[i].loaded;
        }
    }

    SifInitRpc(0);
    const int id = SifLoadModule(path, 0, nullptr);
    const bool loaded = (id >= 0);

    if (loaded)
    {
        Com_Printf("IOP module '%s' started (id %d).\n", path, id);
    }
    else
    {
        Com_Printf("WARNING: IOP module '%s' failed to load (%d).\n", path, id);
    }

    if (s_numRomModules < kMaxRomModules)
    {
        s_romModules[s_numRomModules++] = { path, loaded };
    }
    return loaded;
}

bool FileIoRemovePatched()
{
    return s_fileIoRemovePatched;
}

bool StartIopModuleFromBuffer(const char * name, void * image, u32 sizeBytes)
{
    if (!EnsureModuleLoaderReady())
    {
        return false;
    }

    int moduleResult = 0;
    const int id = SifExecModuleBuffer(image, sizeBytes, 0, nullptr, &moduleResult);

    // Negative id = the load itself failed; result 1 = the module's _start bailed
    // out (NO_RESIDENT_END) - either way the driver is not running.
    if (id < 0 || moduleResult == 1)
    {
        Com_Printf("WARNING: IOP module '%s' failed (id %d, result %d).\n", name, id, moduleResult);
        return false;
    }

    Com_Printf("IOP module '%s' started (id %d).\n", name, id);
    return true;
}

void PrintLoadedIopModules(const int maxModules, void (*printer)(const char *, ...))
{
    // Module names live in IOP RAM and arrive here by SIF DMA, so this buffer must be
    // cache-line aligned, not merely "a char array": the DMAC ignores the low bits of
    // an unaligned destination, dropping the transfer short of the buffer, and a
    // neighbour sharing the first or last line can later write its dirty line back
    // over the transferred bytes. 64 is the EE cache line size; ps2sdk aligns its own
    // smod scratch buffer the same way. A plain char[] only gets 8-byte alignment.
    // 128 bytes so the buffer owns both cache lines outright and the guard NUL
    // at [64] can't share a line with anything else.
    constexpr int kNameXferSize = 64; // Bytes fetched per module, one cache line.
    alignas(64) static char s_moduleNameBuff[kNameXferSize * 2];

    smod_mod_info_t moduleInfo = {};
    SifRpcReceiveData_t rpcRecvData = {};

    if (smod_get_next_mod(nullptr, &moduleInfo) == 0)
    {
        printer("Error: Couldn't get module list!");
        return;
    }

    int evenOdd = 0;
    int printedCount = 0;

    // Table header:
    // (Print two tables side-by side, since our console has very few lines).
    printer("|    IOP module name    | id |    IOP module name    | id |\n");
    s_moduleNameBuff[kNameXferSize] = '\0'; // Terminator for an unterminated 64-byte name.
    do
    {
        // Write back and invalidate before the transfer: the DMA lands in RAM behind
        // the EE's back, so any stale/dirty line for this buffer must be gone first.
        SyncDCache(s_moduleNameBuff, s_moduleNameBuff + kNameXferSize);

        if (SifRpcGetOtherData(&rpcRecvData, moduleInfo.name, s_moduleNameBuff, kNameXferSize, 0) >= 0)
        {
            if (s_moduleNameBuff[0] == '\0')
            {
                // Unnamed module.
                strcpy(s_moduleNameBuff, "???");
            }
            else
            {
                // Truncate to 21 chars, the size of the name column.
                s_moduleNameBuff[21] = '\0';
            }

            // Print a table row (we print two tables side-by-side to save lines).
            if (!(evenOdd++ & 1))
            {
                printer("| %-21s | %-2u |", s_moduleNameBuff, static_cast<u32>(moduleInfo.id));
            }
            else
            {
                printer(" %-21s | %-2u |\n", s_moduleNameBuff, static_cast<u32>(moduleInfo.id));
            }

            if (++printedCount == maxModules)
            {
                break;
            }
        }
    } while (smod_get_next_mod(&moduleInfo, &moduleInfo) != 0);

    if (evenOdd & 1)
    {
        printer("\n");
    }
    printer(">> Listed %d modules\n", printedCount);
}

} // namespace ps2::sys
