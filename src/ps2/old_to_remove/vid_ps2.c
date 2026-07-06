
/* ================================================================================================
 * -*- C -*-
 * File: vid_ps2.c
 * Author: Guilherme R. Lampert
 * Created on: 13/10/15
 * Brief: Implementation of Quake2 VID_ functions (video module) for the PS2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/ref_ps2.h"

// Global video state (used by the game):
viddef_t viddef = {0};
refexport_t re  = {0};

//=============================================================================
//
// Video startup / basic screen IO:
//
//=============================================================================

/*
================
VID_MenuInit
================
*/
void VID_MenuInit(void)
{
    // TODO
    // Video config menu for Quake2.
}

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw(void)
{
    // TODO
    // Video config menu for Quake2.
}

/*
================
VID_MenuKey
================
*/
const char * VID_MenuKey(int k)
{
    // TODO
    return NULL; // Video config menu for Quake2.
}

/*
================
VID_Init
================
*/
void VID_Init(void)
{
    Com_DPrintf("---- VID_Init ----\n");

    //
    // We don't need to interface with the Quake2 Engine
    // via the refimport_t function table, since the PS2
    // Refresh module is statically linked with the game.
    // These functions can also be set directly, instead
    // of using the GetRefAPI function like the ref_gl
    // and ref_soft DLLs did. The game will still talk
    // to the renderer via these function pointers, so
    // we won't have to replace renderer calls in the
    // game code.
    //
    re.api_version         = REF_API_VERSION;
    re.Init                = &PS2_RendererInit;
    re.Shutdown            = &PS2_RendererShutdown;
    re.BeginRegistration   = &PS2_BeginRegistration;
    re.RegisterModel       = &PS2_RegisterModel;
    re.RegisterSkin        = &PS2_RegisterSkin;
    re.RegisterPic         = &PS2_RegisterPic;
    re.SetSky              = &PS2_SetSky;
    re.EndRegistration     = &PS2_EndRegistration;
    re.RenderFrame         = &PS2_RenderFrame;
    re.DrawGetPicSize      = &PS2_DrawGetPicSize;
    re.DrawPic             = &PS2_DrawPic;
    re.DrawStretchPic      = &PS2_DrawStretchPic;
    re.DrawChar            = &PS2_DrawChar;
    re.DrawTileClear       = &PS2_DrawTileClear;
    re.DrawFill            = &PS2_DrawFill;
    re.DrawFadeScreen      = &PS2_DrawFadeScreen;
    re.DrawStretchRaw      = &PS2_DrawStretchRaw;
    re.CinematicSetPalette = &PS2_CinematicSetPalette;
    re.BeginFrame          = &PS2_BeginFrame;
    re.EndFrame            = &PS2_EndFrame;
    re.AppActivate         = &PS2_AppActivate;

    PS2_RendererInit(NULL, NULL);
    Com_DPrintf("---- VID_Init completed! ----\n");
}

/*
================
VID_Shutdown
================
*/
void VID_Shutdown(void)
{
    PS2_RendererShutdown();
}
