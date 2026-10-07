/* ================================================================================================
 * File: iop_boot.cpp
 * Brief: Boot-time IOP bring-up and game-data location. See iop_boot.h.
 *
 *  After the host: probe, both console paths share an IOP reset and iomanX + fileXio.
 *  HDD uses DEV9 + ATAD + APA + PFS; USB uses BDM + FatFs + usbd + usbmass_bd.
 *  fileXioInit() routes newlib fopen/fread - and the whole Quake filesystem - through
 *  iomanX, reaching the mounted pfs0: partition or massN: volumes transparently.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"

#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <kernel.h>
#include <smod.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <loadfile.h>
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

// PCSX2 builds have differed on whether "host:/" is ELF-relative or host-absolute,
// so the explicitly relative form is tried too.
constexpr const char * kHostBasePaths[] = { "host:", "host:." };

constexpr const char * kHddDevices[] = { "hdd0:", "hdd1:" };
constexpr const char * kHddMountPath = "pfs0:";
constexpr int kMountReadWrite = 0; // FIO_MT_RDWR in io_common.h (not a C++-safe include).
constexpr int kMaxHddPartitionPathLen = static_cast<int>(sizeof("hdd0:") - 1) + APA_IDMAX;

// APA holds an open handle for each PFS mount, plus one while listing partitions.
// PFS defaults to only two open files; pak handles, a music stream, temporary
// asset reads and the log can coexist. Its cache needs at least 2 * open files + 8.
constexpr char kHddArgs[] = "-o\0" "4\0" "-n\0" "20";
constexpr char kPfsArgs[] = "-m\0" "1\0" "-o\0" "16\0" "-n\0" "40";

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

// How long to wait for the USB drive: enumeration + FAT mount happen
// asynchronously after usbmass_bd starts, and a drive that has to spin up
// takes seconds. The probe returns as soon as the data shows up, so only a
// boot that finds none waits all of it.
constexpr int kUsbWaitTotalMsec = 10000;
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

// Probes exactly the path the Quake filesystem will build from the base ("<base>/baseq2/...").
bool HasGameData(const char * basePath)
{
    char probePath[MAX_OSPATH];
    std::snprintf(probePath, sizeof(probePath), "%s/baseq2/%s", basePath, kProbeFile);
    return CanOpen(probePath);
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

bool ExecIopModule(const char * name, void * image, u32 sizeBytes, bool required = true,
                   u32 argsLen = 0, const char * args = nullptr)
{
    int moduleResult = 0;
    const int id = SifExecModuleBuffer(image, sizeBytes, argsLen, args, &moduleResult);

    // Negative id = the load itself failed; result 1 = the module's _start
    // bailed out (NO_RESIDENT_END). Some drivers return a negative init error too.
    if (id < 0 || moduleResult < 0 || moduleResult == 1) [[unlikely]]
    {
        if (required)
        {
            Sys_Error("IOP boot: module '%s' failed (id %d, result %d)", name, id, moduleResult);
        }
        std::printf("IOP boot: optional module '%s' unavailable (id %d, result %d).\n", name, id, moduleResult);
        return false;
    }
    std::printf("IOP boot: started '%s' (id %d)\n", name, id);
    return true;
}

const char * ProbeHddPartition(const char * partition, const char * folder)
{
    if (fileXioMount(kHddMountPath, partition, kMountReadWrite) < 0)
    {
        return nullptr;
    }

    std::snprintf(s_hddBasePath, sizeof(s_hddBasePath), "%s%s", kHddMountPath, folder);
    if (!HasGameData(s_hddBasePath))
    {
        fileXioUmount(kHddMountPath);
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
    if (!ExecIopModule("ps2dev9", ps2dev9_irx, size_ps2dev9_irx, false) ||
        !ExecIopModule("ps2atad", ps2atad_irx, size_ps2atad_irx, false) ||
        !ExecIopModule("ps2hdd", ps2hdd_irx, size_ps2hdd_irx, false,
                       static_cast<u32>(sizeof(kHddArgs)), kHddArgs) ||
        !ExecIopModule("ps2fs", ps2fs_irx, size_ps2fs_irx, false,
                       static_cast<u32>(sizeof(kPfsArgs)), kPfsArgs))
    {
        return nullptr;
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
    // hold a matching copy. Search the folder on every partition before roots,
    // as on USB. APA subpartitions belong to their main partition.
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

    // Console boot marks this ready after resetting and patching the IOP. Only
    // the host: fast path reaches here without that shared bring-up.
    if (sbv_patch_enable_lmb() != 0)
    {
        Com_Printf("WARNING: sbv_patch_enable_lmb failed - no IOP module can be loaded!\n");
        return false;
    }

    s_moduleLoaderReady = true;
    return true;
}

} // namespace

const char * DetectBasePathAndBootIop(const char * elfPath)
{
    s_hddSyncDevice = nullptr;

    // host: fast path (PCSX2). Skips the IOP reset entirely.
    for (const char * basePath : kHostBasePaths)
    {
        if (HasGameData(basePath))
        {
            std::printf("IOP boot: game data on %s/baseq2 (emulator host filesystem).\n", basePath);

            // host: is served by the ROM's FILEIO module here, whose remove() is missing a
            // break and runs a mkdir() of the same path after it. The save code deletes files.
            SifInitRpc(0);
            s_fileIoRemovePatched = (sbv_patch_fileio() == 0);
            std::printf("IOP boot: FILEIO remove() patch %s.\n", s_fileIoRemovePatched ? "applied" : "not applicable");
            return basePath;
        }
    }

    char preferredPartition[kMaxHddPartitionPathLen + 1];
    HddPartitionFromPath(elfPath, preferredPartition);

    // Save the launch directory/partition before the reset discards all loader mounts.
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
    std::printf("IOP boot: no host: game data; looking for HDD, then USB...\n");
    std::printf("IOP boot: launched as '%s'; looking for %s.\n", launchedAs, searched);

    // Reboot the IOP into a clean state and patch in support for loading
    // EE-embedded modules. The pad driver's rom0: modules load later (IN_Init),
    // safely after this reset.
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);

    if (sbv_patch_enable_lmb() != 0 || sbv_patch_disable_prefix_check() != 0)
    {
        Sys_Error("IOP boot: module-loader patches failed");
        return nullptr; // unreachable; Sys_Error halts
    }

    s_moduleLoaderReady = true;

    ExecIopModule("iomanX",  iomanX_irx,  size_iomanX_irx);
    ExecIopModule("fileXio", fileXio_irx, size_fileXio_irx);

    // Both PFS and FatFs register with iomanX, reached through fileXio, rather
    // than the ROM FILEIO backend. Initialize before probing either device.
    if (fileXioInit() < 0)
    {
        Sys_Error("IOP boot: fileXio initialization failed");
        return nullptr; // unreachable; Sys_Error halts
    }

    if (const char * basePath = FindHddGameData(preferredPartition, s_elfFolder))
    {
        return basePath;
    }

    std::printf("IOP boot: HDD: %s; bringing up USB mass storage...\n", s_hddStatus);
    ExecIopModule("bdm",         bdm_irx,         size_bdm_irx);
    ExecIopModule("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);
    ExecIopModule("usbd",        usbd_irx,        size_usbd_irx);
    ExecIopModule("usbmass_bd",  usbmass_bd_irx,  size_usbmass_bd_irx);

    s_usbStackStarted = true;

    for (int waited = 0; waited <= kUsbWaitTotalMsec; waited += kUsbWaitStepMsec)
    {
        // Next to the ELF first: that's the copy the player launched, even if the root holds another.
        const char * basePath = (s_elfFolder[0] != '\0') ? FindUsbGameData(s_elfFolder) : nullptr;
        if (basePath == nullptr)
        {
            basePath = FindUsbGameData("");
        }

        if (basePath != nullptr)
        {
            std::printf("IOP boot: game data on %s/baseq2 (USB, ready after ~%d ms).\n", basePath, waited);
            return basePath;
        }
        BusyWaitMsec(kUsbWaitStepMsec);
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
              "USB: put baseq2/ (%s etc) next to the ELF, or at the\n"
              "root of a FAT32/exFAT drive.\n"
              "Launched as: %s\n"
              "Looked for: %s\n"
              "HDD: %s\n"
              "USB volumes mounted:%s",
              kProbeFile, launchedAs, searched, s_hddStatus, (mountedVolumes[0] != '\0') ? mountedVolumes : " none");
    return nullptr; // unreachable; Sys_Error halts
}

int SyncGameDataDevice()
{
    // PFS caches inode/directory metadata after close with our read/write mount.
    // Host and USB need no sync RPC, including after an unsuccessful HDD probe.
    return (s_hddSyncDevice != nullptr) ? fileXioSync(s_hddSyncDevice, 0) : 0;
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
