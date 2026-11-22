/* ================================================================================================
 * File: cdvd_device.cpp
 * Brief: The disc build's read-only ISO9660 device over libcdvd. See cdvd_device.h.
 *
 *  libcglue sends every path newlib opens, stats or removes through one table of path operations
 *  (_libcglue_fdman_path_ops), which fileXioInit() swaps for its own on a USB or HDD boot. Here
 *  the table is copied and the copy put in its place, with the cdrom0: paths answered by this
 *  file and every other path handed to the one before, ROM FILEIO's: under PCSX2 that is still
 *  where host: goes, for the debug files a disc can't take.
 *
 *  Two things about libcdvd shape the reads:
 *
 *  - sceCdRead only starts a read. sceCdSync(0), the way to wait for it, spins on the RPC's
 *    status, and the EE kernel has no time slicing: a thread spinning there keeps every thread
 *    below it off the CPU for the whole read, the background loader's included. So the device
 *    polls sceCdSync(1) and sleeps a millisecond in DelayThread between polls.
 *  - sceCdRead doesn't wait for a command already in flight: it returns 0, as it does while the
 *    drive isn't ready. One semaphore serialises the device (the main thread and the background
 *    loader both read), and a 0 is waited out and tried again.
 *
 *  Nothing here prints after the mount: the background loader calls into it, and it must never
 *  print or touch the engine (async_io.h). A read that fails after its retries returns EIO, which
 *  the caller reports as any failed read; the count of retries goes to the boot report and the
 *  I/O log.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/fs/cdvd_device.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>

#include <delaythread.h>
#include <kernel.h>
#include <libcdvd.h>
#include <ps2sdkapi.h>

namespace ps2::cdvd {
namespace {

using ps2::heap::MemTag;

// What make_iso.py looks for in an ELF before it puts it on a disc: a build without this device
// would look for its data on host:, the HDD and USB instead. Printed in the boot report, so it is
// in the ELF whole.
constexpr char kDiscBuildMarker[] = "Quake II PS2 disc build";

constexpr u32 kSectorBytes   = 2048;
constexpr u32 kFirstVolumeDescriptor = 16; // after the system area (ECMA-119 6.2.1)
constexpr u32 kMaxVolumeDescriptors  = 32;
constexpr int kMaxDirDepth   = 8;

// The bounce buffer: whatever doesn't start a sector, or goes to memory DMA can't write (64-byte
// lines, ps2-platform.md), is read into it and copied out. 64 KB is the largest piece anything
// asks for in one read: a 64 KB stdio buffer, or a 32 KB loader chunk or pkz input piece that
// straddles 17 sectors. It keeps the run of sectors it last read, so small reads walking a file
// cost one drive read per run.
constexpr u32 kBufferSectors = 32;

// A direct read into the caller's memory is split into pieces of at most this many sectors (a
// few hundred KB at a time, so one read never holds the device for long).
constexpr u32 kMaxDirectSectors = 128;

constexpr int kMaxOpenFiles = 32;

// What stdio is told a file's block is (st_blksize), so it sizes and aligns its buffers the way
// it does on USB and the HDD, whose fileXio buffer is 64 KB (iop_boot.cpp).
constexpr int kStdioBlockBytes = 64 * 1024;

// How the drive is waited on. A read retried this many times on an error is a failed read; the
// drive itself retries each one first (sceCdRMode::trycount).
constexpr s32 kPollMicrosec      = 1000;
constexpr int kMaxReadAttempts   = 4;
constexpr u8  kDriveTryCount     = 8;
constexpr int kNotReadyTimeoutMs = 20000;
constexpr int kDiscWaitMs        = 15000;
constexpr int kDiscPollMs        = 100;

// ------------------------------------------------------------------------------------------------
// The index of the disc's files
// ------------------------------------------------------------------------------------------------

struct Entry
{
    u32  lba;
    u32  sizeBytes;
    u32  pathOffset; // into s_paths: lower case, '/' between parts, no ";1", no leading '/'
    u16  pathLength;
    bool isDir;
};

static Entry * s_entries      = nullptr;
static int     s_numEntries   = 0;
static int     s_entryCap     = 0;
static char *  s_paths        = nullptr;
static u32     s_pathsUsed    = 0;
static u32     s_pathsCap     = 0;
static u32 *   s_hashSlots    = nullptr; // entry index + 1, or 0; open addressing
static u32     s_hashMask     = 0;
static int     s_numDirs      = 0;

const char * PathOf(const Entry & entry)
{
    return s_paths + entry.pathOffset;
}

u32 HashPath(const char * path, const u32 length)
{
    u32 hash = 2166136261u;
    for (u32 i = 0; i < length; ++i)
    {
        hash = (hash ^ static_cast<u8>(path[i])) * 16777619u;
    }
    return hash;
}

// Grows a block allocated with the tag (the index is only built at the mount).
template<typename T>
T * Grow(T * old, const u32 oldCount, const u32 newCount)
{
    T * const grown = static_cast<T *>(ps2::heap::Alloc(newCount * sizeof(T), MemTag::Pack));
    if (old != nullptr)
    {
        std::memcpy(static_cast<void *>(grown), static_cast<const void *>(old), oldCount * sizeof(T));
        ps2::heap::Free(old, oldCount * sizeof(T), MemTag::Pack);
    }
    return grown;
}

// Adds a file or directory under the entry `parentIndex` (-1 for the root itself). Both blocks
// can move as they grow, so the parent is found again by its index afterwards.
int AddEntry(const int parentIndex, const char * name, const u32 nameLength, const u32 lba, const u32 sizeBytes,
             const bool isDir)
{
    if (s_numEntries == s_entryCap)
    {
        const int newCap = (s_entryCap == 0) ? 128 : s_entryCap * 2;
        s_entries  = Grow(s_entries, static_cast<u32>(s_entryCap), static_cast<u32>(newCap));
        s_entryCap = newCap;
    }

    const u32 parentLength = (parentIndex >= 0) ? s_entries[parentIndex].pathLength : 0u;
    const u32 length = (parentLength > 0) ? parentLength + 1 + nameLength : nameLength;
    if (s_pathsUsed + length + 1 > s_pathsCap)
    {
        u32 newCap = (s_pathsCap == 0) ? 4096u : s_pathsCap * 2;
        while (s_pathsUsed + length + 1 > newCap)
        {
            newCap *= 2;
        }
        s_paths    = Grow(s_paths, s_pathsCap, newCap);
        s_pathsCap = newCap;
    }

    char * const path = s_paths + s_pathsUsed;
    u32 at = 0;
    if (parentLength > 0)
    {
        std::memcpy(path, PathOf(s_entries[parentIndex]), parentLength);
        path[parentLength] = '/';
        at = parentLength + 1;
    }
    for (u32 i = 0; i < nameLength; ++i)
    {
        const char c = name[i];
        path[at + i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    path[length] = '\0';

    Entry & entry = s_entries[s_numEntries];
    entry.lba        = lba;
    entry.sizeBytes  = sizeBytes;
    entry.pathOffset = s_pathsUsed;
    entry.pathLength = static_cast<u16>(length);
    entry.isDir      = isDir;
    s_pathsUsed += length + 1;
    s_numDirs   += isDir ? 1 : 0;
    return s_numEntries++;
}

void BuildHash()
{
    u32 slots = 64;
    while (slots < static_cast<u32>(s_numEntries) * 2)
    {
        slots *= 2;
    }
    s_hashSlots = static_cast<u32 *>(ps2::heap::Alloc(slots * sizeof(u32), MemTag::Pack));
    std::memset(s_hashSlots, 0, slots * sizeof(u32));
    s_hashMask = slots - 1;

    for (int i = 0; i < s_numEntries; ++i)
    {
        u32 slot = HashPath(PathOf(s_entries[i]), s_entries[i].pathLength) & s_hashMask;
        while (s_hashSlots[slot] != 0)
        {
            slot = (slot + 1) & s_hashMask;
        }
        s_hashSlots[slot] = static_cast<u32>(i) + 1;
    }
}

// A path as newlib passes it ("cdrom0:/baseq2/pak0.pkz", "cdrom0:\BASEQ2\PAK0.PKZ;1", ...) in the
// index's form: no device, lower case, '/' separators, no version, no leading or trailing '/'.
// False if it is too long to be on the disc.
bool NormalizePath(const char * path, char (&out)[MAX_OSPATH], u32 & outLength)
{
    const char * const colon = std::strchr(path, ':');
    const char * in = (colon != nullptr) ? colon + 1 : path;

    u32 length = 0;
    for (; *in != '\0'; ++in)
    {
        char c = *in;
        if (c == ';')
        {
            // A version: ";1" ends the name it follows; skip to the next separator, if any.
            while (in[1] != '\0' && in[1] != '/' && in[1] != '\\')
            {
                ++in;
            }
            continue;
        }
        if (c == '\\')
        {
            c = '/';
        }
        if (c == '/' && (length == 0 || out[length - 1] == '/'))
        {
            continue;
        }
        if (length + 1 >= sizeof(out))
        {
            return false;
        }
        out[length++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    while (length > 0 && out[length - 1] == '/')
    {
        --length;
    }
    out[length] = '\0';
    outLength = length;
    return true;
}

const Entry * Find(const char * path)
{
    char normal[MAX_OSPATH];
    u32 length = 0;
    if (s_hashSlots == nullptr || !NormalizePath(path, normal, length))
    {
        return nullptr;
    }

    for (u32 slot = HashPath(normal, length) & s_hashMask; s_hashSlots[slot] != 0; slot = (slot + 1) & s_hashMask)
    {
        const Entry & entry = s_entries[s_hashSlots[slot] - 1];
        if (entry.pathLength == length && std::memcmp(PathOf(entry), normal, length) == 0)
        {
            return &entry;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// The drive
// ------------------------------------------------------------------------------------------------

alignas(64) static u8 s_bounce[kBufferSectors * kSectorBytes];
static u32 s_bounceLba   = 0;
static u32 s_bounceCount = 0; // sectors s_bounce holds from s_bounceLba; 0 for none

static int s_lockSema = -1;
static ReadStats s_stats = {};

static sceCdRMode s_readMode = {};

class DeviceLock final
{
public:
    DeviceLock() { WaitSema(s_lockSema); }
    ~DeviceLock() { SignalSema(s_lockSema); }

    DeviceLock(const DeviceLock &) = delete;
    DeviceLock & operator=(const DeviceLock &) = delete;
};

// Reads `count` whole sectors from `lba` into `dest`, which DMA can write (64-byte aligned, whole
// lines). Sleeps while the drive works. False after kMaxReadAttempts failed reads.
bool ReadSectors(const u32 lba, const u32 count, void * dest)
{
    for (int attempt = 0; attempt < kMaxReadAttempts; ++attempt)
    {
        // 0 is "not ready" or "busy": a disc still spinning up, or a command of someone else's.
        int waitedMs = 0;
        while (sceCdRead(lba, count, dest, &s_readMode) == 0)
        {
            if (waitedMs >= kNotReadyTimeoutMs)
            {
                s_stats.lastError = -1;
                return false;
            }
            DelayThread(kPollMicrosec);
            ++waitedMs;
        }

        while (sceCdSync(1) != 0)
        {
            DelayThread(kPollMicrosec);
        }

        s_stats.reads   += 1;
        s_stats.sectors += count;
        const int error = sceCdGetError();
        if (error == SCECdErNO)
        {
            return true;
        }
        s_stats.lastError = error;
        s_stats.retries  += 1;
    }
    return false;
}

// Copies `bytes` from `pos` of the extent at `lba` (of `extentBytes`) into `dest`, through the
// bounce buffer or straight from the drive, as the alignment allows. Returns what was read, or
// -EIO. The caller holds the lock.
int ReadExtent(const u32 lba, const u32 extentBytes, const u32 pos, u8 * dest, const u32 bytes)
{
    if (pos >= extentBytes)
    {
        return 0;
    }
    const u32 wanted = (bytes < extentBytes - pos) ? bytes : extentBytes - pos;

    u32 done = 0;
    while (done < wanted)
    {
        const u32 at     = pos + done;
        const u32 sector = lba + at / kSectorBytes;
        const u32 offset = at % kSectorBytes;
        const u32 left   = wanted - done;
        u8 * const to    = dest + done;

        if (offset == 0 && left >= kSectorBytes && (reinterpret_cast<uintptr_t>(to) & 63u) == 0)
        {
            u32 count = left / kSectorBytes;
            count = (count < kMaxDirectSectors) ? count : kMaxDirectSectors;
            if (!ReadSectors(sector, count, to))
            {
                return -EIO;
            }
            done += count * kSectorBytes;
            continue;
        }

        if (s_bounceCount == 0 || sector < s_bounceLba || sector >= s_bounceLba + s_bounceCount)
        {
            // The sectors the rest of this read covers, as many as the buffer holds.
            const u32 lastSector = lba + (at + left - 1) / kSectorBytes;
            u32 count = lastSector - sector + 1;
            count = (count < kBufferSectors) ? count : kBufferSectors;
            s_bounceCount = 0;
            if (!ReadSectors(sector, count, s_bounce))
            {
                return -EIO;
            }
            s_bounceLba   = sector;
            s_bounceCount = count;
        }

        const u32 from      = (sector - s_bounceLba) * kSectorBytes + offset;
        const u32 available = s_bounceCount * kSectorBytes - from;
        const u32 copy      = (left < available) ? left : available;
        std::memcpy(to, s_bounce + from, copy);
        done += copy;
    }
    return static_cast<int>(done);
}

// ------------------------------------------------------------------------------------------------
// Reading the disc's directories
// ------------------------------------------------------------------------------------------------

u32 ReadLe32(const u8 * p)
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// The sector the mount reads volume descriptors and directories into. Static, not on the stack:
// GCC doesn't align the EE's stack past 16 bytes, and DMA wants whole 64-byte lines.
alignas(64) static u8 s_sector[kSectorBytes];

// The root directory's extent, from the primary volume descriptor. False if there is none.
bool FindRoot(u32 & rootLba, u32 & rootBytes)
{
    u8 * const sector = s_sector;
    for (u32 i = 0; i < kMaxVolumeDescriptors; ++i)
    {
        if (ReadExtent(kFirstVolumeDescriptor + i, kSectorBytes, 0, sector, kSectorBytes) != static_cast<int>(kSectorBytes) ||
            std::memcmp(sector + 1, "CD001", 5) != 0)
        {
            return false;
        }
        if (sector[0] == 255) // the set's terminator
        {
            return false;
        }
        if (sector[0] == 1)
        {
            const u8 * const root = sector + 156;
            rootLba   = ReadLe32(root + 2);
            rootBytes = ReadLe32(root + 10);
            return rootBytes > 0;
        }
    }
    return false;
}

// Adds the directory's entries to the index, its subdirectories after everything already there,
// so the caller's walk down the index reaches them in turn. False for a directory that can't be
// read, or a record that can't be right.
bool IndexDirectory(const int dirIndex, const char ** failure)
{
    const u32 lba    = s_entries[dirIndex].lba;
    const u32 extent = s_entries[dirIndex].sizeBytes;

    u8 * const sector = s_sector;
    for (u32 at = 0; at < extent; at += kSectorBytes)
    {
        if (ReadExtent(lba, extent, at, sector, kSectorBytes) <= 0)
        {
            *failure = "a directory of the disc couldn't be read";
            return false;
        }
        const u32 sectorBytes = (extent - at < kSectorBytes) ? extent - at : kSectorBytes;

        // A record never crosses a sector; a zero length pads out the rest of one.
        for (u32 offset = 0; offset < sectorBytes;)
        {
            const u8 * const record = sector + offset;
            const u32 recordBytes = record[0];
            if (recordBytes == 0)
            {
                break;
            }
            if (recordBytes < 34 || offset + recordBytes > sectorBytes || 33u + record[32] > recordBytes)
            {
                *failure = "a directory of the disc is damaged";
                return false;
            }
            offset += recordBytes;

            const u8   flags      = record[25];
            const u32  nameBytes  = record[32];
            const char * name     = reinterpret_cast<const char *>(record + 33);
            if (nameBytes == 1 && (name[0] == '\0' || name[0] == '\1'))
            {
                continue; // "." and ".."
            }
            if ((flags & 0x80) != 0)
            {
                *failure = "a file on the disc is in more than one extent (over 4 GB)";
                return false;
            }

            // "NAME.EXT;1" -> "name.ext", "NAME.;1" -> "name".
            u32 length = 0;
            while (length < nameBytes && name[length] != ';')
            {
                ++length;
            }
            if (length > 0 && name[length - 1] == '.')
            {
                --length;
            }
            if (length == 0)
            {
                continue;
            }

            const bool isDir = (flags & 0x02) != 0;
            AddEntry(dirIndex, name, length, ReadLe32(record + 2), ReadLe32(record + 10), isDir);
        }
    }
    return true;
}

bool BuildIndex(const char ** failure)
{
    u32 rootLba = 0;
    u32 rootBytes = 0;
    if (!FindRoot(rootLba, rootBytes))
    {
        *failure = "the disc has no ISO9660 file system";
        return false;
    }

    AddEntry(-1, "", 0, rootLba, rootBytes, true);
    for (int i = 0; i < s_numEntries; ++i)
    {
        if (!s_entries[i].isDir)
        {
            continue;
        }

        int depth = 0;
        for (const char * c = PathOf(s_entries[i]); *c != '\0'; ++c)
        {
            depth += (*c == '/') ? 1 : 0;
        }
        if (depth > kMaxDirDepth || !IndexDirectory(i, failure))
        {
            if (depth > kMaxDirDepth)
            {
                *failure = "the disc's directories nest too deep";
            }
            return false;
        }
    }

    BuildHash();
    return true;
}

// ------------------------------------------------------------------------------------------------
// newlib's file calls (libcglue's fd and path operations)
// ------------------------------------------------------------------------------------------------

struct OpenFile
{
    const Entry * entry;
    u32           pos;
    bool          used;
    char          name[MAX_OSPATH]; // for fstat: libcglue stats an open file by its name
};

static OpenFile s_files[kMaxOpenFiles];

static _libcglue_fdman_path_ops_t * s_nextPathOps = nullptr;
static _libcglue_fdman_path_ops_t   s_pathOps     = {};
static _libcglue_fdman_fd_ops_t     s_fileOps     = {};

bool IsDiscPath(const char * path)
{
    return std::strncmp(path, "cdrom0:", 7) == 0 || std::strncmp(path, "cdrom:", 6) == 0;
}

int FileGetFd(void * userdata)
{
    return static_cast<int>(static_cast<OpenFile *>(userdata) - s_files);
}

char * FileGetName(void * userdata)
{
    return static_cast<OpenFile *>(userdata)->name;
}

int FileClose(void * userdata)
{
    const DeviceLock lock;
    static_cast<OpenFile *>(userdata)->used = false;
    return 0;
}

int FileRead(void * userdata, void * buf, const int nbytes)
{
    if (nbytes <= 0)
    {
        return 0;
    }
    OpenFile & file = *static_cast<OpenFile *>(userdata);

    const DeviceLock lock;
    const int read = ReadExtent(file.entry->lba, file.entry->sizeBytes, file.pos, static_cast<u8 *>(buf),
                                static_cast<u32>(nbytes));
    if (read > 0)
    {
        file.pos += static_cast<u32>(read);
    }
    return read;
}

s64 Seek(OpenFile & file, const s64 offset, const int whence)
{
    s64 base = 0;
    switch (whence)
    {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = file.pos; break;
    case SEEK_END: base = file.entry->sizeBytes; break;
    default: return -EINVAL;
    }
    const s64 target = base + offset;
    if (target < 0 || target > 0xFFFFFFFFll)
    {
        return -EINVAL;
    }
    file.pos = static_cast<u32>(target);
    return target;
}

int FileLseek(void * userdata, const int offset, const int whence)
{
    const s64 result = Seek(*static_cast<OpenFile *>(userdata), offset, whence);
    return (result > 0x7FFFFFFFll) ? -EOVERFLOW : static_cast<int>(result);
}

int64_t FileLseek64(void * userdata, const int64_t offset, const int whence)
{
    return Seek(*static_cast<OpenFile *>(userdata), offset, whence);
}

int FileWrite(void *, const void *, int) { return -EBADF; }
int FileIoctl(void *, int, void *) { return -ENOTTY; }
int FileIoctl2(void *, int, void *, unsigned int, void *, unsigned int) { return -ENOTTY; }
int FileDread(void *, struct dirent *) { return -ENOTDIR; }
int FileSetFlags(void *, int) { return 0; }

int DiscOpen(_libcglue_fdman_fd_info_t * info, const char * path, const int flags, const mode_t)
{
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0)
    {
        return -EROFS;
    }
    const Entry * const entry = Find(path);
    if (entry == nullptr)
    {
        return -ENOENT;
    }
    if (entry->isDir)
    {
        return -EISDIR; // nothing lists a directory on the disc (Sys_FindFirst finds nothing)
    }

    const DeviceLock lock;
    for (OpenFile & file : s_files)
    {
        if (!file.used)
        {
            file.used  = true;
            file.entry = entry;
            file.pos   = 0;
            std::snprintf(file.name, sizeof(file.name), "%s/%s", kBasePath, PathOf(*entry));
            info->userdata = &file;
            info->ops      = &s_fileOps;
            return 0;
        }
    }
    return -EMFILE;
}

int DiscStat(const Entry & entry, struct stat * buf)
{
    std::memset(static_cast<void *>(buf), 0, sizeof(*buf));
    buf->st_mode    = entry.isDir ? (S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR | S_IXGRP | S_IXOTH)
                                  : (S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
    buf->st_size    = static_cast<off_t>(entry.sizeBytes);
    buf->st_blksize = entry.isDir ? 0 : kStdioBlockBytes;
    buf->st_blocks  = static_cast<blkcnt_t>((entry.sizeBytes + 511u) / 512u);
    return 0;
}

int PathOpen(_libcglue_fdman_fd_info_t * info, const char * path, const int flags, const mode_t mode)
{
    if (IsDiscPath(path))
    {
        return DiscOpen(info, path, flags, mode);
    }
    return (s_nextPathOps->open != nullptr) ? s_nextPathOps->open(info, path, flags, mode) : -ENOSYS;
}

int PathStat(const char * path, struct stat * buf)
{
    if (IsDiscPath(path))
    {
        const Entry * const entry = Find(path);
        return (entry != nullptr) ? DiscStat(*entry, buf) : -ENOENT;
    }
    return (s_nextPathOps->stat != nullptr) ? s_nextPathOps->stat(path, buf) : -ENOSYS;
}

int PathRemove(const char * path)
{
    if (IsDiscPath(path))
    {
        return -EROFS;
    }
    return (s_nextPathOps->remove != nullptr) ? s_nextPathOps->remove(path) : -ENOSYS;
}

int PathRename(const char * from, const char * to)
{
    if (IsDiscPath(from) || IsDiscPath(to))
    {
        return -EROFS;
    }
    return (s_nextPathOps->rename != nullptr) ? s_nextPathOps->rename(from, to) : -ENOSYS;
}

int PathMkdir(const char * path, const int mode)
{
    if (IsDiscPath(path))
    {
        return -EROFS;
    }
    return (s_nextPathOps->mkdir != nullptr) ? s_nextPathOps->mkdir(path, mode) : -ENOSYS;
}

int PathRmdir(const char * path)
{
    if (IsDiscPath(path))
    {
        return -EROFS;
    }
    return (s_nextPathOps->rmdir != nullptr) ? s_nextPathOps->rmdir(path) : -ENOSYS;
}

void InstallOps()
{
    s_fileOps.getfd         = &FileGetFd;
    s_fileOps.getfilename   = &FileGetName;
    s_fileOps.close         = &FileClose;
    s_fileOps.read          = &FileRead;
    s_fileOps.lseek         = &FileLseek;
    s_fileOps.lseek64       = &FileLseek64;
    s_fileOps.write         = &FileWrite;
    s_fileOps.ioctl         = &FileIoctl;
    s_fileOps.ioctl2        = &FileIoctl2;
    s_fileOps.dread         = &FileDread;
    s_fileOps.fcntl_f_setfl = &FileSetFlags;

    // Everything the table had, readlink and symlink included, and the cdrom0: paths in front.
    static _libcglue_fdman_path_ops_t s_none = {};
    s_nextPathOps = (_libcglue_fdman_path_ops != nullptr) ? _libcglue_fdman_path_ops : &s_none;
    s_pathOps        = *s_nextPathOps;
    s_pathOps.open   = &PathOpen;
    s_pathOps.stat   = &PathStat;
    s_pathOps.remove = &PathRemove;
    s_pathOps.rename = &PathRename;
    s_pathOps.mkdir  = &PathMkdir;
    s_pathOps.rmdir  = &PathRmdir;
    _libcglue_fdman_path_ops = &s_pathOps;
}

// ------------------------------------------------------------------------------------------------
// Mounting
// ------------------------------------------------------------------------------------------------

static char s_summary[160] = {};

const char * MediaName(const int type)
{
    switch (type)
    {
    case SCECdPS2CD:   return "PS2 CD";
    case SCECdPS2CDDA: return "PS2 CD with audio tracks";
    case SCECdPS2DVD:  return "PS2 DVD";
    case SCECdPSCD:
    case SCECdPSCDDA:  return "PlayStation CD";
    case SCECdDVDV:    return "DVD video";
    case SCECdCDDA:    return "audio CD";
    default:           return "disc";
    }
}

bool IsDvd(const int type)
{
    return type == SCECdPS2DVD || type == SCECdDVDV || type == SCECdDVDVR ||
           type == SCECdDETCTDVDS || type == SCECdDETCTDVDD;
}

bool IsDetecting(const int type)
{
    return type == SCECdDETCT || type == SCECdDETCTCD || type == SCECdDETCTDVDS || type == SCECdDETCTDVDD;
}

} // namespace

bool Mount(const char ** failure)
{
    const int startMs = Sys_Milliseconds();

    if (sceCdInit(SCECdINIT) == 0)
    {
        *failure = "the CD/DVD drivers on the IOP didn't answer";
        return false;
    }

    // A disc boot finds the disc there, and so does OPL; the drive may still be telling a CD
    // from a DVD for a moment after the tray closes.
    int type = sceCdGetDiskType();
    for (int waited = 0; (type == SCECdNODISC || IsDetecting(type)) && waited < kDiscWaitMs; waited += kDiscPollMs)
    {
        DelayThread(kDiscPollMs * 1000);
        type = sceCdGetDiskType();
    }
    if (type == SCECdNODISC || IsDetecting(type) || type == SCECdGDTFUNCFAIL)
    {
        *failure = "there is no disc in the drive";
        return false;
    }
    if (type == SCECdIllegalMedia || type == SCECdUNKNOWN)
    {
        *failure = "the drive can't read the disc in it";
        return false;
    }

    sceCdMmode(IsDvd(type) ? SCECdMmodeDvd : SCECdMmodeCd);
    sceCdDiskReady(0);

    ee_sema_t sema = {};
    sema.init_count = 1;
    sema.max_count  = 1;
    s_lockSema = CreateSema(&sema);
    if (s_lockSema < 0)
    {
        *failure = "no semaphore for the disc device";
        return false;
    }

    s_readMode.trycount    = kDriveTryCount;
    s_readMode.spindlctrl  = SCECdSpinNom;
    s_readMode.datapattern = SCECdSecS2048;

    bool indexed = false;
    {
        const DeviceLock lock;
        indexed = BuildIndex(failure);
    }
    if (!indexed)
    {
        return false;
    }

    InstallOps();

    const u32 indexBytes = static_cast<u32>(s_entryCap) * static_cast<u32>(sizeof(Entry)) + s_pathsCap +
                           (s_hashMask + 1) * static_cast<u32>(sizeof(u32));
    std::snprintf(s_summary, sizeof(s_summary), "%s: %s, %d files in %d directories, %u KB of index, mounted in %d ms",
                  kDiscBuildMarker, MediaName(type), s_numEntries - s_numDirs, s_numDirs - 1,
                  static_cast<unsigned>((indexBytes + 1023) / 1024), Sys_Milliseconds() - startMs);
    return true;
}

const char * MountSummary()
{
    return s_summary;
}

ReadStats GetReadStats()
{
    return s_stats;
}

} // namespace ps2::cdvd
