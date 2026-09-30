/* ================================================================================================
 * File: working_set.cpp
 * Brief: The save game working set - id's save/current/ directory in RAM - and the stdio
 *        streams the engine and game read and write it through. See working_set.h.
 *
 *  Why RAM: the server writes the level being left on every level change, and reads it back
 *  when the player returns within the same unit, so this is hot storage; a memory card is too
 *  slow for it and a disc boot would have nowhere to put it. Deflated, a level's state is a
 *  few tens of KB - the ~130 KB of configstrings and ~900 bytes per entity it is made of are
 *  mostly zeros.
 *
 *  The streams come from fopencookie, so fwrite/fread/fclose work on them unchanged. Writes
 *  are deflated as they arrive (miniz tdefl, with an output callback appending to the blob's
 *  chunks), reads inflate through a 32 KB wrapping dictionary (tinfl). Only the compressor
 *  state is large (164 KB), and it only exists while a write stream is open.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

// newlib only declares fopencookie for GNU sources, and has to see this before any header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "ps2/save/working_set.h"

#include <cstring>
#include <miniz.h>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;

// Chunks start small, since server.ssv and game.ssv deflate to a few hundred bytes, and grow
// to this for the level files.
constexpr u32 kFirstChunkBytes = 1024u;
constexpr u32 kMaxChunkBytes   = 16u * 1024u;

// A chunk left with more slack than this when its blob is complete is copied to a tight one.
constexpr u32 kMaxChunkSlackBytes = 512u;

// Deflate level (0-10). The data is mostly runs of zeros, which even the fastest levels
// collapse; this one trades a little speed for a noticeably smaller level file.
constexpr int kDeflateLevel = 3;

// stdio buffering on the streams, so the game's many small fwrites reach tdefl in batches.
constexpr size_t kStreamBufferBytes = 8u * 1024u;

static EntrySet s_entries;    // The working set.
static int s_openReaders = 0; // Read streams hold pointers into the blobs; nothing may free them meanwhile.

// Every stream open right now, so CloseEntry can reach its state and AbortStreams can find
// what an error drop left behind. The server never has more than one open at a time.
struct OpenStream
{
    std::FILE * file;
    void *      cookie;
    bool        forWriting;
};

constexpr int kMaxOpenStreams = 4;
static OpenStream s_openStreams[kMaxOpenStreams] = {};

bool RegisterStream(std::FILE * file, void * cookie, const bool forWriting)
{
    for (OpenStream & open : s_openStreams)
    {
        if (open.file == nullptr)
        {
            open = { file, cookie, forWriting };
            return true;
        }
    }
    return false;
}

void UnregisterStream(const void * cookie)
{
    for (OpenStream & open : s_openStreams)
    {
        if (open.cookie == cookie)
        {
            open = {};
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Blob / chunk helpers
// ------------------------------------------------------------------------------------------------

Chunk * AllocChunk(const u32 capacity)
{
    void * mem = ps2::heap::TryAlloc(sizeof(Chunk) + capacity, MemTag::SaveData);
    if (mem == nullptr)
    {
        return nullptr;
    }

    Chunk * chunk  = static_cast<Chunk *>(mem);
    chunk->next     = nullptr;
    chunk->used     = 0;
    chunk->capacity = capacity;
    return chunk;
}

void FreeChunk(Chunk * chunk)
{
    ps2::heap::Free(chunk, sizeof(Chunk) + chunk->capacity, MemTag::SaveData);
}

// Swaps the blob's last chunk for one just big enough, once nothing more will be appended.
void TrimBlob(Blob & blob)
{
    Chunk * const tail = blob.tail;
    if (tail == nullptr || (tail->capacity - tail->used) <= kMaxChunkSlackBytes)
    {
        return;
    }

    Chunk * const tight = AllocChunk(tail->used);
    if (tight == nullptr)
    {
        return; // Keep the slack; it's only memory.
    }

    std::memcpy(tight->Data(), tail->Data(), tail->used);
    tight->used = tail->used;

    if (blob.head == tail)
    {
        blob.head = tight;
    }
    else
    {
        Chunk * prev = blob.head;
        while (prev->next != tail)
        {
            prev = prev->next;
        }
        prev->next = tight;
    }

    blob.tail = tight;
    FreeChunk(tail);
}

Entry * FindEntry(EntrySet & set, const char * name)
{
    for (int i = 0; i < set.count; ++i)
    {
        if (Q_stricmp(set.entries[i].name, name) == 0)
        {
            return &set.entries[i];
        }
    }
    return nullptr;
}

// Puts `blob` in the set under `name`, replacing (and freeing) an entry of that name.
bool StoreEntry(EntrySet & set, const char * name, Blob & blob)
{
    Entry * entry = FindEntry(set, name);
    if (entry != nullptr)
    {
        PS2_AssertMsg(s_openReaders == 0, "Replacing a save entry that is being read");
        BlobFree(entry->blob);
    }
    else
    {
        if (set.count == kMaxEntries)
        {
            SetError("Too many levels in the save game (%d files).", kMaxEntries);
            return false;
        }
        entry = &set.entries[set.count++];
        CopyName(entry->name, name);
    }

    entry->blob = blob;
    blob = Blob{};
    return true;
}

// ------------------------------------------------------------------------------------------------
// Write stream: deflates into a new blob, stored under its name on a successful close
// ------------------------------------------------------------------------------------------------

struct WriteStream
{
    tdefl_compressor * compressor;
    Blob blob;
    u32  crc;
    bool failed;
    bool discard; // CloseEntry(keep = false): drop the blob without it counting as an error.
    char name[kMaxNameLen];
};

mz_bool PutDeflated(const void * data, int sizeBytes, void * user)
{
    WriteStream & stream = *static_cast<WriteStream *>(user);
    return BlobAppend(stream.blob, data, static_cast<u32>(sizeBytes)) ? MZ_TRUE : MZ_FALSE;
}

ssize_t WriteStreamWrite(void * cookie, const char * data, size_t sizeBytes)
{
    WriteStream & stream = *static_cast<WriteStream *>(cookie);
    if (stream.failed)
    {
        return -1;
    }
    if (stream.discard)
    {
        return static_cast<ssize_t>(sizeBytes); // Being thrown away: fclose's final flush.
    }

    stream.crc = Crc32(stream.crc, data, sizeBytes);
    stream.blob.rawBytes += static_cast<u32>(sizeBytes);

    if (tdefl_compress_buffer(stream.compressor, data, sizeBytes, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY)
    {
        SetError("Out of memory saving '%s'.", stream.name);
        stream.failed = true;
        return -1;
    }
    return static_cast<ssize_t>(sizeBytes);
}

int WriteStreamClose(void * cookie)
{
    WriteStream * const stream = static_cast<WriteStream *>(cookie);
    bool ok = !stream->failed && !stream->discard;

    UnregisterStream(cookie);

    if (ok && tdefl_compress_buffer(stream->compressor, nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE)
    {
        SetError("Out of memory saving '%s'.", stream->name);
        ok = false;
    }

    ps2::heap::Free(stream->compressor, sizeof(tdefl_compressor), MemTag::SaveData);

    if (ok)
    {
        TrimBlob(stream->blob);
        stream->blob.rawCrc = stream->crc;
        ok = StoreEntry(s_entries, stream->name, stream->blob);
    }

    BlobFree(stream->blob); // Empty after a successful StoreEntry.
    ps2::heap::Free(stream, sizeof(WriteStream), MemTag::SaveData);
    return ok ? 0 : -1;
}

std::FILE * OpenWriteStream(const char * name)
{
    WriteStream * const stream = static_cast<WriteStream *>(ps2::heap::TryAlloc(sizeof(WriteStream), MemTag::SaveData));
    tdefl_compressor * const compressor = static_cast<tdefl_compressor *>(ps2::heap::TryAlloc(sizeof(tdefl_compressor), MemTag::SaveData));

    if (stream == nullptr || compressor == nullptr)
    {
        ps2::heap::Free(stream, sizeof(WriteStream), MemTag::SaveData);
        ps2::heap::Free(compressor, sizeof(tdefl_compressor), MemTag::SaveData);
        SetError("Not enough memory to save the game.");
        return nullptr;
    }

    stream->compressor = compressor;
    stream->blob       = Blob{};
    stream->crc        = 0;
    stream->failed     = false;
    stream->discard    = false;
    CopyName(stream->name, name);

    // Raw deflate: the blob carries its own size and CRC.
    const mz_uint flags = tdefl_create_comp_flags_from_zip_params(kDeflateLevel, -15, MZ_DEFAULT_STRATEGY);
    tdefl_init(compressor, &PutDeflated, stream, static_cast<int>(flags));

    cookie_io_functions_t io = {};
    io.write = &WriteStreamWrite;
    io.close = &WriteStreamClose;

    std::FILE * const file = fopencookie(stream, "wb", io);
    if (file == nullptr)
    {
        ps2::heap::Free(compressor, sizeof(tdefl_compressor), MemTag::SaveData);
        ps2::heap::Free(stream, sizeof(WriteStream), MemTag::SaveData);
        SetError("Not enough memory to save the game.");
        return nullptr;
    }

    if (!RegisterStream(file, stream, true))
    {
        stream->discard = true;
        std::fclose(file);
        SetError("Too many save files open.");
        return nullptr;
    }

    std::setvbuf(file, nullptr, _IOFBF, kStreamBufferBytes);
    return file;
}

// ------------------------------------------------------------------------------------------------
// Read stream: inflates an entry's blob
// ------------------------------------------------------------------------------------------------

struct ReadStream
{
    tinfl_decompressor inflator;
    const Chunk * chunk;       // Next deflated input, from chunkOffset on.
    u32  chunkOffset;
    u8 * dict;                 // TINFL_LZ_DICT_SIZE bytes; tinfl's output wraps around it.
    u32  dictOffset;           // Where tinfl writes next.
    u32  pendingOffset;        // Inflated bytes not yet handed to the reader:
    u32  pendingBytes;         //   dict[pendingOffset, pendingOffset + pendingBytes).
    u32  rawBytesLeft;
    u32  crc;
    u32  expectedCrc;
    bool inflateDone;
    bool failed;
    char name[kMaxNameLen];
};

ssize_t ReadStreamFail(ReadStream & stream, const char * what)
{
    if (!stream.failed)
    {
        Com_Printf("Save entry '%s' is damaged (%s).\n", stream.name, what);
        SetError("The save game is damaged.");
        stream.failed = true;
    }
    return -1;
}

ssize_t ReadStreamRead(void * cookie, char * dest, size_t sizeBytes)
{
    ReadStream & stream = *static_cast<ReadStream *>(cookie);
    if (stream.failed)
    {
        return -1;
    }

    size_t copied = 0;
    while (copied < sizeBytes)
    {
        if (stream.pendingBytes != 0)
        {
            const u32 wanted = static_cast<u32>(sizeBytes - copied);
            const u32 n = (stream.pendingBytes < wanted) ? stream.pendingBytes : wanted;
            if (n > stream.rawBytesLeft)
            {
                return ReadStreamFail(stream, "longer than recorded");
            }

            const u8 * const src = stream.dict + stream.pendingOffset;
            std::memcpy(dest + copied, src, n);
            stream.crc = Crc32(stream.crc, src, n);

            stream.pendingOffset += n;
            stream.pendingBytes  -= n;
            stream.rawBytesLeft  -= n;
            copied += n;
            continue;
        }

        if (stream.inflateDone)
        {
            break;
        }

        const u8 * input = nullptr;
        size_t inputBytes = 0;
        if (stream.chunk != nullptr)
        {
            input      = stream.chunk->Data() + stream.chunkOffset;
            inputBytes = stream.chunk->used - stream.chunkOffset;
        }

        const bool moreInput = (stream.chunk != nullptr && stream.chunk->next != nullptr);
        size_t outputBytes = TINFL_LZ_DICT_SIZE - stream.dictOffset;

        const tinfl_status status = tinfl_decompress(&stream.inflator, input, &inputBytes,
                                                     stream.dict, stream.dict + stream.dictOffset, &outputBytes,
                                                     moreInput ? TINFL_FLAG_HAS_MORE_INPUT : 0);

        stream.chunkOffset += static_cast<u32>(inputBytes);
        if (stream.chunk != nullptr && stream.chunkOffset == stream.chunk->used)
        {
            stream.chunk       = stream.chunk->next;
            stream.chunkOffset = 0;
        }

        stream.pendingOffset = stream.dictOffset;
        stream.pendingBytes  = static_cast<u32>(outputBytes);
        stream.dictOffset    = (stream.dictOffset + static_cast<u32>(outputBytes)) & (TINFL_LZ_DICT_SIZE - 1u);

        if (status == TINFL_STATUS_DONE)
        {
            stream.inflateDone = true;
        }
        else if (status < TINFL_STATUS_DONE)
        {
            return ReadStreamFail(stream, "bad deflate data");
        }
        else if (status == TINFL_STATUS_NEEDS_MORE_INPUT && stream.chunk == nullptr && outputBytes == 0)
        {
            return ReadStreamFail(stream, "truncated");
        }
    }

    // The whole entry has been handed over: it must be the size and CRC it was written with.
    if (stream.inflateDone && stream.pendingBytes == 0 &&
        (stream.rawBytesLeft != 0 || stream.crc != stream.expectedCrc))
    {
        return ReadStreamFail(stream, stream.rawBytesLeft != 0 ? "shorter than recorded" : "CRC mismatch");
    }

    return static_cast<ssize_t>(copied);
}

int ReadStreamClose(void * cookie)
{
    ReadStream * const stream = static_cast<ReadStream *>(cookie);
    const bool failed = stream->failed;

    UnregisterStream(cookie);

    ps2::heap::Free(stream->dict, TINFL_LZ_DICT_SIZE, MemTag::SaveData);
    ps2::heap::Free(stream, sizeof(ReadStream), MemTag::SaveData);
    --s_openReaders;

    return failed ? -1 : 0;
}

std::FILE * OpenReadStream(const char * name)
{
    const Entry * const entry = FindEntry(s_entries, name);
    if (entry == nullptr)
    {
        SetError("The save game is missing '%s'.", name);
        return nullptr;
    }

    ReadStream * const stream = static_cast<ReadStream *>(ps2::heap::TryAlloc(sizeof(ReadStream), MemTag::SaveData));
    u8 * const dict = static_cast<u8 *>(ps2::heap::TryAlloc(TINFL_LZ_DICT_SIZE, MemTag::SaveData));

    if (stream == nullptr || dict == nullptr)
    {
        ps2::heap::Free(stream, sizeof(ReadStream), MemTag::SaveData);
        ps2::heap::Free(dict, TINFL_LZ_DICT_SIZE, MemTag::SaveData);
        SetError("Not enough memory to load the game.");
        return nullptr;
    }

    tinfl_init(&stream->inflator);
    stream->chunk         = entry->blob.head;
    stream->chunkOffset   = 0;
    stream->dict          = dict;
    stream->dictOffset    = 0;
    stream->pendingOffset = 0;
    stream->pendingBytes  = 0;
    stream->rawBytesLeft  = entry->blob.rawBytes;
    stream->crc           = 0;
    stream->expectedCrc   = entry->blob.rawCrc;
    stream->inflateDone   = false;
    stream->failed        = false;
    CopyName(stream->name, name);

    cookie_io_functions_t io = {};
    io.read  = &ReadStreamRead;
    io.close = &ReadStreamClose;

    std::FILE * const file = fopencookie(stream, "rb", io);
    if (file == nullptr)
    {
        ps2::heap::Free(dict, TINFL_LZ_DICT_SIZE, MemTag::SaveData);
        ps2::heap::Free(stream, sizeof(ReadStream), MemTag::SaveData);
        SetError("Not enough memory to load the game.");
        return nullptr;
    }

    ++s_openReaders;
    if (!RegisterStream(file, stream, false))
    {
        std::fclose(file);
        SetError("Too many save files open.");
        return nullptr;
    }

    std::setvbuf(file, nullptr, _IOFBF, kStreamBufferBytes);
    return file;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

u32 Crc32(const u32 crc, const void * data, const size_t sizeBytes)
{
    return static_cast<u32>(mz_crc32(crc, static_cast<const unsigned char *>(data), sizeBytes));
}

bool BlobAppend(Blob & blob, const void * data, const u32 sizeBytes)
{
    const u8 * src = static_cast<const u8 *>(data);
    u32 left = sizeBytes;

    // Room in the tail first, then new chunks, each double the last up to the cap. Chunks
    // are only linked in once all of them exist, so a failure leaves the blob as it was.
    Chunk * tail = blob.tail;
    const u32 tailRoom = (tail != nullptr) ? (tail->capacity - tail->used) : 0u;
    const u32 intoTail = (left < tailRoom) ? left : tailRoom;
    left -= intoTail;

    Chunk * newHead = nullptr;
    Chunk * newTail = nullptr;
    u32 nextCapacity = (tail != nullptr) ? tail->capacity : (kFirstChunkBytes / 2u);
    for (u32 remaining = left; remaining != 0;)
    {
        nextCapacity = (nextCapacity * 2u < kMaxChunkBytes) ? nextCapacity * 2u : kMaxChunkBytes;
        Chunk * const chunk = AllocChunk(nextCapacity);
        if (chunk == nullptr)
        {
            while (newHead != nullptr)
            {
                Chunk * const next = newHead->next;
                FreeChunk(newHead);
                newHead = next;
            }
            return false;
        }

        if (newTail != nullptr) { newTail->next = chunk; } else { newHead = chunk; }
        newTail = chunk;
        remaining -= (remaining < nextCapacity) ? remaining : nextCapacity;
    }

    if (intoTail != 0)
    {
        std::memcpy(tail->Data() + tail->used, src, intoTail);
        tail->used += intoTail;
        src += intoTail;
    }

    for (Chunk * chunk = newHead; chunk != nullptr; chunk = chunk->next)
    {
        const u32 n = (left < chunk->capacity) ? left : chunk->capacity;
        std::memcpy(chunk->Data(), src, n);
        chunk->used = n;
        src  += n;
        left -= n;
    }

    if (newHead != nullptr)
    {
        if (tail != nullptr) { tail->next = newHead; } else { blob.head = newHead; }
        blob.tail = newTail;
    }

    blob.packedBytes += sizeBytes;
    return true;
}

void BlobFree(Blob & blob)
{
    for (Chunk * chunk = blob.head; chunk != nullptr;)
    {
        Chunk * const next = chunk->next;
        FreeChunk(chunk);
        chunk = next;
    }
    blob = Blob{};
}

u32 BlobPackedCrc(const Blob & blob, u32 crc)
{
    for (const Chunk * chunk = blob.head; chunk != nullptr; chunk = chunk->next)
    {
        crc = Crc32(crc, chunk->Data(), chunk->used);
    }
    return crc;
}

std::FILE * OpenEntry(const char * name, const char * mode)
{
    if (name == nullptr || name[0] == '\0' || std::strlen(name) >= kMaxNameLen)
    {
        SetError("Bad save file name '%s'.", (name != nullptr) ? name : "");
        return nullptr;
    }

    if (mode[0] == 'w')
    {
        return OpenWriteStream(name);
    }
    return OpenReadStream(name);
}

bool CloseEntry(std::FILE * file, const bool keep)
{
    for (const OpenStream & open : s_openStreams)
    {
        if (open.file == file && open.forWriting)
        {
            WriteStream & stream = *static_cast<WriteStream *>(open.cookie);
            if (!keep)
            {
                stream.discard = true;
            }
            else if (std::ferror(file) && !stream.failed)
            {
                SetError("Could not save '%s'.", stream.name);
                stream.failed = true;
            }
            break;
        }
    }

    // fclose flushes the stdio buffer through the cookie, so a write can still fail here.
    return (std::fclose(file) == 0) && keep;
}

void AbortStreams()
{
    for (const OpenStream & open : s_openStreams)
    {
        if (open.file != nullptr)
        {
            CloseEntry(open.file, false);
        }
    }
}

bool EntryExists(const char * name)
{
    return FindEntry(s_entries, name) != nullptr;
}

void ClearEntries()
{
    FreeEntries(s_entries);
}

const EntrySet & CurrentEntries()
{
    return s_entries;
}

void ReplaceEntries(EntrySet & staged)
{
    FreeEntries(s_entries);

    for (int i = 0; i < staged.count; ++i)
    {
        s_entries.entries[i] = staged.entries[i];
        staged.entries[i].blob = Blob{};
    }
    s_entries.count = staged.count;
    staged.count = 0;
}

void FreeEntries(EntrySet & set)
{
    PS2_AssertMsg(&set != &s_entries || s_openReaders == 0, "Freeing save entries that are being read");

    for (int i = 0; i < set.count; ++i)
    {
        BlobFree(set.entries[i].blob);
    }
    set.count = 0;
}

void PrintEntries()
{
    u32 packedTotal = 0;
    u32 rawTotal    = 0;

    Com_Printf("Save working set: %d entries\n", s_entries.count);
    for (int i = 0; i < s_entries.count; ++i)
    {
        const Entry & entry = s_entries.entries[i];
        Com_Printf("  %-24s %7u -> %6u bytes\n", entry.name,
                   static_cast<unsigned>(entry.blob.rawBytes), static_cast<unsigned>(entry.blob.packedBytes));
        packedTotal += entry.blob.packedBytes;
        rawTotal    += entry.blob.rawBytes;
    }
    Com_Printf("  %-24s %7u -> %6u bytes\n", "total", static_cast<unsigned>(rawTotal), static_cast<unsigned>(packedTotal));
}

} // namespace ps2::save
