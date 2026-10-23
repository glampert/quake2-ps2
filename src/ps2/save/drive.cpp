/* ================================================================================================
 * File: drive.cpp
 * Brief: The boot drive's save Device. See drive.h.
 *
 *  Plain files through stdio, which newlib sends to the ROM's FILEIO for host: and to fileXio for
 *  mass: and pfs0: - the same way the engine reads the game data and writes screenshots. It was
 *  the host-file Device, offered under the emulator only; on a console's drives it needs:
 *
 *  - The hard disk's PFS keeps a file's metadata cached after its close with the game's read/write
 *    mount; ps2::sys::SyncGameDataDevice flushes it, after every file written or deleted. host:
 *    and USB need nothing: FatFs puts a file on the USB drive at its close.
 *  - On host: the ROM FILEIO's remove() runs a mkdir() of the same path after it, unless the boot
 *    patched it (ps2::sys::FileIoRemovePatched), so a deleted file comes back as a directory,
 *    which is removed too.
 *
 *  Free space isn't asked for: no driver here says how much a drive has, and a full one fails the
 *  write, which StoreSlot reports as it does any other.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/drive.h"
#include "ps2/save/save_notice.h"
#include "ps2/system/iop_boot.h"

#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps2::save {
namespace {

// The save directory, under the game directory.
constexpr const char * kDriveSaveDir = "save";

// As the memory card's (memcard.cpp): every read and write of the saved data goes on under the
// save notice. A Probe goes without.
void AssertUnderNotice()
{
    PS2_AssertMsg(SaveNoticeShown(), "Saved data on the drive accessed outside the save notice!");
}

class DriveDevice final : public Device
{
public:
    // From the game directory's device, which the boot picked (ps2::sys::DetectBasePathAndBootIop):
    // "host:", "massN:" or "pfs0:".
    DeviceKind Kind() const override
    {
        const char * const gamedir = FS_Gamedir();
        if (std::strncmp(gamedir, "mass", 4) == 0)
        {
            return DeviceKind::UsbDrive;
        }
        if (std::strncmp(gamedir, "pfs", 3) == 0)
        {
            return DeviceKind::HardDisk;
        }
        return DeviceKind::HostFiles;
    }

    // The drive holds the game data being played, so it is there; the directory follows the game
    // directory, which "game" can change.
    bool Probe() override
    {
        std::snprintf(m_dir, sizeof(m_dir), "%s/%s", FS_Gamedir(), kDriveSaveDir);
        return true;
    }

    const char * StatusText() override
    {
        Probe();

        const char * title = "Host files";
        switch (Kind())
        {
        case DeviceKind::UsbDrive:   title = "USB drive"; break;
        case DeviceKind::HardDisk:   title = "Hard disk"; break;
        case DeviceKind::HostFiles:  break;
        case DeviceKind::MemoryCard: break;
        }
        std::snprintf(m_status, sizeof(m_status), "%s: %s", title, m_dir);
        return m_status;
    }

    // Unlimited, as far as a save cares - yet small enough to add a file's size to.
    u32 FreeBytes() const override { return 0x40000000u; }
    u32 FileCostBytes(const u32 sizeBytes) const override { return sizeBytes; }

    // A drive can't be swapped for another under the game running from it.
    u32 Generation() const override { return 0; }

    bool EnsureSaveDir() override
    {
        AssertUnderNotice();

        // Already there is the usual case, and mkdir failing for it is not worth telling apart
        // from failing for real: that shows up at the first write.
        mkdir(m_dir, 0777);
        ps2::sys::SyncGameDataDevice();
        return true;
    }

    int List(DirEntry * outEntries, const int maxEntries) override
    {
        AssertUnderNotice();

        DIR * const dir = opendir(m_dir);
        if (dir == nullptr)
        {
            return 0; // No save directory yet: no files.
        }

        int count = 0;
        while (count < maxEntries)
        {
            const dirent * const found = readdir(dir);
            if (found == nullptr)
            {
                break;
            }

            u32 sizeBytes = 0;
            if (found->d_name[0] != '.' && std::strlen(found->d_name) < kMaxNameLen && FileSize(found->d_name, sizeBytes))
            {
                CopyName(outEntries[count].name, found->d_name);
                outEntries[count].sizeBytes = sizeBytes;
                ++count;
            }
        }

        closedir(dir);
        return count;
    }

    bool FileSize(const char * name, u32 & outSizeBytes) override
    {
        AssertUnderNotice();

        std::FILE * const file = std::fopen(PathOf(name), "rb");
        if (file == nullptr)
        {
            return false;
        }

        const bool ok = (std::fseek(file, 0, SEEK_END) == 0);
        const long size = ok ? std::ftell(file) : -1;
        std::fclose(file);

        if (size < 0)
        {
            return false;
        }
        outSizeBytes = static_cast<u32>(size);
        return true;
    }

    static constexpr FileHandle kOpenHandle = FileHandle(0);

    FileHandle Open(const char * name, const OpenMode mode) override
    {
        AssertUnderNotice();

        PS2_AssertMsg(m_file == nullptr, "DriveDevice: Only one file can be opened at a time!");
        m_writing = (mode == OpenMode::Write);
        m_file    = std::fopen(PathOf(name), m_writing ? "wb" : "rb");
        return (m_file != nullptr) ? kOpenHandle : FileHandle::Invalid;
    }

    bool Read(const FileHandle handle, void * dest, const u32 sizeBytes) override
    {
        AssertUnderNotice();
        return handle == kOpenHandle && m_file != nullptr && std::fread(dest, 1, sizeBytes, m_file) == sizeBytes;
    }

    bool Write(const FileHandle handle, const void * src, const u32 sizeBytes) override
    {
        AssertUnderNotice();
        return handle == kOpenHandle && m_file != nullptr && m_writing &&
               std::fwrite(src, 1, sizeBytes, m_file) == sizeBytes;
    }

    bool Close(const FileHandle handle) override
    {
        AssertUnderNotice();

        if (handle != kOpenHandle || m_file == nullptr)
        {
            return false;
        }

        const bool ok     = (std::fflush(m_file) == 0) && !std::ferror(m_file);
        const bool closed = (std::fclose(m_file) == 0);
        m_file = nullptr;

        // Even a write that failed may have changed what the hard disk's PFS has cached.
        const bool synced = !m_writing || ps2::sys::SyncGameDataDevice() == 0;
        return ok && closed && synced;
    }

    bool Delete(const char * name) override
    {
        AssertUnderNotice();

        const char * const path = PathOf(name);
        std::remove(path);
        if (Kind() == DeviceKind::HostFiles && !ps2::sys::FileIoRemovePatched())
        {
            rmdir(path);
        }
        ps2::sys::SyncGameDataDevice();

        u32 sizeBytes = 0;
        return !FileSize(name, sizeBytes);
    }

    const char * Describe(const char * name) override
    {
        return PathOf(name);
    }

private:
    const char * PathOf(const char * name)
    {
        std::snprintf(m_path, sizeof(m_path), "%s/%s", m_dir, name);
        return m_path;
    }

    // The game directory, its longest, and "/save"; then a file in it, and the status line.
    static constexpr int kDirLen = MAX_OSPATH + 8;

    char m_dir[kDirLen]                    = {};
    char m_path[kDirLen + kMaxNameLen + 1] = {};
    char m_status[kDirLen + 16]            = {};

    std::FILE * m_file    = nullptr;
    bool        m_writing = false;
};

static DriveDevice s_driveDevice;

} // namespace

Device & GetDriveDevice()
{
    s_driveDevice.Probe(); // Its directory, before anything asks for a file in it.
    return s_driveDevice;
}

} // namespace ps2::save
