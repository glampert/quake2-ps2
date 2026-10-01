/* ================================================================================================
 * File: memcard.cpp
 * Brief: The memory card save Device. See memcard.h.
 *
 *  Drivers: the ROM's MCMAN and MCSERV, loaded on first use on top of the ROM's SIO2MAN - the
 *  pad driver (input/pad.cpp) runs the ROM's PADMAN over the same SIO2MAN, and the SDK's own
 *  mcman.irx would need the SDK's sio2man instead. Every model's ROM has these three, and libmc
 *  detects the older MCSERV protocol itself. It lacks rename and reports "unformatted" through
 *  mcGetInfo's result rather than its format flag; nothing here needs more.
 *
 *  All file I/O goes through libmc rather than stdio on "mc0:" paths: the latter reaches the
 *  card through the ROM FILEIO on the host: boot and through fileXio on the USB boot, each with
 *  its own quirks, and can't tell a missing card from a missing file. libmc is asynchronous;
 *  every call here is followed by mcSync, which waits for the result.
 *
 *  Every transfer goes through one cache line aligned bounce buffer, since the IOP DMAs
 *  straight into EE memory (see the notes on SIF DMA buffers in iop_boot.cpp).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/memcard.h"
#include "ps2/save/mc_icon.h"
#include "ps2/system/iop_boot.h"

#include <cstring>
#include <libmc.h>

namespace ps2::save {
namespace {

constexpr int kPort = 0; // MEMORY CARD slot 1.
constexpr int kSlot = 0; // No multitap.

// A cluster is the card's allocation unit: two 512-byte pages.
constexpr u32 kClusterBytes = 1024u;

// The save directory holds the two icon files and at most two files per slot.
constexpr int kMaxDirEntries = 48;

constexpr u32 kBounceBytes = 16u * 1024u;

// mcOpen takes the IOP's open flags (io_common.h's FIO_O_*), not newlib's O_* - which differ.
constexpr int kOpenRead   = 0x0001;
constexpr int kOpenWrite  = 0x0002;
constexpr int kOpenCreate = sceMcFileCreateFile; // 0x0200

alignas(64) static u8 s_mcReadWriteBuffer[kBounceBytes];
static sceMcTblGetDir s_dirTable[kMaxDirEntries]; // sceMcTblGetDir is 64-byte aligned by declaration.

// Waits for the libmc call just issued and returns its result.
int Sync()
{
    int command = 0;
    int result  = 0;
    mcSync(MC_WAIT, &command, &result);
    return result;
}

class MemoryCardDevice final : public Device
{
public:
    bool Probe() override
    {
        const State state = Refresh();
        switch (state)
        {
        case State::Ready:        return true;
        case State::DriverFailed: SetError("The memory card driver could not be started."); break;
        case State::NoCard:       SetError("No memory card in MEMORY CARD slot 1."); break;
        case State::NotPs2Card:   SetError("The card in MEMORY CARD slot 1 is not a PS2 memory card."); break;
        case State::Unformatted:  SetError("The memory card in MEMORY CARD slot 1 is not formatted."); break;
        }
        return false;
    }

    const char * StatusText() override
    {
        // As of the last Probe: the menus ask right after listing the slots, which probed.
        switch (m_state)
        {
        case State::Ready:
            std::snprintf(m_status, sizeof(m_status), "Memory card in slot 1: %d KB free", m_freeClusters);
            break;
        case State::DriverFailed: CopyName(m_status, "The memory card driver failed to start"); break;
        case State::NoCard:       CopyName(m_status, "No memory card in MEMORY CARD slot 1"); break;
        case State::NotPs2Card:   CopyName(m_status, "Not a PS2 memory card in slot 1"); break;
        case State::Unformatted:  CopyName(m_status, "The memory card in slot 1 is not formatted"); break;
        }
        return m_status;
    }

    u32 FreeBytes() const override
    {
        return static_cast<u32>(m_freeClusters) * kClusterBytes;
    }

    u32 FileCostBytes(const u32 sizeBytes) const override
    {
        // Whole clusters, plus a directory entry (512 bytes, half a cluster) rounded up.
        return ((sizeBytes + kClusterBytes - 1u) / kClusterBytes) * kClusterBytes + kClusterBytes;
    }

    bool EnsureSaveDir() override
    {
        if (m_saveDirReady)
        {
            return true;
        }

        char dirPath[kMaxNameLen];
        std::snprintf(dirPath, sizeof(dirPath), "/%s", kCardSaveDir);

        mcGetDir(kPort, kSlot, dirPath, 0, 1, s_dirTable);
        if (Sync() != 1)
        {
            mcMkDir(kPort, kSlot, dirPath);
            const int result = Sync();
            if (result < 0)
            {
                SetError("Could not create the save directory on the memory card (%d).", result);
                return false;
            }
            m_listingValid = false;
        }

        // A save directory the PS2 browser can't describe shows up there as corrupted data,
        // and invites the player to delete it: always have the icon files in it, and this
        // build's ones - checked once per card, since the result is cached below.
        if (!EnsureSaveIcons(*this))
        {
            return false;
        }

        m_saveDirReady = true;
        return true;
    }

    int List(DirEntry * outEntries, const int maxEntries) override
    {
        if (!RefreshListing())
        {
            return -1;
        }

        const int count = (m_listingCount < maxEntries) ? m_listingCount : maxEntries;
        std::memcpy(outEntries, m_listing, static_cast<size_t>(count) * sizeof(DirEntry));
        return count;
    }

    bool FileSize(const char * name, u32 & outSizeBytes) override
    {
        if (!RefreshListing())
        {
            return false;
        }

        for (int i = 0; i < m_listingCount; ++i)
        {
            if (std::strcmp(m_listing[i].name, name) == 0)
            {
                outSizeBytes = m_listing[i].sizeBytes;
                return true;
            }
        }
        return false;
    }

    static constexpr FileHandle kOpenHandle = FileHandle(0);

    FileHandle Open(const char * name, const OpenMode mode) override
    {
        PS2_AssertMsg(m_fd < 0, "MemoryCardDevice: Only one file can be opened at a time!");

        const bool forWriting = (mode == OpenMode::Write);

        // The ROM's MCMAN has no truncate: a file is written fresh or not at all.
        if (forWriting && !Delete(name))
        {
            return FileHandle::Invalid;
        }

        mcOpen(kPort, kSlot, PathOf(name), forWriting ? (kOpenWrite | kOpenCreate) : kOpenRead);
        const int fd = Sync();
        if (fd < 0)
        {
            Com_Printf("Memory card: can't open %s (%d).\n", Describe(name), fd);
            return FileHandle::Invalid;
        }

        m_fd = fd;
        m_fdWriting = forWriting;
        if (forWriting)
        {
            m_listingValid = false;
        }
        return kOpenHandle;
    }

    bool Read(const FileHandle handle, void * dest, const u32 sizeBytes) override
    {
        if (handle != kOpenHandle || m_fd < 0)
        {
            return false;
        }

        u8 * out = static_cast<u8 *>(dest);
        for (u32 left = sizeBytes; left != 0;)
        {
            const u32 n = (left < kBounceBytes) ? left : kBounceBytes;
            mcRead(m_fd, s_mcReadWriteBuffer, static_cast<int>(n));
            if (Sync() != static_cast<int>(n))
            {
                return false;
            }

            std::memcpy(out, s_mcReadWriteBuffer, n);
            out  += n;
            left -= n;
        }
        return true;
    }

    bool Write(const FileHandle handle, const void * src, const u32 sizeBytes) override
    {
        if (handle != kOpenHandle || m_fd < 0 || !m_fdWriting)
        {
            return false;
        }

        const u8 * in = static_cast<const u8 *>(src);
        for (u32 left = sizeBytes; left != 0;)
        {
            const u32 n = (left < kBounceBytes) ? left : kBounceBytes;
            std::memcpy(s_mcReadWriteBuffer, in, n);

            mcWrite(m_fd, s_mcReadWriteBuffer, static_cast<int>(n));
            const int written = Sync();
            if (written != static_cast<int>(n))
            {
                Com_Printf("Memory card: write failed (%d of %u bytes).\n", written, n);
                return false;
            }

            in   += n;
            left -= n;
        }
        return true;
    }

    bool Close(const FileHandle handle) override
    {
        if (handle != kOpenHandle || m_fd < 0)
        {
            return false;
        }

        mcClose(m_fd);
        const int result = Sync();
        m_fd = -1;
        return result >= 0;
    }

    bool Delete(const char * name) override
    {
        mcDelete(kPort, kSlot, PathOf(name));
        const int result = Sync();
        m_listingValid = false;
        return result == sceMcResSucceed || result == sceMcResNoEntry;
    }

    const char * Describe(const char * name) override
    {
        std::snprintf(m_describe, sizeof(m_describe), "mc0:/%s/%.*s", kCardSaveDir, kMaxNameLen - 1, name);
        return m_describe;
    }

private:
    enum class State
    {
        Ready,
        DriverFailed,
        NoCard,
        NotPs2Card,
        Unformatted,
    };

    // Loads the drivers on first use. mcInit waits for MCSERV's RPC server to appear and
    // never gives up, so it is only called once all three modules are known to be running.
    bool StartDriver()
    {
        if (!m_driverTried)
        {
            m_driverTried = true;
            m_driverStarted = ps2::sys::LoadRomModuleOnce("rom0:SIO2MAN") &&
                              ps2::sys::LoadRomModuleOnce("rom0:MCMAN")   &&
                              ps2::sys::LoadRomModuleOnce("rom0:MCSERV");

            if (m_driverStarted)
            {
                const int result = mcInit(MC_TYPE_MC);
                if (result < 0)
                {
                    Com_Printf("Memory card: mcInit failed (%d).\n", result);
                    m_driverStarted = false;
                }
            }
        }
        return m_driverStarted;
    }

    State Refresh()
    {
        if (!StartDriver())
        {
            return m_state = State::DriverFailed;
        }

        // The first call after a card goes in reports the change (-1); what it says about the
        // card is only settled on the next one.
        int type = 0;
        int freeClusters = 0;
        int format = 0;
        int result = 0;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            mcGetInfo(kPort, kSlot, &type, &freeClusters, &format);
            result = Sync();
            if (result != sceMcResChangedCard)
            {
                break;
            }
            m_listingValid = false; // Another card: nothing known about the old one holds.
            m_saveDirReady = false;
        }

        if (result <= -10 || type == sceMcTypeNoCard)
        {
            m_listingValid = false;
            m_saveDirReady = false;
            return m_state = State::NoCard;
        }
        if (type != sceMcTypePS2)
        {
            return m_state = State::NotPs2Card;
        }
        if (result == sceMcResNoFormat)
        {
            m_listingValid = false;
            m_saveDirReady = false;
            return m_state = State::Unformatted;
        }

        m_freeClusters = freeClusters;
        return m_state = State::Ready;
    }

    bool RefreshListing()
    {
        if (m_listingValid)
        {
            return true;
        }

        char pattern[kMaxNameLen];
        std::snprintf(pattern, sizeof(pattern), "/%s/*", kCardSaveDir);

        mcGetDir(kPort, kSlot, pattern, 0, kMaxDirEntries, s_dirTable);
        const int result = Sync();

        m_listingCount = 0;
        if (result == sceMcResNoEntry)
        {
            m_listingValid = true; // No save directory yet: no files.
            return true;
        }
        if (result < 0)
        {
            return false;
        }

        for (int i = 0; i < result; ++i)
        {
            const sceMcTblGetDir & found = s_dirTable[i];
            if ((found.AttrFile & MC_ATTR_FILE) == 0)
            {
                continue; // "." and ".."
            }

            DirEntry & entry = m_listing[m_listingCount++];
            std::snprintf(entry.name, sizeof(entry.name), "%.*s",
                          static_cast<int>(sizeof(entry.name) - 1), reinterpret_cast<const char *>(found.EntryName));
            entry.sizeBytes = found.FileSizeByte;
        }

        m_listingValid = true;
        return true;
    }

    const char * PathOf(const char * name)
    {
        std::snprintf(m_path, sizeof(m_path), "/%s/%.*s", kCardSaveDir, kMaxNameLen - 1, name);
        return m_path;
    }

    State m_state         = State::NoCard;
    int   m_freeClusters  = 0;
    bool  m_driverTried   = false;
    bool  m_driverStarted = false;
    bool  m_saveDirReady  = false; // The save directory and its icons are known to be there.
    bool  m_listingValid  = false;
    int   m_listingCount  = 0;
    int   m_fd            = -1;
    bool  m_fdWriting     = false;

    DirEntry m_listing[kMaxDirEntries] = {};

    char m_path[64]     = {};
    char m_describe[64] = {};
    char m_status[64]   = {};
};

static MemoryCardDevice s_memoryCardDevice;

} // namespace

Device & GetMemoryCardDevice()
{
    return s_memoryCardDevice;
}

} // namespace ps2::save
