/* ================================================================================================
 * File: ref.cpp
 * Brief: The refexport_t implementation - the functions the Quake II client calls
 *        to draw. This pass implements the full 2D overlay path (console, HUD,
 *        menus) on top of the built-in textures: pics, glyphs, tile fills, solid
 *        fills and fades. Cinematics and the 3D world are stubbed and land in
 *        the next milestones.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/tests/draw_cube.h"
#include "ps2/builtin/builtin.h"

#include <cstdio>

namespace {

// Size in pixels of one console font glyph (conchars is a 16x16 grid of these).
constexpr int kGlyphSize = 8;

// Vertex colour applied to textured 2D (GS modulate: 128 = texels unchanged).
constexpr std::uint8_t kUiBrightness = 128;

// Built-ins used every frame, cached at init to skip the name lookup.
static const ps2::tex::Texture * s_texConchars = nullptr;
static const ps2::tex::Texture * s_texBacktile = nullptr;

// A missing image draws as the pink/black checkerboard instead of crashing or
// silently vanishing - obvious on screen, and callers get sane dimensions.
const ps2::tex::Texture * FindPicOrPlaceholder(const char * name)
{
    const ps2::tex::Texture * texture = ps2::tex::Find(name);
    if (texture == nullptr)
    {
        Com_DPrintf("Missing pic '%s', using placeholder.\n", name);
        texture = &ps2::tex::DebugTexture();
    }
    return texture;
}

void DrawGlyph(int x, int y, int c)
{
    // Draws one 8x8 graphics character with 0 being transparent.
    // It can be clipped to the top of the screen to allow the console
    // to be smoothly scrolled off. Based on Draw_Char() from ref_gl.

    c &= 255;

    if ((c & 127) == ' ')
    {
        return; // Whitespace
    }
    if (y <= -kGlyphSize)
    {
        return; // Totally off screen
    }

    const int row = (c >> 4) * kGlyphSize;
    const int col = (c & 15) * kGlyphSize;

    ps2::gs::SetTexture(*s_texConchars);
    ps2::gs::DrawTexturedRect(x, y, kGlyphSize, kGlyphSize,
                              col, row, col + kGlyphSize, row + kGlyphSize, kUiBrightness);
}

void DrawInternalString(int x, int y, const char * str)
{
    const int initialX = x;
    for (; *str != '\0'; ++str)
    {
        DrawGlyph(x, y, *str);
        x += kGlyphSize;
        if (*str == '\n')
        {
            y += kGlyphSize + 2; // 2 pixels of spacing between lines.
            x = initialX;
        }
    }
}

// Frames-per-second counter at the top-right corner of the screen.
// Averages a few frames to smooth changes out a bit.
void DrawFpsCounter()
{
    constexpr int kMaxFpsHist = 4;
    static struct
    {
        int index;
        int count;
        int previousTime;
        int timesHist[kMaxFpsHist];
    } s_fps;

    const int timeMillisec = Sys_Milliseconds(); // Real time clock
    const int frameTime = timeMillisec - s_fps.previousTime;

    s_fps.timesHist[s_fps.index++] = frameTime;
    s_fps.previousTime = timeMillisec;

    if (s_fps.index == kMaxFpsHist)
    {
        int total = 0;
        for (int i = 0; i < kMaxFpsHist; ++i)
        {
            total += s_fps.timesHist[i];
        }
        if (total == 0)
        {
            total = 1;
        }
        s_fps.count = ((1000 * kMaxFpsHist) + (total / 2)) / total;
        s_fps.index = 0;
    }

    char text[32];
    std::snprintf(text, sizeof(text), "FPS %d", s_fps.count);

    // A black background to give the text more contrast.
    ps2::gs::FillRect(viddef.width - 68, 2, 64, 12, 0, 0, 0, 255);
    DrawInternalString(viddef.width - 64, 4, text);
}

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

qboolean PS2_RefInit(void * hinstance, void * wndproc)
{
    (void)hinstance;
    (void)wndproc;

    ps2::gs::Init();
    ps2::tex::Init();
    ps2::vu1::Init();

    s_texConchars = ps2::tex::Find("conchars");
    s_texBacktile = ps2::tex::Find("backtile");
    PS2_Assert(s_texConchars != nullptr && s_texBacktile != nullptr);

    viddef.width  = ps2::gs::Width();
    viddef.height = ps2::gs::Height();

    Com_Printf("PS2 refresh initialised: %dx%d\n", viddef.width, viddef.height);
    return true;
}

void PS2_RefShutdown() {}

// ------------------------------------------------------------------------------------------------
// Registration (no file-loaded assets yet - built-ins only)
// ------------------------------------------------------------------------------------------------

void PS2_BeginRegistration(const char * map_name) { (void)map_name; }
void PS2_EndRegistration() {}

struct model_s * PS2_RegisterModel(const char * name) { (void)name; return nullptr; }
void PS2_SetSky(const char * name, float rotate, vec3_t axis) { (void)name; (void)rotate; (void)axis; }

struct image_s * PS2_RegisterSkin(const char * name)
{
    return const_cast<struct image_s*>(
        reinterpret_cast<const struct image_s *>(FindPicOrPlaceholder(name)));
}

struct image_s * PS2_RegisterPic(const char * name)
{
    return const_cast<struct image_s*>(
        reinterpret_cast<const struct image_s *>(FindPicOrPlaceholder(name)));
}

// ------------------------------------------------------------------------------------------------
// 3D (not yet implemented)
// ------------------------------------------------------------------------------------------------

void PS2_RenderFrame(refdef_t * fd) { (void)fd; }

// ------------------------------------------------------------------------------------------------
// 2D overlay
// ------------------------------------------------------------------------------------------------

void PS2_DrawGetPicSize(int * w, int * h, const char * name)
{
    // Callable outside Begin/EndFrame. Placeholder dimensions keep the
    // callers' centering math sane when the pic is missing.
    const ps2::tex::Texture * texture = FindPicOrPlaceholder(name);
    *w = texture->width;
    *h = texture->height;
}

void PS2_DrawStretchPic(int x, int y, int w, int h, const char * name)
{
    const ps2::tex::Texture * texture = FindPicOrPlaceholder(name);
    ps2::gs::SetTexture(*texture);
    ps2::gs::DrawTexturedRect(x, y, w, h, 0, 0, texture->width, texture->height, kUiBrightness);
}

void PS2_DrawPic(int x, int y, const char * name)
{
    const ps2::tex::Texture * texture = FindPicOrPlaceholder(name);
    ps2::gs::SetTexture(*texture);
    ps2::gs::DrawTexturedRect(x, y, texture->width, texture->height,
                              0, 0, texture->width, texture->height, kUiBrightness);
}

void PS2_DrawChar(int x, int y, int c)
{
    DrawGlyph(x, y, c);
}

void PS2_DrawTileClear(int x, int y, int w, int h, const char * name)
{
    // Tiles the image over the given screen rectangle: texels are addressed in
    // screen space and wrap via the REPEAT mode set up in gs::Init().
    (void)name; // Quake only ever tiles "backtile" here.
    ps2::gs::SetTexture(*s_texBacktile);
    ps2::gs::DrawTexturedRect(x, y, w, h, x, y, x + w, y + h, kUiBrightness);
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

void PS2_EndFrame()
{
    DrawFpsCounter();

    // VU1 bring-up scene: the engine only calls PS2_RenderFrame once game
    // assets load, and while disconnected Quake forces the fullscreen console,
    // which would hide anything drawn under the 2D overlay. So the test cube
    // draws after the 2D flush, on top of everything (its batch programs its
    // own z-test). Disable with "ps2_testcube 0".
    ps2::gs::Flush2D();
    ps2::test::DrawRotatingCube();

    ps2::gs::EndFrame();
}

void PS2_AppActivate(qboolean activate) { (void)activate; }

} // extern "C"
