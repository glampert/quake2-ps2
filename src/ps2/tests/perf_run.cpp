/* ================================================================================================
 * File: perf_run.cpp
 * Brief: Unattended performance run. See perf_run.h.
 *
 *  Drives the demos through the console command buffer rather than calling into the server
 *  directly, exactly as map_cycle drives maps and for the same two reasons: the sequence
 *  exercised is then byte for byte the one a player produces, and Cbuf_AddText defers the
 *  work out of the frame we are in - spawning a server frees the resident world model, and
 *  the renderer is on the stack at the point this runs.
 *
 *  Ending the run is the part that needs care. The stock attract loop never ends: the d1..d4
 *  aliases in default.cfg chain through "nextserver" and d4 points back at d1, so waiting
 *  for it to finish waits forever. Issuing each demo ourselves gives the run a last frame -
 *  a "demomap" with no '+' in it leaves "nextserver" empty, so when the demo file runs out
 *  SV_DemoCompleted -> SV_Nextserver issues "killserver" instead of chaining. The server
 *  going away is therefore the demo finishing, and that is what the state machine watches.
 *
 *  The run ends with two cinematics, issued through "gamemap" as the level chains issue them
 *  ("eou1_.cin+base2"). A cinematic is the one stream the game reads from the drive while it
 *  draws frames, so its stdio refills are what a capture of it is for. Without a '+' in the
 *  name, SCR_FinishCinematic's "nextserver" finds nothing to chain to and kills the server:
 *  the same end signal as a demo's.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/tests/perf_run.h"
#include "ps2/tests/test_quit.h"
#if PS2_QUAKE_PROFILE
#include "ps2/renderer/profile.h"
#endif // PS2_QUAKE_PROFILE

#include <cstdio>
#include <cstring>

namespace ps2::test {
namespace {

// Archived, because there is no command line on this target: the only way to
// arm a run before the first frame is a line in config.cfg, and the game
// rewrites that file from the archived cvars on the way out. Without the flag
// the run's own quit would erase the line that started it.
static const cvar_t * s_enabled = nullptr;

#if PS2_QUAKE_PROFILE
// What the run plays, in order: the demos the stock attract loop cycles through, in its order,
// then two cinematics. The demos come first so their rows keep the frame numbers of older
// captures. eou1_.cin is a typical between-levels cinematic (20 s, about 290 KB/s); ntro.cin
// is the heaviest the game has (210 s, 384 KB/s averaged, 700 KB/s peaks), the one a USB stick
// can least keep up with. idlog.cin, which the attract aliases interleave, stays out: its 6 s
// say nothing the other two don't.
constexpr const char * kSequence[] = { "demo1.dm2", "demo2.dm2", "eou1_.cin", "ntro.cin" };

bool IsCinematic(const char * const name)
{
    const char * const dot = std::strrchr(name, '.');
    return dot != nullptr && std::strcmp(dot, ".cin") == 0;
}

// Com_ServerState() hands back a server_state_t (server/server.h) as a plain int.
// Only "no map loaded" is needed here, and only that it is the first enumerator -
// which is what it has always been. Worth the assumption to keep the backend from
// including a server header for one comparison.
constexpr int kServerDead = 0; // ss_dead

// A demo that never comes up, or never ends, is a failed run rather than a reason
// to sit there forever. Both are far past anything the stock demos take, so if
// either fires something is actually wrong. The load allowance is a console's:
// over USB a map load has taken from 78 s to over two minutes.
constexpr int kLoadTimeoutMs = 240 * 1000;
constexpr int kPlayTimeoutMs = 10 * 60 * 1000;

// The cvars a run sets for its capture, with the values they take during it. Each one's value
// from before is put back when the run ends, before any quit, so the config written on the way
// out keeps the player's settings.
//
// developer 0 keeps Com_DPrintf out of the run - each line is a round trip to the IOP, which
// lands in the timings as a spike in whatever frame it happened to fall in.
//
// And every on-screen debug panel off. Measured at 1.80ms per frame with all five on - 13% of
// the frame's EE work, and enough on its own to push 7% of a capture's frames past the 16.68ms
// target. The panels are ~900 glyphs, each a textured quad pushed through the 2D path, and none
// of them is being read during an unattended run: the frame log already carries every number
// they display, and more.
//
// ps2_frame_log is set apart: a second run in the session appends to the same log, so it is
// turned on for each run and off after it, whatever it was before.
struct RunCvar
{
    const char * name;
    const char * runValue;
};

constexpr RunCvar kRunCvars[] = {
    { "developer",          "0" },
    { "ps2_show_fps",       "0" },
    { "ps2_show_memstats",  "0" },
    { "ps2_show_vramstats", "0" },
    { "ps2_show_drawstats", "0" },
    { "ps2_show_profile",   "0" },
};

// Their values before the run, "" for one that didn't exist then.
constexpr int kMaxCvarValueLen = 32;
static char s_savedValues[ArrayLength(kRunCvars)][kMaxCvarValueLen];

enum class State
{
    Idle,    // Cvars not applied yet; set them and drop whatever is running.
    Killing, // Waiting for the startup server to go away.
    Loading, // demomap or gamemap issued, waiting for its server to come up.
    Playing  // Demo running - the part being measured.
};

static State s_state       = State::Idle;
static bool  s_done        = false;
static int   s_nextDemo    = 0;
static int   s_issuedAtMs  = 0;
static int   s_startedAtMs = 0;
static int   s_failed      = 0;

bool ServerIsUp()
{
    return Com_ServerState() != kServerDead;
}

void ApplyRunCvars()
{
    for (int i = 0; i < ArrayLength(kRunCvars); ++i)
    {
        std::snprintf(s_savedValues[i], sizeof(s_savedValues[i]), "%s", Cvar_VariableString(kRunCvars[i].name));
        Cvar_Set(kRunCvars[i].name, kRunCvars[i].runValue);
    }
}

void RestoreRunCvars()
{
    for (int i = 0; i < ArrayLength(kRunCvars); ++i)
    {
        if (s_savedValues[i][0] != '\0')
        {
            Cvar_Set(kRunCvars[i].name, s_savedValues[i]);
        }
    }
}

void StartDemo()
{
    const char * const name = kSequence[s_nextDemo];
    Com_Printf("PerfRun: [%d/%d] playing %s\n", s_nextDemo + 1, ArrayLength(kSequence), name);

    // A cinematic goes through gamemap, as the level chains issue it: SV_Map spawns a cinematic
    // server, the client plays the file, and with no '+' to chain to the end kills the server.
    Cbuf_AddText(va("%s %s\n", IsCinematic(name) ? "gamemap" : "demomap", name));

    s_issuedAtMs = Sys_Milliseconds();
    s_state      = State::Loading;
}

// Moves past the demo just finished, or ends the run if that was the last one.
void NextDemoOrFinish()
{
    ++s_nextDemo;
    if (s_nextDemo < ArrayLength(kSequence))
    {
        StartDemo();
        return;
    }

    Com_Printf("PerfRun: complete - %d of %d played, %d timed out.\n",
               ArrayLength(kSequence) - s_failed, ArrayLength(kSequence), s_failed);

    // The frame log writes in batches, so the tail of the run is still buffered.
    // This also marks the end, which is what tells a completed capture apart from
    // one the emulator cut short.
    ps2::debug::FrameLogFinish();
    Cvar_Set("ps2_frame_log", "0");
    RestoreRunCvars();

    // EndTest disarms it before quitting, so the config the quit writes has it back
    // at 0. A run that force-quits the program on startup must not be able to leave
    // the build in a state where it does that every launch - arming it is a one line
    // edit, digging back out of it should not need one. A run that never reaches
    // here (a crash, or the emulator being closed) deliberately stays armed.
    if (EndTest("PerfRun", "ps2_perftest"))
    {
        s_done = true;
        return;
    }

    // Staying up for another test.
    s_state    = State::Idle;
    s_nextDemo = 0;
    s_failed   = 0;
}
#endif // PS2_QUAKE_PROFILE

} // namespace

void RegisterPerfTestCvar()
{
    s_enabled = Cvar_Get("ps2_perftest", "0", CVAR_ARCHIVE);
}

#if PS2_QUAKE_PROFILE
void RunPerfTest()
{
    if (s_enabled->value == 0.0f || s_done)
    {
        return;
    }

    switch (s_state)
    {
    case State::Idle:
        // Set directly rather than through the command buffer: they have to be in
        // effect before the first measured frame, and a queued command would not
        // run until the next one. See kRunCvars.
        ApplyRunCvars();
        Cvar_Set("ps2_frame_log", "1");

        Com_Printf("PerfRun: starting - %d demos and cinematics, developer 0, overlays off, frame log on.\n",
                   ArrayLength(kSequence));

        // Whatever the startup left running - the d1 attract loop, or a map forced
        // in Qcommon_Init - is still up. Drop it, so the first demo's server coming
        // up is an unambiguous signal rather than something already true.
        Cbuf_AddText("killserver\n");
        s_issuedAtMs = Sys_Milliseconds();
        s_state      = State::Killing;
        break;

    case State::Killing:
        // Queued last frame, so it has already run by the time this is reached.
        if (!ServerIsUp())
        {
            StartDemo();
        }
        else if ((Sys_Milliseconds() - s_issuedAtMs) > kLoadTimeoutMs)
        {
            Com_Printf("PerfRun: server never went down after killserver - starting anyway.\n");
            StartDemo();
        }
        break;

    case State::Loading:
        if (ServerIsUp())
        {
            s_startedAtMs = Sys_Milliseconds();
            s_state       = State::Playing;

            // A map marks itself in the frame log when it registers (PS2_BeginRegistration); a
            // cinematic registers nothing, so mark it here, where its server has just come up.
            if (IsCinematic(kSequence[s_nextDemo]))
            {
                ps2::debug::FrameLogMarkMap(kSequence[s_nextDemo]);
            }
            break;
        }
        if ((Sys_Milliseconds() - s_issuedAtMs) > kLoadTimeoutMs)
        {
            Com_Printf("PerfRun: '%s' never came up after %d seconds - moving on.\n",
                       kSequence[s_nextDemo], kLoadTimeoutMs / 1000);
            ++s_failed;
            NextDemoOrFinish();
        }
        break;

    case State::Playing:
        // demomap left "nextserver" empty, so the end of the demo file reaches
        // SV_Nextserver with nothing to chain to and it issues killserver. The
        // server going away is the demo finishing.
        if (!ServerIsUp())
        {
            NextDemoOrFinish();
            break;
        }
        if ((Sys_Milliseconds() - s_startedAtMs) > kPlayTimeoutMs)
        {
            Com_Printf("PerfRun: '%s' still running after %d minutes - moving on.\n",
                       kSequence[s_nextDemo], kPlayTimeoutMs / (60 * 1000));
            ++s_failed;
            NextDemoOrFinish();
        }
        break;
    }
}

#endif // PS2_QUAKE_PROFILE

} // namespace ps2::test
