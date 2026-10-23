/* ================================================================================================
 * File: save_notice.cpp
 * Brief: The save notice. See save_notice.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_notice.h"
#include "ps2/save/save_system.h"
#include "ps2/audio/cd_audio.h"
#include "ps2/audio/sound_hold.h"
#include "ps2/builtin/builtin.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/loading_screen.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/splash_screen.h"

extern "C" {
    #include "client/qmenu.h" // Menu_DrawString, Menu_DrawStringDark

    // Client and refresh functions no engine header declares, or none this can include
    // (client/screen.h repeats q_common.h's prototypes):
    void SCR_UpdateScreen(void);
    void M_DrawTextBox(int x, int y, int width, int lines);
    void IN_RumbleStop(void);
    void PS2_DrawGetPicSize(int * w, int * h, const char * name);
    void PS2_DrawFadeScreen();
}

#include <algorithm>
#include <cstring>
#include <delaythread.h>

namespace ps2::save {
namespace {

// However quick the device was, the notice stays up this long, so that it can be read.
constexpr int kMinShownMsec = 1000;

// EndSaveNotice waits out the rest of kMinShownMsec in steps this long.
constexpr int kWaitStepMicros = 10000;

// What is happening, then what not to do meanwhile, which is printed white, as the message boxes'
// text is (M_Popup): the first line, the device, a gap and the warning. The box is as wide as the
// longest line any device has, so it is the same box whatever it names.
constexpr int kNumLines       = 5;
constexpr int kFirstWhiteLine = 3;

constexpr const char * kSavingLine  = "Data is being saved to the";
constexpr const char * kLoadingLine = "Data is being loaded from the";

// By DeviceKind.
constexpr int kNumKinds = 4;
static_assert(static_cast<int>(DeviceKind::HardDisk) + 1 == kNumKinds, "A line for every DeviceKind.");

constexpr const char * kDeviceLines[kNumKinds] = {
    "Memory Card in slot 1.",
    "host filesystem.",
    "USB drive.",
    "hard disk drive.",
};

constexpr const char * kWarningLines[kNumKinds][2] = {
    { "Do not remove the Memory Card", "or reset/switch off the console." },
    { "Do not reset or switch off",    "the console."                     },
    { "Do not remove the USB drive",   "or reset/switch off the console." },
    { "Do not reset or switch off",    "the console."                     },
};

constexpr int TextLength(const char * text)
{
    int length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

constexpr int LongestLine()
{
    int longest = std::max(TextLength(kSavingLine), TextLength(kLoadingLine));
    for (int kind = 0; kind < kNumKinds; ++kind)
    {
        longest = std::max(longest, TextLength(kDeviceLines[kind]));
        longest = std::max(longest, TextLength(kWarningLines[kind][0]));
        longest = std::max(longest, TextLength(kWarningLines[kind][1]));
    }
    return longest;
}

// The layout, in the 320x240 space the menus are drawn in, centred on the screen (M_DrawTextBox):
// the box's inside is the longest line with a space either side, in conchars' 8-pixel characters,
// and its border takes a character on every side.
constexpr int kGlyph     = 8;
constexpr int kBoxChars  = LongestLine() + 2;
constexpr int kBoxWidth  = (kBoxChars + 2) * kGlyph;
constexpr int kBoxHeight = (kNumLines + 2) * kGlyph;
constexpr int kBoxLeft   = (320 - kBoxWidth) / 2;

static_assert(kBoxLeft >= 0, "The notice's lines are too long for the menus' space.");

// Over the game the box is centred. On the loading screen it goes under the plaque, which
// SCR_DrawLoading centres on the screen, two lines below it.
constexpr int kBoxTopOverGame = (240 - kBoxHeight) / 2;
constexpr int kGapUnderPlaque = 2 * kGlyph;

// Where the notice went up: what EndSaveNotice has to take it off.
enum class Where : u8
{
    Nowhere,       // Nothing could be drawn (inside a frame, between gs::Init and SCR_Init, while
                   // the world loader holds the frame chain): it doesn't go up, and the device is
                   // left alone.
    Game,          // Over the game's frame.
    LoadingScreen, // On the loading screen.
    Splash,        // Over the boot splash.
};

static bool       s_shown       = false;
static SaveAccess s_access      = SaveAccess::Saving;
static DeviceKind s_kind        = DeviceKind::MemoryCard;
static Where      s_where       = Where::Nowhere;
static int        s_shownMsec   = 0; // Sys_Milliseconds when it went up.
static int        s_framesDrawn = 0; // Game frames DrawSaveNotice has put it in.

// The lines for the access and the device the notice is up for.
struct Lines
{
    const char * line[kNumLines];
};

Lines NoticeLines()
{
    const int kind = static_cast<int>(s_kind);
    return { { (s_access == SaveAccess::Saving) ? kSavingLine : kLoadingLine, kDeviceLines[kind], "",
               kWarningLines[kind][0], kWarningLines[kind][1] } };
}

// Where a line starts inside the box, the box's inside being kBoxChars characters wide.
int LineOffset(const char * line)
{
    return kGlyph + (((kBoxChars - TextLength(line)) * kGlyph) / 2);
}

// The box and the lines with the box's top-left corner at (boxX, boxY), in the menus' space.
void DrawBox(const int boxX, const int boxY)
{
    M_DrawTextBox(boxX, boxY, kBoxChars, kNumLines);

    // M_DrawTextBox centres the menus' space on the screen; the strings are drawn on the screen.
    const int originX = (viddef.width - 320) / 2;
    const int originY = (viddef.height - 240) / 2;

    const Lines notice = NoticeLines();
    for (int i = 0; i < kNumLines; ++i)
    {
        const char * const line = notice.line[i];
        const int x = originX + boxX + LineOffset(line);
        const int y = originY + boxY + ((i + 1) * kGlyph);
        if (i < kFirstWhiteLine)
        {
            Menu_DrawStringDark(x, y, line);
        }
        else
        {
            Menu_DrawString(x, y, line);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// The notice as one image, for the boot splash
// ------------------------------------------------------------------------------------------------
//
// The boot reads config.cfg off the card in Qcommon_Init, before the renderer exists, so the
// splash draws the notice, and all it draws is textures. The image is put together here as
// DrawBox puts the notice on the screen, one texel to a pixel: the box laid out as M_DrawTextBox
// lays it, and the lines, all in conchars' glyphs through the built-in palette, the second line on
// in the glyphs' other colour as Menu_DrawStringDark draws them. Texels the glyphs leave
// see-through stay at alpha 0, which the splash leaves out.

// Copies conchars' glyph `glyph` into the image at (x, y), leaving out its see-through texels:
// palette index 255, as the renderer's alpha test does.
void BlitGlyph(u32 * const image, const int glyph, const int x, const int y)
{
    const u8 * const pixels = conchars_data + ((glyph >> 4) * kGlyph * conchars_width) + ((glyph & 15) * kGlyph);
    for (int row = 0; row < kGlyph; ++row)
    {
        for (int col = 0; col < kGlyph; ++col)
        {
            const int index = pixels[(row * conchars_width) + col];
            if (index != 255)
            {
                image[((y + row) * kBoxWidth) + x + col] = global_palette[index];
            }
        }
    }
}

// M_DrawTextBox's layout, at (0, 0): the left column, the middle a character at a time, then the
// right column, each of its top glyph, kNumLines middle ones and its bottom one.
void BlitBox(u32 * const image)
{
    constexpr int kColumns[3][3] = {
        { 1, 4, 7 }, // left: top, middle, bottom
        { 2, 5, 8 }, // middle
        { 3, 6, 9 }, // right
    };

    for (int x = 0; x < kBoxChars + 2; ++x)
    {
        const int * const column = kColumns[(x == 0) ? 0 : (x == kBoxChars + 1) ? 2 : 1];
        BlitGlyph(image, column[0], x * kGlyph, 0);
        for (int line = 0; line < kNumLines; ++line)
        {
            BlitGlyph(image, column[1], x * kGlyph, (line + 1) * kGlyph);
        }
        BlitGlyph(image, column[2], x * kGlyph, (kNumLines + 1) * kGlyph);
    }
}

constexpr size_t kImageBytes = static_cast<size_t>(kBoxWidth * kBoxHeight) * sizeof(u32);

// The image, kBoxWidth x kBoxHeight RGBA texels in a block of kImageBytes; null if out of memory.
u32 * ComposeNoticeImage()
{
    u32 * const image = static_cast<u32 *>(ps2::heap::TryAlloc(kImageBytes, ps2::heap::MemTag::SaveData));
    if (image == nullptr)
    {
        return nullptr;
    }

    std::memset(image, 0, kImageBytes);
    BlitBox(image);

    const Lines notice = NoticeLines();
    for (int i = 0; i < kNumLines; ++i)
    {
        const char * const line = notice.line[i];
        const int offset = (i < kFirstWhiteLine) ? 128 : 0; // Menu_DrawStringDark's glyphs
        for (int c = 0; line[c] != '\0'; ++c)
        {
            const int glyph = (static_cast<unsigned char>(line[c]) + offset) & 255;
            BlitGlyph(image, glyph, LineOffset(line) + (c * kGlyph), (i + 1) * kGlyph);
        }
    }
    return image;
}

bool ShowOnSplash()
{
    u32 * const image = ComposeNoticeImage();
    if (image == nullptr)
    {
        return false;
    }

    const bool shown = ps2::splash::ShowImage(image, kBoxWidth, kBoxHeight);
    ps2::heap::Free(image, kImageBytes, ps2::heap::MemTag::SaveData);
    return shown;
}

// ------------------------------------------------------------------------------------------------

// Puts a frame with the notice in it on screen before returning, wherever the game has got to:
//  - inside a frame, none: one frame can't be drawn inside another; nor while the world loader
//    holds the frame chain's memory, as the loading screen can't be drawn then either;
//  - with the loading plaque up, the loading screen's frame, the notice under the plaque;
//  - before gs::Init, the boot splash, the notice under the logo: Qcommon_Init reads config.cfg
//    before the renderer exists;
//  - otherwise the game's frame, which PS2_EndFrame draws the notice over and shows at once.
//    SCR_UpdateScreen draws nothing between gs::Init and SCR_Init, nor while the client has the
//    screen off, so whether one was drawn is counted.
Where ShowFrame()
{
    if (ps2::rs::FrameStarted() || ps2::cmdbuf::LentToWorldLoad())
    {
        return Where::Nowhere;
    }
    if (ps2::loading::Shown())
    {
        return ps2::loading::Redraw() ? Where::LoadingScreen : Where::Nowhere;
    }
    if (ps2::splash::CanShowImage())
    {
        return ShowOnSplash() ? Where::Splash : Where::Nowhere;
    }

    const int framesBefore = s_framesDrawn;
    SCR_UpdateScreen();
    return (s_framesDrawn != framesBefore) ? Where::Game : Where::Nowhere;
}

// Waits until the notice has been up kMinShownMsec.
void WaitOutMinimum()
{
    while (Sys_Milliseconds() - s_shownMsec < kMinShownMsec)
    {
        DelayThread(kWaitStepMicros);
    }
}

} // namespace

bool BeginSaveNotice(Device & device, const SaveAccess access)
{
    if (s_shown)
    {
        return true;
    }

    // The probe and the frame below hold the main thread up for some tens of milliseconds each
    // (the first card probe of a session starts MCMAN and MCSERV, about 200), while audsrv plays
    // what the mixer painted ahead (s_mixahead, 0.2 s). Topped up before each, it lasts until the
    // music's fade begins, rather than running out first and stopping it dead.
    ps2::audio::MixAhead();

    // A device that can't be used (no card in the slot, ...) fails the access at once: no notice
    // for that.
    if (!device.Probe())
    {
        return false;
    }

    s_shown  = true;
    s_access = access;
    s_kind   = device.Kind();

    // The game stands still from here until EndSaveNotice, with no frame to stop a motor.
    IN_RumbleStop();

    ps2::audio::MixAhead();
    s_where = ShowFrame();
    if (s_where == Where::Nowhere)
    {
        // A device is never read or written with nobody told. No path gets here now: everything
        // that reads or writes a device does it between frames, with the screen on or the
        // loading plaque up, and the boot's config.cfg read comes before gs::Init.
        s_shown = false;
        Com_DPrintf("Save notice: nothing can be drawn here, so the %s is left alone.\n", DeviceName(s_kind));
        return false;
    }
    s_shownMsec = Sys_Milliseconds();

    // The music fades out under the notice, and is heard out; then the output holds still, as
    // audsrv would loop its last tenth of a second through the device's stall (sound_hold.h).
    ps2::audio::HoldMusic();
    PS2_SoundHoldBegin();
    return true;
}

void EndSaveNotice()
{
    if (!s_shown)
    {
        return;
    }

    WaitOutMinimum();
    Com_DPrintf("Save notice: down after %d ms.\n", Sys_Milliseconds() - s_shownMsec);

    s_shown = false;

    // Off the loading screen or the splash at once. Over the game, the next frame is drawn
    // without it.
    if (ps2::loading::Shown())
    {
        ps2::loading::Redraw();
    }
    else if (s_where == Where::Splash)
    {
        ps2::splash::HideImage();
    }

    PS2_SoundHoldEnd();
    ps2::audio::ReleaseMusic();
}

bool SaveNoticeShown()
{
    return s_shown;
}

void DrawSaveNotice(const bool onLoadingScreen)
{
    if (onLoadingScreen)
    {
        int plaqueWidth  = 0;
        int plaqueHeight = 0;
        PS2_DrawGetPicSize(&plaqueWidth, &plaqueHeight, "loading");

        const int plaqueBottom = ((viddef.height - plaqueHeight) / 2) + plaqueHeight;
        DrawBox(kBoxLeft, (plaqueBottom + kGapUnderPlaque) - ((viddef.height - 240) / 2));
        return;
    }

    PS2_DrawFadeScreen();
    DrawBox(kBoxLeft, kBoxTopOverGame);
    ++s_framesDrawn;
}

} // namespace ps2::save
