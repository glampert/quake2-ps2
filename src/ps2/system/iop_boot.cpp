/* ================================================================================================
 * File: iop_boot.cpp
 * Brief: Boot-time IOP bring-up and game-data location, and every IOP module the game loads
 *        afterwards (pad, memory card, sound, keyboard), with the sbv patches that loading
 *        them takes. See iop_boot.h.
 *
 *  After the host: probe, both console paths run on iomanX + fileXio. HDD uses DEV9 + ATAD + APA
 *  + PFS; USB uses BDM + FatFs + usbd + usbmass_bd. fileXioInit() routes newlib fopen/fread - and
 *  the whole Quake filesystem - through iomanX, reaching the mounted pfs0: partition or massN:
 *  volumes transparently.
 *
 *  The IOP is taken as the launcher left it, which in practice is one of two things:
 *
 *   - Freshly reset: the boot ROM's modules, sometimes with the ROM's SIO2MAN, MCMAN and MCSERV
 *     on top. LoadExecPS2 launches leave this, and so do wLaunchELF since May 2026 (ps2sdk's
 *     elf-loader) and older wLaunchELF for an ELF on the HDD.
 *   - The launcher's own drivers, still running: older wLaunchELF and uLaunchELF reset nothing
 *     for an ELF on USB or a memory card. They leave iomanX, fileXio, a USB stack (BDM, or
 *     usbhdfsd before 2022), the ROM's SIO2MAN and PADMAN, ps2sdk's memory card drivers, and
 *     the HDD's drivers if they read it.
 *
 *  Either way, the reset the boot used to start with only threw away what it would load again.
 *  So each module is now taken from the running ones when a module doing its job is there -
 *  recognised by the name it registers, see kModuleIdentities - and loaded otherwise. A USB drive
 *  the launcher already mounted is then read with no IOP reset and no wait for the drive. The
 *  reset stays for what can't work alongside the game's drivers (FindResidentConflict), and as
 *  the fallback when a module won't start on top of what was there, when the launcher's drivers
 *  show no game data, or when too little IOP memory is left for the drivers that load later.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <kernel.h>
#include <smod.h>
#include <slib.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <libpad.h>
#include <sbv_patches.h>
#include <hdd-ioctl.h>

// The header refuses direct fio/fileXio use alongside newlib unless told the
// caller knows what it is doing. File reads use the newlib backend installed by
// fileXioInit(); direct calls below enumerate, mount and sync HDD partitions.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

// IRX module images embedded by the Makefile's bin2c rule (IRX_FILES).
extern "C" {
extern unsigned char iomanX_irx[];
extern unsigned int  size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int  size_fileXio_irx;
extern unsigned char ps2dev9_irx[];
extern unsigned int  size_ps2dev9_irx;
extern unsigned char ps2atad_irx[];
extern unsigned int  size_ps2atad_irx;
extern unsigned char ps2hdd_irx[];
extern unsigned int  size_ps2hdd_irx;
extern unsigned char ps2fs_irx[];
extern unsigned int  size_ps2fs_irx;
extern unsigned char bdm_irx[];
extern unsigned int  size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[];
extern unsigned int  size_bdmfs_fatfs_irx;
extern unsigned char usbd_irx[];
extern unsigned int  size_usbd_irx;
extern unsigned char usbmass_bd_irx[];
extern unsigned int  size_usbmass_bd_irx;
extern unsigned char ps2kbd_irx[];
extern unsigned int  size_ps2kbd_irx;
extern unsigned char libsd_irx[];
extern unsigned int  size_libsd_irx;
extern unsigned char audsrv_irx[];
extern unsigned int  size_audsrv_irx;
}

// 1 resets the IOP on every console boot, as boots did before they took the IOP as launched:
// to compare the two on hardware, or should a launcher ever leave something that gets past
// FindResidentConflict.
#ifndef PS2_QUAKE_ALWAYS_RESET_IOP
#define PS2_QUAKE_ALWAYS_RESET_IOP 0
#endif

namespace ps2::sys {
namespace {

// ------------------------------------------------------------------------------------------------
// The modules the game runs on
// ------------------------------------------------------------------------------------------------

// What a module doing each job registers itself as with the IOP's module manager - the name in
// its IRX_ID - so that one a launcher left running is recognised and used rather than loaded
// again: a second copy would fail to start next to it, finding its export library, RPC server or
// device name taken. The game's own module comes first in each list, then what a launcher may
// run for the same job: Sony's LIBSD, the ps2sdk or X-module memory card manager. Names are from
// ps2sdk's IRX files and the ROM's (SCPH-70012 BIOS 2.00).
//
// A module can also be known by the library it exports, whatever its name: iomanX's builds have
// no IRX_ID at all, and the USB driver wLaunchELF 4.43a left on a console (2026-10-12) didn't go
// by ps2sdk's name - the game's usbd then found the "usbd" library taken and wouldn't start.
struct ModuleIdentity
{
    const char * label;      // the game's name for it: the IRX's file name, or the rom0: path
    const char * names[2];   // what the IOP lists a module doing the job as
    const char * library;    // the export library any module doing the job registers, or null
    u16          minVersion; // the oldest that talks the protocol of our EE library; 0 for any
};

constexpr ModuleIdentity kModuleIdentities[] = {
    { "iomanX",       { nullptr,                   nullptr                }, "iomanx", 0      },
    { "fileXio",      { "IOX/File_Manager_Rpc",    nullptr                }, nullptr,  0x0102 },
    { "ps2dev9",      { "dev9",                    nullptr                }, nullptr,  0      },
    { "ps2atad",      { "atad",                    nullptr                }, nullptr,  0      },
    { "ps2hdd",       { "hdd_driver",              nullptr                }, nullptr,  0      },
    { "ps2fs",        { "pfs_driver",              nullptr                }, nullptr,  0      },
    { "bdm",          { "bdm",                     nullptr                }, nullptr,  0      },
    { "bdmfs_fatfs",  { "bdmff",                   nullptr                }, nullptr,  0      },
    { "usbd",         { "USB_driver",              nullptr                }, "usbd",   0      },
    { "usbmass_bd",   { "usbmass_bd",              nullptr                }, nullptr,  0      },
    { "ps2kbd",       { "PS2 USB keyboard driver", nullptr                }, nullptr,  0      },
    { "libsd",        { "freesd",                  "Sound_Device_Library" }, nullptr,  0      },
    { "audsrv",       { "audsrv",                  nullptr                }, nullptr,  0x0104 },
    { "rom0:SIO2MAN", { "sio2man",                 nullptr                }, nullptr,  0      },
    { "rom0:PADMAN",  { "padman",                  nullptr                }, nullptr,  0      },
    { "rom0:MCMAN",   { "mcman",                   "mcman_cex"            }, nullptr,  0      },
    { "rom0:MCSERV",  { "mcserv",                  nullptr                }, nullptr,  0      },
};

// ps2sdk's USB mass storage driver before BDM, which older launchers still run. It serves mass:
// by itself, so BDM's FAT driver couldn't register that device next to it.
constexpr const char * kUsbhdfsdName = "usbhdfsd";

// The boot ROM's module loader, whose version says whether it loads modules out of EE memory.
constexpr const char * kLoadfileName = "LoadModuleByEE";

// APA holds an open handle for each PFS mount, plus one while listing partitions.
// PFS defaults to only two open files; pak handles, a music stream, temporary
// asset reads and the log can coexist. Its cache needs at least 2 * open files + 8.
constexpr char kHddArgs[] = "-o\0" "4\0" "-n\0" "20";
constexpr char kPfsArgs[] = "-m\0" "1\0" "-o\0" "16\0" "-n\0" "40";

// An IRX the ELF carries, with the arguments the boot starts it with.
struct IrxModule
{
    const char *         label; // as in kModuleIdentities
    unsigned char *      image;
    const unsigned int * sizeBytes;
    const char *         args;
    u32                  argsBytes;
};

const IrxModule kIomanX     = { "iomanX",      iomanX_irx,      &size_iomanX_irx,      nullptr,  0                };
const IrxModule kFileXio    = { "fileXio",     fileXio_irx,     &size_fileXio_irx,     nullptr,  0                };
const IrxModule kPs2Dev9    = { "ps2dev9",     ps2dev9_irx,     &size_ps2dev9_irx,     nullptr,  0                };
const IrxModule kPs2Atad    = { "ps2atad",     ps2atad_irx,     &size_ps2atad_irx,     nullptr,  0                };
const IrxModule kPs2Hdd     = { "ps2hdd",      ps2hdd_irx,      &size_ps2hdd_irx,      kHddArgs, sizeof(kHddArgs) };
const IrxModule kPs2Fs      = { "ps2fs",       ps2fs_irx,       &size_ps2fs_irx,       kPfsArgs, sizeof(kPfsArgs) };
const IrxModule kBdm        = { "bdm",         bdm_irx,         &size_bdm_irx,         nullptr,  0                };
const IrxModule kBdmFsFatfs = { "bdmfs_fatfs", bdmfs_fatfs_irx, &size_bdmfs_fatfs_irx, nullptr,  0                };
const IrxModule kUsbd       = { "usbd",        usbd_irx,        &size_usbd_irx,        nullptr,  0                };
const IrxModule kUsbmassBd  = { "usbmass_bd",  usbmass_bd_irx,  &size_usbmass_bd_irx,  nullptr,  0                };
const IrxModule kPs2Kbd     = { "ps2kbd",      ps2kbd_irx,      &size_ps2kbd_irx,      nullptr,  0                };
const IrxModule kLibsd      = { "libsd",       libsd_irx,       &size_libsd_irx,       nullptr,  0                };
const IrxModule kAudsrv     = { "audsrv",      audsrv_irx,      &size_audsrv_irx,      nullptr,  0                };

// What the modules that load after the boot need of the IOP's memory: sound (libsd, audsrv and its
// buffers), the ROM's SIO2MAN, PADMAN, MCMAN and MCSERV, and the keyboard driver. A boot that kept
// the IOP as launched, with less than this in one block, resets it instead, rather than leave a
// later driver nowhere to go. They took 312 KB in PCSX2 (2026-10-12): a USB boot from a reset IOP
// ended with 1220 KB free in one block, and 908 KB were left once all of them were running, with
// the music playing too. This leaves a margin over that, and resetting when it wasn't needed
// only costs the time a reset takes.
constexpr int kLaterModulesIopBytes = 384 * 1024;

const ModuleIdentity * FindIdentity(const char * label)
{
    for (const ModuleIdentity & identity : kModuleIdentities)
    {
        if (std::strcmp(identity.label, label) == 0)
        {
            return &identity;
        }
    }
    return nullptr;
}

const ModuleIdentity & IdentityOf(const char * label)
{
    const ModuleIdentity * identity = FindIdentity(label);
    PS2_AssertMsg(identity != nullptr, "An IOP module missing from kModuleIdentities!");
    return (identity != nullptr) ? *identity : kModuleIdentities[0];
}

int VersionMajor(const u16 version) { return version >> 8; }
int VersionMinor(const u16 version) { return version & 0xFF; }

// ------------------------------------------------------------------------------------------------
// What is running on the IOP
// ------------------------------------------------------------------------------------------------

// Module names are kept to this length; the ones the boot looks for are all shorter.
constexpr int kMaxModuleNameLen = 31;
constexpr int kMaxResident      = 64;

struct ResidentModule
{
    char name[kMaxModuleNameLen + 1]; // "" for a module without an IRX_ID
    u16  version;                     // major << 8 | minor; 0 for one the boot started itself
};

// The IOP's module list as last read, plus each module started since.
static ResidentModule s_resident[kMaxResident];
static int  s_numResident    = 0;
// Per kModuleIdentities entry, whether its export library is registered (see ModuleIdentity).
static bool s_libraryResident[ArrayLength(kModuleIdentities)] = {};

// Copies a module's name out of IOP memory. The buffer is a SIF DMA target: aligned to, and as
// large as, a cache line, so no neighbour's dirty line can be written back over the transfer.
void ReadModuleName(const smod_mod_info_t & info, char (&name)[kMaxModuleNameLen + 1])
{
    constexpr int kNameBufferBytes = 64;
    alignas(64) static char s_nameBuffer[kNameBufferBytes];

    name[0] = '\0';
    if (info.name == nullptr)
    {
        return;
    }

    SyncDCache(s_nameBuffer, s_nameBuffer + kNameBufferBytes);
    SifRpcReceiveData_t rpcData = {};
    if (SifRpcGetOtherData(&rpcData, info.name, s_nameBuffer, kNameBufferBytes, 0) >= 0)
    {
        std::memcpy(name, s_nameBuffer, kMaxModuleNameLen);
        name[kMaxModuleNameLen] = '\0';
    }
}

// Reads the IOP's module list, and looks for the export libraries kModuleIdentities names.
void TakeInventory()
{
    s_numResident = 0;

    smod_mod_info_t info = {};
    if (smod_get_next_mod(nullptr, &info) != 0)
    {
        do
        {
            if (s_numResident == kMaxResident)
            {
                break;
            }
            ResidentModule & module = s_resident[s_numResident++];
            ReadModuleName(info, module.name);
            module.version = info.version;
        } while (smod_get_next_mod(&info, &info) != 0);
    }

    // slib caches where the IOP keeps its library list: read it afresh, the IOP may have been reset.
    const bool haveLibraryList = (slib_exp_lib_list() != nullptr);
    for (int i = 0; i < ArrayLength(kModuleIdentities); ++i)
    {
        const char * const library = kModuleIdentities[i].library;
        s_libraryResident[i] = haveLibraryList && (library != nullptr) && (slib_get_exp_lib(library, nullptr) > 0);
    }
}

int IdentityIndex(const ModuleIdentity & identity)
{
    return static_cast<int>(&identity - kModuleIdentities);
}

const ResidentModule * FindResidentByName(const char * name)
{
    for (int i = 0; i < s_numResident; ++i)
    {
        if (std::strcmp(s_resident[i].name, name) == 0)
        {
            return &s_resident[i];
        }
    }
    return nullptr;
}

// The running module doing a job by its name, or null - also for one known only by its library.
const ResidentModule * FindResident(const ModuleIdentity & identity)
{
    for (const char * name : identity.names)
    {
        if (name != nullptr)
        {
            if (const ResidentModule * module = FindResidentByName(name))
            {
                return module;
            }
        }
    }
    return nullptr;
}

bool IsResident(const ModuleIdentity & identity)
{
    return s_libraryResident[IdentityIndex(identity)] || (FindResident(identity) != nullptr);
}

// Records a module the boot started, so that a later request for the same job finds it running.
void NoteStarted(const ModuleIdentity & identity)
{
    if (identity.library != nullptr)
    {
        s_libraryResident[IdentityIndex(identity)] = true;
    }
    if (identity.names[0] != nullptr && s_numResident < kMaxResident)
    {
        ResidentModule & module = s_resident[s_numResident++];
        std::snprintf(module.name, sizeof(module.name), "%s", identity.names[0]);
        module.version = 0;
    }
}

// "x.y", or a note that the boot started it.
const char * DescribeVersion(const ResidentModule & module, char (&text)[24])
{
    if (module.version == 0)
    {
        std::snprintf(text, sizeof(text), "started by the boot");
    }
    else
    {
        std::snprintf(text, sizeof(text), "%d.%d", VersionMajor(module.version), VersionMinor(module.version));
    }
    return text;
}

// Says which running module a driver that starts after the boot is using instead of its own.
void PrintReused(const char * label, const ModuleIdentity & identity)
{
    if (const ResidentModule * running = FindResident(identity))
    {
        char version[24];
        Com_Printf("IOP module '%s': using the %s already running (%s).\n", label, running->name, DescribeVersion(*running, version));
    }
    else
    {
        Com_Printf("IOP module '%s': using the one already running (its '%s' library is registered).\n", label, identity.library);
    }
}

// ------------------------------------------------------------------------------------------------
// The boot report (see PrintIopBootReport)
// ------------------------------------------------------------------------------------------------

constexpr int kReportBytes = 6144;
static char s_report[kReportBytes];
static int  s_reportUsed = 0;

// The modules a bring-up step reused, started and failed to start, for a line each in the report.
static char s_reusedModules[256];
static char s_startedModules[512];
static char s_failedModules[256];

// One line of the report, "IOP boot: " and the text, printed at once as well.
void ReportLine(const char * format, ...) Q_PRINTF_FUNC(1, 2);
void ReportLine(const char * format, ...)
{
    char line[640];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    std::printf("IOP boot: %s\n", line);

    const int room = kReportBytes - s_reportUsed;
    if (room > 1)
    {
        const int written = std::snprintf(&s_report[s_reportUsed], static_cast<size_t>(room), "IOP boot: %s\n", line);
        if (written > 0)
        {
            s_reportUsed += (written < room) ? written : (room - 1);
        }
    }
}

void ListAppend(char * list, const size_t listBytes, const char * format, ...) Q_PRINTF_FUNC(3, 4);
void ListAppend(char * list, const size_t listBytes, const char * format, ...)
{
    const size_t used = std::strlen(list);
    if (used + 1 < listBytes)
    {
        va_list args;
        va_start(args, format);
        std::vsnprintf(list + used, listBytes - used, format, args);
        va_end(args);
    }
}

// Lists every module running, a few to a line, and the export libraries kModuleIdentities looks
// for that are registered. On a console the report is the only record of what a launcher left.
void ReportResidentModules()
{
    constexpr int kLineChars = 100;
    char line[kLineChars + 48] = {};
    int  used = 0;
    for (int i = 0; i < s_numResident; ++i)
    {
        const ResidentModule & module = s_resident[i];
        char entry[48];
        const int length = std::snprintf(entry, sizeof(entry), "%s %d.%d", (module.name[0] != '\0') ? module.name : "(unnamed)",
                                         VersionMajor(module.version), VersionMinor(module.version));
        if (used > 0 && used + 2 + length > kLineChars)
        {
            ReportLine("running: %s", line);
            used = 0;
        }
        used += std::snprintf(&line[used], sizeof(line) - static_cast<size_t>(used), "%s%s", (used > 0) ? ", " : "", entry);
    }
    if (used > 0)
    {
        ReportLine("running: %s", line);
    }

    char libraries[160] = {};
    for (int i = 0; i < ArrayLength(kModuleIdentities); ++i)
    {
        if (s_libraryResident[i])
        {
            ListAppend(libraries, sizeof(libraries), " %s", kModuleIdentities[i].library);
        }
    }
    ReportLine("libraries registered:%s.", (libraries[0] != '\0') ? libraries : " none of those looked for");
}

void ReportModules()
{
    if (s_reusedModules[0] != '\0')
    {
        ReportLine("reused%s.", s_reusedModules);
        s_reusedModules[0] = '\0';
    }
    if (s_startedModules[0] != '\0')
    {
        ReportLine("started%s.", s_startedModules);
        s_startedModules[0] = '\0';
    }
    if (s_failedModules[0] != '\0')
    {
        ReportLine("didn't start%s.", s_failedModules);
        s_failedModules[0] = '\0';
    }
}

// ------------------------------------------------------------------------------------------------
// Game data location
// ------------------------------------------------------------------------------------------------

// The files probed for under "<base>/baseq2/" to decide a base path works: the compressed pack
// the game loads in preference (fs/pkz_pack.h), or id's.
constexpr const char * kProbeFiles[] = { "pak0.pkz", "pak0.pak" };

// PCSX2 builds have differed on whether "host:/" is ELF-relative or host-absolute,
// so the explicitly relative form is tried too.
constexpr const char * kHostBasePaths[] = { "host:", "host:." };

constexpr const char * kHddDevices[] = { "hdd0:", "hdd1:" };
constexpr const char * kHddMountPath = "pfs0:";
constexpr int kMountReadWrite = 0; // FIO_MT_RDWR in io_common.h (not a C++-safe include).
constexpr int kMaxHddPartitionPathLen = static_cast<int>(sizeof("hdd0:") - 1) + APA_IDMAX;

// bdmfs_fatfs serves the FAT/exFAT volumes it mounts as mass0: to mass9: (FatFs's
// FF_VOLUMES), numbered in mount order. Every partition is a volume of its own, and
// usbmass_bd drives two USB drives at once, so the data need not be on mass0:. A bare
// "mass:" is no extra candidate: iomanX reads a missing unit number as 0, so it is mass0:.
constexpr const char * kUsbBasePaths[] = {
    "mass0:", "mass1:", "mass2:", "mass3:", "mass4:",
    "mass5:", "mass6:", "mass7:", "mass8:", "mass9:",
};

// The engine builds every file path in a MAX_OSPATH buffer as "<base>/baseq2/<name>", where a
// name runs up to MAX_QPATH - 1 chars, so a longer base path would cut some of them short.
// That leaves the ELF's folder whatever a "massN:" prefix doesn't take.
constexpr int kMaxBasePathLen  = MAX_OSPATH - static_cast<int>(sizeof("/baseq2/") - 1) - MAX_QPATH;
constexpr int kMaxElfFolderLen = kMaxBasePathLen - static_cast<int>(sizeof("mass0:") - 1);

// The folder on the device the ELF was launched from, "/dir/sub" or empty for the root.
// See ElfFolderFromPath().
static char s_elfFolder[kMaxElfFolderLen + 1];

// The USB base path DetectBasePathAndBootIop() settled on: a massN: unit plus a folder.
static char s_usbBasePath[kMaxBasePathLen + 1];
static char s_hddBasePath[kMaxBasePathLen + 1];
static const char * s_hddSyncDevice = nullptr; // Only the selected, surviving HDD mount.
static const char * s_hddStatus = "not checked";

// What the HDD probe cost: the PFS partitions it mounted, and the time spent in them.
static int s_hddMounts     = 0;
static int s_hddMountsMsec = 0;

// Set for an ELF launched from USB, whose boot leaves the HDD alone (see UsbLaunchPath).
static bool s_launchedFromUsb = false;

// How long to wait for the USB drive: enumeration + FAT mount happen
// asynchronously after usbmass_bd starts, and a drive that has to spin up
// takes seconds. The probe returns as soon as the data shows up, so only a
// boot that finds none waits all of it.
constexpr int kUsbWaitTotalMsec = 10000;
constexpr int kUsbWaitStepMsec  = 100;

// The same wait through a USB stack the launcher left running. A drive it had mounted is there
// at once; one it hadn't by now isn't going to show up through it, and a boot that finds no
// data falls back to the reset, which gives the game's own drivers the whole wait above.
constexpr int kUsbWaitRunningMsec = 2000;

// fileXio serves a read in pieces the size of its IOP buffer, each one a device read and a DMA
// to the EE, waited out before the next (fileXio_iop.c). Its default of 16 KB makes every PFS
// read reach the drive as 16 KB transfers. 64 KB is the most usbmass_bd asks of a drive in one
// SCSI command (128 sectors). FatFs also ends each read at a cluster's end, so on USB a larger
// buffer only pays on a volume with clusters over 16 KB. Pass -DPS2_QUAKE_FILEXIO_BUFFER_KB=16
// for the stock size, to compare.
#ifndef PS2_QUAKE_FILEXIO_BUFFER_KB
#define PS2_QUAKE_FILEXIO_BUFFER_KB 64
#endif
constexpr int kStockFileXioBufferBytes = 16 * 1024;
constexpr int kFileXioBufferBytes      = PS2_QUAKE_FILEXIO_BUFFER_KB * 1024;
static_assert(kFileXioBufferBytes >= kStockFileXioBufferBytes && kFileXioBufferBytes <= 256 * 1024,
              "PS2_QUAKE_FILEXIO_BUFFER_KB must be 16 to 256");

// See FileXioBufferBytes().
static int s_fileXioBufferBytes = 0;

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

// Probes exactly the path the Quake filesystem will build from the base ("<base>/baseq2/...").
bool HasGameData(const char * basePath)
{
    for (const char * probeFile : kProbeFiles)
    {
        char probePath[MAX_OSPATH];
        std::snprintf(probePath, sizeof(probePath), "%s/baseq2/%s", basePath, probeFile);
        if (CanOpen(probePath))
        {
            return true;
        }
    }
    return false;
}

// Retain the launch partition before the IOP reset discards the loader's PFS mount.
// Both the ELF-loader form (hdd0:+Q2:pfs:/dir/quake2.elf) and the file-browser
// form (hdd0:/+Q2/dir/quake2.elf) identify it. A bare pfsN: path cannot, so the
// HDD search below falls back to enumerating partitions in that case.
bool HddPartitionFromPath(const char * elfPath, char (&partition)[kMaxHddPartitionPathLen + 1])
{
    partition[0] = '\0';
    if (elfPath == nullptr ||
        (std::strncmp(elfPath, "hdd0:", 5) != 0 && std::strncmp(elfPath, "hdd1:", 5) != 0))
    {
        return false;
    }

    const char * start = elfPath + 5;
    while (*start == '/' || *start == '\\') { ++start; }
    const char * end = start;
    while (*end != '\0' && *end != ':' && *end != '/' && *end != '\\') { ++end; }
    const int length = static_cast<int>(end - start);
    if (length == 0 || length > APA_IDMAX || *end == '\0')
    {
        return false;
    }
    if (*end == ':')
    {
        const char * fileDevice = end + 1;
        if (std::strncmp(fileDevice, "pfs:", 4) != 0 &&
            !(std::strncmp(fileDevice, "pfs", 3) == 0 && fileDevice[3] >= '0' &&
              fileDevice[3] <= '9' && fileDevice[4] == ':'))
        {
            return false;
        }
    }

    std::memcpy(partition, elfPath, 5);
    std::memcpy(partition + 5, start, static_cast<size_t>(length));
    partition[5 + length] = '\0';
    return true;
}

// The folder the loader launched the ELF from, so the game data can sit next to it rather than
// only at the root of the drive. argv[0] holds the ELF's path, but each loader spells the device
// its own way ("mass:/APPS/Q2/quake2.elf", "mass0:APPS/Q2/quake2.elf", "hdd0:__common:pfs:/...")
// and the IOP reset renumbers the drives anyway, so only what follows the last colon is kept,
// and every massN: unit gets tried with it. Writes "/APPS/Q2", whatever separators the loader
// used, or an empty string for the root or a null path. Returns false, writing an empty
// string, when the folder is too long for the engine's paths.
bool ElfFolderFromPath(const char * elfPath, char (&folder)[kMaxElfFolderLen + 1])
{
    folder[0] = '\0';
    if (elfPath == nullptr)
    {
        return true;
    }

    const char * path = elfPath;
    for (const char * p = elfPath; *p != '\0'; ++p)
    {
        if (*p == ':')
        {
            path = p + 1;
        }
    }

    // The browser form puts the APA partition in the first path component; it is
    // not part of the directory inside PFS. Canonical HDD paths were handled by
    // the last-colon rule above, as were massN: and bare pfsN: launch paths.
    if ((std::strncmp(elfPath, "hdd0:", 5) == 0 || std::strncmp(elfPath, "hdd1:", 5) == 0) &&
        std::strchr(elfPath + 5, ':') == nullptr)
    {
        path = elfPath + 5;
        while (*path == '/' || *path == '\\') { ++path; }
        while (*path != '\0' && *path != '/' && *path != '\\') { ++path; }
    }

    // The folder ends at the last separator; without one the ELF sits at the root.
    const char * folderEnd = path;
    for (const char * p = path; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            folderEnd = p;
        }
    }

    // Copied one name at a time, which drops leading, doubled and trailing separators.
    int length = 0;
    for (const char * p = path; p < folderEnd;)
    {
        if (*p == '/' || *p == '\\')
        {
            ++p;
            continue;
        }

        const char * nameEnd = p;
        while (nameEnd < folderEnd && *nameEnd != '/' && *nameEnd != '\\')
        {
            ++nameEnd;
        }

        const int nameLength = static_cast<int>(nameEnd - p);
        if (length + 1 + nameLength > kMaxElfFolderLen)
        {
            folder[0] = '\0';
            return false;
        }

        folder[length++] = '/';
        std::memcpy(&folder[length], p, static_cast<size_t>(nameLength));
        length += nameLength;
        p = nameEnd;
    }

    folder[length] = '\0';
    return true;
}

// Whether the loader launched the ELF from USB: "mass:" or "mass0:" to "mass9:". Such a boot looks
// for the game data on USB only. Searching the HDD first cost a console with its HDD attached
// (2026-10-12, wLaunchELF 4.43a) 24.6 s of mounting PFS partitions, plus 3.4 s to start ps2hdd
// when the launcher hadn't, before the USB drive was even looked at.
bool UsbLaunchPath(const char * elfPath)
{
    if (elfPath == nullptr || std::strncmp(elfPath, "mass", 4) != 0)
    {
        return false;
    }
    const char next = elfPath[4];
    return (next == ':') || (next >= '0' && next <= '9' && elfPath[5] == ':');
}

// Tries a folder ("" for the root) on every USB volume. Returns the base path that has the
// game data, or null.
const char * FindUsbGameData(const char * folder)
{
    for (const char * unit : kUsbBasePaths)
    {
        std::snprintf(s_usbBasePath, sizeof(s_usbBasePath), "%s%s", unit, folder);
        if (HasGameData(s_usbBasePath))
        {
            return s_usbBasePath;
        }
    }
    return nullptr;
}

// Whether a FAT volume is mounted behind a massN: unit: only then does its root directory open.
bool IsUsbVolumeMounted(const char * basePath)
{
    char rootPath[32];
    std::snprintf(rootPath, sizeof(rootPath), "%s/", basePath);

    DIR * dir = opendir(rootPath);
    if (dir == nullptr)
    {
        return false;
    }
    closedir(dir);
    return true;
}

// ------------------------------------------------------------------------------------------------
// Starting modules
// ------------------------------------------------------------------------------------------------

// Set once SIF RPC is up and modules can be loaded out of an EE buffer.
// See EnsureModuleLoaderReady().
static bool s_moduleLoaderReady = false;

// Set while no module loaded out of an EE buffer has been seen running yet, when the patch below
// couldn't say the loader is up to it. A LOADFILE without the RPC answers the call with whatever
// its reply buffer held, which reads as a module started.
static bool s_verifyNextLoad = false;

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

// How each driver that starts after the boot came up. See StartOnce().
enum class DriverStart
{
    NotTried,
    Up,
    Failed,
};

static DriverStart s_padDriver      = DriverStart::NotTried;
static DriverStart s_cardDriver     = DriverStart::NotTried;
static DriverStart s_soundDriver    = DriverStart::NotTried;
static DriverStart s_keyboardDriver = DriverStart::NotTried;

// What the last module that wouldn't start answered, for the error that halts the boot.
static int s_lastFailedId     = 0;
static int s_lastFailedResult = 0;

// Lets SifExecModuleBuffer load a module out of EE memory. The boot ROM's LOADFILE (1.x) has no
// RPC for that and sbv patches one in; LOADFILE 2.x, which LoadExecPS2 launches leave running,
// has its own. sbv answers 1 both for that one and for a 1.x patched already (by the launcher,
// or earlier in this boot), and also for a LOADFILE it doesn't recognise. So unless it patched
// one just now, the next module loaded is checked to be running.
bool PatchModuleLoader()
{
    SifInitRpc(0);

    const int lmbPatch = sbv_patch_enable_lmb();
    if (lmbPatch < 0 || sbv_patch_disable_prefix_check() != 0)
    {
        return false;
    }

    s_verifyNextLoad    = (lmbPatch != 0);
    s_moduleLoaderReady = true;
    return true;
}

// Prerequisites for SifExecModuleBuffer, for drivers started after boot: SIF RPC up, and the
// patches above. Console boots have these already; only the host: fast path reaches here
// without them, once, however many drivers call in.
bool EnsureModuleLoaderReady()
{
    if (s_moduleLoaderReady)
    {
        return true;
    }
    if (!PatchModuleLoader())
    {
        Com_Printf("WARNING: the IOP module loader can't be patched - no IOP module can be loaded!\n");
        return false;
    }
    return true;
}

// Loads an IRX out of EE memory and starts it. False when it isn't running afterwards: a negative
// id is a load that failed, and result 1 (NO_RESIDENT_END) a module whose _start bailed out; some
// drivers return a negative init error too. 'identity' may be null for a module the boot doesn't
// track, which goes unrecorded.
bool ExecModuleBuffer(const ModuleIdentity * identity, void * image, const u32 sizeBytes,
                      const u32 argsBytes, const char * args, int & id, int & moduleResult)
{
    moduleResult = 0;
    id = SifExecModuleBuffer(image, sizeBytes, argsBytes, args, &moduleResult);
    if (id < 0 || moduleResult < 0 || moduleResult == 1)
    {
        return false;
    }

    if (s_verifyNextLoad && identity != nullptr)
    {
        TakeInventory();
        if (!IsResident(*identity))
        {
            return false;
        }
        s_verifyNextLoad = false;
    }

    if (identity != nullptr)
    {
        NoteStarted(*identity);
    }
    return true;
}

enum class ModuleStart
{
    Reused,
    Started,
    Failed,
};

// One module of the boot chain: the running one doing its job, or this one, started now.
ModuleStart StartModule(const IrxModule & module)
{
    const ModuleIdentity & identity = IdentityOf(module.label);
    if (IsResident(identity))
    {
        ListAppend(s_reusedModules, sizeof(s_reusedModules), " %s", module.label);
        return ModuleStart::Reused;
    }

    const int startMsec = Sys_Milliseconds();
    int id = 0;
    int moduleResult = 0;
    if (!ExecModuleBuffer(&identity, module.image, *module.sizeBytes, module.argsBytes, module.args, id, moduleResult))
    {
        std::printf("IOP boot: module '%s' didn't start (id %d, result %d).\n", module.label, id, moduleResult);
        ListAppend(s_failedModules, sizeof(s_failedModules), " %s (id %d, result %d)", module.label, id, moduleResult);
        s_lastFailedId     = id;
        s_lastFailedResult = moduleResult;
        return ModuleStart::Failed;
    }

    const int msec = Sys_Milliseconds() - startMsec;
    std::printf("IOP boot: started '%s' (id %d) in %d ms.\n", module.label, id, msec);
    ListAppend(s_startedModules, sizeof(s_startedModules), " %s %d ms", module.label, msec);
    return ModuleStart::Started;
}

// A module the boot can't go on without. After a reset there is nothing left to fall back on,
// and it halts the boot, as boots always did; before one it says what failed, for the reset.
ModuleStart StartRequired(const IrxModule & module, const bool afterReset, const char ** failure)
{
    const ModuleStart start = StartModule(module);
    if (start != ModuleStart::Failed)
    {
        return start;
    }

    if (afterReset)
    {
        Sys_Error("IOP boot: module '%s' failed (id %d, result %d)", module.label, s_lastFailedId, s_lastFailedResult);
    }

    static char s_failure[64];
    std::snprintf(s_failure, sizeof(s_failure), "'%s' wouldn't start", module.label);
    *failure = s_failure;
    return ModuleStart::Failed;
}

// Why the game's drivers can't run alongside what is running, or null when they can.
const char * FindResidentConflict()
{
    static char s_reason[160];

    // A module too old to talk its EE library's protocol can't be swapped for the game's own
    // either, which would find its RPC server or name taken.
    for (const ModuleIdentity & identity : kModuleIdentities)
    {
        const ResidentModule * module = FindResident(identity);
        if (module != nullptr && identity.minVersion != 0 && module->version < identity.minVersion)
        {
            std::snprintf(s_reason, sizeof(s_reason), "a resident %s %d.%d, older than the %d.%d the game talks to",
                          module->name, VersionMajor(module->version), VersionMinor(module->version),
                          VersionMajor(identity.minVersion), VersionMinor(identity.minVersion));
            return s_reason;
        }
    }

    // The ROM's PADMAN and MCMAN are built against the ROM's SIO2MAN, version 1, so next to an
    // X-module or ps2sdk SIO2MAN they wouldn't link. That one can stay only with every pad and
    // card driver the game would otherwise load on top of it.
    const ResidentModule * sio2man = FindResident(IdentityOf("rom0:SIO2MAN"));
    if (sio2man != nullptr && VersionMajor(sio2man->version) != 1 &&
        (!IsResident(IdentityOf("rom0:PADMAN")) || !IsResident(IdentityOf("rom0:MCMAN")) ||
         !IsResident(IdentityOf("rom0:MCSERV"))))
    {
        std::snprintf(s_reason, sizeof(s_reason), "a resident sio2man %d.%d without all of its pad and card drivers",
                      VersionMajor(sio2man->version), VersionMinor(sio2man->version));
        return s_reason;
    }
    return nullptr;
}

// See LargestFreeIopBlockBytes().
int LargestFreeIopBlock()
{
    if (SifInitIopHeap() < 0)
    {
        return 0;
    }

    constexpr int kIopRamBytes    = 2 * 1024 * 1024;
    constexpr int kPrecisionBytes = 4 * 1024;

    int fits  = 0;
    int fails = kIopRamBytes;
    while (fails - fits > kPrecisionBytes)
    {
        const int size = ((fits + fails) / 2) & ~255;
        void * block = SifAllocIopHeap(size);
        if (block != nullptr)
        {
            SifFreeIopHeap(block);
            fits = size;
        }
        else
        {
            fails = size;
        }
    }
    return fits;
}

// The boot of old: a fresh IOP with the boot ROM's modules only. Nothing started on the one
// before - by the launcher, or by this boot - survives it.
void ResetIop(const char * reason)
{
    ReportLine("resetting the IOP: %s.", reason);
    const int startMsec = Sys_Milliseconds();

    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);

    if (!PatchModuleLoader())
    {
        Sys_Error("IOP boot: module-loader patches failed");
        return; // unreachable; Sys_Error halts
    }

    // Only the pad driver can have started by now (when the launcher left its PADMAN), but none
    // of them is running any more.
    TakeInventory();
    s_numRomModules  = 0;
    s_padDriver      = DriverStart::NotTried;
    s_cardDriver     = DriverStart::NotTried;
    s_soundDriver    = DriverStart::NotTried;
    s_keyboardDriver = DriverStart::NotTried;
    s_hddSyncDevice  = nullptr;

    ReportLine("the reset took %d ms, leaving %d modules.", Sys_Milliseconds() - startMsec, s_numResident);
}

// ------------------------------------------------------------------------------------------------
// The drivers that start after the boot
// ------------------------------------------------------------------------------------------------

// Loads a module from the console's ROM the first time any driver asks for it, and returns that
// first result to every later caller: the pad and memory card drivers share the ROM's SIO2MAN,
// and a second copy of it must not be loaded. A module doing the same job that is already
// running counts as loaded: a launcher may leave its own serial, pad or memory card drivers,
// which libpad and libmc can talk to as well.
bool LoadRomModuleOnce(const char * path)
{
    for (int i = 0; i < s_numRomModules; ++i)
    {
        if (std::strcmp(s_romModules[i].path, path) == 0)
        {
            return s_romModules[i].loaded;
        }
    }

    bool loaded = false;
    const ModuleIdentity & identity = IdentityOf(path);
    if (IsResident(identity))
    {
        PrintReused(path, identity);
        loaded = true;
    }
    else
    {
        SifInitRpc(0);
        int moduleResult = 0;
        const int id = SifLoadStartModule(path, 0, nullptr, &moduleResult);
        loaded = (id >= 0 && moduleResult != 1);

        if (loaded)
        {
            Com_Printf("IOP module '%s' started (id %d).\n", path, id);
            NoteStarted(identity);
        }
        else
        {
            Com_Printf("WARNING: IOP module '%s' failed to load (id %d, result %d).\n", path, id, moduleResult);
        }
    }

    if (s_numRomModules < kMaxRomModules)
    {
        s_romModules[s_numRomModules++] = { path, loaded };
    }
    return loaded;
}

// Starts an IRX the ELF carries, unless a module doing its job is already running - started by
// the boot, or left by the launcher - which is then used instead: a second copy would fail to
// start next to it.
bool StartLaterModule(const IrxModule & module)
{
    const ModuleIdentity & identity = IdentityOf(module.label);
    if (IsResident(identity))
    {
        PrintReused(module.label, identity);
        return true;
    }

    if (!EnsureModuleLoaderReady())
    {
        return false;
    }

    int id = 0;
    int moduleResult = 0;
    if (!ExecModuleBuffer(&identity, module.image, *module.sizeBytes, module.argsBytes, module.args, id, moduleResult))
    {
        Com_Printf("WARNING: IOP module '%s' failed (id %d, result %d).\n", module.label, id, moduleResult);
        return false;
    }

    Com_Printf("IOP module '%s' started (id %d).\n", module.label, id);
    return true;
}

// Brings a driver up the first time it is asked for, and gives every later caller that first
// result: a module that wouldn't start won't the second time either.
bool StartOnce(DriverStart & state, bool (*bringUp)())
{
    if (state == DriverStart::NotTried)
    {
        state = bringUp() ? DriverStart::Up : DriverStart::Failed;
    }
    return (state == DriverStart::Up);
}

bool BringUpPadDriver()
{
    // Asked before the loads below, which would make it look resident either way.
    const bool launcherPadman = IsResident(IdentityOf("rom0:PADMAN"));

    // SIO2MAN is shared with the memory card driver, which loads the ROM's MCMAN/MCSERV - drivers
    // of the same generation as the ROM's PADMAN - on top of it.
    SifInitRpc(0);
    if (!LoadRomModuleOnce("rom0:SIO2MAN") || !LoadRomModuleOnce("rom0:PADMAN"))
    {
        Com_Printf("WARNING: failed to load the pad IOP modules!\n");
        return false;
    }

    // libpad talks to the ROM's PADMAN or to an X-module or ps2sdk one, whichever it finds.
    if (padInit(0) != 1)
    {
        Com_Printf("WARNING: padInit failed!\n");
        return false;
    }

    // padman closes a port that isn't open without complaint (freepad's padPortClose).
    if (launcherPadman)
    {
        padPortClose(0, 0);
        padPortClose(1, 0);
    }
    return true;
}

// The ROM's: the SDK's own mcman.irx would need the SDK's sio2man, not the ROM's that the pad
// driver runs on. Every model's ROM has all three.
bool BringUpCardDriver()
{
    return LoadRomModuleOnce("rom0:SIO2MAN") &&
           LoadRomModuleOnce("rom0:MCMAN")   &&
           LoadRomModuleOnce("rom0:MCSERV");
}

// libsd is ps2sdk's open-source reimplementation of Sony's sound driver (freesd.irx is a symlink
// to the same binary). audsrv imports its sceSd* entry points, so it has to be running first.
bool BringUpSoundDriver()
{
    return StartLaterModule(kLibsd) && StartLaterModule(kAudsrv);
}

// ps2kbd speaks USB, so it needs usbd under it. A USB boot has usbd running already, and so may
// the launcher; it starts here otherwise.
bool BringUpKeyboardDriver()
{
    return StartLaterModule(kUsbd) && StartLaterModule(kPs2Kbd);
}

// ------------------------------------------------------------------------------------------------
// Storage bring-up
// ------------------------------------------------------------------------------------------------

const char * ProbeHddPartition(const char * partition, const char * folder)
{
    const int startMsec = Sys_Milliseconds();
    if (fileXioMount(kHddMountPath, partition, kMountReadWrite) < 0)
    {
        s_hddMountsMsec += Sys_Milliseconds() - startMsec;
        return nullptr;
    }
    ++s_hddMounts;

    std::snprintf(s_hddBasePath, sizeof(s_hddBasePath), "%s%s", kHddMountPath, folder);
    const bool found = HasGameData(s_hddBasePath);
    if (!found)
    {
        fileXioUmount(kHddMountPath);
    }
    s_hddMountsMsec += Sys_Milliseconds() - startMsec;
    if (!found)
    {
        return nullptr;
    }

    // Keep this mount alive: all subsequent engine file I/O uses pfs0:.
    s_hddSyncDevice = kHddMountPath;
    std::printf("IOP boot: game data on %s/baseq2 (HDD partition %s).\n", s_hddBasePath, partition);
    return s_hddBasePath;
}

const char * FindHddGameData(const char * preferredPartition, const char * folder)
{
    // No DEV9/HDD is normal on a Slim or a USB-only setup. A failed optional
    // module must not stop the remaining USB path, and no formatting is done here.
    s_hddStatus = "drivers unavailable";
    const IrxModule * const hddDrivers[] = { &kPs2Dev9, &kPs2Atad, &kPs2Hdd, &kPs2Fs };
    for (const IrxModule * module : hddDrivers)
    {
        const ModuleStart start = StartModule(*module);
        if (start == ModuleStart::Failed)
        {
            return nullptr;
        }

        // A PFS driver the launcher left may still have a partition mounted where this mounts one.
        if (module == &kPs2Fs && start == ModuleStart::Reused)
        {
            fileXioUmount(kHddMountPath);
        }
    }

    s_hddStatus = "no matching PFS game data";
    if (preferredPartition[0] != '\0')
    {
        if (const char * basePath = ProbeHddPartition(preferredPartition, folder))
        {
            return basePath;
        }
        if (folder[0] != '\0')
        {
            if (const char * basePath = ProbeHddPartition(preferredPartition, ""))
            {
                return basePath;
            }
        }
    }

    // Prefer the launch partition, then all other main PFS partitions. This also
    // handles launchers that only pass pfsN:, and makes HDD beat USB when both
    // hold a matching copy, for an ELF not launched from USB (which never gets
    // here). Search the folder on every partition before roots, as on USB. APA
    // subpartitions belong to their main partition.
    const char * folders[] = { folder, "" };
    const int numFolders = (folder[0] != '\0') ? 2 : 1;
    for (int folderIndex = 0; folderIndex < numFolders; ++folderIndex)
    {
        for (const char * device : kHddDevices)
        {
            const int status = fileXioDevctl(device, HDIOC_STATUS, nullptr, 0, nullptr, 0);
            if (status != 0)
            {
                if (folderIndex == 0)
                {
                    std::printf("IOP boot: %s unavailable (status %d).\n", device, status);
                }
                continue;
            }
            const int fd = fileXioDopen(device);
            if (fd < 0)
            {
                continue;
            }

            alignas(64) static iox_dirent_t entry;
            while (fileXioDread(fd, &entry) > 0)
            {
                if (entry.stat.mode != APA_TYPE_PFS || (entry.stat.attr & APA_FLAG_SUB) != 0)
                {
                    continue;
                }
                char partition[kMaxHddPartitionPathLen + 1];
                std::snprintf(partition, sizeof(partition), "%s%.*s", device, APA_IDMAX, entry.name);
                if (std::strcmp(partition, preferredPartition) == 0)
                {
                    continue;
                }
                if (const char * basePath = ProbeHddPartition(partition, folders[folderIndex]))
                {
                    fileXioDclose(fd);
                    return basePath;
                }
            }
            fileXioDclose(fd);
        }
    }
    return nullptr;
}

// Gives fileXio its larger read buffer, once the drive's own modules have taken their IOP memory.
void SetFileXioBuffer()
{
    s_fileXioBufferBytes = kStockFileXioBufferBytes;
    if (kFileXioBufferBytes == kStockFileXioBufferBytes)
    {
        return;
    }

    const int result = fileXioSetRWBufferSize(kFileXioBufferBytes);
    if (result == 0)
    {
        s_fileXioBufferBytes = kFileXioBufferBytes;
        std::printf("IOP boot: fileXio read buffer %d KB.\n", kFileXioBufferBytes / 1024);
        return;
    }

    // fileXio frees its old buffer before allocating the new one, and keeps a null one when that
    // fails, which every read would then DMA from: put the stock size back.
    std::printf("IOP boot: no IOP memory for a %d KB fileXio read buffer (%d); keeping %d KB.\n",
                kFileXioBufferBytes / 1024, result, kStockFileXioBufferBytes / 1024);
    if (fileXioSetRWBufferSize(kStockFileXioBufferBytes) != 0)
    {
        Sys_Error("IOP boot: fileXio has no read buffer left");
    }
}

// Crude millisecond wait; fine for boot-time polling.
void BusyWaitMsec(int msec)
{
    const int until = Sys_Milliseconds() + msec;
    while (Sys_Milliseconds() < until) {}
}

// Starts iomanX, fileXio and the drives' drivers on the IOP as it is, and finds the game data
// through them: the HDD first, then USB. Null when there is none, or when a module the boot
// can't do without won't start, with the reason in *failure. 'afterReset' is the last attempt:
// a module that won't start then halts the boot instead.
const char * BringUpStorage(const char * preferredPartition, const bool afterReset, const char ** failure)
{
    s_hddSyncDevice = nullptr;

    if (StartRequired(kIomanX, afterReset, failure) == ModuleStart::Failed ||
        StartRequired(kFileXio, afterReset, failure) == ModuleStart::Failed)
    {
        ReportModules();
        return nullptr;
    }

    // Both PFS and FatFs register with iomanX, reached through fileXio, rather
    // than the ROM FILEIO backend. Initialize before probing either device.
    if (fileXioInit() < 0)
    {
        if (afterReset)
        {
            Sys_Error("IOP boot: fileXio initialization failed");
        }
        ReportModules();
        *failure = "fileXio wouldn't initialise";
        return nullptr;
    }

    const char * hddBasePath = nullptr;
    if (s_launchedFromUsb)
    {
        s_hddStatus = "not searched (launched from USB)";
        ReportModules();
        ReportLine("HDD: not searched, the ELF was launched from USB.");
    }
    else
    {
        const int hddStartMsec = Sys_Milliseconds();
        s_hddMounts     = 0;
        s_hddMountsMsec = 0;
        hddBasePath = FindHddGameData(preferredPartition, s_elfFolder);
        ReportModules();
        ReportLine("HDD: %s, after %d ms; %d PFS partition mounts took %d ms of it.",
                   (hddBasePath != nullptr) ? "game data found" : s_hddStatus, Sys_Milliseconds() - hddStartMsec,
                   s_hddMounts, s_hddMountsMsec);
    }
    if (hddBasePath != nullptr)
    {
        SetFileXioBuffer();
        return hddBasePath;
    }

    // A usbhdfsd the launcher left running serves mass: on its own. It reads FAT32 only: a drive
    // it can't read leaves the boot to fall back on the reset, and BDM.
    const bool usbhdfsd = (FindResidentByName(kUsbhdfsdName) != nullptr);
    const IrxModule * const usbDrivers[] = { &kBdm, &kBdmFsFatfs, &kUsbd, &kUsbmassBd };
    bool usbAlreadyRunning = true;
    for (const IrxModule * module : usbDrivers)
    {
        if (usbhdfsd && module != &kUsbd)
        {
            continue;
        }

        const ModuleStart start = StartRequired(*module, afterReset, failure);
        if (start == ModuleStart::Failed)
        {
            ReportModules();
            return nullptr;
        }
        usbAlreadyRunning = usbAlreadyRunning && (start == ModuleStart::Reused);
    }
    ReportModules();

    const int waitMsec = usbAlreadyRunning ? kUsbWaitRunningMsec : kUsbWaitTotalMsec;
    for (int waited = 0; waited <= waitMsec; waited += kUsbWaitStepMsec)
    {
        // Next to the ELF first: that's the copy the player launched, even if the root holds another.
        const char * basePath = (s_elfFolder[0] != '\0') ? FindUsbGameData(s_elfFolder) : nullptr;
        if (basePath == nullptr)
        {
            basePath = FindUsbGameData("");
        }

        if (basePath != nullptr)
        {
            ReportLine("USB: game data after %d ms of waiting for the drive%s.", waited,
                       usbhdfsd ? ", through the launcher's usbhdfsd" : "");
            SetFileXioBuffer();
            return basePath;
        }
        BusyWaitMsec(kUsbWaitStepMsec);
    }

    ReportLine("USB: no game data after %d ms.", waitMsec);
    *failure = "no game data through the drivers the launcher left";
    return nullptr;
}

// Ends the report with where the data is, when, and the IOP memory left.
void ReportGameData(const char * basePath, const int freeIopBytes)
{
    ReportLine("game data on %s at %d ms; %d KB of IOP memory free in one block.", basePath,
               Sys_Milliseconds(), freeIopBytes / 1024);
}

} // namespace

const char * DetectBasePathAndBootIop(const char * elfPath)
{
    s_hddSyncDevice = nullptr;

    SifInitRpc(0);
    TakeInventory();
    const int numAtStart = s_numResident;

    // host: fast path (PCSX2). Skips the IOP reset entirely.
    for (const char * basePath : kHostBasePaths)
    {
        if (HasGameData(basePath))
        {
            std::printf("IOP boot: game data on %s/baseq2 (emulator host filesystem).\n", basePath);

            // host: is served by the ROM's FILEIO module here, whose remove() is missing a
            // break and runs a mkdir() of the same path after it. The save code deletes files.
            s_fileIoRemovePatched = (sbv_patch_fileio() == 0);
            std::printf("IOP boot: FILEIO remove() patch %s.\n", s_fileIoRemovePatched ? "applied" : "not applicable");
            ReportLine("game data on %s (the emulator's host filesystem) at %d ms, %d modules running.",
                       basePath, Sys_Milliseconds(), numAtStart);
            return basePath;
        }
    }

    char preferredPartition[kMaxHddPartitionPathLen + 1];
    HddPartitionFromPath(elfPath, preferredPartition);
    s_launchedFromUsb = UsbLaunchPath(elfPath);

    // Save the launch directory/partition before a reset discards all loader mounts.
    char searched[MAX_OSPATH];
    if (!ElfFolderFromPath(elfPath, s_elfFolder))
    {
        std::snprintf(searched, sizeof(searched), "/baseq2 only (ELF folder over %d chars)", kMaxElfFolderLen);
    }
    else if (s_elfFolder[0] != '\0')
    {
        std::snprintf(searched, sizeof(searched), "%s/baseq2 and /baseq2", s_elfFolder);
    }
    else
    {
        std::snprintf(searched, sizeof(searched), "/baseq2");
    }

    const char * const launchedAs = (elfPath != nullptr && elfPath[0] != '\0') ? elfPath : "(no path from the loader)";
    std::printf("IOP boot: no host: game data; looking %s...\n", s_launchedFromUsb ? "on USB, the launch device" : "for HDD, then USB");
    ReportLine("launched as '%s'; looking for %s.", launchedAs, searched);

    const ResidentModule * loadfile = FindResidentByName(kLoadfileName);
    ReportLine("%d modules running at %d ms, LOADFILE %d.%d.", numAtStart, Sys_Milliseconds(),
               (loadfile != nullptr) ? VersionMajor(loadfile->version) : 0,
               (loadfile != nullptr) ? VersionMinor(loadfile->version) : 0);
    ReportResidentModules();

    const char * resetReason = PS2_QUAKE_ALWAYS_RESET_IOP ? "PS2_QUAKE_ALWAYS_RESET_IOP is set" : FindResidentConflict();
    if (resetReason == nullptr && !PatchModuleLoader())
    {
        resetReason = "its module loader couldn't be patched";
    }

    if (resetReason == nullptr)
    {
        ReportLine("keeping the IOP as launched.");

        // A pad driver the launcher left may still be feeding a port it opened into EE memory
        // that is the game's now.
        if (IsResident(IdentityOf("rom0:PADMAN")))
        {
            StartPadDriver();
        }

        const char * failure = nullptr;
        if (const char * basePath = BringUpStorage(preferredPartition, /*afterReset=*/false, &failure))
        {
            const int freeIopBytes = LargestFreeIopBlock();
            if (freeIopBytes >= kLaterModulesIopBytes)
            {
                ReportGameData(basePath, freeIopBytes);
                return basePath;
            }

            static char s_memoryFailure[96];
            std::snprintf(s_memoryFailure, sizeof(s_memoryFailure), "only %d KB of IOP memory left for the drivers that load later",
                          freeIopBytes / 1024);
            failure = s_memoryFailure;
        }
        resetReason = (failure != nullptr) ? failure : "the storage bring-up failed";
    }

    ResetIop(resetReason);

    const char * failure = nullptr;
    if (const char * basePath = BringUpStorage(preferredPartition, /*afterReset=*/true, &failure))
    {
        ReportGameData(basePath, LargestFreeIopBlock());
        return basePath;
    }

    // A console has no log to read, so the error screen says how far the drive got:
    // no volume means no FAT/exFAT drive came up in time, and a volume means the
    // files are not where they were looked for. Each entry takes at most " massN:"
    // of the buffer.
    char mountedVolumes[ArrayLength(kUsbBasePaths) * sizeof(" massN:")] = {};
    for (const char * basePath : kUsbBasePaths)
    {
        if (IsUsbVolumeMounted(basePath))
        {
            std::strcat(mountedVolumes, " ");
            std::strcat(mountedVolumes, basePath);
        }
    }

    // Lines kept within the debug screen's 64 columns.
    Sys_Error("No game data found!\n"
              "Emulator: enable the host filesystem; baseq2/ goes by the ELF.\n"
              "HDD: baseq2/ goes by the ELF on an APA/PFS partition.\n"
              "USB: put baseq2/ (%s or %s etc) next to the ELF,\n"
              "or at the root of a FAT32/exFAT drive.\n"
              "Launched as: %s\n"
              "Looked for: %s\n"
              "HDD: %s\n"
              "USB volumes mounted:%s",
              kProbeFiles[1], kProbeFiles[0], launchedAs, searched, s_hddStatus, (mountedVolumes[0] != '\0') ? mountedVolumes : " none");
    return nullptr; // unreachable; Sys_Error halts
}

