/* ================================================================================================
 * File: pkz_pack.cpp
 * Brief: Reading a compressed game pack. See pkz_pack.h, and pkz_format.h for the layout.
 *
 *        A pack keeps a small table per file (where its data is, its sizes, its restart points)
 *        and one descriptor, opened for the life of the game like a pak's handle. Whole files
 *        (FS_LoadFile) inflate straight into the caller's buffer, which holds the whole history
 *        tinfl needs, fed from one 32 KB input buffer all packs share. Streams (FS_FOpenFile)
 *        are fopencookie streams inflating through a 32 KB wrapping dictionary, as the save
 *        game's are (save/working_set.cpp), with a seek the save game's don't need:
 *
 *          - forward: inflate and drop, unless the target is in a later block, where the stream
 *            starts over at that block's restart point instead;
 *          - backward within the last 32 KB inflated: those bytes are still in the dictionary
 *            (the MD2 loader reads a model's id and seeks back to its start);
 *          - any other backward seek: start over at the target's block.
 *
 *        A stream allocates its buffers on its first read or seek, since the sound code opens
 *        files only to see whether they exist.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

// newlib only declares fopencookie for GNU sources, and has to see this before any header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "ps2/fs/pkz_pack.h"
#include "ps2/fs/pkz_format.h"
#include "ps2/debug/io_log.h"
#include "ps2/common.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <miniz.h>
#include <tamtypes.h>
#include <unistd.h>

using ps2::heap::MemTag;
namespace pkz = ps2::pkz;

namespace {

// Input buffer sizes. Big enough that most compressed files are one read - a wall texture
// deflates to under 32 KB - small enough to stay out of the way at a level change's memory peak.
constexpr u32 kReadInputBytes   = 32u * 1024u; // whole-file reads, one buffer for every pack
constexpr u32 kStreamInputBytes = 32u * 1024u; // per stream, or the file's compressed size if less

// At most this much compressed input per tinfl call on a stream. The demo is read a message
// a frame, and a call that refilled the whole 32 KB dictionary at once took ~4 ms of a frame in
// PCSX2 (more on a console). The output can't be capped instead: in its wrapping mode tinfl
// must be offered all of the dictionary up to its end.
constexpr u32 kStreamInflateStepBytes = 2u * 1024u;

constexpr u32 kDictBytes = TINFL_LZ_DICT_SIZE;
constexpr u32 kDictMask  = kDictBytes - 1u;
static_assert((kDictBytes & kDictMask) == 0, "tinfl's wrapping dictionary must be a power of two");

// A descriptor position nobody can ask for, so the next read seeks.
constexpr u32 kUnknownFilePos = 0xFFFFFFFFu;

constexpr u32 RoundUp64(const u32 n)
{
    return (n + 63u) & ~63u;
}

// One file of a pack.
struct Entry
{
    u32 dataOffset;     // Where its data starts in the pack; until 'resolved', its local header's offset.
    u32 compBytes;      // Its size in the pack.
    u32 rawBytes;       // Its size inflated.
    u32 firstRestart;   // Its restart offsets are restarts[firstRestart, + RestartCount(rawBytes, log2BlockBytes)).
    u8  method;         // pkz::kMethodStored or kMethodDeflate.
    u8  log2BlockBytes; // 0: no restart points.
    u8  resolved;       // dataOffset is final: the Q2 extra field vouched for it, or the local header was read.
    u8  unused;
#if PS2_QUAKE_ASSERTS
    u32 crc;            // The zip's CRC-32 of the inflated bytes, checked after reads.
#endif // PS2_QUAKE_ASSERTS
};

u32 RestartCountOf(const Entry & e)
{
    return pkz::RestartCount(e.rawBytes, e.log2BlockBytes);
}

} // namespace

struct ps2_pkz_s
{
    char    path[MAX_OSPATH];
    int     fd;
    int     readerFd;     // The background loader's descriptor on the pack (PS2_PkzLocate), or -1.
    u32     filePos;      // Where fd is: a read that continues from there skips its lseek.
    u32     fileBytes;
    int     numFiles;
    Entry * entries;
    u32 *   restarts;
    u32     entryCapacity;   // What the tables were allocated with (at least 1 each), for Free.
    u32     restartCapacity;
    int     openStreams;  // Streams hold pointers into the tables: the pack outlives them.
    bool    closing;

    // The zip's directory, kept from PS2_PkzOpen until PS2_PkzListFiles reports the names.
    u8 *    dir;
    u32     dirBytes;
    u32 *   dirHeaderOfs; // Each file's central directory header, as an offset into 'dir'.
};

namespace {

// ------------------------------------------------------------------------------------------------
// Drive reads
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_IO_LOG
u32 IoTicks()
{
    return PS2_IoLogTicks();
}
#endif // PS2_QUAKE_IO_LOG

// Reads [offset, offset + bytes) of the pack into dest. Over USB every request is a round trip
// through the IOP's file server, so a read that continues where the last one ended skips the
// lseek - streams and whole-file reads alike are mostly sequential.
bool ReadAt(ps2_pkz_t & pkz, const u32 offset, void * dest, const u32 bytes)
{
#if PS2_QUAKE_IO_LOG
    const u32 startTicks = IoTicks();
#endif // PS2_QUAKE_IO_LOG

    if (pkz.filePos != offset)
    {
        if (lseek(pkz.fd, static_cast<off_t>(offset), SEEK_SET) != static_cast<off_t>(offset))
        {
            pkz.filePos = kUnknownFilePos;
            return false;
        }
        pkz.filePos = offset;
    }

    u8 * to = static_cast<u8 *>(dest);
    u32 left = bytes;
    while (left != 0)
    {
        const ssize_t n = read(pkz.fd, to, left);
        if (n <= 0)
        {
            pkz.filePos = kUnknownFilePos;
            return false;
        }
        to          += n;
        left        -= static_cast<u32>(n);
        pkz.filePos += static_cast<u32>(n);
    }

#if PS2_QUAKE_IO_LOG
    PS2_IoLogStorageRead(bytes, IoTicks() - startTicks);
#endif // PS2_QUAKE_IO_LOG
    return true;
}

// Sets e.dataOffset from its local header, for a file of a zip pakzip didn't write: other tools
// put an extra field there whose size only the local header has.
bool ResolveDataOffset(ps2_pkz_t & pkz, Entry & e)
{
    if (e.resolved)
    {
        return true;
    }

    u8 local[pkz::kLocalHeaderBytes];
    if (!ReadAt(pkz, e.dataOffset, local, sizeof(local)) || pkz::ReadU32(local) != pkz::kLocalHeaderSig)
    {
        return false;
    }

    const u32 dataOffset = e.dataOffset + pkz::kLocalHeaderBytes + pkz::ReadU16(local + pkz::kLocalNameLenOfs) +
                           pkz::ReadU16(local + pkz::kLocalExtraLenOfs);
    if (dataOffset < e.dataOffset || dataOffset > pkz.fileBytes || e.compBytes > pkz.fileBytes - dataOffset)
    {
        return false;
    }

    e.dataOffset = dataOffset;
    e.resolved   = 1;
    return true;
}

#if PS2_QUAKE_ASSERTS
u32 Crc32(const u32 crc, const void * data, const u32 bytes)
{
    return static_cast<u32>(mz_crc32(crc, static_cast<const unsigned char *>(data), bytes));
}
#endif // PS2_QUAKE_ASSERTS

// tinfl_decompress, timed for the I/O log.
tinfl_status Inflate(tinfl_decompressor * inflator, const u8 * in, size_t * inBytes, u8 * outStart,
                     u8 * outNext, size_t * outBytes, const int flags)
{
#if PS2_QUAKE_IO_LOG
    const u32 startTicks = IoTicks();
#endif // PS2_QUAKE_IO_LOG

    const tinfl_status status = tinfl_decompress(inflator, in, inBytes, outStart, outNext, outBytes, static_cast<mz_uint32>(flags));

#if PS2_QUAKE_IO_LOG
    PS2_IoLogInflate(IoTicks() - startTicks);
#endif // PS2_QUAKE_IO_LOG
    return status;
}

// ------------------------------------------------------------------------------------------------
// Opening a pack
// ------------------------------------------------------------------------------------------------

[[noreturn]] void BadPack(const char * path, const char * why)
{
    Sys_Error("%s is not a pack this build can read: %s.", path, why);
    for (;;) {} // Sys_Error halts (it is not marked noreturn).
}

// Finds the end of central directory record: the last 22 bytes, when the zip has no comment (as
// pakzip writes it); otherwise searched for backwards through the comment.
bool FindEndOfCentralDir(ps2_pkz_t & pkz, u8 (&end)[pkz::kEndOfCentralDirBytes])
{
    const u32 lastRecord = pkz.fileBytes - pkz::kEndOfCentralDirBytes;
    if (ReadAt(pkz, lastRecord, end, sizeof(end)) && pkz::ReadU32(end) == pkz::kEndOfCentralDirSig &&
        pkz::ReadU16(end + pkz::kEndCommentLenOfs) == 0)
    {
        return true;
    }

    const u32 tailBytes = (pkz.fileBytes < pkz::kEndOfCentralDirBytes + pkz::kMaxZipCommentBytes)
                              ? pkz.fileBytes : (pkz::kEndOfCentralDirBytes + pkz::kMaxZipCommentBytes);
    u8 * const tail = static_cast<u8 *>(ps2::heap::Alloc(tailBytes, MemTag::Pack));

    bool found = false;
    if (ReadAt(pkz, pkz.fileBytes - tailBytes, tail, tailBytes))
    {
        for (u32 at = tailBytes - pkz::kEndOfCentralDirBytes + 1u; at-- > 0;)
        {
            if (pkz::ReadU32(tail + at) == pkz::kEndOfCentralDirSig &&
                at + pkz::kEndOfCentralDirBytes + pkz::ReadU16(tail + at + pkz::kEndCommentLenOfs) == tailBytes)
            {
                std::memcpy(end, tail + at, sizeof(end));
                found = true;
                break;
            }
        }
    }

    ps2::heap::Free(tail, tailBytes, MemTag::Pack);
    return found;
}

// One central directory header, checked. Returns false for an entry the pack skips (a directory,
// or a name too long for the engine), and halts on anything this reader can't handle.
struct CentralEntry
{
    u32                  headerBytes; // header + name + extra + comment
    const u8 *           name;
    u32                  nameLen;
    pkz::Q2Extra         q2;
    int                  q2Found;
    Entry                entry;
};

bool ParseCentralEntry(const ps2_pkz_t & pkz, const u32 dirOffset, const u8 * dir, const u32 dirBytes, const u32 at,
                       CentralEntry & out)
{
    if (at + pkz::kCentralHeaderBytes > dirBytes || pkz::ReadU32(dir + at) != pkz::kCentralHeaderSig)
    {
        BadPack(pkz.path, "its central directory is damaged");
    }

    const u8 * h = dir + at;
    const u32 nameLen    = pkz::ReadU16(h + pkz::kCentralNameLenOfs);
    const u32 extraLen   = pkz::ReadU16(h + pkz::kCentralExtraLenOfs);
    const u32 commentLen = pkz::ReadU16(h + pkz::kCentralCommentLenOfs);

    out.headerBytes = pkz::kCentralHeaderBytes + nameLen + extraLen + commentLen;
    out.name        = h + pkz::kCentralHeaderBytes;
    out.nameLen     = nameLen;
    if (at + out.headerBytes > dirBytes)
    {
        BadPack(pkz.path, "its central directory is damaged");
    }

    // A directory has no data, and a name the engine's packfile_t can't hold can't be looked up.
    if (nameLen == 0 || out.name[nameLen - 1] == '/')
    {
        return false;
    }
    if (nameLen >= MAX_QPATH)
    {
        Com_Printf("WARNING: %s: skipping '%.*s', a name longer than %d characters.\n",
                   pkz.path, static_cast<int>(nameLen), reinterpret_cast<const char *>(out.name), MAX_QPATH - 1);
        return false;
    }

    const u16 flags    = pkz::ReadU16(h + pkz::kCentralFlagsOfs);
    const u16 method   = pkz::ReadU16(h + pkz::kCentralMethodOfs);
    const u32 compSize = pkz::ReadU32(h + pkz::kCentralCompSizeOfs);
    const u32 rawSize  = pkz::ReadU32(h + pkz::kCentralRawSizeOfs);
    const u32 localOfs = pkz::ReadU32(h + pkz::kCentralLocalHeaderOfs);

    if ((flags & (pkz::kFlagEncrypted | pkz::kFlagStrongCrypto)) != 0)
    {
        BadPack(pkz.path, "it has encrypted files");
    }
    if (method != pkz::kMethodStored && method != pkz::kMethodDeflate)
    {
        BadPack(pkz.path, "it has files compressed with something other than deflate");
    }
    if (compSize == pkz::kZip64Marker || rawSize == pkz::kZip64Marker || localOfs == pkz::kZip64Marker ||
        pkz::ReadU16(h + pkz::kCentralDiskStartOfs) != 0)
    {
        BadPack(pkz.path, "it needs zip64 or spans disks");
    }
    if (rawSize > 0x7FFFFFFFu || (method == pkz::kMethodStored && compSize != rawSize))
    {
        BadPack(pkz.path, "a file's sizes are wrong");
    }

    out.q2Found = pkz::FindQ2Extra(out.name + nameLen, extraLen, rawSize, out.q2);
    if (out.q2Found < 0 || (out.q2Found > 0 && method == pkz::kMethodStored && out.q2.restartCount != 0))
    {
        BadPack(pkz.path, "a file's restart points are damaged");
    }

    Entry & e = out.entry;
    e = {};
    e.compBytes      = compSize;
    e.rawBytes       = rawSize;
    e.method         = static_cast<u8>(method);
    e.log2BlockBytes = (out.q2Found > 0 && method == pkz::kMethodDeflate) ? static_cast<u8>(out.q2.log2BlockBytes) : 0u;
    e.resolved       = (out.q2Found > 0) ? 1u : 0u;
    e.dataOffset     = e.resolved ? (localOfs + pkz::kLocalHeaderBytes + nameLen) : localOfs;
#if PS2_QUAKE_ASSERTS
    e.crc            = pkz::ReadU32(h + pkz::kCentralCrcOfs);
#endif // PS2_QUAKE_ASSERTS

    // Its data has to lie before the central directory. An unresolved file is checked again
    // once its local header gives the real start.
    const u32 dataEnd = e.dataOffset + (e.resolved ? compSize : pkz::kLocalHeaderBytes);
    if (e.dataOffset < localOfs || dataEnd < e.dataOffset || dataEnd > dirOffset)
    {
        BadPack(pkz.path, "a file lies outside the pack");
    }
    return true;
}

void DestroyPack(ps2_pkz_t * pkz)
{
    if (pkz->dir != nullptr)
    {
        ps2::heap::Free(pkz->dir, pkz->dirBytes, MemTag::Pack);
        ps2::heap::Free(pkz->dirHeaderOfs, sizeof(u32) * pkz->entryCapacity, MemTag::Pack);
    }
    ps2::heap::Free(pkz->entries, sizeof(Entry) * pkz->entryCapacity, MemTag::Pack);
    ps2::heap::Free(pkz->restarts, sizeof(u32) * pkz->restartCapacity, MemTag::Pack);
    close(pkz->fd);
    if (pkz->readerFd >= 0)
    {
        close(pkz->readerFd);
    }
    ps2::heap::Free(pkz, sizeof(ps2_pkz_t), MemTag::Pack);
}

// ------------------------------------------------------------------------------------------------
// Whole-file reads
// ------------------------------------------------------------------------------------------------

// The inflater and input buffer every whole-file read uses, made on the first one.
struct ReadWorkspace
{
    alignas(64) u8 input[kReadInputBytes]; // A SIF DMA target: whole cache lines (see ps2-platform.md).
    tinfl_decompressor inflator;
};

static ReadWorkspace * s_readWorkspace = nullptr;

ReadWorkspace & GetReadWorkspace()
{
    if (s_readWorkspace == nullptr)
    {
        s_readWorkspace = static_cast<ReadWorkspace *>(
            ps2::heap::AllocAligned(ps2::heap::MemAlign(64), sizeof(ReadWorkspace), MemTag::Pack));
    }
    return *s_readWorkspace;
}

// Inflates a whole file into dest. dest holds all of it, so it is also all the history tinfl
// needs: no dictionary, and no copy out of one.
bool InflateWhole(ps2_pkz_t & pkz, const Entry & e, u8 * dest)
{
    ReadWorkspace & ws = GetReadWorkspace();
    tinfl_init(&ws.inflator);

    u32 inStart = 0; // the input buffer holds compressed bytes [inStart, inStart + inBytes)
    u32 inBytes = 0;
    u32 inPos   = 0;
    u32 outPos  = 0;

    for (;;)
    {
        if (inPos == inBytes && inStart + inBytes < e.compBytes)
        {
            inStart += inBytes;
            inBytes  = (e.compBytes - inStart < kReadInputBytes) ? (e.compBytes - inStart) : kReadInputBytes;
            inPos    = 0;
            if (!ReadAt(pkz, e.dataOffset + inStart, ws.input, inBytes))
            {
                return false;
            }
        }

        const bool moreInput = (inStart + inBytes < e.compBytes);
        size_t inSize  = inBytes - inPos;
        size_t outSize = e.rawBytes - outPos;

        const tinfl_status status = Inflate(&ws.inflator, ws.input + inPos, &inSize, dest, dest + outPos, &outSize,
                                            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (moreInput ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        inPos  += static_cast<u32>(inSize);
        outPos += static_cast<u32>(outSize);

        if (status == TINFL_STATUS_DONE)
        {
            return outPos == e.rawBytes;
        }
        if (status < TINFL_STATUS_DONE || status == TINFL_STATUS_HAS_MORE_OUTPUT ||
            (status == TINFL_STATUS_NEEDS_MORE_INPUT && !moreInput))
        {
            return false; // Bad data, longer than recorded, or truncated.
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Streams
// ------------------------------------------------------------------------------------------------

#ifdef __LARGE64_FILES
using CookieOffset = _off64_t;
#else
using CookieOffset = off_t;
#endif

struct Stream
{
    ps2_pkz_t *   pkz;
    const Entry * entry;

    // Raw positions: 'position' is the next byte the reader gets. The pending bytes are those
    // after it already inflated, dict[pendingOffset, + pendingBytes) wrapping, so the frontier
    // tinfl writes at - dictOffset - is raw position + pendingBytes. 'runStart' is where the
    // current inflate run began: the dictionary holds nothing from before it.
    u32 position;
    u32 runStart;
    u32 dictOffset;
    u32 pendingOffset;
    u32 pendingBytes;
    bool inflateDone;
    bool failed;

    // The input buffer holds compressed bytes [inStart, inStart + inBytes) of the file; inPos is
    // the next one tinfl takes.
    u32 inStart;
    u32 inBytes;
    u32 inPos;
    u32 inCapacity;

    // One allocation, made on the first read or seek: the inflater, the dictionary, the input.
    u8 *                 buffers;
    u32                  bufferBytes;
    tinfl_decompressor * inflator;
    u8 *                 dict;
    u8 *                 input;

#if PS2_QUAKE_ASSERTS
    // CRC-32 of raw bytes [0, crcPos), while the reads have covered them without a gap. A stream
    // read through from its start (the collision model's checksum pass over a .bsp, a demo) is
    // checked against the zip's CRC when it gets to the end.
    u32  crc;
    u32  crcPos;
    bool crcValid;
#endif // PS2_QUAKE_ASSERTS

    char name[MAX_QPATH];
};

// Marks the stream failed, once, naming the file. Every read and seek after it fails too, and
// FS_Read turns the short read into its ERR_FATAL.
void StreamFail(Stream & s, const char * what)
{
    if (!s.failed)
    {
        Com_Printf("ERROR: %s in %s is damaged (%s).\n", s.name, s.pkz->path, what);
        s.failed = true;
    }
    errno = EIO;
}

// Starts a new inflate run at the start of block 'block' (0: the file's start).
void Restart(Stream & s, const u32 block)
{
    const Entry & e = *s.entry;
    const u32 compOffset = (block == 0) ? 0u : s.pkz->restarts[e.firstRestart + block - 1u];

    tinfl_init(s.inflator);

    // A restart point already in the input buffer needs no read: a file whose compressed bytes
    // fit the buffer is read from the drive once, however often it is rewound.
    if (compOffset >= s.inStart && compOffset < s.inStart + s.inBytes)
    {
        s.inPos = compOffset - s.inStart;
    }
    else
    {
        s.inStart = compOffset;
        s.inBytes = 0;
        s.inPos   = 0;
    }

    s.position      = (block == 0) ? 0u : (block << e.log2BlockBytes);
    s.runStart      = s.position;
    s.dictOffset    = 0;
    s.pendingOffset = 0;
    s.pendingBytes  = 0;
    s.inflateDone   = false;
}

bool EnsureBuffers(Stream & s)
{
    if (s.buffers != nullptr)
    {
        return true;
    }

    const u32 inflatorBytes = RoundUp64(static_cast<u32>(sizeof(tinfl_decompressor)));
    s.inCapacity  = RoundUp64((s.entry->compBytes < kStreamInputBytes) ? s.entry->compBytes : kStreamInputBytes);
    s.bufferBytes = kDictBytes + s.inCapacity + inflatorBytes;
    s.buffers     = static_cast<u8 *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(64), s.bufferBytes, MemTag::Pack));

    // The input buffer first: it is a SIF DMA target and must own whole cache lines.
    s.input    = s.buffers;
    s.dict     = s.buffers + s.inCapacity;
    s.inflator = static_cast<tinfl_decompressor *>(static_cast<void *>(s.dict + kDictBytes));

    s.inStart = 0;
    s.inBytes = 0;
    s.inPos   = 0;
    Restart(s, 0);
    return true;
}

// Inflates the next piece after the frontier into the dictionary, as pending bytes. Only called
// with none pending, so the frontier is the stream's position.
bool InflateMore(Stream & s)
{
    const Entry & e = *s.entry;

    if (s.inflateDone)
    {
        StreamFail(s, "shorter than recorded");
        return false;
    }

    if (s.inPos == s.inBytes && s.inStart + s.inBytes < e.compBytes)
    {
        s.inStart += s.inBytes;
        s.inBytes  = (e.compBytes - s.inStart < s.inCapacity) ? (e.compBytes - s.inStart) : s.inCapacity;
        s.inPos    = 0;
        if (!ReadAt(*s.pkz, e.dataOffset + s.inStart, s.input, s.inBytes))
        {
            StreamFail(s, "read error");
            return false;
        }
    }

    const u32  available = s.inBytes - s.inPos;
    const u32  step      = (available < kStreamInflateStepBytes) ? available : kStreamInflateStepBytes;
    const bool moreInput = (s.inPos + step < s.inBytes) || (s.inStart + s.inBytes < e.compBytes);
    size_t inSize  = step;
    size_t outSize = kDictBytes - s.dictOffset;

    const tinfl_status status = Inflate(s.inflator, s.input + s.inPos, &inSize, s.dict, s.dict + s.dictOffset,
                                        &outSize, moreInput ? TINFL_FLAG_HAS_MORE_INPUT : 0);
    s.inPos += static_cast<u32>(inSize);

    s.pendingOffset = s.dictOffset;
    s.pendingBytes  = static_cast<u32>(outSize);
    s.dictOffset    = (s.dictOffset + static_cast<u32>(outSize)) & kDictMask;

    if (s.pendingBytes > e.rawBytes - s.position)
    {
        StreamFail(s, "longer than recorded");
        return false;
    }
    if (status == TINFL_STATUS_DONE)
    {
        s.inflateDone = true;
        if (s.position + s.pendingBytes != e.rawBytes)
        {
            StreamFail(s, "shorter than recorded");
            return false;
        }
    }
    else if (status < TINFL_STATUS_DONE)
    {
        StreamFail(s, "bad deflate data");
        return false;
    }
    else if (status == TINFL_STATUS_NEEDS_MORE_INPUT && !moreInput && outSize == 0)
    {
        StreamFail(s, "truncated");
        return false;
    }
    return true;
}

// Hands up to 'bytes' pending bytes to 'dest' (or drops them, with dest null), in at most two
// pieces where the dictionary wraps.
u32 TakePending(Stream & s, char * dest, const u32 bytes)
{
    u32 taken = 0;
    while (taken < bytes && s.pendingBytes != 0)
    {
        u32 n = bytes - taken;
        n = (n < s.pendingBytes) ? n : s.pendingBytes;
        n = (n < kDictBytes - s.pendingOffset) ? n : (kDictBytes - s.pendingOffset);

        const u8 * const src = s.dict + s.pendingOffset;
        if (dest != nullptr)
        {
            std::memcpy(dest + taken, src, n);
        }

#if PS2_QUAKE_ASSERTS
        // Extends the CRC with any bytes of this piece past crcPos, as long as there is no gap.
        if (s.crcValid)
        {
            const u32 end = s.position + n;
            if (s.position > s.crcPos || dest == nullptr)
            {
                s.crcValid = (s.position + n <= s.crcPos); // A skip past the covered bytes breaks it.
            }
            else if (end > s.crcPos)
            {
                s.crc    = Crc32(s.crc, src + (s.crcPos - s.position), end - s.crcPos);
                s.crcPos = end;
            }
        }
#endif // PS2_QUAKE_ASSERTS

        s.pendingOffset = (s.pendingOffset + n) & kDictMask;
        s.pendingBytes -= n;
        s.position     += n;
        taken          += n;
    }
    return taken;
}

// Moves forward to 'target' by inflating and dropping.
bool Discard(Stream & s, const u32 target)
{
    while (s.position < target)
    {
        if (s.pendingBytes == 0 && !InflateMore(s))
        {
            return false;
        }
        TakePending(s, nullptr, target - s.position);
    }
    return true;
}

bool SeekTo(Stream & s, const u32 target)
{
    const Entry & e = *s.entry;
    const int log2  = e.log2BlockBytes;
    const u32 lastBlock = RestartCountOf(e);
    const u32 frontier  = s.position + s.pendingBytes;

    const auto BlockOf = [&](const u32 at) { return (log2 == 0) ? 0u : ((at >> log2) < lastBlock ? (at >> log2) : lastBlock); };

    if (target >= s.position)
    {
        // Up to the frontier's block, inflating on is no more work than starting over.
        if (target > frontier && BlockOf(target) > BlockOf(frontier))
        {
            Restart(s, BlockOf(target));
        }
        return Discard(s, target);
    }

    // Backwards, but still in the dictionary: those bytes become pending again.
    if (target >= s.runStart && frontier - target <= kDictBytes)
    {
        s.pendingBytes  = frontier - target;
        s.pendingOffset = (s.dictOffset - s.pendingBytes) & kDictMask;
        s.position      = target;
        return true;
    }

    Restart(s, BlockOf(target));
    return Discard(s, target);
}

ssize_t StreamRead(void * cookie, char * dest, const size_t bytes)
{
    Stream & s = *static_cast<Stream *>(cookie);
    if (s.failed || !EnsureBuffers(s))
    {
        return -1;
    }

    const Entry & e = *s.entry;
    const u32 wanted = (bytes < e.rawBytes - s.position) ? static_cast<u32>(bytes) : (e.rawBytes - s.position);

    u32 copied = 0;
    while (copied < wanted)
    {
        if (s.pendingBytes == 0 && !InflateMore(s))
        {
            return -1;
        }
        copied += TakePending(s, dest + copied, wanted - copied);
    }

#if PS2_QUAKE_ASSERTS
    if (s.crcValid && s.crcPos == e.rawBytes)
    {
        s.crcValid = false; // Checked once.
        if (s.crc != e.crc)
        {
            StreamFail(s, "CRC mismatch");
            return -1;
        }
    }
#endif // PS2_QUAKE_ASSERTS

    return static_cast<ssize_t>(copied);
}

int StreamSeek(void * cookie, CookieOffset * offset, const int whence)
{
    Stream & s = *static_cast<Stream *>(cookie);
    if (s.failed)
    {
        return -1;
    }

    CookieOffset target = *offset;
    if (whence == SEEK_CUR)
    {
        target += s.position;
    }
    else if (whence == SEEK_END)
    {
        target += s.entry->rawBytes;
    }
    else if (whence != SEEK_SET)
    {
        errno = EINVAL;
        return -1;
    }

    // Past the end isn't supported: the loaders check their offsets against the file's length
    // before seeking, so one that gets here has a bug to report.
    if (target < 0 || target > static_cast<CookieOffset>(s.entry->rawBytes))
    {
        errno = EINVAL;
        return -1;
    }

    // ftell asks for the position this way, and must not cost the buffers.
    if (static_cast<u32>(target) != s.position)
    {
        if (!EnsureBuffers(s) || !SeekTo(s, static_cast<u32>(target)))
        {
            return -1;
        }
    }

    *offset = s.position;
    return 0;
}

int StreamClose(void * cookie)
{
    Stream * const s = static_cast<Stream *>(cookie);
    ps2_pkz_t * const pkz = s->pkz;
    const bool failed = s->failed;

    if (s->buffers != nullptr)
    {
        ps2::heap::Free(s->buffers, s->bufferBytes, MemTag::Pack);
    }
    ps2::heap::Free(s, sizeof(Stream), MemTag::Pack);

    pkz->openStreams -= 1;
    if (pkz->closing && pkz->openStreams == 0)
    {
        DestroyPack(pkz);
    }
    return failed ? -1 : 0;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

extern "C" {

ps2_pkz_t * PS2_PkzOpen(const char * path)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return nullptr;
    }

    auto * const pkz = static_cast<ps2_pkz_t *>(ps2::heap::Alloc(sizeof(ps2_pkz_t), MemTag::Pack));
    std::memset(static_cast<void *>(pkz), 0, sizeof(*pkz));
    pkz->fd       = fd;
    pkz->readerFd = -1;

    // Stored files reopen the pack by this name (PS2_PkzOpenStream), so it must not be cut short.
    if (std::snprintf(pkz->path, sizeof(pkz->path), "%s", path) >= static_cast<int>(sizeof(pkz->path)))
    {
        BadPack(path, "its path is too long");
    }

    const off_t fileBytes = lseek(fd, 0, SEEK_END);
    pkz->filePos = kUnknownFilePos;
    if (fileBytes < static_cast<off_t>(pkz::kEndOfCentralDirBytes))
    {
        BadPack(path, "it is too small to be a zip, or over 2 GB");
    }
    pkz->fileBytes = static_cast<u32>(fileBytes);

    // The end of central directory record says where the directory is.
    u8 end[pkz::kEndOfCentralDirBytes];
    if (!FindEndOfCentralDir(*pkz, end))
    {
        BadPack(path, "it has no zip directory");
    }

    const u32 numEntries = pkz::ReadU16(end + pkz::kEndTotalEntriesOfs);
    const u32 dirBytes   = pkz::ReadU32(end + pkz::kEndCentralSizeOfs);
    const u32 dirOffset  = pkz::ReadU32(end + pkz::kEndCentralOfsOfs);

    if (pkz::ReadU16(end + pkz::kEndDiskOfs) != 0 || pkz::ReadU16(end + pkz::kEndCentralDiskOfs) != 0 ||
        pkz::ReadU16(end + pkz::kEndDiskEntriesOfs) != numEntries)
    {
        BadPack(path, "it spans disks");
    }
    if (numEntries == 0xFFFFu || dirOffset == pkz::kZip64Marker || dirBytes > pkz->fileBytes ||
        dirOffset > pkz->fileBytes - dirBytes)
    {
        BadPack(path, "its directory needs zip64 or is damaged");
    }

    // Kept until PS2_PkzListFiles: the names live in it.
    pkz->dir      = static_cast<u8 *>(ps2::heap::Alloc((dirBytes != 0) ? dirBytes : 1u, MemTag::Pack));
    pkz->dirBytes = (dirBytes != 0) ? dirBytes : 1u;
    if (dirBytes != 0 && !ReadAt(*pkz, dirOffset, pkz->dir, dirBytes))
    {
        BadPack(path, "its directory can't be read");
    }

    // First pass: how many files the pack keeps, and how many restart points they have.
    CentralEntry ce;
    u32 numFiles = 0;
    u32 numRestarts = 0;
    u32 at = 0;
    for (u32 i = 0; i < numEntries; ++i)
    {
        if (ParseCentralEntry(*pkz, dirOffset, pkz->dir, dirBytes, at, ce))
        {
            numFiles    += 1;
            numRestarts += RestartCountOf(ce.entry);
        }
        at += ce.headerBytes;
    }

    // The same stop FS_LoadPackFile has for a pak: the directory hash indexes files with a u16.
    if (numFiles > MAX_FILES_IN_PACK)
    {
        Sys_Error("Pack file %s has %u entries; this build supports at most %i.", path, numFiles, MAX_FILES_IN_PACK);
    }

    pkz->numFiles        = static_cast<int>(numFiles);
    pkz->entryCapacity   = (numFiles != 0) ? numFiles : 1u;
    pkz->restartCapacity = (numRestarts != 0) ? numRestarts : 1u;
    pkz->entries         = static_cast<Entry *>(ps2::heap::Alloc(sizeof(Entry) * pkz->entryCapacity, MemTag::Pack));
    pkz->restarts        = static_cast<u32 *>(ps2::heap::Alloc(sizeof(u32) * pkz->restartCapacity, MemTag::Pack));
    pkz->dirHeaderOfs    = static_cast<u32 *>(ps2::heap::Alloc(sizeof(u32) * pkz->entryCapacity, MemTag::Pack));

    // Second pass: the tables.
    u32 file = 0;
    u32 restart = 0;
    at = 0;
    for (u32 i = 0; i < numEntries; ++i)
    {
        if (ParseCentralEntry(*pkz, dirOffset, pkz->dir, dirBytes, at, ce))
        {
            Entry & e = pkz->entries[file];
            e = ce.entry;
            e.firstRestart = restart;

            // Strictly increasing and inside the file, or a seek could land in the wrong place.
            const u32 count = RestartCountOf(e);
            u32 previous = 0;
            for (u32 k = 0; k < count; ++k)
            {
                const u32 offset = pkz::ReadU32(ce.q2.restartOffsets + k * pkz::kRestartOffsetSize);
                if (offset <= previous || offset >= e.compBytes)
                {
                    BadPack(path, "a file's restart points are damaged");
                }
                pkz->restarts[restart++] = offset;
                previous = offset;
            }

            pkz->dirHeaderOfs[file] = at;
            file += 1;
        }
        at += ce.headerBytes;
    }

    if (numFiles == 0)
    {
        Com_Printf("WARNING: %s has no files.\n", path);
    }
    return pkz;
}

int PS2_PkzNumFiles(const ps2_pkz_t * pkz)
{
    return pkz->numFiles;
}

void PS2_PkzListFiles(ps2_pkz_t * pkz, const ps2_pkz_file_fn fn, void * context)
{
    if (pkz->dir == nullptr)
    {
        return;
    }

    for (int i = 0; i < pkz->numFiles; ++i)
    {
        const u8 * const h = pkz->dir + pkz->dirHeaderOfs[i];
        const u32 nameLen = pkz::ReadU16(h + pkz::kCentralNameLenOfs);

        char name[MAX_QPATH];
        std::memcpy(name, h + pkz::kCentralHeaderBytes, nameLen); // < MAX_QPATH, checked at open
        name[nameLen] = '\0';

        fn(context, i, name, static_cast<int>(pkz->entries[i].rawBytes));
    }

    ps2::heap::Free(pkz->dir, pkz->dirBytes, MemTag::Pack);
    ps2::heap::Free(pkz->dirHeaderOfs, sizeof(u32) * pkz->entryCapacity, MemTag::Pack);
    pkz->dir          = nullptr;
    pkz->dirHeaderOfs = nullptr;
}

int PS2_PkzRead(ps2_pkz_t * pkz, const int index, void * dest)
{
    Entry & e = pkz->entries[index];
    if (e.rawBytes == 0)
    {
        return 1;
    }

    if (!ResolveDataOffset(*pkz, e))
    {
        return 0;
    }

    const bool ok = (e.method == pkz::kMethodStored) ? ReadAt(*pkz, e.dataOffset, dest, e.rawBytes)
                                                    : InflateWhole(*pkz, e, static_cast<u8 *>(dest));

#if PS2_QUAKE_ASSERTS
    if (ok && Crc32(0, dest, e.rawBytes) != e.crc)
    {
        return 0;
    }
#endif // PS2_QUAKE_ASSERTS

    return ok ? 1 : 0;
}

FILE * PS2_PkzOpenStream(ps2_pkz_t * pkz, const int index, const char * name)
{
    Entry & e = pkz->entries[index];
    if (!ResolveDataOffset(*pkz, e))
    {
        return nullptr;
    }

    // A stored file is a stream on the pack like a pak file's, with the same file-sized stdio
    // buffer FS_FOpenFile gives those (FS_SetStreamBuffer).
    if (e.method == pkz::kMethodStored)
    {
        FILE * const file = std::fopen(pkz->path, "rb");
        if (file == nullptr)
        {
            return nullptr;
        }
        std::setvbuf(file, nullptr, _IOFBF, (e.rawBytes > 0 && e.rawBytes < 0x10000u) ? e.rawBytes : 0x10000u);
        if (std::fseek(file, static_cast<long>(e.dataOffset), SEEK_SET) != 0)
        {
            std::fclose(file);
            return nullptr;
        }
        return file;
    }

    auto * const s = static_cast<Stream *>(ps2::heap::Alloc(sizeof(Stream), MemTag::Pack));
    std::memset(static_cast<void *>(s), 0, sizeof(*s));
    s->pkz   = pkz;
    s->entry = &e;
    std::snprintf(s->name, sizeof(s->name), "%s", name);
#if PS2_QUAKE_ASSERTS
    s->crcValid = true;
#endif // PS2_QUAKE_ASSERTS

    cookie_io_functions_t io = {};
    io.read  = &StreamRead;
    io.seek  = &StreamSeek;
    io.close = &StreamClose;

    FILE * const file = fopencookie(s, "rb", io);
    if (file == nullptr)
    {
        ps2::heap::Free(s, sizeof(Stream), MemTag::Pack);
        return nullptr;
    }

    // Unbuffered: the stream buffers already, and newlib then reads straight into the caller's
    // memory, one call per fread.
    std::setvbuf(file, nullptr, _IONBF, 0);
    pkz->openStreams += 1;

#if PS2_QUAKE_IO_LOG
    PS2_IoLogCompressedStream(file);
#endif // PS2_QUAKE_IO_LOG
    return file;
}

int PS2_PkzLocate(ps2_pkz_t * pkz, const int index, int * fd, unsigned * offset, unsigned * compBytes,
                  unsigned * rawBytes, int * deflated)
{
    Entry & e = pkz->entries[index];
    if (!ResolveDataOffset(*pkz, e))
    {
        return 0;
    }
    if (pkz->readerFd < 0)
    {
        pkz->readerFd = open(pkz->path, O_RDONLY);
        if (pkz->readerFd < 0)
        {
            return -1;
        }
    }
    *fd        = pkz->readerFd;
    *offset    = e.dataOffset;
    *compBytes = e.compBytes;
    *rawBytes  = e.rawBytes;
    *deflated  = (e.method == pkz::kMethodDeflate) ? 1 : 0;
    return 1;
}

void PS2_PkzClose(ps2_pkz_t * pkz)
{
    pkz->closing = true;
    if (pkz->openStreams == 0)
    {
        DestroyPack(pkz);
    }
}

} // extern "C"
