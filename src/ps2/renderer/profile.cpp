/* ================================================================================================
 * File: profile.cpp
 * Brief: Profile events shared by more than one renderer source file.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/profile.h"
#include "ps2/debug/engine_profile.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/vram.h"
#include "ps2/renderer/view.h"

#include <cstdio>

#include <cstdarg>

#if PS2_QUAKE_FRAME_LOG_FILE
#include "ps2/system/iop_boot.h" // SyncGameDataDevice
#include <fcntl.h>
#include <unistd.h>
#endif // PS2_QUAKE_FRAME_LOG_FILE

namespace ps2::prof_evt {

PS2_PROFILE_DEFINE_EVENT(Frame,      "Frame",       kScreenOverlay, 0);
PS2_PROFILE_DEFINE_EVENT(VSync,      "VSync",       kScreenOverlay, 1);
PS2_PROFILE_DEFINE_EVENT(GsWait,     "GsWait",      kScreenOverlay, 2);
PS2_PROFILE_DEFINE_EVENT(DmaSend,    "DmaSend",     kScreenOverlay, 3);
PS2_PROFILE_DEFINE_EVENT(DmaFlush,   " CacheFlsh",  kScreenOverlay, 4);
PS2_PROFILE_DEFINE_EVENT(View,       "View",        kScreenOverlay, 5);
PS2_PROFILE_DEFINE_EVENT(World,      "World",       kScreenOverlay, 6);
PS2_PROFILE_DEFINE_EVENT(Vis,        "Vis",         kScreenOverlay, 7);
PS2_PROFILE_DEFINE_EVENT(MarkLeaves, " Leaves",     kScreenOverlay, 8);
PS2_PROFILE_DEFINE_EVENT(BspWalk,    " BspWalk",    kScreenOverlay, 9);
PS2_PROFILE_DEFINE_EVENT(LmChain,    "  LmChain",   kScreenOverlay, 10);
PS2_PROFILE_DEFINE_EVENT(TexChains,  "TexChains",   kScreenOverlay, 11);
PS2_PROFILE_DEFINE_EVENT(LmChains,   "LmChains",    kScreenOverlay, 12);
PS2_PROFILE_DEFINE_EVENT(Entities,   "Entities",    kScreenOverlay, 13);
PS2_PROFILE_DEFINE_EVENT(EntCull,    " Cull",       kScreenOverlay, 14);
PS2_PROFILE_DEFINE_EVENT(EntShade,   " Shade",      kScreenOverlay, 15);
PS2_PROFILE_DEFINE_EVENT(EntColorLUT," ColorLUT",   kScreenOverlay, 16);
PS2_PROFILE_DEFINE_EVENT(EntGeom,    " Geom",       kScreenOverlay, 17);
PS2_PROFILE_DEFINE_EVENT(EntShadow,  " Shadow",     kScreenOverlay, 18);
PS2_PROFILE_DEFINE_EVENT(EntBrush,   " Brush",      kScreenOverlay, 19);
PS2_PROFILE_DEFINE_EVENT(Particles,  "Particles",   kScreenOverlay, 20);
PS2_PROFILE_DEFINE_EVENT(AlphaSurfs, "AlphaSurfs",  kScreenOverlay, 21);
PS2_PROFILE_DEFINE_EVENT(TurbSurfs,  " TurbSurfs",  kScreenOverlay, 22);
PS2_PROFILE_DEFINE_EVENT(Sky,        "Sky",         kScreenOverlay, 23);
PS2_PROFILE_DEFINE_EVENT(Ui,         "Ui",          kScreenOverlay, 24);
PS2_PROFILE_DEFINE_EVENT(Overlay,    "Overlay",     kScreenOverlay, 25);
PS2_PROFILE_DEFINE_EVENT(Sound,      "Sound",       kScreenOverlay, 26);
PS2_PROFILE_DEFINE_EVENT(Server,     "Server",      kScreenOverlay, 27);
PS2_PROFILE_DEFINE_EVENT(ClParse,    "ClParse",     kScreenOverlay, 28);
PS2_PROFILE_DEFINE_EVENT(ClScene,    "ClScene",     kScreenOverlay, 29);
PS2_PROFILE_DEFINE_EVENT(SndMix,     "SndMix",      kScreenOverlay, 30);
PS2_PROFILE_DEFINE_EVENT(FsIo,       "FsIo",        kScreenOverlay, 31);
PS2_PROFILE_DEFINE_EVENT(Music,      "Music",       kScreenOverlay, 32);
PS2_PROFILE_DEFINE_EVENT(Input,      "Input",       kScreenOverlay, 33);

} // namespace ps2::prof_evt

// ------------------------------------------------------------------------------------------------
// Engine probes (debug/engine_profile.h)
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE
namespace {

static ps2::debug::ProfileEvent * const s_engineEvents[PS2_PROF_SITE_COUNT] = {
    &ps2::prof_evt::Server,  // PS2_PROF_SERVER
    &ps2::prof_evt::ClParse, // PS2_PROF_CL_PARSE
    &ps2::prof_evt::ClScene, // PS2_PROF_CL_SCENE
    &ps2::prof_evt::SndMix,  // PS2_PROF_SND_MIX
    &ps2::prof_evt::FsIo,    // PS2_PROF_FS_IO
};

static ps2::debug::CpuCycles s_engineStart[PS2_PROF_SITE_COUNT];

} // namespace

extern "C" void PS2Quake_ProfileBegin(const int site)
{
    s_engineStart[site] = ps2::debug::ReadCycles();
}

extern "C" void PS2Quake_ProfileEnd(const int site)
{
    ps2::debug::ProfileAccumulate(s_engineEvents[site],
                                  ps2::debug::ReadCycles() - s_engineStart[site]);
}

extern "C" void PS2Quake_FrameLogNoteOpen(const char * fileName)
{
    ps2::debug::FrameLogNoteOpen(fileName);
}
#endif // PS2_QUAKE_PROFILE

// ------------------------------------------------------------------------------------------------
// Frame log
// ------------------------------------------------------------------------------------------------

#if PS2_QUAKE_PROFILE
namespace ps2::debug {
namespace {

// Frames per dump. At 30-60fps this is a batch every one to two seconds, which
// keeps the hitch the dump causes rare while still bounding the buffer. Sized
// against the cost of the dump itself: one printf per row, each a round trip to
// the IOP, so a batch is tens of milliseconds no matter how it is arranged.
constexpr int kBatchFrames = 64;

// Columns taken from the profile registry, in header order.
constexpr int kNumEvents = 34;

// One frame's sample. Timings are held as raw cycles and converted at dump time,
// so capture stays a load and a store per field.
struct FrameSample
{
    u32 frameIndex;
    u32 cycles[kNumEvents];

    // view::DrawStats/rs::DrawStats
    int nodes, surfs, surfsAlpha, surfsTurb, skyFaces;
    int tris, trisClipped, trisCulled, boxesCulled;
    int trisClipNearOnly, trisClipNoNear, trisClipMixed, trisClipFar, clipMaxVerts;
    int batches, entities, particles, dlights;

    // lm::Stats
    int lmAtlases, lmStyle, lmDynamic, lmRestore;

    // vram::Stats. Uploads are bursty around map transitions and each one that
    // followed an eviction also forced a GS drain, so these are the first thing
    // to check against a frame-time spike.
    int vramUploads, vramOomSyncs, vramResident;

    // chain counters. The chain is built front to back across a whole frame and
    // only moves to the other half when it runs out, so chainKB against
    // cmdbuf::kHalfBytes is what says whether the capacity is right - and
    // chainDrains is what says it was not: every one of those is a mid-frame kick
    // the frame did not ask for (it was a full pipeline stall until the overflow
    // stopped waiting for the GS). chainKicks is the number this refactor exists
    // to bring down.
    int chainKB, chainKicks, chainDrains;
};

static FrameSample s_samples[kBatchFrames];
static int  s_count      = 0;
static u32  s_frameIndex = 0;
static bool s_skipNext   = false; // the frame a dump landed in is not representative
static bool s_headerDone = false;

static const cvar_t * s_frameLog = nullptr;

// Files opened since the last dump, written out with it. A file opened while a level runs is a
// synchronous read inside whatever frame asked for it, and without a name the log can only show
// the spike. Loading a map opens hundreds; past the buffer only the count is kept.
constexpr int kMaxOpenNotes = 16;

struct OpenNote
{
    u32  frameIndex;
    char name[MAX_QPATH];
};

static OpenNote s_openNotes[kMaxOpenNotes];
static int s_openCount = 0; // including the ones past the buffer

// Cycles to microseconds. Cold - only runs at dump time, so the 64-bit divide
// (a libgcc call on the R5900) is fine; it is what the capture path exists to avoid.
u32 ToMicrosec(u32 cycles)
{
    const u32 perMillisec = ps2::debug::ProfileCyclesPerMillisec();
    if (perMillisec == 0)
    {
        return 0;
    }
    return static_cast<u32>((static_cast<u64>(cycles) * 1000u) / perMillisec);
}

// Everything the frame log writes goes through Emit, and each dump ends with Commit. Emit prints
// straight to stdout - the PCSX2 emulog's path - one call per row, as ever. With
// PS2_QUAKE_FRAME_LOG_FILE (profile.h) a dump is built here instead and goes out at Commit: to
// stdout as one write, and appended to <gamedir>/frame_log.txt with one open, write and close.
// That is how a capture gets off a console, which has no stdout anyone reads; ps2_logfile can't
// carry it, since every console print it copies is a USB write inside the frames being measured.
void Emit(const char * format, ...) Q_PRINTF_FUNC(1, 2);

#if PS2_QUAKE_FRAME_LOG_FILE

// A dump: a 64-frame batch at a few hundred characters a row, its open notes and the header.
constexpr int kOutBytes = 64 * 1024;
static char s_out[kOutBytes];
static int  s_outUsed    = 0;
static bool s_outStarted = false; // the first dump of a run truncates the file
static bool s_outFailed  = false; // and one that fails to write turns it off for the run

void Emit(const char * format, ...)
{
    const int room = kOutBytes - s_outUsed;
    if (room <= 1)
    {
        return;
    }

    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(s_out + s_outUsed, static_cast<size_t>(room), format, args);
    va_end(args);

    if (n > 0)
    {
        s_outUsed += (n < room) ? n : (room - 1);
    }
}

void Commit()
{
    if (s_outUsed == 0)
    {
        return;
    }

    std::fputs(s_out, stdout);

    if (!s_outFailed)
    {
        char path[MAX_OSPATH];
        std::snprintf(path, sizeof(path), "%s/frame_log.txt", FS_Gamedir());

        const int flags = s_outStarted ? O_WRONLY : (O_WRONLY | O_CREAT | O_TRUNC);
        const int fd    = open(path, flags, 0666);
        bool written    = (fd >= 0);
        if (fd >= 0)
        {
            written = (lseek(fd, 0, SEEK_END) >= 0) && (write(fd, s_out, static_cast<size_t>(s_outUsed)) == s_outUsed);
            // A successful close on USB/HDD returns the iomanX slot, not 0 (see ps2-platform.md).
            written = (close(fd) >= 0) && written;
            written = (ps2::sys::SyncGameDataDevice() == 0) && written;
        }
        s_outStarted = true;

        if (!written)
        {
            s_outFailed = true;
            std::printf("FLOG#note,writing %s failed; stdout only from here\n", path);
        }
    }

    s_outUsed = 0;
    s_out[0]  = '\0';
}

#else // !PS2_QUAKE_FRAME_LOG_FILE

void Emit(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}

void Commit()
{
}

#endif // PS2_QUAKE_FRAME_LOG_FILE

bool Enabled()
{
    if (s_frameLog == nullptr)
    {
        s_frameLog = Cvar_Get("ps2_frame_log", "0", 0); // <-- ENABLE FRAME LOG HERE
    }
    return s_frameLog->value != 0.0f;
}

// Writes every sample the batch holds and empties it. Callers decide whether a
// partial batch is worth writing; see FrameLogFlush and FrameLogFinish.
void WriteBatch()
{
    if (!s_headerDone)
    {
        s_headerDone = true;
        Emit("FLOG#hdr,frame,"
                    "Frame,VSync,GsWait,DmaSend,DmaFlush,View,World,Vis,MarkLeaves,BspWalk,LmChain,"
                    "TexChains,LmChains,Entities,EntCull,EntShade,EntColorLUT,EntGeom,EntShadow,EntBrush,"
                    "Particles,AlphaSurfs,TurbSurfs,Sky,Ui,Overlay,Sound,Server,ClParse,ClScene,SndMix,FsIo,Music,Input,"
                    "nodes,surfs,surfsAlpha,surfsTurb,skyFaces,tris,trisClipped,trisCulled,"
                    "clipNear,clipNoNear,clipMixed,clipFar,clipMaxV,"
                    "boxesCulled,batches,entities,particles,dlights,"
                    "lmAtlases,lmStyle,lmDynamic,lmRestore,"
                    "vramUploads,vramOomSyncs,vramResident,"
                    "chainKB,chainKicks,chainDrains\n");
        Emit("FLOG#note,timings are microseconds\n");
    }

    // Built into one buffer and written with a single printf: every call is a
    // round trip to the IOP, so a per-column printf would turn the dump from
    // one stretched frame into several. The timings go through a loop rather
    // than a fixed argument list so adding an event needs no changes here.
    for (int i = 0; i < s_count; ++i)
    {
        const FrameSample & s = s_samples[i];

        char line[640];
        int at = std::snprintf(line, sizeof(line), "FLOG,%u", s.frameIndex);

        for (int e = 0; e < kNumEvents && at > 0 && at < static_cast<int>(sizeof(line)); ++e)
        {
            at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                                ",%u", ToMicrosec(s.cycles[e]));
        }

        if (at > 0 && at < static_cast<int>(sizeof(line)))
        {
            std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
                          ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                          "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                          s.nodes, s.surfs, s.surfsAlpha, s.surfsTurb, s.skyFaces,
                          s.tris, s.trisClipped, s.trisCulled,
                          s.trisClipNearOnly, s.trisClipNoNear, s.trisClipMixed,
                          s.trisClipFar, s.clipMaxVerts, s.boxesCulled,
                          s.batches, s.entities, s.particles, s.dlights,
                          s.lmAtlases, s.lmStyle, s.lmDynamic, s.lmRestore,
                          s.vramUploads, s.vramOomSyncs, s.vramResident,
                          s.chainKB, s.chainKicks, s.chainDrains);
        }

        Emit("%s", line);
    }

    for (int i = 0; i < s_openCount && i < kMaxOpenNotes; ++i)
    {
        Emit("FLOG#open,%u,%s\n", s_openNotes[i].frameIndex, s_openNotes[i].name);
    }
    if (s_openCount > kMaxOpenNotes)
    {
        Emit("FLOG#open,%u,+%d more\n", s_frameIndex, s_openCount - kMaxOpenNotes);
    }
    s_openCount = 0;

    s_count    = 0;
    s_skipNext = true; // this frame just absorbed the whole dump
}

} // namespace

void FrameLogCapture()
{
    if (!Enabled())
    {
        return;
    }

    ++s_frameIndex;

    // The previous dump stretched this frame; logging it would read as a spike
    // in the renderer rather than in the logging.
    if (s_skipNext)
    {
        s_skipNext = false;
        return;
    }

    if (s_count >= kBatchFrames)
    {
        return; // Batch already full and waiting on FrameLogFlush.
    }

    FrameSample & s = s_samples[s_count++];
    s.frameIndex = s_frameIndex;

    static const ps2::debug::ProfileEvent * const s_events[kNumEvents] = {
        &prof_evt::Frame,       &prof_evt::VSync,      &prof_evt::GsWait,    &prof_evt::DmaSend,
        &prof_evt::DmaFlush,    &prof_evt::View,       &prof_evt::World,     &prof_evt::Vis,
        &prof_evt::MarkLeaves,  &prof_evt::BspWalk,    &prof_evt::LmChain,   &prof_evt::TexChains,
        &prof_evt::LmChains,    &prof_evt::Entities,   &prof_evt::EntCull,   &prof_evt::EntShade,
        &prof_evt::EntColorLUT, &prof_evt::EntGeom,    &prof_evt::EntShadow, &prof_evt::EntBrush,
        &prof_evt::Particles,   &prof_evt::AlphaSurfs, &prof_evt::TurbSurfs, &prof_evt::Sky,
        &prof_evt::Ui,          &prof_evt::Overlay,    &prof_evt::Sound,     &prof_evt::Server,
        &prof_evt::ClParse,     &prof_evt::ClScene,    &prof_evt::SndMix,    &prof_evt::FsIo,
        &prof_evt::Music,       &prof_evt::Input,
    };
    for (int i = 0; i < kNumEvents; ++i)
    {
        s.cycles[i] = s_events[i]->lastFrameCycles;
    }

    // All of these still hold the finished frame's values here: the view counters are cleared at
    // the top of view::RenderFrame, the submission counters by rs::BeginFrame and the lightmap
    // ones by lm::BeginFrame, none of which has run yet for the new frame.
    const view::DrawStats & d = view::GetStats();
    s.nodes          = d.nodesWalked;
    s.surfs          = d.surfaces;
    s.surfsAlpha     = d.surfacesAlpha;
    s.surfsTurb      = d.surfacesTurb;
    s.skyFaces       = d.skyFaces;
    s.boxesCulled    = d.boxesCulled;
    s.entities       = d.entities;
    s.dlights        = d.dlights;

    const rs::DrawStats & r = rs::GetStats();
    s.tris             = r.trisDrawn;
    s.trisClipped      = r.trisClipped;
    s.trisCulled       = r.trisCulled;
    s.trisClipNearOnly = r.trisClipNearOnly;
    s.trisClipNoNear   = r.trisClipNoNear;
    s.trisClipMixed    = r.trisClipMixed;
    s.trisClipFar      = r.trisClipFar;
    s.clipMaxVerts     = r.clipMaxVerts;
    s.batches          = r.drawBatches;
    s.particles        = r.particles;

    const lm::Stats & l = lm::GetStats();
    s.lmAtlases = l.atlases;
    s.lmStyle   = l.styleUpdates;
    s.lmDynamic = l.dynamicUpdates;
    s.lmRestore = l.restoreUpdates;

    const vram::Stats v = vram::GetStats();
    s.vramUploads  = v.uploadsThisFrame;
    s.vramOomSyncs = v.oomSyncsThisFrame;
    s.vramResident = v.residentTextures;

    // cmdbuf::EndFrame has not run for the new frame either, so these are still the finished
    // frame's. Rounded to KB because the interesting comparison is against a half, which is
    // cmdbuf::kHalfBytes.
    s.chainKB     = static_cast<int>(cmdbuf::BytesLastFrame() / 1024u);
    s.chainKicks  = cmdbuf::KicksLastFrame();
    s.chainDrains = cmdbuf::EmergencyDrainsLastFrame();
}

void FrameLogFlush()
{
    if (s_count < kBatchFrames || !Enabled())
    {
        return;
    }
    WriteBatch();
    Commit();
}

void FrameLogFinish()
{
    if (!Enabled())
    {
        return;
    }

    // Whatever is left has nowhere else to go - this is the end of the run.
    if (s_count > 0)
    {
        WriteBatch();
    }

    // Lets the reader tell a completed capture from one the emulator cut short.
    Emit("FLOG#end,%u\n", s_frameIndex);
    Commit();
    std::fflush(stdout);
}

void FrameLogNoteOpen(const char * fileName)
{
    if (!Enabled())
    {
        return;
    }
    if (s_openCount < kMaxOpenNotes)
    {
        // The row the open is charged to: the frame being measured now, which FrameLogCapture
        // will number one past the last row it wrote.
        OpenNote & note = s_openNotes[s_openCount];
        note.frameIndex = s_frameIndex + 1;
        std::snprintf(note.name, sizeof(note.name), "%s", (fileName != nullptr) ? fileName : "?");
    }
    ++s_openCount;
}

void FrameLogMarkMap(const char * mapName)
{
    if (!Enabled())
    {
        return;
    }
    Emit("FLOG#map,%u,%s\n", s_frameIndex, (mapName != nullptr) ? mapName : "?");
    Commit();
}

} // namespace ps2::debug
#endif // PS2_QUAKE_PROFILE
