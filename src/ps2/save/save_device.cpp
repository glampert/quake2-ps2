/* ================================================================================================
 * File: save_device.cpp
 * Brief: The host-file save Device and the save system's error reporting. See save_device.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_device.h"

#include <cstdarg>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps2::save {

// ------------------------------------------------------------------------------------------------
// Error reporting
// ------------------------------------------------------------------------------------------------

static char s_lastError[160] = {};

void SetError(const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(s_lastError, sizeof(s_lastError), fmt, args);
    va_end(args);

    Com_Printf("Save: %s\n", s_lastError);
}

void ClearError()
{
    s_lastError[0] = '\0';
}

const char * LastError()
{
    return s_lastError;
}

// ------------------------------------------------------------------------------------------------
// Whole-file helpers
// ------------------------------------------------------------------------------------------------

bool WriteWholeFile(Device & device, const char * name, const void * data, const u32 sizeBytes)
{
    const FileHandle handle = device.Open(name, OpenMode::Write);
    if (handle == FileHandle::Invalid)
    {
        return false;
    }

    const bool written = device.Write(handle, data, sizeBytes);
    return device.Close(handle) && written;
}

bool FileMatches(Device & device, const char * name, const void * expected, const u32 sizeBytes)
{
    u32 sizeOnDevice = 0;
    if (!device.FileSize(name, sizeOnDevice) || sizeOnDevice != sizeBytes)
    {
        return false;
    }

    const FileHandle handle = device.Open(name, OpenMode::Read);
    if (handle == FileHandle::Invalid)
    {
        return false;
    }

    u8 chunk[2048];
    const u8 * const bytes = static_cast<const u8 *>(expected);
    bool same = true;

    for (u32 offset = 0; same && offset < sizeBytes;)
    {
        const u32 n = (sizeBytes - offset < sizeof(chunk)) ? sizeBytes - offset : static_cast<u32>(sizeof(chunk));
        same = device.Read(handle, chunk, n) && std::memcmp(chunk, bytes + offset, n) == 0;
        offset += n;
    }

    device.Close(handle);
    return same;
}

// ------------------------------------------------------------------------------------------------
// HostDevice
// ------------------------------------------------------------------------------------------------

namespace {

static bool s_deleteLeavesDirectory = false;

class HostDevice final : public Device
{
public:
    bool Probe() override
    {
        std::snprintf(m_dir, sizeof(m_dir), "%s/save", FS_Gamedir());
        return true;
    }

    const char * StatusText() override
    {
        Probe();
        std::snprintf(m_status, sizeof(m_status), "Host files: %s", m_dir);
        return m_status;
    }

    // Unlimited, as far as a save cares - yet small enough to add a file's size to.
    u32 FreeBytes() const override { return 0x40000000u; }
    u32 FileCostBytes(const u32 sizeBytes) const override { return sizeBytes; }

    bool EnsureSaveDir() override
    {
        // Already there is the usual case, and mkdir failing for it is not worth telling apart
        // from failing for real: that shows up at the first write.
        mkdir(m_dir, 0777);
        return true;
    }

    int List(DirEntry * outEntries, const int maxEntries) override
    {
        DIR * dir = opendir(m_dir);
        if (dir == nullptr)
        {
            return -1;
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
        PS2_AssertMsg(m_file == nullptr, "HostDevice: Only one file can be opened at a time!");
        m_file = std::fopen(PathOf(name), (mode == OpenMode::Write) ? "wb" : "rb");
        return (m_file != nullptr) ? kOpenHandle : FileHandle::Invalid;
    }

    bool Read(const FileHandle handle, void * dest, const u32 sizeBytes) override
    {
        return handle == kOpenHandle && m_file != nullptr && std::fread(dest, 1, sizeBytes, m_file) == sizeBytes;
    }

    bool Write(const FileHandle handle, const void * src, const u32 sizeBytes) override
    {
        return handle == kOpenHandle && m_file != nullptr && std::fwrite(src, 1, sizeBytes, m_file) == sizeBytes;
    }

    bool Close(const FileHandle handle) override
    {
        if (handle != kOpenHandle || m_file == nullptr)
        {
            return false;
        }

        const bool ok = (std::fflush(m_file) == 0) && !std::ferror(m_file);
        const bool closed = (std::fclose(m_file) == 0);
        m_file = nullptr;
        return ok && closed;
    }

    bool Delete(const char * name) override
    {
        const char * const path = PathOf(name);
        std::remove(path);

        if (s_deleteLeavesDirectory)
        {
            rmdir(path);
        }

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

    char m_dir[MAX_OSPATH]                    = {};
    char m_path[MAX_OSPATH + kMaxNameLen + 1] = {};
    char m_status[MAX_OSPATH + 16]            = {};

    std::FILE * m_file = nullptr;
};

static HostDevice s_hostDevice;

} // namespace

Device & GetHostDevice()
{
    return s_hostDevice;
}

void SetHostDeleteLeavesDirectory(const bool leavesDirectory)
{
    s_deleteLeavesDirectory = leavesDirectory;
}

} // namespace ps2::save
