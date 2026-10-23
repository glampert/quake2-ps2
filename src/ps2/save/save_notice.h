#pragma once
/* ================================================================================================
 * File: save_notice.h
 * Brief: What the screen says while a save goes to its device or comes back off it, the way PS2
 *        games say it:
 *
 *            Data is being saved to the
 *            Memory Card in slot 1.
 *
 *            Do not remove the Memory Card
 *            or reset/switch off the console.
 *
 *        in the menus' text box, naming the device ps2_savedevice picks: the memory card, the USB
 *        drive, the hard disk or the emulator's host filesystem. On a console a card save stalls
 *        the game for a second or more, with nothing drawn or mixed meanwhile. So before the
 *        device is touched the notice goes up, in a frame shown before BeginSaveNotice returns;
 *        the music fades out under it rather than stopping mid-note (audio::HoldMusic), and the
 *        sound output then holds still (sound_hold.h). Afterwards the notice stays until it has
 *        been up for a second, so a quick access can still be read, and the music fades back in.
 *        A drive is quicker than the card, and gets the same notice: every save device looks and
 *        behaves the same.
 *
 *        Over the game the notice sits on the faded view; with the loading plaque up, on the
 *        loading screen, under the plaque; before the renderer exists, on the boot splash, under
 *        the id logo, which is where the boot's read of the card's config.cfg finds it. A save
 *        puts it up before the level is written out (Sys_SaveNoticeBegin, in SV_Savegame_f), the
 *        autosave as it stores the slot, a load as it reads the slot, the load and save menus
 *        around their slot listing, and config.cfg as the save device's copy is read and written
 *        (save_api.cpp).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::save {

class Device;

enum class SaveAccess
{
    Saving,
    Loading,
};

// Puts the notice up, the music out and the sound output on hold ahead of an access to `device`:
// true once all three are done, and its saved data may be read and written until EndSaveNotice
// (memcard.cpp and drive.cpp assert it). A device that can't be used (no card in the slot,
// unformatted, ...) gets no notice: false, with the reason given by SetError. False too where
// nothing can be drawn (inside a frame, between gs::Init and SCR_Init, while the world loader
// holds the frame chain): a device is never touched with nobody told. True at once if the notice
// is up already: a save puts it up before it writes the level out, and the slot's store finds it
// there.
bool BeginSaveNotice(Device & device, SaveAccess access);

// Takes the notice down, once it has been up long enough to read, lets the sound output go and
// the music fade back in. Does nothing if it isn't up.
void EndSaveNotice();

// Whether the notice is up. Every frame drawn meanwhile shows it, and is shown before the call
// that drew it returns: PS2_EndFrame and the loading screen ask.
bool SaveNoticeShown();

// Draws the notice into the frame being drawn: over the game, which it fades, or on the loading
// screen, under the plaque.
void DrawSaveNotice(bool onLoadingScreen);

} // namespace ps2::save