void PrintIopBootReport(void (*printer)(const char *, ...))
{
    // A line at a time, as each line is its own message to whoever prints them.
    for (const char * line = s_report; *line != '\0';)
    {
        const char * end = std::strchr(line, '\n');
        const int length = (end != nullptr) ? static_cast<int>(end - line) : static_cast<int>(std::strlen(line));
        printer("%.*s\n", length, line);
        line += length + ((end != nullptr) ? 1 : 0);
    }
}

int SyncGameDataDevice()
{
    // PFS caches inode/directory metadata after close with our read/write mount.
    // Host and USB need no sync RPC, including after an unsuccessful HDD probe.
    return (s_hddSyncDevice != nullptr) ? fileXioSync(s_hddSyncDevice, 0) : 0;
}

bool GameDataOnHdd()
{
    return s_hddSyncDevice != nullptr;
}

int FileXioBufferBytes()
{
    return s_fileXioBufferBytes;
}

bool StartPadDriver()
{
    return StartOnce(s_padDriver, BringUpPadDriver);
}

bool StartMemoryCardDriver()
{
    return StartOnce(s_cardDriver, BringUpCardDriver);
}

bool StartSoundDriver()
{
    return StartOnce(s_soundDriver, BringUpSoundDriver);
}

bool StartKeyboardDriver()
{
    return StartOnce(s_keyboardDriver, BringUpKeyboardDriver);
}

bool FileIoRemovePatched()
{
    return s_fileIoRemovePatched;
}

int LargestFreeIopBlockBytes()
{
    return LargestFreeIopBlock();
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
