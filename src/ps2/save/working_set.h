#pragma once
/* ================================================================================================
 * File: working_set.h
 * Brief: The save game working set: id's save/current/ directory, kept in RAM as one deflated
 *        blob per file. See save_system.h for where it sits.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

#include <cstdio>

namespace ps2::save {

// A slice of a blob's deflated bytes; the payload follows the header in the same allocation.
struct Chunk
{
    Chunk * next;
    u32     used;     // Bytes of payload filled in.
    u32     capacity; // Bytes of payload allocated.

    u8 *       Data()       { return static_cast<u8 *>(static_cast<void *>(this + 1)); }
    const u8 * Data() const { return static_cast<const u8 *>(static_cast<const void *>(this + 1)); }
};

// One file's contents, deflated (raw deflate, no zlib wrapper), as a chain of chunks.
struct Blob
{
    Chunk * head        = nullptr;
    Chunk * tail        = nullptr;
    u32     packedBytes = 0;
    u32     rawBytes    = 0;
    u32     rawCrc      = 0; // Crc32 of the inflated contents.
};

struct Entry
{
    char name[kMaxNameLen];
    Blob blob;
};

// Enough for every level of the longest unit, twice over (.sav + .sv2 each), and the two .ssv.
constexpr int kMaxEntries = 64;

struct EntrySet
{
    Entry entries[kMaxEntries];
    int   count;
};

// Appends bytes to the end of a blob. False, with the blob unchanged, if out of memory.
bool BlobAppend(Blob & blob, const void * data, u32 sizeBytes);

// Frees the chunks and resets the blob to empty.
void BlobFree(Blob & blob);

// Crc32 over the blob's packed bytes, continuing from `crc`.
u32 BlobPackedCrc(const Blob & blob, u32 crc);

// Opens a working set entry as a stdio stream. "wb" deflates what is written into a new blob,
// which replaces the entry of that name on fclose - only if all of it made it, so a failed
// write leaves the previous contents in place. "rb" inflates the entry and fails the read if
// it doesn't check out. Null (and SetError) if the entry is missing or memory ran out.
std::FILE * OpenEntry(const char * name, const char * mode);

// Closes a stream from OpenEntry. For a write stream, `keep` false discards what was written
// and leaves the entry as it was. True if a write was stored, or a read checked out in full.
bool CloseEntry(std::FILE * file, bool keep);

// Closes every stream still open, discarding writes (an error drop unwound past their owners).
void AbortStreams();

bool EntryExists(const char * name);

// Deletes every entry: a new unit, or a map started from the console.
void ClearEntries();

// The entries as they stand, for writing them out to a slot.
const EntrySet & CurrentEntries();

// Swaps a whole new set in (a slot being restored), freeing the old one. `staged` is left empty.
void ReplaceEntries(EntrySet & staged);

// Frees every blob of a set that never made it in.
void FreeEntries(EntrySet & set);

// Prints the entries and their sizes to the console.
void PrintEntries();

} // namespace ps2::save
