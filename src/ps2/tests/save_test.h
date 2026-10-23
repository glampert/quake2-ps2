#pragma once
/* ================================================================================================
 * File: save_test.h
 * Brief: End-to-end save game test: plays through saving, loading, level changes and the
 *        autosave with the real console commands, and checks what comes back.
 *
 *        "ps2_testsaves 1" runs it against the drive the game booted from (host files under the
 *        emulator, the USB drive or the hard disk on a console); "2" repeats the save and load on
 *        the memory card as well; "3" only loads the slot 7 save an earlier run left behind on the
 *        boot drive, to check that a save outlives a rebuild. Each step logs a "SaveTest:" line
 *        and the run ends with a PASS/FAIL summary, then quits unless "ps2_test_quit" is 0
 *        (test_quit.h).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::test {

// Advances the save test by one frame. Call once a frame, between frames (main.cpp): its checks
// read the save device under the save notice, which draws a frame of its own.
void RunSaveTest();

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
