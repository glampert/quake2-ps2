#pragma once
/* ================================================================================================
 * File: rumble.h
 * Brief: Force feedback through the gamepad's two vibration motors. The client reports
 *        what happens to the local player through the IN_Rumble* hooks declared in
 *        client/input.h - weapon fire, damage taken, item pickups and powerups coming
 *        on - and rumble.cpp answers each with a short burst from its effect tables,
 *        overlapping bursts that run at the same time. The input seam (input.cpp)
 *        sends the result to the pad once per frame. Gated by the in_rumble cvar
 *        (on by default); set in_rumbledebug to echo each effect as it starts.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::input {

class GamePad;

// Registers the rumble cvars and binds the pad the effects play on. Call from IN_Init.
void InitRumble(GamePad & pad);

// Runs the pad's motors from the effects currently playing, or stops them while
// rumble isn't wanted: in_rumble off, no level running, the game paused, a menu or
// the console up, or a demo playing. Call once per client frame, after the pad's
// Update().
void UpdateRumble();

} // namespace ps2::input
