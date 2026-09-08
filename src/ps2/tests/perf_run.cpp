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
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/common.h"
#include "ps2/tests/perf_run.h"
#include "ps2/renderer/render_profile.h"

namespace ps2::test {
namespace {

// The demos the stock attract loop cycles through, in its order. The idlog.cin
// cinematic those aliases interleave is deliberately left out: a cinematic is a
// full screen blit down a path the world renderer never touches, so it would
// only pad the log with frames that measure nothing we are trying to compare.
constexpr const char * kDemos[] = { "demo1.dm2", "demo2.dm2" };

// Com_ServerState() hands back a server_state_t (server/server.h) as a plain int.
// Only "no map loaded" is needed here, and only that it is the first enumerator -
// which is what it has always been. Worth the assumption to keep the backend from
// including a server header for one comparison.
constexpr int kServerDead = 0; // ss_dead

// A demo that never comes up, or never ends, is a failed run rather than a reason
// to sit there forever. Both are far past anything the stock demos take, so if
// either fires something is actually wrong.
constexpr int kLoadTimeoutMs = 90 * 1000;
constexpr int kPlayTimeoutMs = 10 * 60 * 1000;

enum class State
{
    Idle,    // Cvars not applied yet; set them and drop whatever is running.
    Killing, // Waiting for the startup server to go away.
    Loading, // demomap issued, waiting for the demo's server to come up.
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

void StartDemo()
{
    Com_Printf("PerfRun: [%d/%d] playing %s\n",
               s_nextDemo + 1, ArrayLength(kDemos), kDemos[s_nextDemo]);

    Cbuf_AddText(va("demomap %s\n", kDemos[s_nextDemo]));

    s_issuedAtMs = Sys_Milliseconds();
    s_state      = State::Loading;
}

// Moves past the demo just finished, or ends the run if that was the last one.
void NextDemoOrFinish()
{
    ++s_nextDemo;
    if (s_nextDemo < ArrayLength(kDemos))
    {
        StartDemo();
        return;
    }

    Com_Printf("PerfRun: complete - %d of %d demos played, %d timed out.\n",
               ArrayLength(kDemos) - s_failed, ArrayLength(kDemos), s_failed);

    // The frame log writes in batches, so the tail of the run is still buffered.
    // This also marks the end, which is what tells a completed capture apart from
    // one the emulator cut short.
    ps2::debug::FrameLogFinish();

    // Disarm before quitting, so the config the quit writes has it back at 0. A
    // run that force-quits the program on startup must not be able to leave the
    // build in a state where it does that every launch - arming it is a one line
    // edit, digging back out of it should not need one. A run that never reaches
    // here (a crash, or the emulator being closed) deliberately stays armed.
    Cvar_Set("ps2_perftest", "0");

    // "quit" through the command buffer rather than Sys_Quit() directly: this runs
    // from inside PS2_EndFrame with the renderer mid-frame on the stack, and
    // Qcommon_Shutdown would tear it down underneath itself. The buffer runs the
    // command from the top of the next Qcommon_Frame, where CL_Quit_f can shut
    // down in the usual order.
    Cbuf_AddText("quit\n");
    s_done = true;
}

} // namespace

void RunPerfTest()
{
    // Archived, because there is no command line on this target: the only way to
    // arm a run before the first frame is a line in config.cfg, and the game
    // rewrites that file from the archived cvars on the way out. Without the flag
    // the run's own quit would erase the line that started it.
    static const cvar_t * s_enabled = Cvar_Get("ps2_perftest", "0", CVAR_ARCHIVE);

    if (s_enabled->value == 0.0f || s_done)
    {
        return;
    }

    switch (s_state)
    {
    case State::Idle:
        // Set directly rather than through the command buffer: both have to be in
        // effect before the first measured frame, and a queued command would not
        // run until the next one.
        //
        // developer 0 keeps Com_DPrintf out of the run - each line is a round trip
        // to the IOP, which lands in the timings as a spike in whatever frame it
        // happened to fall in.
        Cvar_Set("developer", "0");
        Cvar_Set("ps2_frame_log", "1");

        Com_Printf("PerfRun: starting - %d demos, developer 0, frame log on.\n", ArrayLength(kDemos));

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
            break;
        }
        if ((Sys_Milliseconds() - s_issuedAtMs) > kLoadTimeoutMs)
        {
            Com_Printf("PerfRun: '%s' never came up after %d seconds - moving on.\n",
                       kDemos[s_nextDemo], kLoadTimeoutMs / 1000);
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
                       kDemos[s_nextDemo], kPlayTimeoutMs / (60 * 1000));
            ++s_failed;
            NextDemoOrFinish();
        }
        break;
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
