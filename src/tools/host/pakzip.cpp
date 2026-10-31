/* ================================================================================================
 * File: pakzip.cpp
 * Brief: Converts id's pakN.pak into pakN.pkz, the compressed pack the game loads in its place
 *        when both are present. ps2/fs/pkz_format.h has the format and the reasons for it.
 *
 *          pakzip [--level N] [--block-kb N] [--min-saving P] [--store-ext LIST] <in.pak> <out.pkz>
 *
 *            --level N       deflate level, 0-10 (default 9). 10 is miniz's slowest search.
 *            --block-kb N    restart point spacing in KB, a power of two from 4 to 16384
 *                            (default 32). Smaller blocks make a seek cheaper and the pack
 *                            bigger.
 *            --min-saving P  store an entry unless deflating it saves at least P percent
 *                            (default 10). Inflating costs the console time; for most sounds
 *                            that time buys back fewer bytes than it costs to decode them.
 *            --store-ext LIST  store every entry whose name ends in one of these extensions,
 *                            comma separated, case ignored (e.g. ".dm2"). The game streams a
 *                            stored entry through its read-ahead ring (fs/readahead_stream.h),
 *                            which needs plain byte ranges; a deflated one it reads on the main
 *                            thread.
 *
 *        Entries keep the pak's order and names. The pack is written to <out>.tmp, read back
 *        and checked: every entry is inflated whole and every block from its own restart point,
 *        and compared with the pak's bytes. Only then does it replace <out>. The game prefers a
 *        pkz to its pak, so a damaged one must never be left where the game would find it.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "../../ps2/fs/pkz_format.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pkz = ps2::pkz;

namespace {

// ------------------------------------------------------------------------------------------------
// id's pak format (common/q_files.h)
// ------------------------------------------------------------------------------------------------

constexpr std::uint32_t kPakIdent        = 0x4B434150u; // "PACK"
constexpr std::uint32_t kPakHeaderBytes  = 12;
constexpr std::uint32_t kPakEntryBytes   = 64;
constexpr std::uint32_t kPakNameBytes    = 56;

struct PakEntry
{
    std::string   name;
    std::uint32_t offset;
    std::uint32_t size;
};

// ------------------------------------------------------------------------------------------------
// Zip output
// ------------------------------------------------------------------------------------------------

// Every entry gets the same timestamp, so converting the same pak twice gives the same bytes:
// 9 December 1997, Quake II's release, in MS-DOS date format. Midnight.
constexpr std::uint16_t kDosDate = ((1997 - 1980) << 9) | (12 << 5) | 9;
constexpr std::uint16_t kDosTime = 0;

struct ZipEntry
{
    std::string                name;
    std::uint32_t              rawBytes;
    std::uint32_t              compBytes;
    std::uint32_t              crc;
    std::uint32_t              localOffset;
    std::uint16_t              method;
    int                        log2BlockBytes; // 0 for a stored entry.
    std::vector<std::uint32_t> restarts;
};

struct Options
{
    int         level          = 9;
    int         log2BlockBytes = pkz::kDefaultLog2BlockBytes;
    int         minSavingPct   = 10;
    std::vector<std::string> storeExts; // lowercase, each with its dot
    const char * inPath        = nullptr;
    const char * outPath       = nullptr;
};

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

bool Fail(const char * format, const char * arg = "")
{
    std::fprintf(stderr, "pakzip: error: ");
    std::fprintf(stderr, format, arg);
    std::fprintf(stderr, "\n");
    return false;
}

bool ReadWholeFile(const char * path, std::vector<std::uint8_t> & out)
{
    std::FILE * file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return Fail("cannot open '%s'", path);
    }

    bool ok = (std::fseek(file, 0, SEEK_END) == 0);
    const long size = ok ? std::ftell(file) : -1;
    ok = ok && size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (ok)
    {
        out.resize(static_cast<size_t>(size));
        ok = (size == 0) || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok ? true : Fail("cannot read '%s'", path);
}

bool WriteWholeFile(const char * path, const std::vector<std::uint8_t> & data)
{
    std::FILE * file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return Fail("cannot create '%s'", path);
    }

    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
    ok = (std::fclose(file) == 0) && ok;
    return ok ? true : Fail("cannot write '%s'", path);
}

void Append(std::vector<std::uint8_t> & out, const void * data, const size_t bytes)
{
    const auto * p = static_cast<const std::uint8_t *>(data);
    out.insert(out.end(), p, p + bytes);
}

void AppendU16(std::vector<std::uint8_t> & out, const std::uint16_t v)
{
    std::uint8_t b[2];
    pkz::WriteU16(b, v);
    Append(out, b, sizeof(b));
}

void AppendU32(std::vector<std::uint8_t> & out, const std::uint32_t v)
{
    std::uint8_t b[4];
    pkz::WriteU32(b, v);
    Append(out, b, sizeof(b));
}

std::uint32_t Crc32(const std::uint8_t * data, const size_t bytes)
{
    return static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, data, bytes));
}

double Megabytes(const std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// ------------------------------------------------------------------------------------------------
// Reading the pak
// ------------------------------------------------------------------------------------------------

bool ParsePak(const std::vector<std::uint8_t> & pak, const char * path, std::vector<PakEntry> & entries)
{
    if (pak.size() < kPakHeaderBytes || pkz::ReadU32(pak.data()) != kPakIdent)
    {
        return Fail("'%s' is not a Quake II pak file", path);
    }

    const std::uint32_t dirOffset = pkz::ReadU32(pak.data() + 4);
    const std::uint32_t dirBytes  = pkz::ReadU32(pak.data() + 8);
    if (dirBytes % kPakEntryBytes != 0 || static_cast<std::uint64_t>(dirOffset) + dirBytes > pak.size())
    {
        return Fail("'%s' has a damaged directory", path);
    }

    const std::uint32_t count = dirBytes / kPakEntryBytes;
    entries.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t * e = pak.data() + dirOffset + i * kPakEntryBytes;
        const char * name = reinterpret_cast<const char *>(e);
        const size_t nameLen = strnlen(name, kPakNameBytes);

        PakEntry entry;
        entry.name   = std::string(name, nameLen);
        entry.offset = pkz::ReadU32(e + kPakNameBytes);
        entry.size   = pkz::ReadU32(e + kPakNameBytes + 4);

        if (nameLen == 0 || nameLen == kPakNameBytes ||
            static_cast<std::uint64_t>(entry.offset) + entry.size > pak.size())
        {
            return Fail("'%s' has a damaged directory entry", path);
        }
        entries.push_back(entry);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Deflating
// ------------------------------------------------------------------------------------------------

mz_bool PutBuf(const void * data, const int bytes, void * user)
{
    Append(*static_cast<std::vector<std::uint8_t> *>(user), data, static_cast<size_t>(bytes));
    return MZ_TRUE;
}

// Deflates one entry, with a full flush closing every block but the last, and records the
// compressed offset each following block starts at.
bool DeflateEntry(tdefl_compressor * compressor, const int flags, const std::uint8_t * data,
                  const std::uint32_t bytes, const int log2BlockBytes,
                  std::vector<std::uint8_t> & out, std::vector<std::uint32_t> & restarts)
{
    out.clear();
    restarts.clear();

    if (tdefl_init(compressor, &PutBuf, &out, flags) != TDEFL_STATUS_OKAY)
    {
        return false;
    }

    const std::uint32_t blockBytes = 1u << log2BlockBytes;
    std::uint32_t at = 0;
    for (;;)
    {
        const std::uint32_t chunk = std::min(blockBytes, bytes - at);
        const bool last = (at + chunk == bytes);

        const tdefl_status status = tdefl_compress_buffer(compressor, data + at, chunk,
                                                          last ? TDEFL_FINISH : TDEFL_FULL_FLUSH);
        if (status != (last ? TDEFL_STATUS_DONE : TDEFL_STATUS_OKAY))
        {
            return false;
        }

        at += chunk;
        if (last)
        {
            return true;
        }

        // The full flush has byte-aligned the stream and emptied the compressor into 'out', so
        // the next block's first byte is the next one written.
        restarts.push_back(static_cast<std::uint32_t>(out.size()));
    }
}

// ------------------------------------------------------------------------------------------------
// Writing the zip
// ------------------------------------------------------------------------------------------------

void AppendLocalHeader(std::vector<std::uint8_t> & zip, const ZipEntry & e)
{
    AppendU32(zip, pkz::kLocalHeaderSig);
    AppendU16(zip, pkz::kZipVersion);
    AppendU16(zip, 0); // flags
    AppendU16(zip, e.method);
    AppendU16(zip, kDosTime);
    AppendU16(zip, kDosDate);
    AppendU32(zip, e.crc);
    AppendU32(zip, e.compBytes);
    AppendU32(zip, e.rawBytes);
    AppendU16(zip, static_cast<std::uint16_t>(e.name.size()));
    AppendU16(zip, 0); // No extra field: the reader relies on it (see pkz_format.h).
    Append(zip, e.name.data(), e.name.size());
}

void AppendCentralHeader(std::vector<std::uint8_t> & zip, const ZipEntry & e)
{
    const std::uint32_t restartBytes = static_cast<std::uint32_t>(e.restarts.size()) * pkz::kRestartOffsetSize;
    const std::uint16_t extraBytes   = static_cast<std::uint16_t>(pkz::kExtraHeaderBytes + pkz::kExtraFixedBytes + restartBytes);

    AppendU32(zip, pkz::kCentralHeaderSig);
    AppendU16(zip, pkz::kZipVersion); // made by: MS-DOS, 2.0
    AppendU16(zip, pkz::kZipVersion); // needed
    AppendU16(zip, 0);                // flags
    AppendU16(zip, e.method);
    AppendU16(zip, kDosTime);
    AppendU16(zip, kDosDate);
    AppendU32(zip, e.crc);
    AppendU32(zip, e.compBytes);
    AppendU32(zip, e.rawBytes);
    AppendU16(zip, static_cast<std::uint16_t>(e.name.size()));
    AppendU16(zip, extraBytes);
    AppendU16(zip, 0); // comment
    AppendU16(zip, 0); // disk number start
    AppendU16(zip, 0); // internal attributes
    AppendU32(zip, 0); // external attributes
    AppendU32(zip, e.localOffset);
    Append(zip, e.name.data(), e.name.size());

    AppendU16(zip, pkz::kExtraId);
    AppendU16(zip, static_cast<std::uint16_t>(extraBytes - pkz::kExtraHeaderBytes));
    zip.push_back(pkz::kExtraVersion);
    zip.push_back(static_cast<std::uint8_t>(e.log2BlockBytes));
    AppendU16(zip, 0); // reserved
    for (const std::uint32_t offset : e.restarts)
    {
        AppendU32(zip, offset);
    }
}

// ------------------------------------------------------------------------------------------------
// Checking the written pack
// ------------------------------------------------------------------------------------------------

// Inflates compressed[0, compBytes) into out[0, outBytes). The output buffer is all the history
// the inflater gets, so a back-reference to anything before it fails: for a block inflated from
// its restart point, that proves the full flush really cut the history there. 'wholeStream'
// says whether this output ends the entry (the stream must end with it) or is one block of it.
bool InflateCheck(const std::uint8_t * compressed, const std::uint32_t compBytes,
                  std::uint8_t * out, const std::uint32_t outBytes, const bool wholeStream)
{
    static tinfl_decompressor inflator; // ~11 KB; not something for the stack.
    tinfl_init(&inflator);

    size_t inSize  = compBytes;
    size_t outSize = outBytes;
    const tinfl_status status = tinfl_decompress(&inflator, compressed, &inSize, out, out, &outSize,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (outSize != outBytes)
    {
        return false;
    }
    if (wholeStream)
    {
        return status == TINFL_STATUS_DONE && inSize == compBytes;
    }
    return status == TINFL_STATUS_DONE || status == TINFL_STATUS_HAS_MORE_OUTPUT;
}

// Reads the pack back the way the game does - end of central directory, central directory, Q2
// extra fields, local headers - and checks every entry against the pak.
bool VerifyPack(const std::vector<std::uint8_t> & zip, const std::vector<std::uint8_t> & pak,
                const std::vector<PakEntry> & pakEntries, std::uint64_t & restartsChecked)
{
    restartsChecked = 0;

    if (zip.size() < pkz::kEndOfCentralDirBytes)
    {
        return Fail("verify: pack too small");
    }

    // pakzip writes no archive comment, so the record ends the file.
    const std::uint8_t * end = zip.data() + zip.size() - pkz::kEndOfCentralDirBytes;
    if (pkz::ReadU32(end) != pkz::kEndOfCentralDirSig)
    {
        return Fail("verify: no end of central directory record");
    }

    const std::uint32_t count     = pkz::ReadU16(end + pkz::kEndTotalEntriesOfs);
    const std::uint32_t dirBytes  = pkz::ReadU32(end + pkz::kEndCentralSizeOfs);
    const std::uint32_t dirOffset = pkz::ReadU32(end + pkz::kEndCentralOfsOfs);
    if (count != pakEntries.size() || static_cast<std::uint64_t>(dirOffset) + dirBytes > zip.size() - pkz::kEndOfCentralDirBytes)
    {
        return Fail("verify: bad end of central directory record");
    }

    std::vector<std::uint8_t> inflated;
    std::uint32_t at = dirOffset;

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const PakEntry & source = pakEntries[i];
        const std::uint8_t * h = zip.data() + at;

        if (at + pkz::kCentralHeaderBytes > dirOffset + dirBytes || pkz::ReadU32(h) != pkz::kCentralHeaderSig)
        {
            return Fail("verify: bad central directory header for '%s'", source.name.c_str());
        }

        const std::uint16_t method    = pkz::ReadU16(h + pkz::kCentralMethodOfs);
        const std::uint32_t crc       = pkz::ReadU32(h + pkz::kCentralCrcOfs);
        const std::uint32_t compBytes = pkz::ReadU32(h + pkz::kCentralCompSizeOfs);
        const std::uint32_t rawBytes  = pkz::ReadU32(h + pkz::kCentralRawSizeOfs);
        const std::uint32_t nameLen   = pkz::ReadU16(h + pkz::kCentralNameLenOfs);
        const std::uint32_t extraLen  = pkz::ReadU16(h + pkz::kCentralExtraLenOfs);
        const std::uint32_t localOfs  = pkz::ReadU32(h + pkz::kCentralLocalHeaderOfs);
        const std::uint8_t * name     = h + pkz::kCentralHeaderBytes;

        at += pkz::kCentralHeaderBytes + nameLen + extraLen + pkz::ReadU16(h + pkz::kCentralCommentLenOfs);

        if (nameLen != source.name.size() || std::memcmp(name, source.name.data(), nameLen) != 0 ||
            rawBytes != source.size)
        {
            return Fail("verify: central directory entry does not match '%s'", source.name.c_str());
        }

        pkz::Q2Extra extra{};
        if (pkz::FindQ2Extra(name + nameLen, extraLen, rawBytes, extra) != 1)
        {
            return Fail("verify: missing or bad Q2 extra field on '%s'", source.name.c_str());
        }

        const std::uint8_t * local = zip.data() + localOfs;
        const std::uint64_t dataOffset = static_cast<std::uint64_t>(localOfs) + pkz::kLocalHeaderBytes + nameLen;
        if (dataOffset + compBytes > dirOffset || pkz::ReadU32(local) != pkz::kLocalHeaderSig ||
            pkz::ReadU16(local + pkz::kLocalNameLenOfs) != nameLen || pkz::ReadU16(local + pkz::kLocalExtraLenOfs) != 0 ||
            std::memcmp(local + pkz::kLocalHeaderBytes, name, nameLen) != 0)
        {
            return Fail("verify: bad local header for '%s'", source.name.c_str());
        }

        const std::uint8_t * data     = zip.data() + dataOffset;
        const std::uint8_t * expected = pak.data() + source.offset;

        if (method == pkz::kMethodStored)
        {
            if (compBytes != rawBytes || extra.restartCount != 0 || std::memcmp(data, expected, rawBytes) != 0)
            {
                return Fail("verify: stored entry '%s' differs from the pak", source.name.c_str());
            }
        }
        else if (method == pkz::kMethodDeflate)
        {
            inflated.assign(rawBytes, 0);
            if (!InflateCheck(data, compBytes, inflated.data(), rawBytes, true) ||
                std::memcmp(inflated.data(), expected, rawBytes) != 0)
            {
                return Fail("verify: '%s' does not inflate back to the pak's bytes", source.name.c_str());
            }

            // Every block on its own, from its restart point, with no history before it.
            const std::uint32_t blockBytes = 1u << extra.log2BlockBytes;
            for (std::uint32_t k = 1; k <= extra.restartCount; ++k)
            {
                const std::uint32_t restart  = pkz::ReadU32(extra.restartOffsets + (k - 1) * pkz::kRestartOffsetSize);
                const std::uint32_t rawStart = k * blockBytes;
                const std::uint32_t outBytes = std::min(blockBytes, rawBytes - rawStart);

                if (restart >= compBytes ||
                    !InflateCheck(data + restart, compBytes - restart, inflated.data(), outBytes, k == extra.restartCount) ||
                    std::memcmp(inflated.data(), expected + rawStart, outBytes) != 0)
                {
                    return Fail("verify: a restart point of '%s' does not decode on its own", source.name.c_str());
                }
                ++restartsChecked;
            }
        }
        else
        {
            return Fail("verify: '%s' has an unexpected method", source.name.c_str());
        }

        if (crc != Crc32(expected, rawBytes))
        {
            return Fail("verify: CRC-32 of '%s' is wrong", source.name.c_str());
        }
    }

    if (at != dirOffset + dirBytes)
    {
        return Fail("verify: central directory size mismatch");
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Command line
// ------------------------------------------------------------------------------------------------

void PrintUsage()
{
    std::fprintf(stderr,
                 "usage: pakzip [--level N] [--block-kb N] [--min-saving P] [--store-ext LIST] <in.pak> <out.pkz>\n"
                 "  --level N         deflate level, 0-10 (default 9)\n"
                 "  --block-kb N      restart point spacing in KB, a power of two, 4-16384 (default 32)\n"
                 "  --min-saving P    store an entry unless deflate saves at least P%% (default 10)\n"
                 "  --store-ext LIST  always store entries with these extensions, e.g. .dm2,.cin\n");
}

std::string ToLower(std::string text)
{
    for (char & c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// Splits "--store-ext .dm2,.cin" into its lowercase extensions, each starting with a dot.
bool ParseExtensions(const char * text, std::vector<std::string> & out)
{
    std::string list = ToLower(text);
    std::size_t start = 0;
    while (start <= list.size())
    {
        std::size_t end = list.find(',', start);
        if (end == std::string::npos)
        {
            end = list.size();
        }
        const std::string ext = list.substr(start, end - start);
        if (ext.size() < 2 || ext[0] != '.')
        {
            return false;
        }
        out.push_back(ext);
        start = end + 1;
    }
    return !out.empty();
}

bool HasStoredExtension(const std::string & name, const std::vector<std::string> & exts)
{
    const std::string lower = ToLower(name);
    for (const std::string & ext : exts)
    {
        if (lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0)
        {
            return true;
        }
    }
    return false;
}

bool ParseInt(const char * text, const int lo, const int hi, int & out)
{
    char * end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < lo || v > hi)
    {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool ParseArgs(const int argc, char *argv[], Options & opts)
{
    int positional = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char * arg = argv[i];
        const bool hasValue = (i + 1 < argc);

        if (std::strcmp(arg, "--level") == 0 && hasValue)
        {
            if (!ParseInt(argv[++i], 0, 10, opts.level))
            {
                return Fail("--level takes 0-10");
            }
        }
        else if (std::strcmp(arg, "--block-kb") == 0 && hasValue)
        {
            int kb = 0;
            if (!ParseInt(argv[++i], 1, 1 << 14, kb) || (kb & (kb - 1)) != 0)
            {
                return Fail("--block-kb takes a power of two from 4 to 16384");
            }
            int log2 = 10;
            while ((1 << (log2 - 10)) < kb)
            {
                ++log2;
            }
            if (log2 < pkz::kMinLog2BlockBytes || log2 > pkz::kMaxLog2BlockBytes)
            {
                return Fail("--block-kb takes a power of two from 4 to 16384");
            }
            opts.log2BlockBytes = log2;
        }
        else if (std::strcmp(arg, "--min-saving") == 0 && hasValue)
        {
            if (!ParseInt(argv[++i], 0, 99, opts.minSavingPct))
            {
                return Fail("--min-saving takes 0-99");
            }
        }
        else if (std::strcmp(arg, "--store-ext") == 0 && hasValue)
        {
            if (!ParseExtensions(argv[++i], opts.storeExts))
            {
                return Fail("--store-ext takes extensions like .dm2,.cin");
            }
        }
        else if (arg[0] == '-' && arg[1] != '\0')
        {
            return Fail("unknown or incomplete option '%s'", arg);
        }
        else if (positional == 0)
        {
            opts.inPath = arg;
            ++positional;
        }
        else if (positional == 1)
        {
            opts.outPath = arg;
            ++positional;
        }
        else
        {
            return Fail("too many arguments");
        }
    }

    return (positional == 2) ? true : Fail("expected an input pak and an output pkz");
}

} // namespace

int main(int argc, char *argv[])
{
    Options opts;
    if (!ParseArgs(argc, argv, opts))
    {
        PrintUsage();
        return 1;
    }

    const auto startTime = std::chrono::steady_clock::now();

    std::vector<std::uint8_t> pak;
    std::vector<PakEntry> pakEntries;
    if (!ReadWholeFile(opts.inPath, pak) || !ParsePak(pak, opts.inPath, pakEntries))
    {
        return 1;
    }

    if (pakEntries.size() > 0xFFFFu)
    {
        Fail("too many entries for a zip without zip64");
        return 1;
    }

    // Same flags miniz's zip writer would use at this level, minus the zlib header: raw deflate.
    const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(opts.level, -15, MZ_DEFAULT_STRATEGY));
    auto * compressor = static_cast<tdefl_compressor *>(std::malloc(sizeof(tdefl_compressor)));
    if (compressor == nullptr)
    {
        Fail("out of memory");
        return 1;
    }

    std::vector<ZipEntry> entries;
    entries.reserve(pakEntries.size());

    std::vector<std::uint8_t> zip;
    zip.reserve(pak.size());

    std::vector<std::uint8_t>  deflated;
    std::vector<std::uint32_t> restarts;
    std::uint64_t rawTotal = 0;
    std::uint32_t numDeflated = 0;
    std::uint64_t numRestarts = 0;

    for (const PakEntry & source : pakEntries)
    {
        const std::uint8_t * data = pak.data() + source.offset;

        ZipEntry e;
        e.name           = source.name;
        e.rawBytes       = source.size;
        e.crc            = Crc32(data, source.size);
        e.localOffset    = static_cast<std::uint32_t>(zip.size());
        e.method         = pkz::kMethodStored;
        e.compBytes      = source.size;
        e.log2BlockBytes = 0;

        if (source.size != 0 && !HasStoredExtension(source.name, opts.storeExts))
        {
            if (!DeflateEntry(compressor, flags, data, source.size, opts.log2BlockBytes, deflated, restarts))
            {
                std::free(compressor);
                Fail("deflate failed on '%s'", source.name.c_str());
                return 1;
            }

            // Kept deflated only if it saves at least --min-saving percent.
            const std::uint64_t keepAtMost = static_cast<std::uint64_t>(source.size) * static_cast<std::uint64_t>(100 - opts.minSavingPct) / 100u;
            if (deflated.size() < source.size && deflated.size() <= keepAtMost)
            {
                if (restarts.size() > pkz::kMaxRestartsPerEntry)
                {
                    std::free(compressor);
                    Fail("'%s' needs more restart points than a zip extra field holds; use a larger --block-kb", source.name.c_str());
                    return 1;
                }
                e.method         = pkz::kMethodDeflate;
                e.compBytes      = static_cast<std::uint32_t>(deflated.size());
                e.log2BlockBytes = opts.log2BlockBytes;
                e.restarts       = restarts;
                ++numDeflated;
                numRestarts += restarts.size();
            }
        }

        AppendLocalHeader(zip, e);
        if (e.method == pkz::kMethodDeflate)
        {
            Append(zip, deflated.data(), deflated.size());
        }
        else
        {
            Append(zip, data, source.size);
        }

        rawTotal += source.size;
        entries.push_back(std::move(e));

        if (zip.size() > 0xFFFF0000u)
        {
            std::free(compressor);
            Fail("the pack would pass 4 GB, which needs zip64");
            return 1;
        }
    }

    std::free(compressor);

    const std::uint32_t dirOffset = static_cast<std::uint32_t>(zip.size());
    for (const ZipEntry & e : entries)
    {
        AppendCentralHeader(zip, e);
    }
    const std::uint32_t dirBytes = static_cast<std::uint32_t>(zip.size()) - dirOffset;

    AppendU32(zip, pkz::kEndOfCentralDirSig);
    AppendU16(zip, 0); // this disk
    AppendU16(zip, 0); // disk with the central directory
    AppendU16(zip, static_cast<std::uint16_t>(entries.size()));
    AppendU16(zip, static_cast<std::uint16_t>(entries.size()));
    AppendU32(zip, dirBytes);
    AppendU32(zip, dirOffset);
    AppendU16(zip, 0); // comment

    // Written beside the destination, read back from the drive and checked, then renamed over it.
    const std::string tmpPath = std::string(opts.outPath) + ".tmp";
    std::vector<std::uint8_t> written;
    std::uint64_t restartsChecked = 0;

    if (!WriteWholeFile(tmpPath.c_str(), zip) || !ReadWholeFile(tmpPath.c_str(), written) ||
        !VerifyPack(written, pak, pakEntries, restartsChecked))
    {
        std::remove(tmpPath.c_str());
        return 1;
    }

    if (std::rename(tmpPath.c_str(), opts.outPath) != 0)
    {
        std::remove(tmpPath.c_str());
        Fail("cannot rename the pack to '%s'", opts.outPath);
        return 1;
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    std::printf("%s: %zu files (%u deflated, %zu stored), %.2f MB -> %.2f MB (%.1f%%), "
                "%llu restart points every %d KB, all verified, in %.1f s\n",
                opts.outPath, entries.size(), numDeflated, entries.size() - numDeflated,
                Megabytes(rawTotal), Megabytes(written.size()),
                (rawTotal != 0) ? 100.0 * static_cast<double>(written.size()) / static_cast<double>(rawTotal) : 0.0,
                static_cast<unsigned long long>(numRestarts), 1 << (opts.log2BlockBytes - 10), seconds);

    if (restartsChecked != numRestarts)
    {
        Fail("verify: restart point count mismatch");
        return 1;
    }
    return 0;
}
