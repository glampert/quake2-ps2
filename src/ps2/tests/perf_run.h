#pragma once
/* ================================================================================================
 * File: perf_run.h
 * Brief: Unattended performance run: plays the attract loop demos with the profiling cvars
 *        set, then quits, so a capture needs nobody watching it.
 *
 *        The value of a performance number is entirely in being able to compare it to the
 *        last one, and that needs the run to be identical: same demos, same order, same
 *        cvars, same start and same end. A human driving the game reproduces none of those.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::test {

// Advances the performance run by one frame. Call every frame from PS2_EndFrame.
// Gated by the "ps2_perftest" cvar; a no-op when it is 0 and once the run is over.
//
// To arm a run, add this to baseq2/config.cfg with the game closed:
//
//     set ps2_perftest "1"
//
// From there it takes itself: forces "developer 0" and "ps2_frame_log 1", drops
// whatever the startup left running, plays each attract loop demo once, ends the
// frame log cleanly and quits. The emulator log then holds one complete capture,
// terminated by an "FLOG#end" row so a truncated one is recognisable.
//
// One shot: the cvar is archived and set back to 0 before quitting, so the config
// written on the way out disarms the next launch. A run that does not finish
// stays armed, which is what you want when the emulator was closed mid-capture.
//
// Do not enable alongside "ps2_testmaps" - both drive the server through the
// command buffer and would fight over it.
void RunPerfTest();

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
