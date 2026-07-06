
/* ================================================================================================
 * -*- C -*-
 * File: test_draw2d.c
 * Author: Guilherme R. Lampert
 * Created on: 17/10/15
 * Brief: Tests for the 2D drawing functions of the Refresh module.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "client/client.h"
#include "ps2/ref_ps2.h"
#include "ps2/mem_alloc.h"

// Functions exported from this file:
void Test_PS2_Draw2D(void);
void Test_PS2_Cinematics(void);
void Test_PS2_QuakeMenus(void);

//=============================================================================
//
// Img_ScrapAlloc tests:
//
//=============================================================================

static ps2_teximage_t * scrap_tex_0 = NULL;
static ps2_teximage_t * scrap_tex_1 = NULL;
static ps2_teximage_t * scrap_tex_2 = NULL;
static ps2_teximage_t * scrap_tex_3 = NULL;

static byte * CheckerPattern(byte color0, byte color1, int size, byte * buffer)
{
    int x, y, colorindex;
    const int checkersize = size / 4;
    const byte colors[2]  = { color0, color1 };

    for (y = 0; y < size; ++y)
    {
        for (x = 0; x < size; ++x)
        {
            colorindex = ((y / checkersize) + (x / checkersize)) % 2;
            buffer[x + (y * size)] = colors[colorindex];
        }
    }

    return buffer;
}

static void InitTestScraps(void)
{
    // These get copied into the scrap.
    byte scrap_test_0[24 * 24] PS2_ALIGN(16);
    byte scrap_test_1[32 * 32] PS2_ALIGN(16);
    byte scrap_test_2[64 * 64] PS2_ALIGN(16);
    byte scrap_test_3[16 * 16] PS2_ALIGN(16);

    // Color values are indexed into the global_palette.
    scrap_tex_0 = Img_ScrapAlloc(CheckerPattern(50,  65,  24, scrap_test_0), 24, 24, "scrap_test_0");
    scrap_tex_1 = Img_ScrapAlloc(CheckerPattern(70,  85,  32, scrap_test_1), 32, 32, "scrap_test_1");
    scrap_tex_2 = Img_ScrapAlloc(CheckerPattern(90,  110, 64, scrap_test_2), 64, 64, "scrap_test_2");
    scrap_tex_3 = Img_ScrapAlloc(CheckerPattern(150, 210, 16, scrap_test_3), 16, 16, "scrap_test_3");

    if (scrap_tex_0 == NULL) { Sys_Error("scrap_tex_0 not allocated!"); }
    if (scrap_tex_1 == NULL) { Sys_Error("scrap_tex_1 not allocated!"); }
    if (scrap_tex_2 == NULL) { Sys_Error("scrap_tex_2 not allocated!"); }
    if (scrap_tex_3 == NULL) { Sys_Error("scrap_tex_3 not allocated!"); }
}

static void DrawTestScraps(void)
{
    PS2_DrawFill(0, 3, 105, 13, 0);
    PS2_DrawString(10, 5, "Scraps:");

    PS2_DrawStretchTexImage(10,  20, 60, 60, scrap_tex_0);
    PS2_DrawStretchTexImage(80,  20, 60, 60, scrap_tex_1);
    PS2_DrawStretchTexImage(150, 20, 60, 60, scrap_tex_2);
    PS2_DrawStretchTexImage(220, 20, 60, 60, scrap_tex_3);
}

//=============================================================================
//
// DrawPic / DrawFill / DrawString tests:
//
//=============================================================================

static void DrawFillTests(void)
{
    PS2_DrawFill(0, 93, 105, 13, 0);
    PS2_DrawString(10, 95, "Draw Fill:");

    PS2_DrawFill(10,  110, 60, 60, 54);
    PS2_DrawFill(80,  110, 60, 60, 116);
    PS2_DrawFill(150, 110, 60, 60, 22);
    PS2_DrawFill(220, 110, 60, 60, 202);
}

static void DrawPicTests(void)
{
    PS2_DrawFill(0, 183, 105, 13, 0);
    PS2_DrawString(10, 185, "Draw Pic:");

    PS2_DrawStretchPic(10,  200, 60, 60, "debug");
    PS2_DrawStretchPic(80,  200, 60, 60, "backtile");
    PS2_DrawStretchPic(150, 200, 60, 60, "conchars");
    PS2_DrawStretchPic(220, 200, 60, 60, "help");
}

static void DrawStringTests(void)
{
    // A darker background around the text so it stands out more
    PS2_DrawStretchPic(10, 270, 270, viddef.height - 270 - 10, "conback");

    PS2_DrawString(10, 300,
        "This is a test for the 2D drawing\n"
        "functions of the Quake 2 Engine.\n");

    PS2_DrawAltString(25, 380, "This is a green text string.");
}

static void DrawMiscTests(void)
{
    PS2_DrawString(350, 70, "Draw \nTile Clear");
    PS2_DrawTileClear(300, 35, viddef.width - 300 - 10, 100, "backtile");

    PS2_DrawFill(300, 150, 60, 60, 54);
    PS2_DrawFill(330, 150, 60, 60, 116);
    PS2_DrawFill(360, 150, 60, 60, 22);
    PS2_DrawFill(390, 150, 60, 60, 202);

    /* Logo looks broken in the string because of the escaped backslashes.
       This is how it should print, more or less:

         /\       /\
        / /       \ \
       / /         \ \
      | |           | |
      | |  ___ ___  | |
      \ \  | | | |  / /
       \ \_| |_| |_/ /
        \__| |_| |__/
           | | | |
           | | | |
           | | | |
            V   V
    */
    PS2_DrawPic(300, 230, "inventory");
    PS2_DrawAltString(315, 260,
        "   /\\       /\\    \n"
        "  / /       \\ \\   \n"
        " / /         \\ \\  \n"
        "| |           | |   \n"
        "| |  ___ ___  | |   \n"
        "\\ \\  | | | |  / / \n"
        " \\ \\_| |_| |_/ /  \n"
        "  \\__| |_| |__/    \n"
        "     | | | |        \n"
        "     | | | |  QPS2 - Quake 2 \n"
        "     | | | |  on the PS2     \n"
        "      V   V         \n"
    );

    // Move pic up and down in the corner.
    static int y = 80;
    static int y_incr = 1;
    PS2_DrawPic(viddef.width - 70, viddef.height - y, "debug");
    y += y_incr;
    if (y > 300)
    {
        y_incr = (y_incr > 0) ? -1 : 1;
    }
    else if (y < 64)
    {
        y_incr = (y_incr > 0) ? -1 : 1;
    }
}

