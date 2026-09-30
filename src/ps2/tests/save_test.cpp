/* ================================================================================================
 * File: save_test.cpp
 * Brief: End-to-end save game test. See save_test.h.
 *
 *  Like the map cycle, it only queues console commands - the ones a player's actions produce -
 *  and reads the results off the client and the save system between frames. The player's
 *  health is the marker: set with "give health N" (hence cheats 1), saved, changed, and
 *  expected back at the saved value after a load, or carried over by a level change.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/common.h"
#include "ps2/tests/save_test.h"
#include "ps2/save/working_set.h"

// Client state, as in input/rumble.cpp; the legacy headers redeclare a few q_common.h functions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
}
#pragma GCC diagnostic pop

#include <cstring>

namespace ps2::test {
namespace {

// How a step knows its command has taken effect.
enum class Wait
{
    Frames, // A fixed number of frames: commands forwarded to the server, a save.
    InGame, // The client is in the game on `map`, the loading plaque gone.
};

struct Step
{
    const char * what;
    const char * command;
    Wait         wait;
    const char * map;          // Wait::InGame
    int          health;       // Expected STAT_HEALTH once there; 0 = don't check.
    int          validSlot;    // saveN expected valid on the save device; -1 = don't check.
    const char * entry;        // Expected in the save working set; nullptr = don't check.
    bool         cardOnly;     // Only run with ps2_testsaves 2.
};

constexpr Step kSteps[] = {
    { "start base1 afresh",
      "killserver ; deathmatch 0 ; coop 0 ; maxclients 1 ; skill 1 ; cheats 1 ; ps2_savedevice host ; map base1",
      Wait::InGame, "base1", 0, 0, "game.ssv", false },
    { "set health 37",         "give health 37", Wait::Frames, nullptr, 37, -1, nullptr,     false },
    { "save to slot 7",        "save save7",     Wait::Frames, nullptr, 37,  7, "base1.sav", false },
    { "set health 80",         "give health 80", Wait::Frames, nullptr, 80, -1, nullptr,     false },
    { "change level",          "gamemap base2",  Wait::InGame, "base2", 80,  0, "base1.sav", false },
    { "return to base1",       "gamemap base1",  Wait::InGame, "base1", 80,  0, "base2.sav", false },
    { "load slot 7",           "load save7",     Wait::InGame, "base1", 37, -1, "base1.sav", false },
    { "load the autosave",     "load save0",     Wait::InGame, "base1", 80, -1, "base2.sav", false },
    { "save to memory card",   "ps2_savedevice mc ; save save8", Wait::Frames, nullptr, 80, 8, nullptr, true },
    { "set health 55",         "give health 55", Wait::Frames, nullptr, 55, -1, nullptr,     true },
    { "load from memory card", "load save8",     Wait::InGame, "base1", 80, -1, nullptr,     true },
    { "back to host files",    "ps2_savedevice host", Wait::Frames, nullptr, 0, 7, nullptr,  true },
};

// "ps2_testsaves 3": only loads the slot 7 save a full run left behind - made by an earlier
// build, to check that a rebuild which moves code and data around can still read it.
constexpr Step kLoadOnlySteps[] = {
    { "load slot 7 from an earlier build", "ps2_savedevice host ; load save7", Wait::InGame, "base1", 37, -1, "base1.sav", false },
};

constexpr int kFramesPerStep = 60;        // ~1 s: long enough for a forwarded command's result.
constexpr int kStartDelayMs  = 3000;      // Let the attract loop's first demo get going.
constexpr int kTimeoutMs     = 120 * 1000;

static const Step * s_steps  = kSteps;
static int  s_numSteps       = ps2::ArrayLength(kSteps);
static int  s_step           = -1; // -1: waiting to start.
static bool s_done           = false;
static bool s_stepIssued     = false;
static int  s_frames         = 0;
static int  s_issuedAtMs     = 0;
static int  s_startAtMs      = 0;
static int  s_failures       = 0;

bool InGameOn(const char * map)
{
    char bsp[MAX_QPATH];
    std::snprintf(bsp, sizeof(bsp), "maps/%s.bsp", map);

    return cls.state == ca_active && cl.refresh_prepped && cls.disable_screen == 0.0f &&
           std::strcmp(cl.configstrings[CS_MODELS + 1], bsp) == 0;
}

bool SlotValid(const int slot)
{
    saveslotinfo_t info[10];
    Sys_SaveListSlots("save", ps2::ArrayLength(info), info);
    return slot >= 0 && slot < ps2::ArrayLength(info) && info[slot].state == SAVESLOT_VALID;
}

void Check(const bool ok, const Step & step, const char * what)
{
    if (!ok)
    {
        ++s_failures;
        Com_Printf("SaveTest: FAIL [%s] %s\n", step.what, what);
    }
}

void Finish()
{
    ps2::save::PrintEntries();

    const ps2::heap::MemStats & stats = ps2::heap::GetStatsForMemTag(ps2::heap::MemTag::SaveData);
    Com_Printf("SaveTest: SaveData now %u bytes, peak %u bytes\n",
               static_cast<unsigned>(stats.totalBytes), static_cast<unsigned>(stats.peakBytes));
    Com_Printf("SaveTest: %s (%d failure%s)\n", (s_failures == 0) ? "PASS" : "FAIL",
               s_failures, (s_failures == 1) ? "" : "s");
    s_done = true;
}

void Advance(const bool withCard)
{
    do
    {
        ++s_step;
    } while (s_step < s_numSteps && s_steps[s_step].cardOnly && !withCard);

    s_stepIssued = false;
    s_frames     = 0;
}

} // namespace

void RunSaveTest()
{
    static const cvar_t * s_enabled = Cvar_Get("ps2_testsaves", "0", 0);

    if (s_enabled->value == 0.0f || s_done)
    {
        return;
    }

    const int now = Sys_Milliseconds();
    const bool loadOnly = (s_enabled->value >= 3.0f);
    const bool withCard = (s_enabled->value >= 2.0f) && !loadOnly;

    if (s_step < 0)
    {
        if (s_startAtMs == 0)
        {
            s_startAtMs = now + kStartDelayMs;
            if (loadOnly)
            {
                s_steps    = kLoadOnlySteps;
                s_numSteps = ps2::ArrayLength(kLoadOnlySteps);
            }
            Com_Printf("SaveTest: starting%s\n", loadOnly ? " (loading an earlier build's save)" :
                                                 withCard ? " (host files and memory card)" : " (host files)");
        }
        if (now >= s_startAtMs)
        {
            Advance(withCard);
        }
        return;
    }

    if (s_step >= s_numSteps)
    {
        Finish();
        return;
    }

    const Step & step = s_steps[s_step];

    if (!s_stepIssued)
    {
        Com_Printf("SaveTest: -- %s: %s\n", step.what, step.command);
        Cbuf_AddText(va("%s\n", step.command));
        s_stepIssued = true;
        s_issuedAtMs = now;
        s_frames     = 0;
        return;
    }

    // A message box means something failed and is waiting for a button; nothing will change.
    if (cls.key_dest == key_menu)
    {
        Check(false, step, "a message box came up - see the console");
        M_ForceMenuOff();
        Advance(withCard);
        return;
    }

    ++s_frames;
    const bool arrived = (step.wait == Wait::InGame) ? (InGameOn(step.map) && s_frames >= 10)
                                                     : (s_frames >= kFramesPerStep);
    if (!arrived)
    {
        if (now - s_issuedAtMs > kTimeoutMs)
        {
            Check(false, step, "timed out");
            Finish();
        }
        return;
    }

    const int elapsedMs = now - s_issuedAtMs;
    const int health = cl.frame.playerstate.stats[STAT_HEALTH];

    if (step.health != 0)
    {
        Check(health == step.health, step, va("health %d, expected %d", health, step.health));
    }
    if (step.validSlot >= 0)
    {
        Check(SlotValid(step.validSlot), step, va("slot save%d is not a valid save (%s)", step.validSlot, Sys_SaveDeviceStatus()));
    }
    if (step.entry != nullptr)
    {
        Check(Sys_SaveExists(step.entry) != 0, step, va("no '%s' in the working set", step.entry));
    }

    Com_Printf("SaveTest: done [%s] in %d ms, health %d, device: %s\n",
               step.what, elapsedMs, health, Sys_SaveDeviceStatus());
    Advance(withCard);
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
