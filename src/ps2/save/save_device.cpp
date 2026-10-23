/* ================================================================================================
 * File: save_device.cpp
 * Brief: What the save devices share: error reporting, their names, and the whole-file helpers.
 *        See save_system.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

#include <cstdarg>
#include <cstring>

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
// Device names
// ------------------------------------------------------------------------------------------------

const char * DeviceName(const DeviceKind kind)
{
    switch (kind)
    {
    case DeviceKind::MemoryCard: return "memory card";
    case DeviceKind::HostFiles:  return "host files";
    case DeviceKind::UsbDrive:   return "USB drive";
    case DeviceKind::HardDisk:   return "hard disk";
    }
    return "memory card";
}

const char * DeviceValue(const DeviceKind kind)
{
    switch (kind)
    {
    case DeviceKind::MemoryCard: return "mc";
    case DeviceKind::HostFiles:  return "host";
    case DeviceKind::UsbDrive:   return "usb";
    case DeviceKind::HardDisk:   return "hdd";
    }
    return "mc";
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

} // namespace ps2::save
