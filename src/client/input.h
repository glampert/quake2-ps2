/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef CL_INPUT_H
#define CL_INPUT_H

// input.h -- external (non-keyboard) input devices

void IN_Init(void);
void IN_Shutdown(void);
void IN_Activate(qboolean active);

void IN_Frame(void);
void IN_Commands(void);        // opportunity for devices to stick commands on the script buffer
void IN_Move(usercmd_t * cmd); // add additional movement on top of the keyboard move cmd

// [PS2_QUAKE] 2026-09-27
// Force feedback: the client reports what happens to the local player, and the input
// backend decides how the controller responds, if at all.
void IN_RumbleMuzzleFlash(int weapon);       // the player fired: MZ_*, silenced bit stripped
void IN_RumbleItemSound(const char * sound); // a sound on the player's CHAN_ITEM: item pickups
void IN_RumbleFrame(void);                   // a valid frame was parsed: damage, powerups...
void IN_RumbleStop(void);                    // stop the motors now, e.g. ahead of a load

#endif // CL_INPUT_H
