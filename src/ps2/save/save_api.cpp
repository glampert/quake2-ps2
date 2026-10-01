/* ================================================================================================
 * File: save_api.cpp
 * Brief: The Sys_Save* hooks the engine saves and loads through (declared in q_common.h), and
 *        the choice of save device. See save_system.h for the overview.
 *
 *  Saves go to the memory card in MEMORY CARD slot 1. Running from the emulator's host:
 *  filesystem, the archived ps2_savedevice cvar can send them to plain files under
 *  <gamedir>/save/ instead ("host", the default there) - easier to look at and back up, and
 *  no formatted virtual card needed. The game menu has it as "saves".
 *
 *  config.cfg is kept where the platform keeps settings (Sys_SaveStoreConfig/LoadConfig):
 *  under the emulator, the host:/ file the engine always wrote, which also wins when reading -
 *  it is the one edited by hand while developing - with the card as a copy when saves go there;
 *  on a console, the memory card, never the USB stick, which only stands in when the card has
 *  no config.cfg.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"
#include "ps2/save/working_set.h"
#include "ps2/save/slot_archive.h"
#include "ps2/save/save_device.h"
#include "ps2/save/memcard.h"
#include "ps2/system/iop_boot.h"

#include <cstring>

extern "C" {
// game/g_save.c: the game's save layout fingerprint (see game.h).
unsigned int G_SaveFingerprint(void);
}

namespace ps2::save {
namespace {

static const cvar_t * s_saveDevice = nullptr; // "host" or "mc"

// The config's name in the card's save directory, and the most it is trusted to hold: the
// engine's own config.cfg is a few KB, so anything far bigger is not one it wrote.
constexpr const char * kConfigFile = "config.cfg";
constexpr u32 kMaxConfigBytes = 64u * 1024u;

bool HostFilesAvailable()
{
    return std::strncmp(FS_Gamedir(), "host:", 5) == 0;
}

Device & ActiveDevice()
{
    if (HostFilesAvailable() && s_saveDevice != nullptr && Q_stricmp(s_saveDevice->string, "host") == 0)
    {
        return GetHostDevice();
    }
    return GetMemoryCardDevice();
}

ArchiveStamp CurrentStamp()
{
    // The engine's own save files are written as fixed-size blocks: server.ssv (comment, map
    // command, cvar name/value pairs) and <map>.sv2 (the configstrings, then the area portals).
    const u32 layout[] = {
        32u, static_cast<u32>(MAX_TOKEN_CHARS), static_cast<u32>(MAX_OSPATH), 128u,
        static_cast<u32>(MAX_CONFIGSTRINGS), static_cast<u32>(MAX_QPATH),
        static_cast<u32>(MAX_MAP_AREAPORTALS), static_cast<u32>(sizeof(qboolean)),
    };
    return { G_SaveFingerprint(), Crc32(0, layout, sizeof(layout)) };
}

// <gamedir>/config.cfg - the emulator's host:/ file, written the way the engine always has.
void WriteGameDataConfig(const char * text, const u32 sizeBytes)
{
    char path[MAX_OSPATH];
    std::snprintf(path, sizeof(path), "%s/%s", FS_Gamedir(), kConfigFile);

    std::FILE * const file = std::fopen(path, "w");
    bool written = (file != nullptr) && (std::fwrite(text, 1, sizeBytes, file) == sizeBytes);
    if (file != nullptr)
    {
        written = (std::fclose(file) == 0) && written;
    }

    if (!written)
    {
        Com_Printf("Couldn't write %s.\n", path);
    }
}

// The card's copy. Not worth bothering the player over - this runs on leaving the video menu
// and on quit - so a card that isn't there or has no room just means the console says so.
void WriteCardConfig(const char * text, const u32 sizeBytes)
{
    Device & card = GetMemoryCardDevice();
    if (!card.Probe() || !card.EnsureSaveDir())
    {
        Com_Printf("config.cfg not saved to the memory card.\n");
        return;
    }

    if (FileMatches(card, kConfigFile, text, sizeBytes))
    {
        return; // The card has it already: no write.
    }

    u32 oldSizeBytes = 0;
    const u32 reclaimable = card.FileSize(kConfigFile, oldSizeBytes) ? card.FileCostBytes(oldSizeBytes) : 0u;
    if (card.FreeBytes() + reclaimable < card.FileCostBytes(sizeBytes))
    {
        Com_Printf("No room on the memory card for config.cfg.\n");
        return;
    }

    if (!WriteWholeFile(card, kConfigFile, text, sizeBytes))
    {
        card.Delete(kConfigFile); // Half a config would only be read back as one.
        Com_Printf("Couldn't save config.cfg to the memory card.\n");
        return;
    }
    Com_Printf("config.cfg saved to %s.\n", card.Describe(kConfigFile));
}

// The game data's config.cfg - the host:/ file, or the one on a console's USB stick - through
// the filesystem, as the engine read it. Null if there is none.
char * ReadGameDataConfig(int & outLength)
{
    void * buffer = nullptr;
    const int length = FS_LoadFile(kConfigFile, &buffer);
    if (buffer == nullptr || length <= 0)
    {
        if (buffer != nullptr)
        {
            FS_FreeFile(buffer);
        }
        return nullptr;
    }

    outLength = length;
    return static_cast<char *>(buffer);
}

// The card's config.cfg, in a Z_Malloc buffer like FS_LoadFile's: just the file's bytes, no
// terminating 0 (Cmd_Exec_f copies it off and adds one). Null if there is none.
char * ReadCardConfig(int & outLength)
{
    Device & card = GetMemoryCardDevice();
    u32 sizeBytes = 0;
    if (!card.Probe() || !card.FileSize(kConfigFile, sizeBytes) || sizeBytes == 0 || sizeBytes > kMaxConfigBytes)
    {
        return nullptr;
    }

    char * const text = static_cast<char *>(Z_Malloc(static_cast<int>(sizeBytes)));

    const FileHandle handle = card.Open(kConfigFile, OpenMode::Read);
    const bool read = (handle != FileHandle::Invalid) && card.Read(handle, text, sizeBytes);
    if (handle != FileHandle::Invalid)
    {
        card.Close(handle);
    }

    if (!read)
    {
        Z_Free(text);
        Com_Printf("Couldn't read config.cfg from the memory card.\n");
        return nullptr;
    }

    Com_Printf("config.cfg read from %s.\n", card.Describe(kConfigFile));
    outLength = static_cast<int>(sizeBytes);
    return text;
}

void SaveInfoCommand()
{
    PrintEntries();

    Device & device = ActiveDevice();
    const bool ready = device.Probe();
    Com_Printf("Save device: %s\n", device.StatusText());
    if (!ready)
    {
        return;
    }

    DirEntry files[48];
    const int count = device.List(files, ps2::ArrayLength(files));
    if (count < 0)
    {
        Com_Printf("  (can't list the save directory)\n");
    }
    for (int i = 0; i < count; ++i)
    {
        Com_Printf("  %-32s %7u bytes\n", files[i].name, static_cast<unsigned>(files[i].sizeBytes));
    }
}

} // namespace

void Init()
{
    s_saveDevice = Cvar_Get("ps2_savedevice", "host", CVAR_ARCHIVE);

    // host: runs through the ROM's FILEIO, whose remove() leaves a directory behind unless
    // iop_boot could patch it.
    SetHostDeleteLeavesDirectory(!ps2::sys::FileIoRemovePatched());

    Cmd_AddCommand("ps2_saveinfo", &SaveInfoCommand);
}

} // namespace ps2::save

// ------------------------------------------------------------------------------------------------
// Sys_Save* - the engine's hooks (q_common.h)
// ------------------------------------------------------------------------------------------------

using namespace ps2::save;

extern "C" {

FILE * Sys_SaveOpen(const char * name, const char * mode)
{
    ClearError();
    return OpenEntry(name, mode);
}

qboolean Sys_SaveClose(FILE * f, const qboolean keep)
{
    return CloseEntry(f, keep != 0) ? 1 : 0;
}

qboolean Sys_SaveExists(const char * name)
{
    return EntryExists(name) ? 1 : 0;
}

void Sys_SaveClearCurrent()
{
    ClearEntries();
}

void Sys_SaveAbortStreams()
{
    AbortStreams();
}

qboolean Sys_SaveStoreSlot(const char * slot, const char * comment)
{
    ClearError();
    if (std::strlen(slot) > static_cast<size_t>(kMaxSlotNameLen))
    {
        SetError("Save name too long: '%s'.", slot);
        return 0;
    }
    return StoreSlot(ActiveDevice(), slot, comment, CurrentStamp()) ? 1 : 0;
}

qboolean Sys_SaveRestoreSlot(const char * slot)
{
    ClearError();
    if (std::strlen(slot) > static_cast<size_t>(kMaxSlotNameLen))
    {
        SetError("Save name too long: '%s'.", slot);
        return 0;
    }
    return RestoreSlot(ActiveDevice(), slot, CurrentStamp()) ? 1 : 0;
}

qboolean Sys_SaveListSlots(const char * prefix, const int count, saveslotinfo_t * info)
{
    constexpr int kMaxSlots = 32;
    SlotInfo slots[kMaxSlots];
    const int numSlots = (count < kMaxSlots) ? count : kMaxSlots;

    ClearError();
    const bool ready = ListSlots(ActiveDevice(), prefix, numSlots, slots, CurrentStamp());

    for (int i = 0; i < count; ++i)
    {
        info[i].state      = SAVESLOT_EMPTY;
        info[i].comment[0] = '\0';
        if (i >= numSlots)
        {
            continue;
        }

        switch (slots[i].state)
        {
        case SlotState::Empty:        info[i].state = SAVESLOT_EMPTY;        break;
        case SlotState::Valid:        info[i].state = SAVESLOT_VALID;        break;
        case SlotState::Corrupt:      info[i].state = SAVESLOT_CORRUPT;      break;
        case SlotState::Incompatible: info[i].state = SAVESLOT_INCOMPATIBLE; break;
        }
        CopyName(info[i].comment, slots[i].comment);
    }
    return ready ? 1 : 0;
}

qboolean Sys_SaveTargetIsCard()
{
    return (&ActiveDevice() == &GetMemoryCardDevice()) ? 1 : 0;
}

qboolean Sys_SaveHostFilesAvailable()
{
    return HostFilesAvailable() ? 1 : 0;
}

const char * Sys_SaveDeviceStatus()
{
    return ActiveDevice().StatusText();
}

void Sys_SaveSetError(const char * message)
{
    SetError("%s", message);
}

const char * Sys_SaveLastError()
{
    return LastError();
}

void Sys_SaveStoreConfig(const char * text, const int length)
{
    if (length <= 0)
    {
        return;
    }
    const u32 sizeBytes = static_cast<u32>(length);

    // The emulator's host:/ file, as the engine always wrote it. Never a console's USB stick:
    // on a console the memory card is where settings are kept.
    if (HostFilesAvailable())
    {
        WriteGameDataConfig(text, sizeBytes);
    }

    // The card, whenever saves go there: always on a console, under the emulator when
    // ps2_savedevice says "mc".
    if (&ActiveDevice() == &GetMemoryCardDevice())
    {
        WriteCardConfig(text, sizeBytes);
    }
}

char * Sys_SaveLoadConfig(int * const outLength)
{
    // Under the emulator the host:/ file comes first: it is the one edited by hand while
    // developing. On a console the card's does: it is the player's own, and the USB stick's
    // only stands in for one the card doesn't have. Whatever ps2_savedevice says - this runs
    // before config.cfg has set it.
    char * text = nullptr;
    if (HostFilesAvailable())
    {
        text = ReadGameDataConfig(*outLength);
        if (text == nullptr)
        {
            text = ReadCardConfig(*outLength);
        }
    }
    else
    {
        text = ReadCardConfig(*outLength);
        if (text == nullptr)
        {
            text = ReadGameDataConfig(*outLength);
        }
    }
    return text;
}

} // extern "C"
