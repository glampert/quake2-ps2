/* ================================================================================================
 * File: log_file.cpp
 * Brief: The console log on the game data's drive. See log_file.h.
 *
 *  Every write opens the file, appends and closes it again, because closing it is the only way
 *  to get a line onto a USB drive. FatFs keeps a file's last partial sector in a buffer of its
 *  own and writes the file's size into its directory entry only when the file is synced or
 *  closed. bdmfs_fatfs has no sync op (it returns EIO) and libcglue's fsync() is ENOSYS, so a
 *  log held open could come back empty after the console is switched off. BDM's block cache
 *  writes through, so once the file is closed the line is on the drive. That costs a few USB
 *  commands per write, which is fine for a log written mostly while loading.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/log_file.h"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <kernel.h> // CreateSema, WaitSema, SignalSema, GetThreadId

// VERSION is a bare float literal (q_common.h). Spelled as text, it needs no float formatting.
#define PS2_LOG_STRINGIFY2(x) #x
#define PS2_LOG_STRINGIFY(x)  PS2_LOG_STRINGIFY2(x)

namespace ps2::debug {
namespace {

constexpr const char * kLogFileName = "quake2.log";

// What the log does until ps2_logfile is registered, and that cvar's default.
// On in every build for now, while USB loading is being debugged on hardware.
// TEMP: Disable once hardware USB boot issues are debugged.
constexpr bool kLogFileDefaultOn = true;

// A Com_Printf message (MAXPRINTMSG is 4096) with a stamp on each line fits in one write. A
// longer run of short lines just takes more than one.
constexpr int kBufferSize = 8192;

enum class State
{
    Off,     // no path yet, or the file couldn't be written: nothing is logged
    Pending, // has a path; the first line written creates the file, emptying an old one
    Open,    // created, and every write since has gone in
};

static State s_state = State::Off;
static char  s_path[MAX_OSPATH];
static const cvar_t * s_logEnabled = nullptr;

// Serializes writers: the music stream's reader thread prints too. The owner is what lets a
// fatal path tell its own thread's interrupted write from another thread's.
static int s_lockSema  = -1;
static int s_lockOwner = -1;

static char s_buffer[kBufferSize];
static int  s_used = 0;
static bool s_atLineStart = true;

Q_ALWAYS_INLINE bool LogEnabled()
{
    return (s_logEnabled != nullptr) ? (s_logEnabled->value != 0.0f) : kLogFileDefaultOn;
}

// Writes out what the buffer holds, creating the file on the first write. See the note at the
// top of the file for why every write is an open and a close.
void Flush()
{
    if (s_used == 0 || s_state == State::Off)
    {
        s_used = 0;
        return;
    }

    const int flags = (s_state == State::Pending) ? (O_WRONLY | O_CREAT | O_TRUNC) : O_WRONLY;
    const int fd = open(s_path, flags, 0666);
    bool written = false;
    if (fd >= 0)
    {
        written = (lseek(fd, 0, SEEK_END) >= 0) && (write(fd, s_buffer, static_cast<size_t>(s_used)) == s_used);
        close(fd);
    }

    if (written)
    {
        s_state = State::Open;
    }
    else
    {
        // Give up after the first failure: a drive that refused one write would make every
        // Com_Printf after it wait on another.
        std::printf("WARNING: can't write the log file %s (%d) - logging is off.\n", s_path, fd);
        s_state = State::Off;
    }
    s_used = 0;
}

void Put(const char c)
{
    if (s_used == kBufferSize)
    {
        Flush();
    }
    s_buffer[s_used++] = c;
}

void PutStamp(const int msec)
{
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%4d.%03d] ", msec / 1000, msec % 1000);
    for (const char * c = stamp; *c != '\0'; ++c)
    {
        Put(*c);
    }
}

// The console wipes a progress line ("Map: base1\r") by printing spaces over it. The wipe
// carries nothing in a log.
bool IsBlank(const char * text)
{
    for (; *text != '\0'; ++text)
    {
        if (*text != ' ' && *text != '\r' && *text != '\n')
        {
            return false;
        }
    }
    return true;
}

void Lock()
{
    if (s_lockSema >= 0)
    {
        WaitSema(s_lockSema);
        s_lockOwner = GetThreadId();
    }
}

void Unlock()
{
    if (s_lockSema >= 0)
    {
        s_lockOwner = -1;
        SignalSema(s_lockSema);
    }
}

// The caller holds the lock.
void WriteLocked(const char * text)
{
    if (s_state == State::Off || !LogEnabled() || IsBlank(text))
    {
        return;
    }

    const int msec = Sys_Milliseconds();
    for (; *text != '\0'; ++text)
    {
        if (s_atLineStart)
        {
            PutStamp(msec);
            s_atLineStart = false;
        }

        // A "\r" ends a progress line on the console, so it ends a line here too.
        const char c = (*text == '\r') ? '\n' : *text;
        Put(c);
        s_atLineStart = (c == '\n');
    }
    Flush();
}

} // namespace

void LogFileOpen(const char * basePath)
{
    std::snprintf(s_path, sizeof(s_path), "%s/%s", basePath, kLogFileName);

    ee_sema_t sema = {};
    sema.init_count = 1;
    sema.max_count  = 1;
    s_lockSema = CreateSema(&sema);

    s_state = State::Pending;

    char header[MAX_OSPATH + 96];
    std::snprintf(header, sizeof(header), "Quake II %s for PS2 (%s build), game data on %s\n",
                  PS2_LOG_STRINGIFY(VERSION), PS2_QUAKE_DEBUG ? "debug" : "release", basePath);
    LogFileWrite(header);

    if (s_state == State::Open)
    {
        std::printf("Logging to %s.\n", s_path);
    }
}

void LogFileRegisterCvar()
{
    // Not archived: a debugging aid shouldn't stay off across sessions because one run turned it off.
    s_logEnabled = Cvar_Get("ps2_logfile", kLogFileDefaultOn ? "1" : "0", 0);
}

void LogFileWrite(const char * text)
{
    if (s_state == State::Off || text == nullptr || !LogEnabled())
    {
        return;
    }

    Lock();
    WriteLocked(text);
    Unlock();
}

void LogFileWriteFatal(const char * text)
{
    if (s_state == State::Off || text == nullptr || !LogEnabled())
    {
        return;
    }

    // This thread's own write was cut short: it holds the lock already, and waiting for it
    // would never end. What that write left in the buffer goes out ahead of this.
    const bool ownWriteInterrupted = (s_lockSema >= 0) && (s_lockOwner == GetThreadId());
    if (!ownWriteInterrupted)
    {
        Lock();
    }

    if (!s_atLineStart)
    {
        Put('\n');
        s_atLineStart = true;
    }
    WriteLocked(text);

    if (!ownWriteInterrupted)
    {
        Unlock();
    }
}

} // namespace ps2::debug
