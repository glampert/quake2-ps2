/* ================================================================================================
 * File: vid.cpp
 * Brief: VID_* video-shell seam. Owns the engine's 're' refresh handle and the
 *        'viddef' screen state, and wires the refexport_t table directly to the
 *        PS2_* renderer functions (the PS2 refresh is statically linked, so there
 *        is no GetRefAPI/DLL handshake).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/qcommon.h"

extern "C" {

// refexport_t implementations from ref.cpp:
qboolean PS2_RefInit(void * hinstance, void * wndproc);
void PS2_RefShutdown();
void PS2_BeginRegistration(const char * map_name);
struct model_s * PS2_RegisterModel(const char * name);
struct image_s * PS2_RegisterSkin(const char * name);
struct image_s * PS2_RegisterPic(const char * name);
void PS2_SetSky(const char * name, float rotate, vec3_t axis);
void PS2_EndRegistration();
void PS2_RenderFrame(refdef_t * fd);
void PS2_DrawGetPicSize(int * w, int * h, const char * name);
void PS2_DrawPic(int x, int y, const char * name);
void PS2_DrawStretchPic(int x, int y, int w, int h, const char * name);
void PS2_DrawChar(int x, int y, int c);
void PS2_DrawTileClear(int x, int y, int w, int h, const char * name);
void PS2_DrawFill(int x, int y, int w, int h, int c);
void PS2_DrawFadeScreen();
void PS2_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte * data);
void PS2_CinematicSetPalette(const unsigned char * palette);
void PS2_BeginFrame(float camera_separation);
void PS2_EndFrame();
void PS2_AppActivate(qboolean activate);

// Engine-visible globals (referenced unmangled from the C client).
refexport_t re = {};
viddef_t viddef = {};

void VID_Init()
{
    re.api_version = REF_API_VERSION;

    re.Init                = PS2_RefInit;
    re.Shutdown            = PS2_RefShutdown;
    re.BeginRegistration   = PS2_BeginRegistration;
    re.RegisterModel       = PS2_RegisterModel;
    re.RegisterSkin        = PS2_RegisterSkin;
    re.RegisterPic         = PS2_RegisterPic;
    re.SetSky              = PS2_SetSky;
    re.EndRegistration     = PS2_EndRegistration;
    re.RenderFrame         = PS2_RenderFrame;
    re.DrawGetPicSize      = PS2_DrawGetPicSize;
    re.DrawPic             = PS2_DrawPic;
    re.DrawStretchPic      = PS2_DrawStretchPic;
    re.DrawChar            = PS2_DrawChar;
    re.DrawTileClear       = PS2_DrawTileClear;
    re.DrawFill            = PS2_DrawFill;
    re.DrawFadeScreen      = PS2_DrawFadeScreen;
    re.DrawStretchRaw      = PS2_DrawStretchRaw;
    re.CinematicSetPalette = PS2_CinematicSetPalette;
    re.BeginFrame          = PS2_BeginFrame;
    re.EndFrame            = PS2_EndFrame;
    re.AppActivate         = PS2_AppActivate;

    if (!re.Init(nullptr, nullptr))
    {
        Sys_Error("VID_Init: PS2 refresh failed to initialise!");
    }
}

void VID_Shutdown()
{
    if (re.Shutdown != nullptr)
    {
        re.Shutdown();
    }

    re = {};
    viddef = {};
}

// The refresh is statically linked and never swapped at runtime,
// so there is nothing to reload here.
void VID_CheckChanges() {}

// Video menu is not implemented yet.
void VID_MenuInit() {}
void VID_MenuDraw() {}
const char * VID_MenuKey(int key) { (void)key; return nullptr; }

} // extern "C"
