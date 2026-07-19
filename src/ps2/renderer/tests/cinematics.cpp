/* ================================================================================================
 * File: cinematics.cpp
 * Brief: Cinematic playback test. See cinematics.h.
 *
 *  Sequences the stock cinematics through cl_cin.c's CinematicTest_PlayDirect/RunFrame
 *  helpers (decode + 14 fps pacing live there); this file only tracks which file plays
 *  next. Unopenable files are skipped with a log line, so the test degrades gracefully
 *  when some .cin files are absent from baseq2/video/.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/tests/cinematics.h"

// From cl_cin.c (declared in client/screen.h, which the backend doesn't pull in).
extern "C" qboolean CinematicTest_PlayDirect(const char * filename);
extern "C" qboolean CinematicTest_RunFrame(void);

namespace ps2::test {
namespace {

// Engine-relative paths (resolved by the Quake filesystem against the base
// path): the full stock Quake II cinematic set.
const char * const kCinematics[] = {
    "video/idlog.cin",
    "video/ntro.cin",
    "video/eou1_.cin",
    "video/eou2_.cin",
    "video/eou3_.cin",
    "video/eou4_.cin",
    "video/eou5_.cin",
    "video/eou6_.cin",
    "video/eou7_.cin",
    "video/eou8_.cin",
    "video/end.cin",
};

static int  s_nextFile = 0;
static bool s_playing  = false;
static bool s_done     = false;

} // namespace

void RunCinematics()
{
    static const cvar_t * s_testCinematics = Cvar_Get("ps2_testcin", "0", 0);
    if (s_testCinematics->value == 0.0f || s_done)
    {
        return;
    }

    if (s_playing)
    {
        s_playing = CinematicTest_RunFrame(); // false = finished, next file -> next frame
        return;
    }

    // The movies draw fullscreen; keep the 3D cube test from spinning on top.
    if (s_nextFile == 0)
    {
        Cvar_Set("ps2_testcube", "0");
    }

    while (s_nextFile < ArrayLength(kCinematics))
    {
        const char * filename = kCinematics[s_nextFile++];
        if (CinematicTest_PlayDirect(filename))
        {
            Com_Printf("Cinematic test: playing '%s'\n", filename);
            s_playing = true;
            return;
        }
        Com_Printf("Cinematic test: skipping '%s' (can't open)\n", filename);
    }

    Com_Printf("Cinematic test: all files played, test over.\n");
    s_done = true;
}

} // namespace ps2::test
