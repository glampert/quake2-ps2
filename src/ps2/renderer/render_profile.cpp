/* ================================================================================================
 * File: render_profile.cpp
 * Brief: Profile events shared by more than one renderer source file.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/render_profile.h"
#include "ps2/renderer/render_view.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/vram.h"

#include <cstdio>

namespace ps2::prof_evt {

PS2_PROFILE_DEFINE_EVENT(Frame,      "Frame",      kScreenOverlay, 0);
PS2_PROFILE_DEFINE_EVENT(VSync,      "VSync",      kScreenOverlay, 1);
PS2_PROFILE_DEFINE_EVENT(GsWait,     "GsWait",     kScreenOverlay, 2);
PS2_PROFILE_DEFINE_EVENT(View,       "View",       kScreenOverlay, 3);
PS2_PROFILE_DEFINE_EVENT(World,      "World",      kScreenOverlay, 4);
PS2_PROFILE_DEFINE_EVENT(Vis,        "Vis",        kScreenOverlay, 5);
PS2_PROFILE_DEFINE_EVENT(TexChains,  "TexChains",  kScreenOverlay, 6);
PS2_PROFILE_DEFINE_EVENT(LmChains,   "LmChains",   kScreenOverlay, 7);
PS2_PROFILE_DEFINE_EVENT(Entities,   "Entities",   kScreenOverlay, 8);
PS2_PROFILE_DEFINE_EVENT(Particles,  "Particles",  kScreenOverlay, 9);
PS2_PROFILE_DEFINE_EVENT(AlphaSurfs, "AlphaSurfs", kScreenOverlay, 10);
PS2_PROFILE_DEFINE_EVENT(Sky,        "Sky",        kScreenOverlay, 11);

} // namespace ps2::prof_evt

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
constexpr int kNumEvents = 12;

// One frame's sample. Timings are held as raw cycles and converted at dump time,
// so capture stays a load and a store per field.
struct FrameSample
{
    u32 frameIndex;
    u32 cycles[kNumEvents];

    // view::DrawStats
    int nodes, surfs, surfsAlpha, skyFaces;
    int tris, trisClipped, trisCulled, trisBackFacing;
    int boxesCulled, batches, entities, particles, dlights;

    // lm::Stats
    int lmAtlases, lmStyle, lmDynamic, lmRestore;

    // vram::Stats. Uploads are bursty around map transitions and each one that
    // followed an eviction also forced a GS drain, so these are the first thing
    // to check against a frame-time spike.
    int vramUploads, vramOomSyncs, vramResident;

    // Bytes vu1 REF'd to the DMA this frame.
    int submittedBytes;
};

static FrameSample s_samples[kBatchFrames];
static int  s_count      = 0;
static u32  s_frameIndex = 0;
static bool s_skipNext   = false; // the frame a dump landed in is not representative
static bool s_headerDone = false;

static const cvar_t * s_frameLog = nullptr;

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

bool Enabled()
{
    if (s_frameLog == nullptr)
    {
        s_frameLog = Cvar_Get("ps2_frame_log", "0", 0); // <-- ENABLE FRAME LOG HERE
    }
    return s_frameLog->value != 0.0f;
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

    const ps2::debug::ProfileEvent * const events[kNumEvents] = {
        &prof_evt::Frame,    &prof_evt::VSync,     &prof_evt::GsWait,     &prof_evt::View,
        &prof_evt::World,    &prof_evt::Vis,       &prof_evt::TexChains,  &prof_evt::LmChains,
        &prof_evt::Entities, &prof_evt::Particles, &prof_evt::AlphaSurfs, &prof_evt::Sky,
    };
    for (int i = 0; i < kNumEvents; ++i)
    {
        s.cycles[i] = events[i]->lastFrameCycles;
    }

    // Both of these still hold the finished frame's values here: DrawStats is
    // cleared at the top of view::RenderFrame and the lightmap counters by
    // lm::BeginFrame, neither of which has run yet for the new frame.
    const view::DrawStats & d = view::GetDrawStats();
    s.nodes          = d.nodesWalked;
    s.surfs          = d.surfaces;
    s.surfsAlpha     = d.surfacesAlpha;
    s.skyFaces       = d.skyFaces;
    s.tris           = d.trisDrawn;
    s.trisClipped    = d.trisClipped;
    s.trisCulled     = d.trisCulled;
    s.trisBackFacing = d.trisBackFacing;
    s.boxesCulled    = d.boxesCulled;
    s.batches        = d.drawBatches;
    s.entities       = d.entities;
    s.particles      = d.particles;
    s.dlights        = d.dlights;

    const lm::Stats l = lm::GetStats();
    s.lmAtlases = l.atlases;
    s.lmStyle   = l.styleUpdates;
    s.lmDynamic = l.dynamicUpdates;
    s.lmRestore = l.restoreUpdates;

    const vram::Stats v = vram::GetStats();
    s.vramUploads  = v.uploadsThisFrame;
    s.vramOomSyncs = v.oomSyncsThisFrame;
    s.vramResident = v.residentTextures;

    s.submittedBytes = vu1::FrameSubmittedBytes();
}

void FrameLogFlush()
{
    if (s_count < kBatchFrames || !Enabled())
    {
        return;
    }

    if (!s_headerDone)
    {
        s_headerDone = true;
        std::printf("FLOG#hdr,frame,"
                    "Frame,VSync,GsWait,View,World,Vis,TexChains,LmChains,Entities,Particles,AlphaSurfs,Sky,"
                    "nodes,surfs,surfsAlpha,skyFaces,tris,trisClipped,trisCulled,trisBackFacing,"
                    "boxesCulled,batches,entities,particles,dlights,"
                    "lmAtlases,lmStyle,lmDynamic,lmRestore,"
                    "vramUploads,vramOomSyncs,vramResident,submittedBytes\n");
        std::printf("FLOG#note,timings are microseconds\n");
    }

    for (int i = 0; i < s_count; ++i)
    {
        const FrameSample & s = s_samples[i];
        std::printf("FLOG,%u,"
                    "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                    "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                    "%d,%d,%d,%d,%d,%d,%d,%d\n",
                    s.frameIndex,
                    ToMicrosec(s.cycles[0]),  ToMicrosec(s.cycles[1]),  ToMicrosec(s.cycles[2]),
                    ToMicrosec(s.cycles[3]),  ToMicrosec(s.cycles[4]),  ToMicrosec(s.cycles[5]),
                    ToMicrosec(s.cycles[6]),  ToMicrosec(s.cycles[7]),  ToMicrosec(s.cycles[8]),
                    ToMicrosec(s.cycles[9]),  ToMicrosec(s.cycles[10]), ToMicrosec(s.cycles[11]),
                    s.nodes, s.surfs, s.surfsAlpha, s.skyFaces,
                    s.tris, s.trisClipped, s.trisCulled, s.trisBackFacing,
                    s.boxesCulled, s.batches, s.entities, s.particles, s.dlights,
                    s.lmAtlases, s.lmStyle, s.lmDynamic, s.lmRestore,
                    s.vramUploads, s.vramOomSyncs, s.vramResident, s.submittedBytes);
    }

    s_count    = 0;
    s_skipNext = true; // this frame just absorbed the whole dump
}

void FrameLogMarkMap(const char * mapName)
{
    if (!Enabled())
    {
        return;
    }
    std::printf("FLOG#map,%u,%s\n", s_frameIndex, (mapName != nullptr) ? mapName : "?");
}

} // namespace ps2::debug
#endif // PS2_QUAKE_PROFILE
