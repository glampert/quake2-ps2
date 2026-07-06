/* ================================================================================================
 * File: ref.cpp
 * Brief: The refexport_t implementation - the functions the Quake II client calls
 *        to draw. This first pass brings up the GS and implements the untextured
 *        2D path (screen clear, solid fills, fade). Textured 2D (chars/pics),
 *        cinematics and the 3D world are stubbed and land in the next milestones.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/qcommon.h"
#include "ps2/renderer/gs.h"

#include <tamtypes.h>

extern "C" {

// 256-entry RGBA palette (0xAABBGGRR) embedded in ps2/builtin/palette.c.
extern const u32 global_palette[256];

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

qboolean PS2_RefInit(void * hinstance, void * wndproc)
{
    (void)hinstance;
    (void)wndproc;

    ps2::gs::Init();
    viddef.width  = ps2::gs::Width();
    viddef.height = ps2::gs::Height();

    Com_Printf("PS2 refresh initialised: %dx%d\n", viddef.width, viddef.height);
    return true;
}

void PS2_RefShutdown() {}

// ------------------------------------------------------------------------------------------------
// Registration (no asset residency yet)
// ------------------------------------------------------------------------------------------------

void PS2_BeginRegistration(const char * map_name) { (void)map_name; }
struct model_s * PS2_RegisterModel(const char * name) { (void)name; return nullptr; }
struct image_s * PS2_RegisterSkin(const char * name) { (void)name; return nullptr; }
struct image_s * PS2_RegisterPic(const char * name)  { (void)name; return nullptr; }
void PS2_SetSky(const char * name, float rotate, vec3_t axis) { (void)name; (void)rotate; (void)axis; }
void PS2_EndRegistration() {}

// ------------------------------------------------------------------------------------------------
// 3D (not yet implemented)
// ------------------------------------------------------------------------------------------------

void PS2_RenderFrame(refdef_t * fd) { (void)fd; }

// ------------------------------------------------------------------------------------------------
// 2D overlay
// ------------------------------------------------------------------------------------------------

void PS2_DrawGetPicSize(int * w, int * h, const char * name)
{
    (void)name;
    // No pic residency yet: report "not found".
    *w = -1;
    *h = -1;
}

void PS2_DrawPic(int x, int y, const char * name) { (void)x; (void)y; (void)name; }
void PS2_DrawStretchPic(int x, int y, int w, int h, const char * name)
{
    (void)x; (void)y; (void)w; (void)h; (void)name;
}
void PS2_DrawChar(int x, int y, int c) { (void)x; (void)y; (void)c; }

void PS2_DrawTileClear(int x, int y, int w, int h, const char * name)
{
    (void)name;
    ps2::gs::FillRect(x, y, w, h, 0, 0, 0, 255);
}

void PS2_DrawFill(int x, int y, int w, int h, int c)
{
    const std::uint32_t p = global_palette[c & 0xFF];
    const std::uint8_t  r = static_cast<std::uint8_t>(p & 0xFFu);
    const std::uint8_t  g = static_cast<std::uint8_t>((p >> 8) & 0xFFu);
    const std::uint8_t  b = static_cast<std::uint8_t>((p >> 16) & 0xFFu);
    ps2::gs::FillRect(x, y, w, h, r, g, b, 255);
}

void PS2_DrawFadeScreen()
{
    ps2::gs::FillRect(0, 0, ps2::gs::Width(), ps2::gs::Height(), 0, 0, 0, 128);
}

// ------------------------------------------------------------------------------------------------
// Cinematics (stub)
// ------------------------------------------------------------------------------------------------

void PS2_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte * data)
{
    (void)x; (void)y; (void)w; (void)h; (void)cols; (void)rows; (void)data;
}

void PS2_CinematicSetPalette(const unsigned char * palette) { (void)palette; }

// ------------------------------------------------------------------------------------------------
// Frame + app state
// ------------------------------------------------------------------------------------------------

void PS2_BeginFrame(float camera_separation)
{
    (void)camera_separation;
    ps2::gs::BeginFrame();
}

void PS2_EndFrame(void)
{
    ps2::gs::EndFrame();
}

void PS2_AppActivate(qboolean activate) { (void)activate; }

} // extern "C"
