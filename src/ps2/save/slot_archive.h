#pragma once
/* ================================================================================================
 * File: slot_archive.h
 * Brief: Save slots: the working set written out to a device as a single archive file, and
 *        read back. See save_system.h for where they sit, slot_archive.cpp for the format.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

namespace ps2::save {

// Identifies what the running build writes and can read back: the game's layout fingerprint
// (G_SaveFingerprint) and the engine's (sizes of the server-side save files).
struct ArchiveStamp
{
    u32 gameFingerprint;
    u32 engineFingerprint;
};

enum class SlotState
{
    Empty,
    Valid,
    Corrupt,
    Incompatible,
};

struct SlotInfo
{
    SlotState state;
    char      comment[32]; // The server.ssv comment: time and level name, or "ENTERING <level>".
};

// Longest slot name: its files are "<slot>_a.q2s" and "<slot>_b.q2s", within kMaxNameLen.
constexpr int kMaxSlotNameLen = 24;

// Writes the working set to the slot. Never loses what the slot held before unless the device
// has no room for two copies, and says so on the console when it has to.
bool StoreSlot(Device & device, const char * slot, const char * comment, const ArchiveStamp & stamp);

// Reads the slot into the working set. All of it is read and checked before the working set
// is replaced, so on failure (SetError) the working set is exactly as it was.
bool RestoreSlot(Device & device, const char * slot, const ArchiveStamp & stamp);

// The slots <prefix>0 .. <prefix><count - 1>, from their headers only. False (and all Empty)
// when the device can't be used right now.
bool ListSlots(Device & device, const char * prefix, int count, SlotInfo * outInfo, const ArchiveStamp & stamp);

} // namespace ps2::save
