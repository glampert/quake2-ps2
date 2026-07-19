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
    if (id < 0 || moduleResult == 1)
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

} // namespace ps2::sys
