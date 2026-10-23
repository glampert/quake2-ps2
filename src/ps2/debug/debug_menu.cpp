/* ================================================================================================
 * File: debug_menu.cpp
 * Brief: The Options menu's "debug" page. See debug_menu.h.
 *
 *  Each row does what a console command or cvar does, so the page adds nothing a keyboard
 *  couldn't reach: it only puts it under the gamepad. A row that prints a report opens the
 *  console on it as well, keys and all, so the report can be read and scrolled (L2/R2) at once.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/debug_menu.h"
#include "ps2/system/iop_boot.h"

// Menu code is client code: it opens the console through cls/cl, as menu.c does. The legacy
// headers redeclare a few q_common.h functions, hence the pragma.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
    #include "client/qmenu.h" // The menu framework (qmenu.c) every stock menu is built on.
}
#pragma GCC diagnostic pop

#include <cstdio>
#include <cstring>

extern "C" {
// Menu and client functions no engine header declares (menu.c and keys.c keep their own).
void M_PushMenu(void (*draw)(void), const char * (*key)(int k));
const char * Default_MenuKey(menuframework_s * menu, int key);
void Key_ClearTyping(void);
int PS2_GodModeOn(void); // g_cmds.c
} // extern "C"

namespace ps2::debug {
namespace {

// The spin control's labels; qmenu takes them unconst.
static const char * s_offOn[] = { "off", "on", nullptr };

static menuframework_s s_menu;
static menulist_s      s_logFileSpin;
static menuaction_s    s_dumpIopAction;
static menuaction_s    s_saveInfoAction;
static menuaction_s    s_dumpConsoleAction;
static menuaction_s    s_screenshotAction;
static menulist_s      s_godModeSpin;
static menuaction_s    s_giveAllAction;
#if PS2_QUAKE_PROFILE || PS2_QUAKE_DEBUG
static menulist_s      s_testQuitSpin;
#endif // PS2_QUAKE_PROFILE || PS2_QUAKE_DEBUG
#if PS2_QUAKE_PROFILE
static menuaction_s    s_perfTestAction;
#endif // PS2_QUAKE_PROFILE
#if PS2_QUAKE_DEBUG
static menuaction_s    s_mapCycleAction;
#endif // PS2_QUAKE_DEBUG

// What the last row picked did, under the menu, for the rows that stay on the page. Empty for none.
static char s_feedback[MAX_OSPATH + 16]; // Room for "couldn't write " and a path.
static int  s_feedbackY = 0;

// The god mode the switch last showed, so a press past either end (qmenu still calls back on it)
// doesn't send another toggle.
static int s_godModeShown = 0;

// Com_ServerState() hands back a server_state_t (server/server.h) as a plain int, as perf_run.cpp
// has it. Not worth a server header for one comparison.
constexpr int kServerGame = 2; // ss_game

// ------------------------------------------------------------------------------------------------
// Rows
// ------------------------------------------------------------------------------------------------

// Puts the console up with the keys on it, for a report just printed there. Over the attract loop
// it can't be: Key_Event turns every key into Escape while a demo plays. So the demo is stopped
// first, its server and then the client, the order a dropped game takes them in (Com_Error). The
// Options menu's "open console" only stops the demo, and takes a second visit for the console.
// A loading plaque still up goes first, as the console toggle has it: until it comes down nothing
// is drawn, and nothing that runs off a frame (the tests) runs, short of its 150 s timeout.
void ShowConsole()
{
    SCR_EndLoadingPlaque();

    if (cl.attractloop)
    {
        Cmd_ExecuteString("killserver");
        CL_Disconnect();
    }

    Key_ClearTyping();
    Con_ClearNotify();
    M_ForceMenuOff();
    cls.key_dest = key_console;

    // As the console toggle does, so a single player game doesn't go on under it.
    if (Cvar_VariableValue("maxclients") == 1.0f && Com_ServerState())
    {
        Cvar_Set("paused", "1");
    }
}

void LogFileChanged(void * self)
{
    const menulist_s & spin = *static_cast<const menulist_s *>(self);
    Cvar_Set("ps2_logfile", (spin.curvalue != 0) ? "1" : "0");
    s_feedback[0] = '\0';
}

void DumpIopModules(void *)
{
    // Printed first, so it is all on the console when it opens.
    Cmd_ExecuteString("ps2_dump_iop_mods");
    ShowConsole();
}

void ListSaves(void *)
{
    Cmd_ExecuteString("ps2_saveinfo");
    ShowConsole();
}

// id's condump, then the HDD's metadata flush every file the game writes there needs (see
// SyncGameDataDevice). condump reports a failed open only on the console, so the file is looked
// for afterwards to say which it was here.
void DumpConsole(void *)
{
    constexpr const char * kDumpName = "console";
    Cmd_ExecuteString(va("condump %s", kDumpName));
    ps2::sys::SyncGameDataDevice();

    char path[MAX_OSPATH];
    std::snprintf(path, sizeof(path), "%s/%s.txt", FS_Gamedir(), kDumpName);

    std::FILE * file = std::fopen(path, "rb");
    if (file != nullptr)
    {
        std::fclose(file);
    }
    std::snprintf(s_feedback, sizeof(s_feedback), "%s %s", (file != nullptr) ? "wrote" : "couldn't write", path);
}

// Closes the menu and saves the screen without it (renderer/screenshot.h). The command reads back
// the last frame finished, which still has the menu on it, so it waits a frame: by the next
// Cbuf_Execute this frame has been drawn without the menu.
void TakeScreenshot(void *)
{
    M_ForceMenuOff();
    Cbuf_AddText("wait\nscreenshot\n");
}

// The cheat rows act on the player of a game in progress: one the server runs, not a demo or a
// cinematic, with the client in it. They go through the game's own console commands, so its
// rule holds too: a deathmatch needs "cheats 1". A row refused here says why under the menu.
bool InGame()
{
    return Com_ServerState() == kServerGame && cls.state == ca_active;
}

bool CheatsAllowed()
{
    if (!InGame())
    {
        std::snprintf(s_feedback, sizeof(s_feedback), "start a game first");
        return false;
    }
    if (Cvar_VariableValue("deathmatch") != 0.0f && Cvar_VariableValue("cheats") == 0.0f)
    {
        std::snprintf(s_feedback, sizeof(s_feedback), "deathmatch needs cheats 1 for this");
        return false;
    }
    return true;
}

// "god" toggles, so it is sent only when the switch moved. The server runs it next frame, even
// with the game paused under the menu.
void GodModeChanged(void * self)
{
    menulist_s & spin = *static_cast<menulist_s *>(self);
    s_feedback[0] = '\0';

    if (spin.curvalue == s_godModeShown)
    {
        return;
    }
    if (!CheatsAllowed())
    {
        spin.curvalue = s_godModeShown;
        return;
    }

    Cmd_ExecuteString("god");
    s_godModeShown = spin.curvalue;
}

// Every weapon, 1000 of each ammo (up to what the player can carry), body armor, the power
// shield, full health and one of every other item, keys included.
void GiveAll(void *)
{
    if (CheatsAllowed())
    {
        Cmd_ExecuteString("give all");
        std::snprintf(s_feedback, sizeof(s_feedback), "gave all weapons and items");
    }
}

#if PS2_QUAKE_PROFILE || PS2_QUAKE_DEBUG
void TestQuitChanged(void * self)
{
    const menulist_s & spin = *static_cast<const menulist_s *>(self);
    Cvar_Set("ps2_test_quit", (spin.curvalue != 0) ? "1" : "0");
    s_feedback[0] = '\0';
}

// Starts a test that drives the game through the command buffer by its cvar, and closes the
// menu to let it. The two would fight over the server (perf_run.h), so neither starts while the
// other runs.
void StartTest(const char * cvarName)
{
    if (Cvar_VariableValue("ps2_perftest") != 0.0f || Cvar_VariableValue("ps2_testmaps") != 0.0f)
    {
        std::snprintf(s_feedback, sizeof(s_feedback), "a test is running already");
        return;
    }

    Cvar_Set(cvarName, "1");
    M_ForceMenuOff();
}
#endif // PS2_QUAKE_PROFILE || PS2_QUAKE_DEBUG

#if PS2_QUAKE_PROFILE
void StartPerfTest(void *)
{
    StartTest("ps2_perftest");
}
#endif // PS2_QUAKE_PROFILE

#if PS2_QUAKE_DEBUG
void StartMapCycle(void *)
{
    StartTest("ps2_testmaps");
}
#endif // PS2_QUAKE_DEBUG

// ------------------------------------------------------------------------------------------------
// Page
// ------------------------------------------------------------------------------------------------

void AddAction(menuaction_s & action, const int y, const char * name, const char * statusBar,
               void (*callback)(void *))
{
    action = {};
    action.generic.type      = MTYPE_ACTION;
    action.generic.y         = y;
    action.generic.name      = name;
    action.generic.statusbar = statusBar;
    action.generic.callback  = callback;
    Menu_AddItem(&s_menu, &action);
}

// An off/on spin control; left and right change it.
void AddSpin(menulist_s & spin, const int y, const char * name, const char * statusBar, const bool on,
             void (*callback)(void *))
{
    spin = {};
    spin.generic.type      = MTYPE_SPINCONTROL;
    spin.generic.y         = y;
    spin.generic.name      = name;
    spin.generic.statusbar = statusBar;
    spin.generic.callback  = callback;
    spin.itemnames         = s_offOn;
    spin.curvalue          = on ? 1 : 0;
    Menu_AddItem(&s_menu, &spin);
}

// Status bar text stays within 64 columns, the narrowest (512 pixel) screen.
void Init()
{
    s_menu = {};
    s_menu.x = viddef.width / 2;
    s_menu.y = (viddef.height / 2) - 58;
    s_feedback[0] = '\0';

    int y = 0;

    AddSpin(s_logFileSpin, y, "log to file", "copy the console to quake2.log by baseq2/; slow on USB",
            Cvar_VariableValue("ps2_logfile") != 0.0f, LogFileChanged);

    AddAction(s_dumpIopAction, y += 10, "dump IOP modules",
              "the IOP boot report, modules and free memory, on the console", DumpIopModules);
    AddAction(s_saveInfoAction, y += 10, "list saves",
              "the save working set and the files on the save device", ListSaves);
    AddAction(s_dumpConsoleAction, y += 10, "save console to file",
              "write the console's text to console.txt in baseq2/", DumpConsole);
    AddAction(s_screenshotAction, y += 10, "take screenshot",
              "close the menu, then save the screen to baseq2/scrnshot/", TakeScreenshot);

    y += 10; // A blank row between groups, as on the video page.
    s_godModeShown = (InGame() && (PS2_GodModeOn() != 0)) ? 1 : 0;
    AddSpin(s_godModeSpin, y += 10, "god mode", "take no damage; needs a game, and cheats 1 in deathmatch",
            s_godModeShown != 0, GodModeChanged);
    AddAction(s_giveAllAction, y += 10, "give all", "every weapon, ammo, armor and item to the player",
              GiveAll);

#if PS2_QUAKE_PROFILE || PS2_QUAKE_DEBUG
    y += 10;
    AddSpin(s_testQuitSpin, y += 10, "quit after a test",
            "quit the game when a test ends; off stays up for the next one",
            Cvar_VariableValue("ps2_test_quit") != 0.0f, TestQuitChanged);
#endif // PS2_QUAKE_PROFILE || PS2_QUAKE_DEBUG
#if PS2_QUAKE_PROFILE
    AddAction(s_perfTestAction, y += 10, "start perf test",
              "play the attract demos with the frame log on, then quit", StartPerfTest);
#endif // PS2_QUAKE_PROFILE
#if PS2_QUAKE_DEBUG
    AddAction(s_mapCycleAction, y += 10, "start map cycle",
              "load all 39 maps, logging what each one takes, then quit", StartMapCycle);
#endif // PS2_QUAKE_DEBUG

    s_feedbackY = s_menu.y + y + 20;
}

void Draw()
{
    // There is no debug banner among the game's pictures: the page sits under the Options one.
    int w = 0;
    int h = 0;
    re.DrawGetPicSize(&w, &h, "m_banner_options");
    re.DrawPic((viddef.width / 2) - (w / 2), (viddef.height / 2) - 110, "m_banner_options");

    Menu_AdjustCursor(&s_menu, 1);
    Menu_Draw(&s_menu);

    if (s_feedback[0] != '\0')
    {
        const int len = static_cast<int>(std::strlen(s_feedback));
        Menu_DrawStringDark((viddef.width / 2) - (len * 4), s_feedbackY, s_feedback);
    }
}

const char * Key(const int key)
{
    return Default_MenuKey(&s_menu, key);
}

} // namespace
} // namespace ps2::debug

extern "C" void PS2_DebugMenuOpen()
{
    ps2::debug::Init();
    M_PushMenu(ps2::debug::Draw, ps2::debug::Key);
}
