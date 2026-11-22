/* ================================================================================================
 * File: io_log.cpp
 * Brief: The I/O log. See io_log.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/io_log.h"

#if PS2_QUAKE_IO_LOG

#include "ps2/system/iop_boot.h" // SyncGameDataDevice, FileXioBufferBytes, DebugOutputGamedir

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <tamtypes.h>
#include <timer.h> // GetTimerSystemTime / kBUSCLKBY256
#include <unistd.h>

namespace {

// GetTimerSystemTime counts in BUSCLK units, the T2 tick scaled up by 256 (see Sys_Milliseconds);
// shifted back down, a tick is 1/576000 s and 32 bits of them last over two hours.
constexpr u32 kTicksPerMillisec = kBUSCLKBY256 / 1000u;

// Time and bytes charged to one file, or to a whole load.
struct Totals
{
    u32 ticks;        // inside the file system's calls
    u32 storageBytes; // read from the drive
    u32 storageTicks; // waiting for those reads
    u32 inflateTicks; // in tinfl
};

void Add(Totals & to, const Totals & from)
{
    to.ticks        += from.ticks;
    to.storageBytes += from.storageBytes;
    to.storageTicks += from.storageTicks;
    to.inflateTicks += from.inflateTicks;
}

// A stream FS_FOpenFile opened during a load, until FS_FCloseFile closes it.
struct StreamRecord
{
    const void * file;
    const char * source;
    bool         compressed; // its reads report their own storage and inflate time
    u32          bytesRead;
    Totals       totals;
    char         name[MAX_QPATH];
};

// A level load keeps a .bsp, a model and the demo open at most.
constexpr int kMaxStreams = 8;
static StreamRecord s_streams[kMaxStreams] = {};

// The load in progress.
static bool   s_active         = false;
static u32    s_loadIndex      = 0;
static u32    s_loadBeginTicks = 0;
static u32    s_loadFiles      = 0;
static u32    s_loadDelivered  = 0; // bytes the game read
static Totals s_loadTotals     = {};
static char   s_loadName[MAX_QPATH] = {};

// The file system call in progress, and what the pkz backend reported from inside it.
static u32          s_callBeginTicks     = 0;
static Totals       s_callReported       = {};
static bool         s_callHasReport      = false;
static const void * s_callCompressedFile = nullptr;

// The lines of a load, kept until it ends. A typical load is a few hundred lines.
constexpr int kOutBytes = 48 * 1024;
static char s_out[kOutBytes];
static int  s_outUsed    = 0;
static bool s_outStarted = false; // the first write of a run truncates the file
static bool s_outFailed  = false; // and one that fails turns the file off for the run
static char s_outPath[MAX_OSPATH] = {}; // where the last write went, for the console line

u32 Now()
{
    return static_cast<u32>(GetTimerSystemTime() >> 8);
}

u32 Millisecs(const u32 ticks)
{
    return ticks / kTicksPerMillisec;
}

// Prints the buffered lines to stdout and appends them to <gamedir>/io_log.txt, as the frame
// log does (renderer/profile.cpp).
void Flush()
{
    if (s_outUsed == 0)
    {
        return;
    }

    std::fputs(s_out, stdout);

    // A disc build has nowhere to write it on a console (ps2::sys::DebugOutputGamedir).
    const char * const dir = ps2::sys::DebugOutputGamedir();
    if (dir == nullptr)
    {
        s_outFailed = true;
    }

    if (!s_outFailed)
    {
        char path[MAX_OSPATH];
        std::snprintf(path, sizeof(path), "%s/io_log.txt", dir);

        const int flags = s_outStarted ? O_WRONLY : (O_WRONLY | O_CREAT | O_TRUNC);
        const int fd    = open(path, flags, 0666);
        int wrote  = -1;
        int closed = -1;
        bool written = false;
        if (fd >= 0)
        {
            if (lseek(fd, 0, SEEK_END) >= 0)
            {
                wrote = static_cast<int>(write(fd, s_out, static_cast<size_t>(s_outUsed)));
            }
            // A successful close on USB/HDD returns the iomanX slot, not 0 (see ps2-platform.md).
            closed  = close(fd);
            written = (wrote == s_outUsed) && (closed >= 0) && (ps2::sys::SyncGameDataDevice() == 0);
        }
        s_outStarted = true;

        // Through Com_Printf, unlike the lines themselves: on a console stdout reaches nobody, and
        // this is how a run tells, in the in-game console, whether the file was written.
        if (written)
        {
            std::snprintf(s_outPath, sizeof(s_outPath), "%s", path);
        }
        else
        {
            s_outFailed = true;
            Com_Printf("WARNING: I/O log: writing %s failed (open %d, write %d of %d, close %d, errno %d); "
                       "stdout only from here.\n", path, fd, wrote, s_outUsed, closed, errno);
        }
    }

    s_outUsed = 0;
    s_out[0]  = '\0';
}

void Emit(const char * format, ...) Q_PRINTF_FUNC(1, 2);

void Emit(const char * format, ...)
{
    char line[256];

    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (n <= 0)
    {
        return;
    }

    const int length = (n < static_cast<int>(sizeof(line))) ? n : static_cast<int>(sizeof(line)) - 1;
    if (s_outUsed + length + 1 > kOutBytes)
    {
        Flush(); // A drive write inside the load, but only once its lines outgrow the buffer.
    }

    std::memcpy(s_out + s_outUsed, line, static_cast<size_t>(length) + 1u);
    s_outUsed += length;
}

void EmitFile(const char * name, const char * source, const u32 bytes, const Totals & t)
{
    Emit("%8u %10u %11u %8u %11u  %-6s  %s\n", Millisecs(t.ticks), bytes, t.storageBytes,
         Millisecs(t.storageTicks), Millisecs(t.inflateTicks), source, name);
}

// The call that just ended, charged one of two ways. A pkz stream or file reports its own reads
// and inflating. Anything else - a pak, a loose file, a stored pkz entry streamed through stdio -
// is the drive the whole time, and 'plainBytes' is what was asked of it.
Totals EndCall(const bool plain, const u32 plainBytes)
{
    Totals t = {};
    t.ticks = Now() - s_callBeginTicks;

    if (plain)
    {
        t.storageBytes = plainBytes;
        t.storageTicks = t.ticks;
    }
    else
    {
        t.storageBytes = s_callReported.storageBytes;
        t.storageTicks = s_callReported.storageTicks;
        t.inflateTicks = s_callReported.inflateTicks;
    }
    return t;
}

void NoteLoadName(const char * name)
{
    if (s_loadName[0] == '\0' && std::strncmp(name, "maps/", 5) == 0)
    {
        std::snprintf(s_loadName, sizeof(s_loadName), "%s", name);
    }
}

StreamRecord * FindStream(const void * file)
{
    if (file == nullptr)
    {
        return nullptr;
    }
    for (StreamRecord & s : s_streams)
    {
        if (s.file == file)
        {
            return &s;
        }
    }
    return nullptr;
}

// "12.34" from a byte count, in MB, without touching the soft-float doubles.
struct Megabytes
{
    u32 whole;
    u32 hundredths;
};

Megabytes ToMegabytes(const u32 bytes)
{
    const u32 hundredths = static_cast<u32>((static_cast<u64>(bytes) * 100u) >> 20);
    return { hundredths / 100u, hundredths % 100u };
}

// "12.3" seconds from ticks.
struct Seconds
{
    u32 whole;
    u32 tenths;
};

Seconds ToSeconds(const u32 ticks)
{
    const u32 ms = Millisecs(ticks);
    return { ms / 1000u, (ms % 1000u) / 100u };
}

} // namespace

extern "C" {

void PS2_IoLogLoadBegin()
{
    if (s_active)
    {
        return;
    }

    s_active         = true;
    s_loadIndex     += 1;
    s_loadBeginTicks = Now();
    s_loadFiles      = 0;
    s_loadDelivered  = 0;
    s_loadTotals     = {};
    s_loadName[0]    = '\0';

    // Which drive and fileXio buffer the run measured, so logs from different setups can't be
    // mixed up: the source column only tells a pkz from a pak.
    if (s_loadIndex == 1)
    {
        const int bufferBytes = ps2::sys::FileXioBufferBytes();
        if (bufferBytes > 0)
        {
            Emit("# I/O log of %s, fileXio read buffer %d KB\n", FS_Gamedir(), bufferBytes / 1024);
        }
        else
        {
            Emit("# I/O log of %s\n", FS_Gamedir());
        }
    }

    Emit("# load %u\n#     ms      bytes  read_bytes  read_ms  inflate_ms  source  name\n", s_loadIndex);
    Com_Printf("I/O log: recording load %u\n", s_loadIndex);
}

void PS2_IoLogLoadEnd()
{
    if (!s_active)
    {
        return;
    }
    s_active = false;

    const Megabytes delivered = ToMegabytes(s_loadDelivered);
    const Megabytes read      = ToMegabytes(s_loadTotals.storageBytes);
    const Seconds   total     = ToSeconds(Now() - s_loadBeginTicks);
    const Seconds   inCalls   = ToSeconds(s_loadTotals.ticks);
    const Seconds   reading   = ToSeconds(s_loadTotals.storageTicks);
    const Seconds   inflating = ToSeconds(s_loadTotals.inflateTicks);

    Emit("# load %u %s: %u files, %u.%02u MB delivered, %u.%02u MB read; %u.%u s, of which\n"
         "#   %u.%u s in file calls: %u.%u s reading, %u.%u s inflating\n",
         s_loadIndex, (s_loadName[0] != '\0') ? s_loadName : "-", s_loadFiles,
         delivered.whole, delivered.hundredths, read.whole, read.hundredths, total.whole, total.tenths,
         inCalls.whole, inCalls.tenths, reading.whole, reading.tenths, inflating.whole, inflating.tenths);

    Flush();

    if (!s_outFailed)
    {
        Com_Printf("I/O log: load %u (%s) written to %s\n", s_loadIndex,
                   (s_loadName[0] != '\0') ? s_loadName : "-", s_outPath);
    }
}

void PS2_IoLogCallBegin()
{
    s_callBeginTicks     = Now();
    s_callReported       = {};
    s_callHasReport      = false;
    s_callCompressedFile = nullptr;
}

void PS2_IoLogLoadFileEnd(const char * name, const int lengthBytes, const char * source)
{
    if (!s_active || lengthBytes < 0)
    {
        return;
    }

    const u32 bytes = static_cast<u32>(lengthBytes);
    const Totals t  = EndCall(!s_callHasReport, bytes);

    Add(s_loadTotals, t);
    s_loadFiles     += 1;
    s_loadDelivered += bytes;
    NoteLoadName(name);
    EmitFile(name, source, bytes, t);
}

void PS2_IoLogStreamOpenEnd(const char * name, const void * file, const int lengthBytes, const char * source)
{
    (void)lengthBytes;
    if (!s_active || file == nullptr)
    {
        return;
    }

    const bool compressed = (s_callCompressedFile == file);
    const Totals t = EndCall(!compressed, 0);
    Add(s_loadTotals, t);
    s_loadFiles += 1;
    NoteLoadName(name);

    // Without a free record its reads still count towards the load, as plain ones.
    for (StreamRecord & s : s_streams)
    {
        if (s.file == nullptr)
        {
            s.file       = file;
            s.source     = source;
            s.compressed = compressed;
            s.bytesRead  = 0;
            s.totals     = t;
            std::snprintf(s.name, sizeof(s.name), "%s", name);
            break;
        }
    }
}

void PS2_IoLogStreamReadEnd(const void * file, const int bytes)
{
    if (!s_active || bytes <= 0)
    {
        return;
    }

    StreamRecord * const record = FindStream(file);
    const bool compressed = (record != nullptr) && record->compressed;
    const Totals t = EndCall(!compressed, static_cast<u32>(bytes));

    Add(s_loadTotals, t);
    s_loadDelivered += static_cast<u32>(bytes);
    if (record != nullptr)
    {
        Add(record->totals, t);
        record->bytesRead += static_cast<u32>(bytes);
    }
}

void PS2_IoLogStreamCloseEnd(const void * file)
{
    StreamRecord * const record = FindStream(file);
    if (record == nullptr)
    {
        return;
    }

    if (s_active)
    {
        const Totals t = EndCall(!record->compressed, 0);
        Add(s_loadTotals, t);
        Add(record->totals, t);
        EmitFile(record->name, record->source, record->bytesRead, record->totals);
    }
    record->file = nullptr; // A stream closed after its load ended (the demo) goes unlogged.
}

void PS2_IoLogStorageRead(const unsigned bytes, const unsigned ticks)
{
    s_callReported.storageBytes += bytes;
    s_callReported.storageTicks += ticks;
    s_callHasReport = true;
}

void PS2_IoLogInflate(const unsigned ticks)
{
    s_callReported.inflateTicks += ticks;
    s_callHasReport = true;
}

void PS2_IoLogCompressedStream(const void * file)
{
    s_callCompressedFile = file;
}

unsigned PS2_IoLogTicks()
{
    return Now();
}

} // extern "C"

#endif // PS2_QUAKE_IO_LOG
