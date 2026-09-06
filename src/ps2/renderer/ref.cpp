/* ================================================================================================
 * File: ref.cpp
 * Brief: The refexport_t implementation - the functions the Quake II client calls
 *        to draw. Implements the full 2D overlay path (console, HUD, menus) -
 *        pics, glyphs, tile fills, solid fills and fades - plus cinematic
 *        playback (cinematic.cpp) and the image/model registration cycle
 *        (assets load from disk on first use and are freed when a level stops
 *        referencing them). RenderFrame draws the 3D world geometry (render_view.cpp)
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/vram.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/model.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/cinematic.h"
#include "ps2/renderer/render_view.h"
#include "ps2/renderer/render_md2.h"
#include "ps2/renderer/render_sky.h"
#include "ps2/tests/draw_cube.h"
#include "ps2/tests/cinematics.h"
#include "ps2/tests/map_cycle.h"
#include "ps2/builtin/builtin.h"
#include "ps2/debug/profile.h"

#include <algorithm>
#include <cstdio>

namespace {

// Size in pixels of one console font glyph (conchars is a 16x16 grid of these).
constexpr int kGlyphSize = 8;

// Vertex colour applied to textured 2D (GS modulate: 128 = texels unchanged).
constexpr u8 kUiBrightness[3] = { 128, 128, 128 };
constexpr u8 kYellow[3]       = { 128, 128, 0   };
constexpr u8 kGreen[3]        = { 0,   128, 0   };
constexpr u8 kRed[3]          = { 128, 0,   0   };

static const cvar_t * s_showFpsCount     = nullptr;
static const cvar_t * s_showMemStats     = nullptr;
static const cvar_t * s_showVramStats    = nullptr;
static const cvar_t * s_showDrawStats    = nullptr;
static const cvar_t * s_showProfileStats = nullptr;

// Built-ins used every frame, cached at init to skip the name lookup.
static const ps2::tex::Texture * s_texConchars = nullptr;
static const ps2::tex::Texture * s_texBacktile = nullptr;

// A missing image draws as the pink/black checkerboard instead of crashing or
// silently vanishing - obvious on screen, and callers get sane dimensions.
const ps2::tex::Texture & FindTextureOrPlaceholder(const char * name, const ps2::tex::ImageType type)
{
    const ps2::tex::Texture * texture = ps2::tex::Find(name, type);
    if (texture == nullptr)
    {
        Com_DPrintf("Missing texture '%s', using placeholder.\n", name);
        texture = &ps2::tex::DebugTexture();
    }
    return *texture;
}

void DrawGlyph(int x, int y, int c, const u8 color[3])
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

    ps2::gs::SetTextureFor2D(*s_texConchars);
    ps2::gs::DrawTexturedRect(x, y, kGlyphSize, kGlyphSize,
                              col, row, col + kGlyphSize, row + kGlyphSize,
                              color);
}

void DrawInternalString(int x, int y, const char * str, const u8 color[3] = kUiBrightness)
{
    const int initialX = x;
    for (; *str != '\0'; ++str)
    {
        DrawGlyph(x, y, *str, color);
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
    if (s_showFpsCount->value == 0.0f)
    {
        return;
    }

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

    const u8* color = kGreen;
    if (s_fps.count < 60)
    {
        color = kYellow;
    }
    if (s_fps.count < 30)
    {
        color = kRed;
    }

    // A black background to give the text more contrast.
    ps2::gs::FillRect(viddef.width - 68, 2, 64, 12, 0, 0, 0, 255);
    DrawInternalString(viddef.width - 64, 4, text, color);
}

// Per-event frame time overlay, stacked under the FPS counter in the top-right
// corner: one line per PS2_PROFILE_SCOPEDPS2_PROFILE_SCOPED_EVENT site flagged
// kScreenOverlay, showing what that site cost over one frame.
//
// The numbers are the previous frame's, not a running total or an average:
// PS2_BeginFrame rolls the accumulators over, so the frame being reported is
// complete by the time this draws - down to the probes that only close after
// it (VSync, Frame). A site that never ran in that frame (RenderAlphaSurfaces
// with nothing translucent in view, say) reads 0.000, and one that ran several
// times shows the sum of its calls.
void DrawProfileOverlay()
{
#if PS2_QUAKE_PROFILE

    if (s_showProfileStats->value == 0.0f)
    {
        return;
    }

    // Cap the panel so a heavily instrumented build can't run off the screen.
    // 12 overlay events today (FullFrame, the nine view tags, GSWait, VSync);
    // the slack is for probes added while chasing a specific frame cost.
    constexpr int kMaxRows = 16;

    const ps2::debug::ProfileEvent * rows[kMaxRows];
    int numRows = 0;

    for (const auto * ev = ps2::debug::ProfileEventList();
         ev != nullptr && numRows < kMaxRows;
         ev = ev->next)
    {
        if (ev->flags & ps2::debug::kScreenOverlay)
        {
            rows[numRows++] = ev;
        }
    }

    std::sort(rows, rows + numRows,
        [](const ps2::debug::ProfileEvent * a, const ps2::debug::ProfileEvent * b) -> bool
        {
            return a->sortKey < b->sortKey;
        });

    if (numRows == 0)
    {
        return; // Nothing instrumented has been reached yet.
    }

    constexpr int kLineHeight = kGlyphSize + 2; // Matches DrawInternalString spacing.
    constexpr int kPanelWidth = 148;
    constexpr int kPadding    = 4;

    const int panelHeight = ((numRows + 1) * kLineHeight) + (kPadding * 2); // Header + one per event.

    // Right edge aligned with the FPS counter above, which ends at width - 4.
    const int panelX = viddef.width - kPanelWidth - 4;
    const int panelY = 16; // Clears the 12px FPS box at y = 2.

    // A black background to give the text more contrast.
    ps2::gs::FillRect(panelX, panelY, kPanelWidth, panelHeight, 0, 0, 0, 255);

    const int textX = panelX + kPadding;
    int textY = panelY + kPadding;

    DrawInternalString(textX, textY, "FRAME TIMES (ms)");
    textY += kLineHeight;

    char line[64];
    char millisec[16];
    for (int i = 0; i < numRows; ++i)
    {
        const auto * const ev = rows[i];
        std::snprintf(line, sizeof(line), "%-10s %6s", ev->name,
                      ps2::debug::ProfileFormatMillisec(ev->lastFrameCycles, millisec, sizeof(millisec)));

        const u8* color = kUiBrightness;
        if (ev->sortKey == 0) // Sort key 0 = the "Frame" root
        {
            const auto ms = ev->FrameMilliseconds();
            color = kGreen;
            if (ms > 16) // below 60fps
            {
                color = kYellow;
            }
            if (ms > 33) // below 30fps
            {
                color = kRed;
            }
        }

        DrawInternalString(textX, textY, line, color);
        textY += kLineHeight;
    }

#endif // PS2_QUAKE_PROFILE
}

// Memory usage overlay in the lower-right corner: one line per PS2MemTag with
// its running byte total, followed by the grand total across all tags.
void DrawMemUsageOverlay()
{
    if (s_showMemStats->value == 0.0f)
    {
        return;
    }

    constexpr int kLineHeight = kGlyphSize + 2; // Matches DrawInternalString spacing.
    constexpr int kNumLines   = static_cast<int>(ps2::heap::MemTag::TagCount) + 4; // Header + one per tag + total + peak + sbrk left.
    constexpr int kPanelWidth = 176;
    constexpr int kPadding    = 4;

    const int panelHeight = (kNumLines * kLineHeight) + (kPadding * 2);
    const int panelX = viddef.width  - kPanelWidth;
    const int panelY = viddef.height - panelHeight;

    // A black background to give the text more contrast.
    ps2::gs::FillRect(panelX, panelY, kPanelWidth, panelHeight, 0, 0, 0, 255);

    const int textX = panelX + kPadding;
    int textY = panelY + kPadding;

    DrawInternalString(textX, textY, "MEM USAGE");
    textY += kLineHeight;

    char line[64];
    char unit[ps2::heap::kMemUnitStrSize];
    size_t totalBytes = 0;
    for (int i = 0; i < static_cast<int>(ps2::heap::MemTag::TagCount); ++i)
    {
        const auto tag = static_cast<ps2::heap::MemTag>(i);
        const size_t tagBytes = ps2::heap::GetStatsForMemTag(tag).totalBytes;
        totalBytes += tagBytes;

        std::snprintf(line, sizeof(line), "%-10s %s",
                      ps2::heap::GetNameForMemTag(tag),
                      ps2::heap::FormatMemoryUnit(tagBytes, true, unit, sizeof(unit)));

        DrawInternalString(textX, textY, line);
        textY += kLineHeight;
    }

    std::snprintf(line, sizeof(line), "%-10s %s", "Total",
                  ps2::heap::FormatMemoryUnit(totalBytes, true, unit, sizeof(unit)));
    DrawInternalString(textX, textY, line);
    textY += kLineHeight;

    // The high-water of Total, which is the number that decides whether a map
    // change fits: the transient where the old map is still resident while the new
    // one loads is long gone by the time anyone reads Total off the screen.
    std::snprintf(line, sizeof(line), "%-10s %s", "Peak",
                  ps2::heap::FormatMemoryUnit(ps2::heap::GetPeakMemBytes(), true, unit, sizeof(unit)));
    DrawInternalString(textX, textY, line);
    textY += kLineHeight;

    std::snprintf(line, sizeof(line), "%-10s %s", "Sbrk Left",
                  ps2::heap::FormatMemoryUnit(ps2::heap::GetAvailableMemBytes(), true, unit, sizeof(unit)));
    DrawInternalString(textX, textY, line);
}

// GS VRAM texture-heap overlay in the lower-left corner: how much of the heap is
// committed, the number of resident textures, the texture uploads done so far
// this frame (streaming pressure - high or spiking means the heap is thrashing)
// and the GS drains a full heap forced this frame (see gs::EnsureTextureResident;
// anything but zero means the frame's working set does not fit).
void DrawVramUsageOverlay()
{
    if (s_showVramStats->value == 0.0f)
    {
        return;
    }

    constexpr int kLineHeight = kGlyphSize + 2; // Matches DrawInternalString spacing.
    constexpr int kNumLines   = 5;              // Header + four stats.
    constexpr int kPanelWidth = 174;
    constexpr int kPadding    = 4;

    const ps2::vram::Stats stats = ps2::vram::GetStats();

    const int panelHeight = (kNumLines * kLineHeight) + (kPadding * 2);
    const int panelX = 0;                            // flush to the left edge
    const int panelY = viddef.height - panelHeight;  // ...and the bottom

    // A black background to give the text more contrast.
    ps2::gs::FillRect(panelX, panelY, kPanelWidth, panelHeight, 0, 0, 0, 255);

    const int textX = panelX + kPadding;
    int textY = panelY + kPadding;

    DrawInternalString(textX, textY, "VRAM USAGE");
    textY += kLineHeight;

    char line[64];
    char unit[ps2::heap::kMemUnitStrSize];

    std::snprintf(line, sizeof(line), "%-10s %s", "Used",
                  ps2::heap::FormatMemoryUnit(static_cast<size_t>(stats.totalWords - stats.freeWords) * 4u,
                                              true, unit, sizeof(unit)));
    DrawInternalString(textX, textY, line);
    textY += kLineHeight;

    std::snprintf(line, sizeof(line), "%-10s %s", "Total",
                  ps2::heap::FormatMemoryUnit(static_cast<size_t>(stats.totalWords) * 4u,
                                              true, unit, sizeof(unit)));
    DrawInternalString(textX, textY, line);
    textY += kLineHeight;

    std::snprintf(line, sizeof(line), "%-10s %d", "Resident", stats.residentTextures);
    DrawInternalString(textX, textY, line);
    textY += kLineHeight;

    // Uploads and heap-full drains share a line to keep the panel compact; the
    // drain count is the one to watch, since it is zero on a healthy frame.
    std::snprintf(line, sizeof(line), "%-10s %d (%d sync)", "Uploads",
                  stats.uploadsThisFrame, stats.oomSyncsThisFrame);
    DrawInternalString(textX, textY, line);
}

// 3D draw statistics overlay in the top-left corner: what the last rendered
// frame's view pass walked, culled, clipped and submitted to VU1.
void DrawDrawStatsOverlay()
{
    if (s_showDrawStats->value == 0.0f)
    {
        return;
    }

    const ps2::view::DrawStats & stats = ps2::view::GetDrawStats();
    const ps2::lm::Stats lmStats = ps2::lm::GetStats();

    const struct { const char * label; int value; } rows[] = {
        { "Nodes",   stats.nodesWalked   },
        { "Surfs",   stats.surfaces      },
        { "Alpha",   stats.surfacesAlpha },
        { "Sky",     stats.skyFaces      },
        { "Tris",    stats.trisDrawn     },
        { "Ents",    stats.entities      },
        { "Prts",    stats.particles     },
        { "DLights", stats.dlights       },
        { "Batches", stats.drawBatches   },
        { "Clipped", stats.trisClipped   },
        { "Culled",  stats.trisCulled    },
        { "BoxCull", stats.boxesCulled   },
        // Lightmap rebuilds this frame. LmDyn tracks moving dynamic lights and
        // LmRest the surfaces they have just left; both should fall back to
        // zero once the lights stop moving. A stuck LmRest means the restore
        // path is not settling.
        { "LmAtlas", lmStats.atlases        },
        { "LmStyle", lmStats.styleUpdates   },
        { "LmDyn",   lmStats.dynamicUpdates },
        { "LmRest",  lmStats.restoreUpdates },
        // Most qwords a per-frame DMA packet has ever held, against the capacity
        // it was allocated with. The two frame packets are the whole Renderer
        // memory tag, so a peak far below the capacity means kPacketQwords is
        // oversized and can be cut.
        { "DmaPeak", ps2::gs::FramePacketPeakQwords()     },
        { "DmaCap",  ps2::gs::FramePacketCapacityQwords() },
        // Most REF'd payload bytes any one frame has submitted to VU1. This is
        // the per-frame demand the frame arena will have to satisfy, so it is
        // what sizes it - doubled, since the arena is buffered across frames.
        { "ArenaHi", ps2::vu1::PeakFrameSubmittedBytes() },
    };

    constexpr int kLineHeight = kGlyphSize + 2; // Matches DrawInternalString spacing.
    constexpr int kPanelWidth = 136;
    constexpr int kPadding    = 4;
    constexpr int kNumLines   = ps2::ArrayLength(rows) + 1; // Header + one per counter.

    const int panelHeight = (kNumLines * kLineHeight) + (kPadding * 2);

    // A black background to give the text more contrast.
    ps2::gs::FillRect(0, 0, kPanelWidth, panelHeight, 0, 0, 0, 255);

    const int textX = kPadding;
    int textY = kPadding;

    DrawInternalString(textX, textY, "DRAW STATS");
    textY += kLineHeight;

    char line[64];
    for (const auto & row : rows)
    {
        std::snprintf(line, sizeof(line), "%-8s %6d", row.label, row.value);
        DrawInternalString(textX, textY, line);
        textY += kLineHeight;
    }
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
    ps2::lm::Init();
    ps2::mod::Init();
    ps2::view::Init();

    s_showFpsCount     = Cvar_Get("ps2_show_fps",       PS2_QUAKE_DEBUG ? "1" : "0", 0);
    s_showMemStats     = Cvar_Get("ps2_show_memstats",  PS2_QUAKE_DEBUG ? "1" : "0", 0);
    s_showVramStats    = Cvar_Get("ps2_show_vramstats", PS2_QUAKE_DEBUG ? "1" : "0", 0);
    s_showDrawStats    = Cvar_Get("ps2_show_drawstats", PS2_QUAKE_DEBUG ? "1" : "0", 0);
    s_showProfileStats = Cvar_Get("ps2_show_profile",   PS2_QUAKE_DEBUG ? "1" : "0", 0);

    s_texConchars = ps2::tex::Find("conchars", ps2::tex::ImageType::Pic);
    s_texBacktile = ps2::tex::Find("backtile", ps2::tex::ImageType::Pic);
    PS2_Assert(s_texConchars != nullptr && s_texBacktile != nullptr);

    // Seed the cinematic palette from the game palette (the engine normally
    // sets a real one before the first frame; this covers stray draws).
    ps2::cin::SetPalette(nullptr);

    viddef.width  = ps2::gs::Width();
    viddef.height = ps2::gs::Height();

#if !PS2_QUAKE_DEBUG
    // Default clear color to black in release builds.
    ps2::gs::SetClearColor(0, 0, 0);
#endif // PS2_QUAKE_DEBUG

    Com_DPrintf("PS2 refresh initialised: %dx%d\n", viddef.width, viddef.height);
    Com_DPrintf("Debug: %s, Asserts: %s\n",
                PS2_QUAKE_DEBUG   ? "yes" : "no",
                PS2_QUAKE_ASSERTS ? "yes" : "no");

    return true;
}

void PS2_RefShutdown() {}
void PS2_AppActivate(qboolean activate) { (void)activate; }

// ------------------------------------------------------------------------------------------------
// Registration: textures load from disk on first use (PCX/WAL/TGA); the
// Begin/End pair brackets a level change and frees the level assets it no
// longer references. Same for models.
// ------------------------------------------------------------------------------------------------

void PS2_BeginRegistration(const char * mapName)
{
    ps2::tex::BeginRegistration();
    ps2::mod::BeginRegistration(mapName);
    ps2::view::BeginRegistration();
    ps2::sky::BeginRegistration();
}

void PS2_EndRegistration()
{
    ps2::mod::EndRegistration();
    ps2::tex::EndRegistration();
}

// Called by the server just before it builds the next map's collision model,
// so the old world's hunk and lightmap atlases are not still held through
// the whole of server init.
//
// The atlases go if and only if the world went. ReleaseWorldModel keeps the world
// when the new map is the one already loaded (a restart, or a savegame load),
// and then LoadWorldModel finds it in the cache and never needs to reload it.
void PS2_ReleaseWorldModel(const char * bspName)
{
    if (ps2::mod::ReleaseWorldModel(bspName))
    {
        ps2::lm::ReleaseAtlases();
    }
}

void PS2_SetSky(const char * name, float rotate, vec3_t axis)
{
    ps2::sky::SetSky(name, rotate, axis);
}

struct model_s * PS2_RegisterModel(const char * name)
{
    return const_cast<struct model_s*>(
        reinterpret_cast<const struct model_s *>(ps2::mod::Find(name)));
}

struct image_s * PS2_RegisterSkin(const char * name)
{
    return const_cast<struct image_s*>(
        reinterpret_cast<const struct image_s *>(&FindTextureOrPlaceholder(name, ps2::tex::ImageType::Skin)));
}

struct image_s * PS2_RegisterPic(const char * name)
{
    return const_cast<struct image_s*>(
        reinterpret_cast<const struct image_s *>(&FindTextureOrPlaceholder(name, ps2::tex::ImageType::Pic)));
}

// ------------------------------------------------------------------------------------------------
// 2D overlay
// ------------------------------------------------------------------------------------------------

void PS2_DrawGetPicSize(int * w, int * h, const char * name)
{
    // Callable outside Begin/EndFrame. Placeholder dimensions keep the
    // callers' centering math sane when the pic is missing.
    const ps2::tex::Texture & texture = FindTextureOrPlaceholder(name, ps2::tex::ImageType::Pic);
    *w = texture.width;
    *h = texture.height;
}

void PS2_DrawStretchPic(int x, int y, int w, int h, const char * name)
{
    const ps2::tex::Texture & texture = FindTextureOrPlaceholder(name, ps2::tex::ImageType::Pic);
    ps2::gs::SetTextureFor2D(texture);
    ps2::gs::DrawTexturedRect(x, y, w, h, 0, 0, texture.width, texture.height, kUiBrightness);
}

void PS2_DrawPic(int x, int y, const char * name)
{
    const ps2::tex::Texture & texture = FindTextureOrPlaceholder(name, ps2::tex::ImageType::Pic);
    ps2::gs::SetTextureFor2D(texture);
    ps2::gs::DrawTexturedRect(x, y, texture.width, texture.height,
                              0, 0, texture.width, texture.height, kUiBrightness);
}

void PS2_DrawChar(int x, int y, int c)
{
    DrawGlyph(x, y, c, kUiBrightness);
}

void PS2_DrawTileClear(int x, int y, int w, int h, const char * name)
{
    // Tiles the image over the given screen rectangle: texels are addressed in
    // screen space and wrap via the REPEAT mode set up in gs::Init().
    (void)name; // Quake only ever tiles "backtile" here.
    ps2::gs::SetTextureFor2D(*s_texBacktile);
    ps2::gs::DrawTexturedRect(x, y, w, h, x, y, x + w, y + h, kUiBrightness);
}

void PS2_DrawFill(int x, int y, int w, int h, int c)
{
    const u32 p = global_palette[c & 0xFF];
    const u8  r = static_cast<u8>(p & 0xFFu);
    const u8  g = static_cast<u8>((p >> 8) & 0xFFu);
    const u8  b = static_cast<u8>((p >> 16) & 0xFFu);
    ps2::gs::FillRect(x, y, w, h, r, g, b, 255);
}

void PS2_DrawFadeScreen()
{
    ps2::gs::FillRect(0, 0, ps2::gs::Width(), ps2::gs::Height(), 0, 0, 0, 128);
}

// ------------------------------------------------------------------------------------------------
// Cinematics
// ------------------------------------------------------------------------------------------------

void PS2_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte * data)
{
    // Called every frame while a cinematic plays; the movie quad is a 2D draw,
    // so it joins the deferred overlay batch (opened lazily) like any other.
    ps2::cin::DrawFrame(x, y, w, h, cols, rows, data);
}

void PS2_CinematicSetPalette(const unsigned char * palette)
{
    ps2::cin::SetPalette(palette);
}

// ------------------------------------------------------------------------------------------------
// Frame rendering
// ------------------------------------------------------------------------------------------------

void PS2_BeginFrame(float cameraSeparation)
{
    (void)cameraSeparation;

    // Close the frame the profile probes have been charging into, before any of
    // this frame's work is measured. This has to happen here rather than at the
    // end of PS2_EndFrame: the probes that close last - gs::EndFrame's vsync
    // wait, and the Frame scope around Qcommon_Frame - would otherwise be
    // charged to the frame after the one they measured, pairing each frame's
    // view cost with the previous frame's wait.
    ps2::debug::ProfileNewFrame();

    // 2D and 3D now draw freely between here and PS2_EndFrame: 2D primitives
    // open the deferred overlay batch lazily and it flushes automatically at
    // each 2D->3D boundary and in gs::EndFrame().
    ps2::gs::BeginFrame();
}

void PS2_EndFrame()
{
#if PS2_QUAKE_DEBUG
    // Cinematic playback test: the movie quad is a 2D draw, run at frame's end
    // so it lands over the fullscreen console but under the FPS counter. Enable
    // with cvar "ps2_testcin 1".
    ps2::test::RunCinematics();

    // VU1 bring-up scene (cvar "ps2_testcube 1"): a 3D draw, so it flushes the
    // 2D overlay accumulated above and lands on top - staying visible over the
    // fullscreen console Quake forces while disconnected (its batch programs
    // its own z-test). gs::EndFrame() then sends any remaining 2D and flips.
    ps2::test::DrawRotatingCube();

    // Memory smoke test (cvar "ps2_testmaps 1"): loads every stock map in unit
    // order and logs what each one costs. Draws nothing - it only queues console
    // commands - but it lives here because this is the one place guaranteed to
    // be reached once per frame.
    ps2::test::RunMapCycle();
#endif // PS2_QUAKE_DEBUG

    DrawFpsCounter();
    DrawProfileOverlay();
    DrawMemUsageOverlay();
    DrawVramUsageOverlay();
    DrawDrawStatsOverlay();

    ps2::gs::EndFrame();
}

void PS2_RenderFrame(refdef_t * viewDef)
{
    PS2_Assert(viewDef != nullptr);
    ps2::view::RenderFrame(*viewDef);
}

} // extern "C"
