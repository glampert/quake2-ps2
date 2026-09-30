#pragma once
/* ================================================================================================
 * File: save_test.h
 * Brief: End-to-end save game test: plays through saving, loading, level changes and the
 *        autosave with the real console commands, and checks what comes back.
 *
 *        "ps2_testsaves 1" runs it against host files; "2" repeats the save and load on the
 *        memory card as well; "3" only loads the slot 7 save an earlier run left behind, to
 *        check that a save outlives a rebuild. Each step logs a "SaveTest:" line and the run
 *        ends with a PASS/FAIL summary.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::test {

// Advances the save test by one frame. Call every frame from PS2_EndFrame.
void RunSaveTest();

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
