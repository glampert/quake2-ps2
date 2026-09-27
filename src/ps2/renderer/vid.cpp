/* ================================================================================================
 * File: vid.cpp
 * Brief: VID_* video-shell seam. Owns the engine's 're' refresh handle and the
 *        'viddef' screen state, and wires the refexport_t table directly to the
 *        PS2_* renderer functions (the PS2 refresh is statically linked, so there
 *        is no GetRefAPI/DLL handshake). Also home to the video options menu,
 *        VID_Menu*, which puts the renderer's user-facing ps2_* cvars on screen.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

extern "C" {
    #include "client/qmenu.h" // The menu framework (qmenu.c) every stock menu is built on.
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vid_menu { void CaptureStartupValues(); }

extern "C" {

// refexport_t implementations from ref.cpp:
qboolean PS2_RefInit(void * hinstance, void * wndproc);
void PS2_RefShutdown();
void PS2_BeginRegistration(const char * mapName);
struct model_s * PS2_RegisterModel(const char * name);
struct image_s * PS2_RegisterSkin(const char * name);
struct image_s * PS2_RegisterPic(const char * name);
void PS2_SetSky(const char * name, float rotate, vec3_t axis);
void PS2_EndRegistration();
void PS2_ReleaseWorldModel(const char * bspName);
void PS2_SetRegistrationTouchOnly(qboolean enable);
void PS2_FreeUnregistered();
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
void PS2_BeginFrame(float cameraSeparation);
void PS2_EndFrame();
void PS2_AppActivate(qboolean activate);

// Menu and client functions no engine header declares (menu.c keeps its own prototypes):
void M_PopMenu();
const char * Default_MenuKey(menuframework_s * menu, int key);
void M_DrawTextBox(int x, int y, int width, int lines);
void CL_WriteConfiguration();

// Engine-visible globals (referenced unmangled from the C client).
refexport_t re = {};
viddef_t viddef = {};

void VID_Init()
{
    re.api_version = REF_API_VERSION;

    re.Init                     = PS2_RefInit;
    re.Shutdown                 = PS2_RefShutdown;
    re.BeginRegistration        = PS2_BeginRegistration;
    re.RegisterModel            = PS2_RegisterModel;
    re.RegisterSkin             = PS2_RegisterSkin;
    re.RegisterPic              = PS2_RegisterPic;
    re.SetSky                   = PS2_SetSky;
    re.EndRegistration          = PS2_EndRegistration;
    re.ReleaseWorldModel        = PS2_ReleaseWorldModel;
    re.SetRegistrationTouchOnly = PS2_SetRegistrationTouchOnly;
    re.FreeUnregistered         = PS2_FreeUnregistered;
    re.RenderFrame              = PS2_RenderFrame;
    re.DrawGetPicSize           = PS2_DrawGetPicSize;
    re.DrawPic                  = PS2_DrawPic;
    re.DrawStretchPic           = PS2_DrawStretchPic;
    re.DrawChar                 = PS2_DrawChar;
    re.DrawTileClear            = PS2_DrawTileClear;
    re.DrawFill                 = PS2_DrawFill;
    re.DrawFadeScreen           = PS2_DrawFadeScreen;
    re.DrawStretchRaw           = PS2_DrawStretchRaw;
    re.CinematicSetPalette      = PS2_CinematicSetPalette;
    re.BeginFrame               = PS2_BeginFrame;
    re.EndFrame                 = PS2_EndFrame;
    re.AppActivate              = PS2_AppActivate;

    if (!re.Init(nullptr, nullptr)) [[unlikely]]
    {
        Sys_Error("VID_Init: PS2 refresh failed to initialise!");
    }

    // config.cfg has run by now, so these are the values the refresh just latched.
    vid_menu::CaptureStartupValues();
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

} // extern "C"

// ------------------------------------------------------------------------------------------------
// Video Menu
// ------------------------------------------------------------------------------------------------

namespace vid_menu {

// When a change to a setting shows. The renderer samples most of its cvars every frame, but a few
// decide what gets loaded or how VRAM is laid out, and those are only read at that point.
enum class Applies : char
{
    Now,     // Sampled every frame.
    NextMap, // Read when a map loads (PS2_BeginRegistration).
    Restart  // Latched by PS2_RefInit, or cached with what it loaded (the sky faces).
};

// One entry of a setting's spin control: the cvar value it sets, and the
// label shown for it, when that is not the value itself.
struct Choice
{
    const char * value;
    const char * label = nullptr;
};

struct Setting
{
    const char *   cvarName;
    const char *   label;
    const char *   statusBar;
    const Applies  applies;
    const bool     groupStart; // Leaves a blank row above it, between groups.
    const Choice * choices;
    const int      numChoices;
};

constexpr Choice kOffOn[]        = { { "0", "off" }, { "1", "on" } };
constexpr Choice kFbWidths[]     = { { "512" }, { "640" } };
constexpr Choice kFbHeights[]    = { { "448" }, { "512" } }; // Full height for NTSC and PAL.
constexpr Choice kFbFormats[]    = { { "1", "16-bit" }, { "0", "32-bit" } };
constexpr Choice kIntensities[]  = { { "1" }, { "1.5" }, { "2" }, { "2.5" }, { "3" } };
constexpr Choice kMipFilters[]   = { { "nearest" }, { "bilinear" }, { "trilinear" } };
constexpr Choice kMipBiases[]    = { { "-1" }, { "-0.5" }, { "0" }, { "0.5" }, { "1" }, { "1.5" }, { "2" } };
constexpr Choice kSkyMips[]      = { { "0", "full" }, { "1", "half" } };
constexpr Choice kDLightModes[]  = { { "0", "flares" }, { "1", "lightmaps" }, { "2", "vertex lighting" } };
constexpr Choice kDLightScales[] = { { "0.05" }, { "0.1" }, { "0.15" }, { "0.2" }, { "0.25" }, { "0.3" } };

// Menu order. Status bar text stays within 64 columns, the narrowest (512 pixel) screen.
constexpr Setting kSettings[] = {
    { "ps2_debug_overlays",    "debug overlays",    "show developer debug info (draw/mem/profile stats)",    Applies::Now,     false, kOffOn,        ps2::ArrayLength(kOffOn)        },
    { "ps2_show_fps",          "show fps",          "show frames per second at the top right of the screen", Applies::Now,     false, kOffOn,        ps2::ArrayLength(kOffOn)        },
    { "ps2_fb_width",          "screen width",      "framebuffer width in pixels; needs a restart",          Applies::Restart, true,  kFbWidths,     ps2::ArrayLength(kFbWidths)     },
    { "ps2_fb_height",         "screen height",     "448 fits NTSC, 512 fits PAL; needs a restart",          Applies::Restart, false, kFbHeights,    ps2::ArrayLength(kFbHeights)    },
    { "ps2_fb_16bit",          "color depth",       "16-bit leaves more VRAM for textures; needs a restart", Applies::Restart, false, kFbFormats,    ps2::ArrayLength(kFbFormats)    },
    { "ps2_fb_dither",         "dithering",         "smooths the color banding of 16-bit color",             Applies::Now,     false, kOffOn,        ps2::ArrayLength(kOffOn)        },
    { "ps2_gs_latency",        "gs latency",        "draw a frame ahead: faster, one frame more input lag",  Applies::Now,     false, kOffOn,        ps2::ArrayLength(kOffOn)        },
    { "ps2_intensity",         "intensity",         "brightness of walls and models; needs a restart",       Applies::Restart, true,  kIntensities,  ps2::ArrayLength(kIntensities)  },
    { "ps2_mipmaps",           "mipmaps",           "mip levels for walls; applies on the next map load",    Applies::NextMap, false, kOffOn,        ps2::ArrayLength(kOffOn)        },
    { "ps2_mip_filter",        "texture filter",    "filtering of walls and model skins",                    Applies::Now,     false, kMipFilters,   ps2::ArrayLength(kMipFilters)   },
    { "ps2_mip_bias",          "mip bias",          "wall mip level bias; positive is blurrier",             Applies::Now,     false, kMipBiases,    ps2::ArrayLength(kMipBiases)    },
    { "ps2_skymip",            "sky quality",       "a half resolution sky saves VRAM; needs a restart",     Applies::Restart, false, kSkyMips,      ps2::ArrayLength(kSkyMips)      },
    { "ps2_dynamic_lightmaps", "dynamic lights",    "how explosions and muzzle flashes light the world",     Applies::Now,     true,  kDLightModes,  ps2::ArrayLength(kDLightModes)  },
    { "ps2_dlight_scale",      "dlight brightness", "strength of the dynamic lights in vertex mode",         Applies::Now,     false, kDLightScales, ps2::ArrayLength(kDLightScales) },
    { "ps2_md2_shadows",       "model shadows",     "blob shadows under monsters and items",                 Applies::Now,     false, kOffOn,        ps2::ArrayLength(kOffOn)        },
};

constexpr int kNumSettings = ps2::ArrayLength(kSettings);

constexpr int MostChoices()
{
    int most = 0;
    for (const Setting & setting : kSettings)
    {
        most = (setting.numChoices > most) ? setting.numChoices : most;
    }
    return most;
}

constexpr int kMaxChoices = MostChoices();

// Longest cvar value kept, as typed at the console or read back from config.cfg.
constexpr int kMaxValueLen = 32;

// menu.c's menu_in_sound, which it keeps static.
constexpr const char * kMenuInSound = "misc/menu1.wav";

// ------------------------------------------------------------------------------------------------
// Video Menu state
// ------------------------------------------------------------------------------------------------

struct SettingItem
{
    menulist_s spin;

    // The choices' labels, then the custom value if there is one, then the null the spin control
    // stops at. A value that matches no choice - set at the console, say - becomes one more entry
    // rather than being snapped to a choice, so opening the menu never changes anything by itself.
    const char * names[kMaxChoices + 2];
    char customValue[kMaxValueLen];

    // The value when the menu opened, to tell what this visit changed.
    char openValue[kMaxValueLen];
};

static menuframework_s s_menu;
static SettingItem     s_items[kNumSettings];

// Every setting's value right after PS2_RefInit read them, which is what the Restart ones are
// running with. Measured against that rather than the value at menu open, the notice still
// reports a change waiting for a restart on a later visit, and drops one that was set back.
static char s_startupValues[kNumSettings][kMaxValueLen];

// The notice shown in place of the menu on leaving it, while a change has yet to take effect.
struct NoticeLine
{
    enum class Style { Heading, Setting, Prompt };

    const char * text;
    Style        style;
};

constexpr int kNoticeChars    = 34;               // Inner width of the notice's text box.
constexpr int kMaxNoticeLines = kNumSettings + 4; // Two headings, a blank and the prompt.

static NoticeLine s_noticeLines[kMaxNoticeLines];
static int        s_numNoticeLines = 0; // Zero while the menu itself is showing.

// ------------------------------------------------------------------------------------------------
// Video Menu helpers
// ------------------------------------------------------------------------------------------------

void CaptureStartupValues()
{
    for (int i = 0; i < kNumSettings; ++i)
    {
        std::snprintf(s_startupValues[i], sizeof(s_startupValues[i]), "%s",
                      Cvar_VariableString(kSettings[i].cvarName));
    }
}

// Whether a cvar value selects a choice: by name, or as the same number spelled another way
// ("2.0" selects "2").
bool ValueMatches(const char * value, const char * choice)
{
    if (Q_stricmp(value, choice) == 0)
    {
        return true;
    }

    char * valueEnd  = nullptr;
    char * choiceEnd = nullptr;
    const float valueNumber  = std::strtof(value,  &valueEnd);
    const float choiceNumber = std::strtof(choice, &choiceEnd);

    return (valueEnd  != value  && *valueEnd  == '\0') &&
           (choiceEnd != choice && *choiceEnd == '\0') &&
           (valueNumber == choiceNumber);
}

// Spin control callback: every step sets the cvar straight away, so the settings that
// apply live show behind the menu as they are picked.
void OnSettingChanged(void * self)
{
    const menulist_s & spin = *static_cast<const menulist_s *>(self);

    const int index = spin.generic.localdata[0];
    const Setting & setting = kSettings[index];

    const char * const value = (spin.curvalue < setting.numChoices)
                             ? setting.choices[spin.curvalue].value
                             : s_items[index].customValue;

    Cvar_Set(setting.cvarName, value);
}

void AddNoticeLine(const char * text, const NoticeLine::Style style)
{
    PS2_Assert(s_numNoticeLines < kMaxNoticeLines);
    s_noticeLines[s_numNoticeLines++] = { text, style };
}

void AddNoticeSection(const char * heading, const Applies applies, const bool (&pending)[kNumSettings])
{
    bool any = false;
    for (int i = 0; i < kNumSettings; ++i)
    {
        if (pending[i] && kSettings[i].applies == applies)
        {
            if (!any)
            {
                AddNoticeLine(heading, NoticeLine::Style::Heading);
                any = true;
            }
            AddNoticeLine(kSettings[i].label, NoticeLine::Style::Setting);
        }
    }
}

// Leaves the menu: saves config.cfg if anything changed, then either pops the menu or, when a
// change has yet to take effect, swaps it for a notice saying which, which the next key dismisses.
const char * CloseMenu()
{
    bool changed = false;
    bool pending[kNumSettings] = {};

    for (int i = 0; i < kNumSettings; ++i)
    {
        const char * const value = Cvar_VariableString(kSettings[i].cvarName);
        const bool changedHere = (std::strcmp(value, s_items[i].openValue) != 0);

        changed = changed || changedHere;

        switch (kSettings[i].applies)
        {
        case Applies::Now:
            break;
        case Applies::NextMap:
            pending[i] = changedHere;
            break;
        case Applies::Restart:
            pending[i] = (std::strcmp(value, s_startupValues[i]) != 0);
            break;
        }
    }

    // Written here rather than left to CL_Shutdown, which a console switched off never reaches.
    // Not when nothing changed, though: on hardware it is a USB write for nothing.
    if (changed)
    {
        CL_WriteConfiguration();
    }

    s_numNoticeLines = 0;
    AddNoticeSection("Restart the game to apply:", Applies::Restart, pending);
    AddNoticeSection("Load a map to apply:",       Applies::NextMap, pending);

    if (s_numNoticeLines == 0)
    {
        M_PopMenu();
        return nullptr;
    }

    AddNoticeLine(nullptr, NoticeLine::Style::Prompt);
    AddNoticeLine("press any button", NoticeLine::Style::Prompt);
    return kMenuInSound;
}

void DrawNotice()
{
    // Laid out in the 320x240 space M_DrawTextBox centres on the screen, like menu.c's own boxes.
    const int boxX = (320 - ((kNoticeChars + 2) * 8)) / 2;
    const int boxY = (240 - ((s_numNoticeLines + 2) * 8)) / 2;
    M_DrawTextBox(boxX, boxY, kNoticeChars, s_numNoticeLines);

    const int textX = ((viddef.width  - 320) / 2) + boxX + 8;
    const int textY = ((viddef.height - 240) / 2) + boxY + 8;

    for (int i = 0; i < s_numNoticeLines; ++i)
    {
        const NoticeLine & line = s_noticeLines[i];
        if (line.text == nullptr)
        {
            continue; // Blank line.
        }

        const int y = textY + (i * 8);
        switch (line.style)
        {
        case NoticeLine::Style::Heading:
            Menu_DrawStringDark(textX, y, line.text);
            break;
        case NoticeLine::Style::Setting:
            Menu_DrawString(textX + 16, y, line.text);
            break;
        case NoticeLine::Style::Prompt:
            {
                const int len = static_cast<int>(std::strlen(line.text));
                Menu_DrawStringDark(textX + ((kNoticeChars - len) * 4), y, line.text);
            }
            break;
        }
    }
}

} // namespace vid_menu

// ------------------------------------------------------------------------------------------------
// Video Menu hooks (M_Menu_Video_f runs VID_MenuInit, then pushes Draw/Key)
// ------------------------------------------------------------------------------------------------

extern "C" {

void VID_MenuInit()
{
    vid_menu::s_menu.x = viddef.width / 2;
    vid_menu::s_menu.y = (viddef.height / 2) - 58;
    vid_menu::s_menu.nitems = 0;
    vid_menu::s_numNoticeLines = 0;

    int y = 0;
    for (int i = 0; i < vid_menu::kNumSettings; ++i)
    {
        const vid_menu::Setting & setting = vid_menu::kSettings[i];
        vid_menu::SettingItem & item = vid_menu::s_items[i];

        const char * const value = Cvar_VariableString(setting.cvarName);
        std::snprintf(item.openValue, sizeof(item.openValue), "%s", value);

        int current = -1;
        for (int c = 0; c < setting.numChoices; ++c)
        {
            const vid_menu::Choice & choice = setting.choices[c];
            item.names[c] = (choice.label != nullptr) ? choice.label : choice.value;

            if (current < 0 && vid_menu::ValueMatches(value, choice.value))
            {
                current = c;
            }
        }

        int numNames = setting.numChoices;
        if (current < 0)
        {
            std::snprintf(item.customValue, sizeof(item.customValue), "%s", value);
            current = numNames;
            item.names[numNames++] = item.customValue;
        }
        item.names[numNames] = nullptr;

        if (setting.groupStart)
        {
            y += 10;
        }

        item.spin = {};
        item.spin.generic.type         = MTYPE_SPINCONTROL;
        item.spin.generic.y            = y;
        item.spin.generic.name         = setting.label;
        item.spin.generic.statusbar    = setting.statusBar;
        item.spin.generic.callback     = vid_menu::OnSettingChanged;
        item.spin.generic.localdata[0] = i;
        item.spin.itemnames            = item.names;
        item.spin.curvalue             = current;

        Menu_AddItem(&vid_menu::s_menu, &item.spin);
        y += 10;
    }
}

void VID_MenuDraw()
{
    int w = 0;
    int h = 0;
    re.DrawGetPicSize(&w, &h, "m_banner_video");
    re.DrawPic((viddef.width / 2) - (w / 2), (viddef.height / 2) - 110, "m_banner_video");

    if (vid_menu::s_numNoticeLines > 0)
    {
        vid_menu::DrawNotice();
        return;
    }

    Menu_AdjustCursor(&vid_menu::s_menu, 1);
    Menu_Draw(&vid_menu::s_menu);
}

const char * VID_MenuKey(int key)
{
    if (vid_menu::s_numNoticeLines > 0)
    {
        M_PopMenu(); // Any key dismisses the notice, and the menu with it.
        return nullptr;
    }

    if (key == K_ESCAPE)
    {
        return vid_menu::CloseMenu();
    }

    return Default_MenuKey(&vid_menu::s_menu, key);
}

} // extern "C"
