/* ================================================================================================
 * File: save_api.cpp
 * Brief: The Sys_Save* hooks the engine saves, loads and keeps its settings through (declared in
 *        q_common.h), and the choice of save device. See save_system.h for the overview.
 *
 *  Saves go to the memory card in MEMORY CARD slot 1, or, when the archived ps2_savedevice cvar
 *  says so, to the drive the game booted from: "host" for the emulator's host: filesystem, "usb"
 *  for a USB drive, "hdd" for the hard disk. A value naming a drive the game didn't boot from
 *  means the card, so a config.cfg carried from one to another still saves somewhere. Every
 *  device holds the same slot archives (slot_archive.cpp), under the same save notice
 *  (save_notice.h). The Game menu has it as "save device".
 *
 *  config.cfg lives with the saves. On a console, the boot drive's own baseq2/config.cfg is taken
 *  when it says saves go to that drive: the player keeps everything there, and the card isn't
 *  touched. Otherwise the card's is, and the drive's only stands in for one the card hasn't got.
 *  It is written to the save device, and a switch from the drive back to the card rewrites the
 *  drive's once more, so that it no longer names itself. Under the emulator the host file comes
 *  first and is always written, whatever the save device: it is the one edited by hand while
 *  developing, and the card holds a copy when saves go there.
 *
 *  A save or a load that fails says why in a message box, with the way to the other device; the
 *  load and save menus say so over their slots when the device can't be used.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"
#include "ps2/save/working_set.h"
#include "ps2/save/slot_archive.h"
#include "ps2/save/memcard.h"
#include "ps2/save/drive.h"
#include "ps2/save/save_notice.h"
#include "ps2/system/iop_boot.h"

extern "C" {
    #include "client/qmenu.h" // Menu_DrawString, Menu_DrawStringDark

    // game/g_save.c: the game's save layout fingerprint (see game.h).
    unsigned int G_SaveFingerprint(void);

    // menu.c's text box, which no engine header declares.
    void M_DrawTextBox(int x, int y, int width, int lines);
}

#include <algorithm>
#include <cstring>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;

// The config's name, in the card's save directory and in the game directory, and the most it is
// trusted to hold: the engine's own config.cfg is a few KB, so anything far bigger is not one it
// wrote.
constexpr const char * kConfigFile = "config.cfg";
constexpr u32 kMaxConfigBytes = 64u * 1024u;

static const cvar_t * s_saveDevice = nullptr; // "mc", or the boot drive's "host", "usb" or "hdd"

// Why the load and save menus' last listing (Sys_SaveListSlots) found the save device unusable:
// empty when it could be used.
static char s_menuProblem[160] = {};

bool HostFilesAvailable()
{
    return std::strncmp(FS_Gamedir(), "host:", 5) == 0;
}

Device & ActiveDevice()
{
    Device & drive = GetDriveDevice();
    const bool toDrive = (s_saveDevice != nullptr) && Q_stricmp(s_saveDevice->string, DeviceValue(drive.Kind())) == 0;
    return toDrive ? drive : GetMemoryCardDevice();
}

bool SavesToCard()
{
    return &ActiveDevice() == &GetMemoryCardDevice();
}

// The device the Game menu's save device row switches to from `device`.
Device & OtherDevice(const Device & device)
{
    return (&device == &GetMemoryCardDevice()) ? GetDriveDevice() : GetMemoryCardDevice();
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

// The save notice for one access, unless one is up already: a save's goes up before the level is
// written out (Sys_SaveNoticeBegin), and the slot's store comes under it.
class NoticeScope final
{
public:
    NoticeScope(Device & device, const SaveAccess access)
        : m_own(!SaveNoticeShown())
        , m_up(BeginSaveNotice(device, access))
    {
    }

    ~NoticeScope()
    {
        if (m_own && m_up)
        {
            EndSaveNotice();
        }
    }

    NoticeScope(const NoticeScope &) = delete;
    NoticeScope & operator=(const NoticeScope &) = delete;

    // Whether the device may be read and written: false if it can't be used (SetError says why).
    bool Up() const { return m_up; }

private:
    const bool m_own;
    const bool m_up;
};

// ------------------------------------------------------------------------------------------------
// Telling the player
// ------------------------------------------------------------------------------------------------

// The menus' boxes hold lines of at most this many characters, broken between words, as the
// message boxes' (M_Popup) do: the 320 columns the menus centre are 40 characters, and a box's
// border takes one on each side.
constexpr int kMaxLineChars = 38;

// Appends `text` to `out`, a string in a buffer of `outSize`, as lines of at most kMaxLineChars
// characters, each ended by a '\n'.
void AppendWrapped(char * out, const size_t outSize, const char * text)
{
    for (const char * line = text; *line != '\0';)
    {
        while (*line == ' ')
        {
            ++line;
        }

        // The most whole words that fit; a word too long for a line on its own is cut.
        int length = 0;
        int fits   = 0;
        while (line[length] != '\0' && length < kMaxLineChars)
        {
            ++length;
            if (line[length] == '\0' || line[length] == ' ')
            {
                fits = length;
            }
        }
        fits = (fits != 0) ? fits : length;

        const size_t used = std::strlen(out);
        if (fits != 0 && used < outSize)
        {
            std::snprintf(out + used, outSize - used, "%.*s\n", fits, line);
        }
        line += fits;
    }
}

// What went wrong, as the device said; a device that refused without saying why is the rare case
// of a notice that couldn't be drawn.
const char * Reason()
{
    return (LastError()[0] != '\0') ? LastError() : "The save device can't be used right now.";
}

// How to carry on with the other device.
const char * SwitchAdvice()
{
    static char s_advice[96];
    std::snprintf(s_advice, sizeof(s_advice), "To use the %s instead, change save device in the Game menu.",
                  DeviceName(OtherDevice(ActiveDevice()).Kind()));
    return s_advice;
}

// ------------------------------------------------------------------------------------------------
// config.cfg in general
// ------------------------------------------------------------------------------------------------

// `sizeBytes` of text in a block of sizeBytes + 1, with a 0 after it; null if out of memory.
// FreeText gives it back.
char * DuplicateText(const char * text, const u32 sizeBytes)
{
    char * const copy = static_cast<char *>(ps2::heap::TryAlloc(sizeBytes + 1u, MemTag::SaveData));
    if (copy != nullptr)
    {
        std::memcpy(copy, text, sizeBytes);
        copy[sizeBytes] = '\0';
    }
    return copy;
}

void FreeText(char * const text, const u32 sizeBytes)
{
    if (text != nullptr)
    {
        ps2::heap::Free(text, sizeBytes + 1u, MemTag::SaveData);
    }
}

// The text in a Z_Malloc buffer, as FS_LoadFile returns a file: `sizeBytes`, no terminating 0
// (Cmd_Exec_f copies it off and adds one).
char * ZoneCopy(const char * text, const u32 sizeBytes)
{
    char * const copy = static_cast<char *>(Z_Malloc(static_cast<int>(sizeBytes)));
    std::memcpy(copy, text, sizeBytes);
    return copy;
}

// The value config text gives a cvar, from the last line setting it, in `out`; false if no line
// does. Lines as Cvar_WriteVariablesToFile writes them, set <name> "<value>", or as typed by hand,
// without the set or the quotation marks.
bool ConfigValue(const char * text, const char * name, char * out, const size_t outSize)
{
    const auto isSpace = [](const char c) { return c == ' ' || c == '\t' || c == '\r'; };
    const size_t nameLength = std::strlen(name);
    bool found = false;

    for (const char * line = text; *line != '\0';)
    {
        const char * const newline = std::strchr(line, '\n');
        const char * const lineEnd = (newline != nullptr) ? newline : line + std::strlen(line);

        const char * p = line;
        while (p < lineEnd && isSpace(*p))
        {
            ++p;
        }
        if (lineEnd - p > 4 && Q_strncasecmp(p, "set", 3) == 0 && isSpace(p[3]))
        {
            p += 4;
            while (p < lineEnd && isSpace(*p))
            {
                ++p;
            }
        }

        if (static_cast<size_t>(lineEnd - p) > nameLength && std::strncmp(p, name, nameLength) == 0 &&
            isSpace(p[nameLength]))
        {
            p += nameLength;
            while (p < lineEnd && isSpace(*p))
            {
                ++p;
            }

            const char * valueEnd = p;
            if (p < lineEnd && *p == '"')
            {
                ++p;
                valueEnd = p;
                while (valueEnd < lineEnd && *valueEnd != '"')
                {
                    ++valueEnd;
                }
            }
            else
            {
                while (valueEnd < lineEnd && !isSpace(*valueEnd))
                {
                    ++valueEnd;
                }
            }
            std::snprintf(out, outSize, "%.*s", static_cast<int>(valueEnd - p), p);
            found = true;
        }

        line = (newline != nullptr) ? newline + 1 : lineEnd;
    }
    return found;
}

// ------------------------------------------------------------------------------------------------
// config.cfg on the memory card
// ------------------------------------------------------------------------------------------------

// What the card's config.cfg holds, as far as this run knows. The card is read for it once, by
// the boot's "exec config.cfg", and the copy follows every write. A later exec, and a config
// written unchanged, as by a quit with no setting changed since, are answered from it: each
// would otherwise go to the card under the save notice for nothing. A probe that finds another
// card in the slot, or none, makes it stale (Device::Generation).
enum class CardConfig : u8
{
    Unknown, // Not read or written yet, or written and failed.
    Absent,  // The card has none, or none this game would have written (empty, or far too big).
    Present, // The card holds s_cardConfigText.
};

static CardConfig s_cardConfig           = CardConfig::Unknown;
static char *     s_cardConfigText       = nullptr; // DuplicateText: s_cardConfigBytes, then a 0
static u32        s_cardConfigBytes      = 0;
static u32        s_cardConfigGeneration = 0;

// Whether the copy describes the card in the slot, as of the last probe.
bool CardConfigCurrent()
{
    return s_cardConfig != CardConfig::Unknown && s_cardConfigGeneration == GetMemoryCardDevice().Generation();
}

// Records what the card holds now: `text` (taken over: a block from DuplicateText), or nothing
// for Absent and Unknown. A text that couldn't be copied leaves it unknown.
void KeepCardConfig(const CardConfig state, char * const text, const u32 sizeBytes)
{
    FreeText(s_cardConfigText, s_cardConfigBytes);
    s_cardConfig           = (state == CardConfig::Present && text == nullptr) ? CardConfig::Unknown : state;
    s_cardConfigText       = text;
    s_cardConfigBytes      = (text != nullptr) ? sizeBytes : 0u;
    s_cardConfigGeneration = GetMemoryCardDevice().Generation();
}

// Reads the card's config.cfg into the copy, under the save notice: the directory too, to find
// whether there is one, as every read of the card's data goes under it. A card that isn't there,
// or can't be read, leaves the copy unknown.
void ReadCardConfigIntoCopy()
{
    Device & card = GetMemoryCardDevice();
    const NoticeScope notice(card, SaveAccess::Loading);
    if (!notice.Up())
    {
        return;
    }

    u32 sizeBytes = 0;
    if (!card.FileSize(kConfigFile, sizeBytes) || sizeBytes == 0 || sizeBytes > kMaxConfigBytes)
    {
        KeepCardConfig(CardConfig::Absent, nullptr, 0);
        return;
    }

    char * const text = static_cast<char *>(ps2::heap::TryAlloc(sizeBytes + 1u, MemTag::SaveData));
    const FileHandle handle = (text != nullptr) ? card.Open(kConfigFile, OpenMode::Read) : FileHandle::Invalid;
    const bool read = (handle != FileHandle::Invalid) && card.Read(handle, text, sizeBytes);
    if (handle != FileHandle::Invalid)
    {
        card.Close(handle);
    }

    if (!read)
    {
        FreeText(text, sizeBytes);
        Com_Printf("Couldn't read config.cfg from the memory card.\n");
        return;
    }

    text[sizeBytes] = '\0';
    KeepCardConfig(CardConfig::Present, text, sizeBytes);
    Com_Printf("config.cfg read from %s.\n", card.Describe(kConfigFile));
}

// The card's config.cfg as Sys_SaveLoadConfig returns it, in a Z_Malloc buffer of `outLength`;
// null if it has none or can't be read.
char * CardConfigCopy(int & outLength)
{
    if (!CardConfigCurrent())
    {
        ReadCardConfigIntoCopy();
    }
    if (s_cardConfig != CardConfig::Present)
    {
        return nullptr;
    }

    outLength = static_cast<int>(s_cardConfigBytes);
    return ZoneCopy(s_cardConfigText, s_cardConfigBytes);
}

// WriteCardConfig's write, under the save notice.
void StoreCardConfig(const char * text, const u32 sizeBytes)
{
    Device & card = GetMemoryCardDevice();
    if (!card.EnsureSaveDir())
    {
        Com_Printf("config.cfg not saved to the memory card.\n");
        return;
    }

    // With the copy stale (another card) or never taken (the emulator reads the host file), the
    // card may hold these very bytes: no write then.
    if (!CardConfigCurrent() && FileMatches(card, kConfigFile, text, sizeBytes))
    {
        KeepCardConfig(CardConfig::Present, DuplicateText(text, sizeBytes), sizeBytes);
        return;
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
        KeepCardConfig(CardConfig::Unknown, nullptr, 0);
        Com_Printf("Couldn't save config.cfg to the memory card.\n");
        return;
    }
    KeepCardConfig(CardConfig::Present, DuplicateText(text, sizeBytes), sizeBytes);
    Com_Printf("config.cfg saved to %s.\n", card.Describe(kConfigFile));
}

// The card's copy. Not worth bothering the player over - this runs on quitting, and on leaving a
// menu page after a change - so a card that isn't there or has no room just means the console
// says so. A config the card holds already isn't written, nor even looked for: the
// card is only touched, under the save notice, for one it doesn't have.
void WriteCardConfig(const char * text, const u32 sizeBytes)
{
    if (CardConfigCurrent() && s_cardConfig == CardConfig::Present && s_cardConfigBytes == sizeBytes &&
        std::memcmp(s_cardConfigText, text, sizeBytes) == 0)
    {
        return;
    }

    const NoticeScope notice(GetMemoryCardDevice(), SaveAccess::Saving);
    if (!notice.Up())
    {
        Com_Printf("config.cfg not saved to the memory card.\n");
        return;
    }
    StoreCardConfig(text, sizeBytes);
}

// ------------------------------------------------------------------------------------------------
// config.cfg on the boot drive
// ------------------------------------------------------------------------------------------------

// What the drive's config.cfg - the game data's, <gamedir>/config.cfg, where id kept it - holds
// as far as this run knows: read by each read of it, and following every write, which the next
// write compares against. Null for none.
static bool   s_driveConfigRead  = false;
static char * s_driveConfigText  = nullptr; // DuplicateText: s_driveConfigBytes, then a 0
static u32    s_driveConfigBytes = 0;

void KeepDriveConfig(char * const text, const u32 sizeBytes)
{
    FreeText(s_driveConfigText, s_driveConfigBytes);
    s_driveConfigRead  = true;
    s_driveConfigText  = text;
    s_driveConfigBytes = (text != nullptr) ? sizeBytes : 0u;
}

// The drive's config.cfg through the filesystem, as the engine read it, in FS_LoadFile's buffer of
// `outLength`; null if there is none. The copy follows it. It is game data, read in milliseconds
// at boot before the save device is known, so it goes without the save notice.
char * ReadDriveConfig(int & outLength)
{
    void * buffer = nullptr;
    const int length = FS_LoadFile(kConfigFile, &buffer);
    if (buffer == nullptr || length <= 0)
    {
        if (buffer != nullptr)
        {
            FS_FreeFile(buffer);
        }
        KeepDriveConfig(nullptr, 0);
        return nullptr;
    }

    const u32 sizeBytes = static_cast<u32>(length);
    KeepDriveConfig(DuplicateText(static_cast<const char *>(buffer), sizeBytes), sizeBytes);
    outLength = length;
    return static_cast<char *>(buffer);
}

// Reads the drive's config.cfg into the copy, the first time only.
void ReadDriveConfigOnce()
{
    if (!s_driveConfigRead)
    {
        int length = 0;
        char * const text = ReadDriveConfig(length);
        if (text != nullptr)
        {
            FS_FreeFile(text);
        }
    }
}

// Whether the drive's config.cfg says saves go to the drive: then it is the player's own, kept
// there, and not the game data's stand-in for a card without one.
bool DriveConfigNamesDrive()
{
    ReadDriveConfigOnce();

    char value[16];
    return s_driveConfigText != nullptr && ConfigValue(s_driveConfigText, "ps2_savedevice", value, sizeof(value)) &&
           Q_stricmp(value, DeviceValue(GetDriveDevice().Kind())) == 0;
}

// WriteDriveConfig's write, through stdio as the engine wrote config.cfg.
void StoreDriveConfig(const char * text, const u32 sizeBytes)
{
    char path[MAX_OSPATH];
    Com_sprintf(path, sizeof(path), "%s/%s", FS_Gamedir(), kConfigFile);

    std::FILE * const file = std::fopen(path, "w");
    bool written = (file != nullptr) && (std::fwrite(text, 1, sizeBytes, file) == sizeBytes);
    if (file != nullptr)
    {
        written = (std::fclose(file) == 0) && written;
    }
    written = (ps2::sys::SyncGameDataDevice() == 0) && written; // The hard disk's PFS keeps it cached otherwise.

    if (!written)
    {
        KeepDriveConfig(nullptr, 0); // Whatever is there now, it isn't these bytes for sure.
        s_driveConfigRead = false;
        Com_Printf("Couldn't write %s.\n", path);
        return;
    }
    KeepDriveConfig(DuplicateText(text, sizeBytes), sizeBytes);
}

// Writes <gamedir>/config.cfg, unless it holds these bytes already; under the save notice when it
// is the save device's copy (`withNotice`).
void WriteDriveConfig(const char * text, const u32 sizeBytes, const bool withNotice)
{
    ReadDriveConfigOnce();
    if (s_driveConfigText != nullptr && s_driveConfigBytes == sizeBytes &&
        std::memcmp(s_driveConfigText, text, sizeBytes) == 0)
    {
        return;
    }

    if (!withNotice)
    {
        StoreDriveConfig(text, sizeBytes);
        return;
    }

    Device & drive = GetDriveDevice();
    const NoticeScope notice(drive, SaveAccess::Saving);
    if (!notice.Up())
    {
        Com_Printf("config.cfg not saved to the %s.\n", DeviceName(drive.Kind()));
        return;
    }
    StoreDriveConfig(text, sizeBytes);
}

// Where a console takes config.cfg from, decided at its first read, the boot's, and kept: a later
// "exec config.cfg" reads the same one.
enum class ConfigSource : u8
{
    Undecided,
    Drive, // The drive's own names the drive as the save device.
    Card,  // The card's, and the drive's only if the card has none.
};

static ConfigSource s_configSource = ConfigSource::Undecided;

bool ConfigFromDrive()
{
    if (s_configSource == ConfigSource::Undecided)
    {
        s_configSource = DriveConfigNamesDrive() ? ConfigSource::Drive : ConfigSource::Card;
    }
    return s_configSource == ConfigSource::Drive;
}

// config.cfg's text, from where this run takes it, as Sys_SaveLoadConfig returns it; null if
// there is none anywhere. Whatever ps2_savedevice says now: the boot's read comes before
// config.cfg has set it.
char * ReadConfig(int & outLength)
{
    // Under the emulator the host file comes first: it is the one edited by hand while developing.
    if (HostFilesAvailable())
    {
        char * const text = ReadDriveConfig(outLength);
        return (text != nullptr) ? text : CardConfigCopy(outLength);
    }

    if (ConfigFromDrive())
    {
        return ReadDriveConfig(outLength);
    }
    char * const text = CardConfigCopy(outLength);
    return (text != nullptr) ? text : ReadDriveConfig(outLength);
}

// ------------------------------------------------------------------------------------------------
// ps2_saveinfo
// ------------------------------------------------------------------------------------------------

void SaveInfoCommand()
{
    PrintEntries();

    Device & device = ActiveDevice();
    Device & other  = OtherDevice(device);
    Com_Printf("Save devices here: %s (ps2_savedevice \"%s\"), %s (\"%s\").\n", DeviceName(device.Kind()),
               DeviceValue(device.Kind()), DeviceName(other.Kind()), DeviceValue(other.Kind()));

    // The listing is a read of the device, under the save notice like any other.
    ClearError();
    const NoticeScope notice(device, SaveAccess::Loading);
    Com_Printf("Save device: %s\n", device.StatusText());
    if (!notice.Up())
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
    s_saveDevice = Cvar_Get("ps2_savedevice", "mc", CVAR_ARCHIVE);
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

    // An error drop unwound out of a save with its notice up (SV_Savegame_f's), past the end.
    EndSaveNotice();
}

qboolean Sys_SaveNoticeBegin(const qboolean saving)
{
    ClearError();
    return BeginSaveNotice(ActiveDevice(), saving ? SaveAccess::Saving : SaveAccess::Loading) ? 1 : 0;
}

void Sys_SaveNoticeEnd()
{
    EndSaveNotice();
}

qboolean Sys_SaveStoreSlot(const char * slot, const char * comment)
{
    ClearError();
    if (std::strlen(slot) > static_cast<size_t>(kMaxSlotNameLen))
    {
        SetError("Save name too long: '%s'.", slot);
        return 0;
    }
    Device & device = ActiveDevice();
    const NoticeScope notice(device, SaveAccess::Saving);
    return (notice.Up() && StoreSlot(device, slot, comment, CurrentStamp())) ? 1 : 0;
}

qboolean Sys_SaveRestoreSlot(const char * slot)
{
    ClearError();
    if (std::strlen(slot) > static_cast<size_t>(kMaxSlotNameLen))
    {
        SetError("Save name too long: '%s'.", slot);
        return 0;
    }
    Device & device = ActiveDevice();
    const NoticeScope notice(device, SaveAccess::Loading);
    return (notice.Up() && RestoreSlot(device, slot, CurrentStamp())) ? 1 : 0;
}

qboolean Sys_SaveListSlots(const char * prefix, const int count, saveslotinfo_t * info)
{
    constexpr int kMaxSlots = 32;
    SlotInfo slots[kMaxSlots];
    const int numSlots = (count < kMaxSlots) ? count : kMaxSlots;

    // Looked at afresh, as the menus open: a card put in since the last look counts.
    ClearError();
    Device & device = ActiveDevice();
    bool ready = false;
    {
        // The menus list every slot from its header as they open, under one notice.
        const NoticeScope notice(device, SaveAccess::Loading);
        ready = notice.Up() && ListSlots(device, prefix, numSlots, slots, CurrentStamp());
    }
    CopyName(s_menuProblem, ready ? "" : Reason());

    for (int i = 0; i < count; ++i)
    {
        info[i].state      = SAVESLOT_EMPTY;
        info[i].comment[0] = '\0';
        if (!ready || i >= numSlots)
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

qboolean Sys_SaveMenuProblem()
{
    return (s_menuProblem[0] != '\0') ? 1 : 0;
}

void Sys_SaveMenuDrawProblem()
{
    if (s_menuProblem[0] == '\0')
    {
        return;
    }

    // Why, in white, as the message boxes' text is, then how to carry on in the other colour.
    char reason[256] = {};
    char advice[256] = {};
    AppendWrapped(reason, sizeof(reason), s_menuProblem);
    AppendWrapped(advice, sizeof(advice), SwitchAdvice());

    const char * const texts[2] = { reason, advice };
    int numLines = 1; // The gap between the two.
    int longest  = 0;
    for (const char * text : texts)
    {
        for (const char * line = text; *line != '\0'; ++numLines)
        {
            const char * const end = std::strchr(line, '\n'); // AppendWrapped ends every line with one
            if (end == nullptr)
            {
                break;
            }
            longest = std::max(longest, static_cast<int>(end - line));
            line = end + 1;
        }
    }

    // Laid out in the 320x240 space M_DrawTextBox centres on the screen, as the message boxes are:
    // as wide as the longest line with a space either side, at most the space's width, over the
    // middle of the slots.
    constexpr int kGlyph = 8;
    const int chars = std::min(longest + 2, kMaxLineChars);
    const int boxX  = (320 - ((chars + 2) * kGlyph)) / 2;
    const int boxY  = (240 - ((numLines + 2) * kGlyph)) / 2;
    M_DrawTextBox(boxX, boxY, chars, numLines);

    const int originX = (viddef.width - 320) / 2;
    const int originY = (viddef.height - 240) / 2;
    int y = originY + boxY + kGlyph;
    for (int i = 0; i < 2; ++i)
    {
        for (const char * line = texts[i]; *line != '\0'; y += kGlyph)
        {
            const char * const end = std::strchr(line, '\n');
            if (end == nullptr)
            {
                break;
            }
            char text[kMaxLineChars + 1];
            std::snprintf(text, sizeof(text), "%.*s", std::min(static_cast<int>(end - line), kMaxLineChars), line);

            const int x = originX + boxX + kGlyph + (((chars - static_cast<int>(std::strlen(text))) * kGlyph) / 2);
            if (i == 0)
            {
                Menu_DrawString(x, y, text);
            }
            else
            {
                Menu_DrawStringDark(x, y, text);
            }
            line = end + 1;
        }
        y += kGlyph; // The gap.
    }
}

qboolean Sys_SaveTargetIsCard()
{
    return SavesToCard() ? 1 : 0;
}

const char * Sys_SaveDeviceName()
{
    return DeviceName(ActiveDevice().Kind());
}

const char * Sys_SaveDriveName()
{
    return DeviceName(GetDriveDevice().Kind());
}

void Sys_SaveUseCard(const qboolean card)
{
    Cvar_Set("ps2_savedevice", card ? DeviceValue(DeviceKind::MemoryCard) : DeviceValue(GetDriveDevice().Kind()));
}

const char * Sys_SaveDeviceStatus()
{
    return ActiveDevice().StatusText();
}

const char * Sys_SaveSwitchAdvice()
{
    return SwitchAdvice();
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

    if (HostFilesAvailable())
    {
        // The emulator's host file, always - under the save notice when host files are where
        // saves go - and the card's copy when the card is.
        WriteDriveConfig(text, sizeBytes, /*withNotice=*/!SavesToCard());
        if (SavesToCard())
        {
            WriteCardConfig(text, sizeBytes);
        }
    }
    else if (!SavesToCard())
    {
        WriteDriveConfig(text, sizeBytes, /*withNotice=*/true);
    }
    else
    {
        WriteCardConfig(text, sizeBytes);

        // A drive config.cfg still naming the drive would be taken over the card's at the next
        // boot: rewritten once, it names the card.
        if (DriveConfigNamesDrive())
        {
            WriteDriveConfig(text, sizeBytes, /*withNotice=*/true);
        }
    }
}

char * Sys_SaveLoadConfig(int * const outLength)
{
    return ReadConfig(*outLength);
}

} // extern "C"
