/* ================================================================================================
 * File: slot_archive.cpp
 * Brief: Save slots - the working set written to a device as one archive file. See slot_archive.h.
 *
 *  An archive is the header, then the entry table, then each entry's deflated bytes exactly as
 *  the working set holds them (no recompression, so writing one out is only I/O):
 *
 *      ArchiveHeader                 magic, versions, sequence, the menu comment, CRCs
 *      ArchiveEntry[numEntries]      name, raw and packed size, CRC of the inflated contents
 *      packed bytes, entry by entry
 *
 *  The header's CRC covers the header, the payload CRC everything after it, and the file must
 *  be exactly as long as the header says: a copy that was cut short, whose sectors went bad or
 *  that some other build wrote is recognised as such before any of it reaches the game.
 *
 *  Neither a memory card (on its ROM driver) nor the host: file device can rename a file, so a
 *  slot can't be replaced by writing a temporary and renaming it. Instead it has two files,
 *  <slot>_a.q2s and <slot>_b.q2s: a save goes to the one not holding the newest good copy,
 *  numbered one higher, and only once it is complete is the other deleted. A reader takes the
 *  good copy with the highest number. If the device is interrupted mid-write, the previous save
 *  is still there.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/slot_archive.h"
#include "ps2/save/working_set.h"

#include <cstddef>
#include <cstring>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;

constexpr u32 kArchiveMagic   = 0x53503251u; // "Q2PS"
constexpr u32 kArchiveVersion = 1;

struct ArchiveHeader
{
    u32  magic;
    u32  version;
    u32  headerBytes;       // sizeof(ArchiveHeader)
    u32  sequence;          // Which of the slot's two copies is newer: the higher.
    u32  gameFingerprint;   // ArchiveStamp of the build that wrote it.
    u32  engineFingerprint;
    u32  numEntries;
    u32  payloadBytes;      // Entry table + packed bytes.
    u32  payloadCrc;
    char comment[32];
    u32  reserved[6];
    u32  headerCrc;         // Crc32 of everything above.
};
static_assert(sizeof(ArchiveHeader) == 96);

struct ArchiveEntry
{
    char name[kMaxNameLen];
    u32  rawBytes;
    u32  packedBytes;
    u32  rawCrc;
    u32  reserved;
};
static_assert(sizeof(ArchiveEntry) == 48);

// Moves packed bytes between the device and the blobs. Cache line aligned, since a memory
// card read is a DMA; the card device bounces through its own buffer regardless.
constexpr u32 kIoBufferBytes = 8u * 1024u;
alignas(64) static u8 s_ioBuffer[kIoBufferBytes];

static ArchiveEntry s_entryTable[kMaxEntries];

// One of a slot's two files, as found on the device.
struct Copy
{
    char file[kMaxNameLen];
    bool present;
    bool valid; // The header checks out and the file is as long as it says.
    u32  fileBytes;
    ArchiveHeader header;
};

inline u32 HeaderCrc(const ArchiveHeader & header)
{
    return Crc32(0, &header, offsetof(ArchiveHeader, headerCrc));
}

inline u32 ToKb(const u32 bytes)
{
    return (bytes + 1023u) / 1024u;
}

bool ReadHeader(Device & device, const char * file, ArchiveHeader & outHeader)
{
    const FileHandle handle = device.Open(file, OpenMode::Read);
    if (handle == FileHandle::Invalid)
    {
        return false;
    }

    const bool ok = device.Read(handle, &outHeader, sizeof(outHeader));
    device.Close(handle);

    return ok && outHeader.magic == kArchiveMagic &&
           outHeader.headerBytes == sizeof(ArchiveHeader) &&
           outHeader.headerCrc == HeaderCrc(outHeader);
}

void LoadCopies(Device & device, const char * slot, Copy (&copies)[2])
{
    for (int i = 0; i < 2; ++i)
    {
        Copy & copy = copies[i];
        std::snprintf(copy.file, sizeof(copy.file), "%s_%c.q2s", slot, (i == 0) ? 'a' : 'b');

        copy.fileBytes = 0;
        copy.present   = device.FileSize(copy.file, copy.fileBytes);
        copy.valid     = copy.present && ReadHeader(device, copy.file, copy.header) &&
                         copy.fileBytes == sizeof(ArchiveHeader) + copy.header.payloadBytes;
    }
}

// The valid copies, newest first; returns how many there are.
int NewestFirst(Copy (&copies)[2], Copy * (&outOrder)[2])
{
    int count = 0;
    for (Copy & copy : copies)
    {
        if (copy.valid)
        {
            outOrder[count++] = &copy;
        }
    }

    if (count == 2 && outOrder[1]->header.sequence > outOrder[0]->header.sequence)
    {
        Copy * const newer = outOrder[1];
        outOrder[1] = outOrder[0];
        outOrder[0] = newer;
    }
    return count;
}

bool Compatible(const ArchiveHeader & header, const ArchiveStamp & stamp)
{
    return header.version == kArchiveVersion &&
           header.gameFingerprint == stamp.gameFingerprint &&
           header.engineFingerprint == stamp.engineFingerprint;
}

bool WriteArchive(Device & device, const char * file, const ArchiveHeader & header, const EntrySet & set)
{
    const FileHandle handle = device.Open(file, OpenMode::Write);
    if (handle == FileHandle::Invalid)
    {
        SetError("Could not write %s.", device.Describe(file));
        return false;
    }

    bool ok = device.Write(handle, &header, sizeof(header)) &&
              device.Write(handle, s_entryTable, static_cast<u32>(set.count) * sizeof(ArchiveEntry));

    for (int i = 0; ok && i < set.count; ++i)
    {
        for (const Chunk * chunk = set.entries[i].blob.head; ok && chunk != nullptr; chunk = chunk->next)
        {
            ok = device.Write(handle, chunk->Data(), chunk->used);
        }
    }

    ok = device.Close(handle) && ok;
    if (!ok)
    {
        SetError("Could not write %s.", device.Describe(file));
    }
    return ok;
}

enum class ReadResult
{
    Ok,
    Damaged,
    OutOfMemory,
};

// Reads a copy's entries into `staged`, checking everything on the way.
ReadResult ReadArchive(Device & device, const Copy & copy, EntrySet & staged)
{
    const ArchiveHeader & header = copy.header;
    staged.count = 0;

    if (header.numEntries == 0 || header.numEntries > static_cast<u32>(kMaxEntries))
    {
        return ReadResult::Damaged;
    }

    const u32 tableBytes = header.numEntries * sizeof(ArchiveEntry);
    if (tableBytes > header.payloadBytes)
    {
        return ReadResult::Damaged;
    }

    const FileHandle handle = device.Open(copy.file, OpenMode::Read);
    if (handle == FileHandle::Invalid)
    {
        return ReadResult::Damaged;
    }

    ArchiveHeader again;
    bool ok = device.Read(handle, &again, sizeof(again)) && std::memcmp(&again, &header, sizeof(header)) == 0 &&
              device.Read(handle, s_entryTable, tableBytes);
    bool outOfMemory = false;

    u32 crc = ok ? Crc32(0, s_entryTable, tableBytes) : 0u;
    u32 bytesLeft = header.payloadBytes - tableBytes;

    for (u32 i = 0; ok && i < header.numEntries; ++i)
    {
        const ArchiveEntry & archived = s_entryTable[i];
        if (archived.name[0] == '\0' || std::memchr(archived.name, '\0', sizeof(archived.name)) == nullptr ||
            archived.packedBytes > bytesLeft)
        {
            ok = false;
            break;
        }
        bytesLeft -= archived.packedBytes;

        Entry & entry = staged.entries[staged.count++];
        CopyName(entry.name, archived.name);
        entry.blob = Blob{};

        for (u32 left = archived.packedBytes; ok && left != 0;)
        {
            const u32 n = (left < kIoBufferBytes) ? left : kIoBufferBytes;
            ok = device.Read(handle, s_ioBuffer, n);
            if (ok)
            {
                crc = Crc32(crc, s_ioBuffer, n);
                ok = BlobAppend(entry.blob, s_ioBuffer, n);
                outOfMemory = !ok;
            }
            left -= n;
        }

        entry.blob.rawBytes = archived.rawBytes;
        entry.blob.rawCrc   = archived.rawCrc;
    }

    device.Close(handle);

    ok = ok && bytesLeft == 0 && crc == header.payloadCrc;
    if (!ok)
    {
        FreeEntries(staged);
        return outOfMemory ? ReadResult::OutOfMemory : ReadResult::Damaged;
    }
    return ReadResult::Ok;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

bool StoreSlot(Device & device, const char * slot, const char * comment, const ArchiveStamp & stamp)
{
    const EntrySet & set = CurrentEntries();
    if (set.count == 0)
    {
        SetError("There is nothing to save.");
        return false;
    }

    // The save directory may cost room of its own (and a card its icon), so look at the free
    // space once it exists.
    if (!device.Probe() || !device.EnsureSaveDir() || !device.Probe())
    {
        return false;
    }

    Copy copies[2];
    LoadCopies(device, slot, copies);

    Copy * order[2] = {};
    const int numValid = NewestFirst(copies, order);
    const Copy * const newest = (numValid != 0) ? order[0] : nullptr;

    Copy & target = (newest  == &copies[0]) ? copies[1] : copies[0];
    Copy & other  = (&target == &copies[0]) ? copies[1] : copies[0];

    // Header and entry table, and the CRC of everything after the header.
    ArchiveHeader header = {};
    header.magic             = kArchiveMagic;
    header.version           = kArchiveVersion;
    header.headerBytes       = sizeof(ArchiveHeader);
    header.sequence          = (newest != nullptr) ? newest->header.sequence + 1u : 1u;
    header.gameFingerprint   = stamp.gameFingerprint;
    header.engineFingerprint = stamp.engineFingerprint;
    header.numEntries        = static_cast<u32>(set.count);
    std::snprintf(header.comment, sizeof(header.comment), "%.*s", static_cast<int>(sizeof(header.comment) - 1), comment);

    u32 payloadBytes = static_cast<u32>(set.count) * sizeof(ArchiveEntry);
    for (int i = 0; i < set.count; ++i)
    {
        const Entry & entry = set.entries[i];
        ArchiveEntry & archived = s_entryTable[i];

        archived = ArchiveEntry{};
        CopyName(archived.name, entry.name);
        archived.rawBytes    = entry.blob.rawBytes;
        archived.packedBytes = entry.blob.packedBytes;
        archived.rawCrc      = entry.blob.rawCrc;

        payloadBytes += entry.blob.packedBytes;
    }

    u32 payloadCrc = Crc32(0, s_entryTable, static_cast<u32>(set.count) * sizeof(ArchiveEntry));
    for (int i = 0; i < set.count; ++i)
    {
        payloadCrc = BlobPackedCrc(set.entries[i].blob, payloadCrc);
    }

    header.payloadBytes = payloadBytes;
    header.payloadCrc   = payloadCrc;
    header.headerCrc    = HeaderCrc(header);

    // Room for the new copy, counting the one it replaces (the older or a bad one). When there
    // is only room once the newest copy is gone too, it is overwritten in place instead.
    const u32 neededBytes = device.FileCostBytes(sizeof(ArchiveHeader) + payloadBytes);
    const u32 availableBytes = device.FreeBytes() + (target.present ? device.FileCostBytes(target.fileBytes) : 0u);
    bool inPlace = false;

    if (availableBytes < neededBytes)
    {
        if (other.present && availableBytes + device.FileCostBytes(other.fileBytes) >= neededBytes)
        {
            inPlace = true;
            Com_Printf("Save: no room for a second copy of '%s' - replacing the old one first.\n", slot);
        }
        else
        {
            const u32 freeBytes = availableBytes + (other.present ? device.FileCostBytes(other.fileBytes) : 0u);
            SetError("Not enough free space: %u KB needed, %u KB free.",
                     static_cast<unsigned>(ToKb(neededBytes)), static_cast<unsigned>(ToKb(freeBytes)));
            return false;
        }
    }

    if ((inPlace && other.present && !device.Delete(other.file)) ||
        (target.present && !device.Delete(target.file)))
    {
        SetError("Could not replace the old save.");
        return false;
    }

    if (!WriteArchive(device, target.file, header, set))
    {
        device.Delete(target.file); // Don't leave half a copy behind, even though it would be ignored.
        return false;
    }

    if (!inPlace && other.present && !device.Delete(other.file))
    {
        Com_Printf("Save: couldn't delete the previous copy, %s; the new one is used regardless.\n",
                   device.Describe(other.file));
    }
    return true;
}

bool RestoreSlot(Device & device, const char * slot, const ArchiveStamp & stamp)
{
    if (!device.Probe())
    {
        return false;
    }

    Copy copies[2];
    LoadCopies(device, slot, copies);

    Copy * order[2] = {};
    const int numValid = NewestFirst(copies, order);
    if (numValid == 0)
    {
        SetError((copies[0].present || copies[1].present) ? "The save game is damaged." : "There is no saved game in that slot.");
        return false;
    }

    EntrySet * const staged = static_cast<EntrySet *>(ps2::heap::TryAlloc(sizeof(EntrySet), MemTag::SaveData));
    if (staged == nullptr)
    {
        SetError("Not enough memory to load the game.");
        return false;
    }
    staged->count = 0;

    bool restored = false;
    for (int i = 0; i < numValid && !restored; ++i)
    {
        const Copy & copy = *order[i];
        if (!Compatible(copy.header, stamp))
        {
            SetError("This save was made by a different version of the game, and can't be loaded.");
            break;
        }

        const ReadResult result = ReadArchive(device, copy, *staged);
        if (result == ReadResult::Ok)
        {
            ReplaceEntries(*staged);
            restored = true;
        }
        else if (result == ReadResult::OutOfMemory)
        {
            SetError("Not enough memory to load the game.");
            break;
        }
        else
        {
            Com_Printf("Save: %s is damaged%s.\n", device.Describe(copy.file),
                       (i + 1 < numValid) ? " - trying the older copy" : "");
            SetError("The save game is damaged.");
        }
    }

    ps2::heap::Free(staged, sizeof(EntrySet), MemTag::SaveData);
    return restored;
}

bool ListSlots(Device & device, const char * prefix, const int count, SlotInfo * outInfo, const ArchiveStamp & stamp)
{
    for (int i = 0; i < count; ++i)
    {
        outInfo[i].state      = SlotState::Empty;
        outInfo[i].comment[0] = '\0';
    }

    if (!device.Probe())
    {
        return false;
    }

    for (int i = 0; i < count; ++i)
    {
        char slot[kMaxNameLen];
        std::snprintf(slot, sizeof(slot), "%s%d", prefix, i);

        Copy copies[2];
        LoadCopies(device, slot, copies);

        Copy * order[2] = {};
        if (NewestFirst(copies, order) != 0)
        {
            const ArchiveHeader & header = order[0]->header;
            outInfo[i].state = Compatible(header, stamp) ? SlotState::Valid : SlotState::Incompatible;
            std::snprintf(outInfo[i].comment, sizeof(outInfo[i].comment), "%.*s",
                          static_cast<int>(sizeof(header.comment) - 1), header.comment);
        }
        else if (copies[0].present || copies[1].present)
        {
            outInfo[i].state = SlotState::Corrupt;
        }
    }
    return true;
}

} // namespace ps2::save
