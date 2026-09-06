#pragma once
/* ================================================================================================
 * File: render_profile.h
 * Brief: Profile events shared by more than one renderer source file.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/profile.h"

namespace ps2::prof_evt {

PS2_PROFILE_DECLARE_EVENT(Frame);
PS2_PROFILE_DECLARE_EVENT(VSync);
PS2_PROFILE_DECLARE_EVENT(GsWait);
PS2_PROFILE_DECLARE_EVENT(View);
PS2_PROFILE_DECLARE_EVENT(World);
PS2_PROFILE_DECLARE_EVENT(Vis);
PS2_PROFILE_DECLARE_EVENT(TexChains);
PS2_PROFILE_DECLARE_EVENT(LmChains);
PS2_PROFILE_DECLARE_EVENT(Entities);
PS2_PROFILE_DECLARE_EVENT(Particles);
PS2_PROFILE_DECLARE_EVENT(AlphaSurfs);
PS2_PROFILE_DECLARE_EVENT(Sky);

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

#if !PS2_QUAKE_PROFILE
// No-op stubs for when the profiler is disabled.
inline void FrameLogCapture() {}
inline void FrameLogFlush() {}
inline void FrameLogMarkMap(const char *) {}
#endif // PS2_QUAKE_PROFILE

} // namespace ps2::debug
