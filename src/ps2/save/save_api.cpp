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

} // extern "C"
