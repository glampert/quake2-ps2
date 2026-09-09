#pragma once
/* ================================================================================================
 * File: render_profile.h
 * Brief: Profile events shared by more than one source file, and the CSV frame log.
 *
 *        Mostly renderer events, but not exclusively: the frame log writes one column
 *        per event and so needs every one of them declared in a single place. Sound
 *        (the audio backend's submit) lives here for that reason rather than because
 *        it belongs to the renderer.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/profile.h"

namespace ps2::prof_evt {

PS2_PROFILE_DECLARE_EVENT(Frame);
PS2_PROFILE_DECLARE_EVENT(VSync);
PS2_PROFILE_DECLARE_EVENT(GsWait);
PS2_PROFILE_DECLARE_EVENT(DmaSend);
PS2_PROFILE_DECLARE_EVENT(View);
PS2_PROFILE_DECLARE_EVENT(World);
PS2_PROFILE_DECLARE_EVENT(Vis);
PS2_PROFILE_DECLARE_EVENT(MarkLeaves);
PS2_PROFILE_DECLARE_EVENT(BspWalk);
PS2_PROFILE_DECLARE_EVENT(LmChain);
PS2_PROFILE_DECLARE_EVENT(TexChains);
PS2_PROFILE_DECLARE_EVENT(LmChains);
PS2_PROFILE_DECLARE_EVENT(Entities);
PS2_PROFILE_DECLARE_EVENT(EntCull);
PS2_PROFILE_DECLARE_EVENT(EntShade);
PS2_PROFILE_DECLARE_EVENT(EntGeom);
PS2_PROFILE_DECLARE_EVENT(EntShadow);
PS2_PROFILE_DECLARE_EVENT(EntBrush);
PS2_PROFILE_DECLARE_EVENT(Particles);
PS2_PROFILE_DECLARE_EVENT(AlphaSurfs);
PS2_PROFILE_DECLARE_EVENT(Sky);
PS2_PROFILE_DECLARE_EVENT(Ui);
PS2_PROFILE_DECLARE_EVENT(Overlay);
PS2_PROFILE_DECLARE_EVENT(Sound);

} // namespace ps2::prof_evt

// ------------------------------------------------------------------------------------------------
// Frame log
// ------------------------------------------------------------------------------------------------
//
// Buffers per-frame timings and draw statistics in RAM and dumps them to stdout
// in batches as CSV, for offline analysis of a whole run (the attract loop, a
// map cycle) rather than squinting at the on-screen overlay.
//
// The dump is the expensive part, so it never happens inside the measurement:
// Capture() only writes to a RAM buffer, and Flush() - which does the printf -
// is called from the main loop *outside* the Frame scope. The frame a dump
// lands in is still stretched by it, so that one sample is discarded rather
// than logged as a spurious spike.
//
// Rows are prefixed "FLOG" so they can be grepped out of a PCSX2 emulog that
// has the engine's own console output mixed in.
namespace ps2::debug {

// Records the frame that just completed. Call from PS2_BeginFrame right after
// ProfileNewFrame(), and before gs::BeginFrame() resets the per-frame counters
// this reads.
void FrameLogCapture();

// Writes a full batch to stdout, if one is ready. Cheap no-op otherwise. Call
// from the main loop with the Frame profile scope closed.
void FrameLogFlush();

// Emits a marker row so a run can be split by map. Called from PS2_BeginRegistration.
void FrameLogMarkMap(const char * mapName);

// Ends the log: writes whatever the batch still holds, rather than waiting for
// it to fill, then an "FLOG#end" row. Call once when a run finishes - without it
// the last partial batch is lost, and a capture cut short by a crash reads the
// same as one that ran to completion.
void FrameLogFinish();

#if !PS2_QUAKE_PROFILE
// No-op stubs for when the profiler is disabled.
inline void FrameLogCapture() {}
inline void FrameLogFlush() {}
inline void FrameLogMarkMap(const char *) {}
inline void FrameLogFinish() {}
#endif // PS2_QUAKE_PROFILE

} // namespace ps2::debug
