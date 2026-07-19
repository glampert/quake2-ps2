#pragma once
/* ================================================================================================
 * File: cinematics.h
 * Brief: Bring-up test that plays every stock cinematic from baseq2/video/ in sequence,
 *        driving the cl_cin.c decode path and the ps2::cin rendering pipeline end to end.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::test {

// Runs one frame of the cinematic playback test. Call every frame inside the
// gs 2D section (the cinematic quad is a 2D draw). Gated by the "ps2_testcin"
// cvar; a no-op when it is 0 and after the last cinematic finishes.
void RunCinematics();

} // namespace ps2::test