//=============================================================================
//
// Test_PS2_Draw2D -- Miscellaneous 2D drawing tests:
//
//=============================================================================

void Test_PS2_Draw2D(void)
{
    Com_Printf("====== QPS2 - Test_PS2_Draw2D ======\n");

    // Clear the screen to dark gray. Default color is black.
    PS2_SetClearColor(120, 120, 120);

    InitTestScraps();

    for (;;)
    {
        PS2_BeginFrame(0);

        DrawTestScraps();
        DrawFillTests();
        DrawPicTests();
        DrawStringTests();
        DrawMiscTests();

        PS2_EndFrame();
    }
}

//=============================================================================
//
// Test_PS2_Cinematics -- Plays all Quake2 cinematic sequences, then exists
// to the Quake console. NOTE: Assumes running from the USB drive!
//
//=============================================================================

// "mass:/" assumes we are loading directly from the USB drive.
#define CIN_PATH "mass:/video/"
static const char * cinematics_files[] =
{
    CIN_PATH "idlog.cin",
    CIN_PATH "ntro.cin" ,
    CIN_PATH "eou1_.cin",
    CIN_PATH "eou2_.cin",
    CIN_PATH "eou3_.cin",
    CIN_PATH "eou4_.cin",
    CIN_PATH "eou5_.cin",
    CIN_PATH "eou6_.cin",
    CIN_PATH "eou7_.cin",
    CIN_PATH "eou8_.cin",
    CIN_PATH "end.cin"
};
#undef CIN_PATH

static int next_cinematic = 0;
enum { NUM_CINEMATICS = sizeof(cinematics_files) / sizeof(cinematics_files[0]) };

static qboolean StartNextCinematic(void)
{
    return CinematicTest_PlayDirect(cinematics_files[next_cinematic++]);
}

void Test_PS2_Cinematics(void)
{
    Com_Printf("====== QPS2 - Test_PS2_Cinematics ======\n");

    qboolean cin_ok = StartNextCinematic();
    if (!cin_ok)
    {
        Sys_Error("Failed to load first cinematic! Aborting.");
    }

    for (;;)
    {
        PS2_BeginFrame(0);

        // Play until we have gone through all of them.
        // Once the last is played, just render the game console.
        if (cin_ok)
        {
            cin_ok = CinematicTest_RunFrame();
        }
        else
        {
            if (next_cinematic < NUM_CINEMATICS)
            {
                cin_ok = StartNextCinematic();
            }
            else
            {
                SCR_RunConsole();
                Con_DrawConsole(1.0f); // Full screen console.
            }
        }

        PS2_EndFrame();
    }
}

//=============================================================================
//
// Test_PS2_QuakeMenus -- Cycles through the Quake2 menu screens.
//
//=============================================================================

static const char * all_menu_names[] =
{
    "menu_main",
    "menu_game",
    "menu_loadgame",
    "menu_savegame",
    "menu_joinserver",
    "menu_addressbook",
    "menu_startserver",
    "menu_dmoptions",
    "menu_playerconfig",
    "menu_downloadoptions",
    "menu_credits",
    "menu_multiplayer",
    "menu_video",
    "menu_options",
    "menu_keys",
    "menu_quit"
};

enum
{
    NUM_MENUS = sizeof(all_menu_names) / sizeof(all_menu_names[0]),
    MENU_MSEC = 6 * 1000
};

static int next_menu = 0;
static int time_til_next_menu = MENU_MSEC;

void Test_PS2_QuakeMenus(void)
{
    Com_Printf("====== QPS2 - Test_PS2_Cinematics ======\n");

    int time;
    int oldtime;
    int newtime;

    Cbuf_AddText(all_menu_names[next_menu]);
    oldtime = Sys_Milliseconds();

    for (;;)
    {
        do
        {
            newtime = Sys_Milliseconds();
            time = newtime - oldtime;
        } while (time < 1);

        Qcommon_Frame(time);
        oldtime = newtime;

        if (time_til_next_menu <= 0 && next_menu < NUM_MENUS)
        {
            Cbuf_AddText(all_menu_names[next_menu++]);
            time_til_next_menu = MENU_MSEC;
        }
        else if (next_menu >= NUM_MENUS)
        {
            Sys_Error("Menu cycle test completed. Exiting...");
        }

        time_til_next_menu -= time;
    }
}
