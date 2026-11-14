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

#ifndef CL_SCREEN_H
#define CL_SCREEN_H

// screen.h

extern vrect_t scr_vrect;  // position of render window
extern float scr_conlines; // lines of console to display
extern float scr_con_current;
extern int sb_lines;

extern cvar_t * scr_viewsize;
extern cvar_t * crosshair;
extern cvar_t * crosshair_scale; // [PS2_QUAKE]: the crosshair's size on screen (cl_view.c)

extern char crosshair_pic[MAX_QPATH];
extern int crosshair_width;
extern int crosshair_height;

void SCR_Init(void);
void SCR_UpdateScreen(void);
void SCR_SizeUp(void);
void SCR_SizeDown(void);
void SCR_CenterPrint(const char * str);
void SCR_BeginLoadingPlaque(void);
void SCR_EndLoadingPlaque(void);
void SCR_DebugGraph(float value, int color);
void SCR_TouchPics(void);
void SCR_RunConsole(void);
void SCR_AddDirtyPoint(int x, int y);
void SCR_DirtyScreen(void);

//
// scr_cin.c
//
void SCR_PlayCinematic(const char * name);
void SCR_RunCinematic(void);
void SCR_StopCinematic(void);
void SCR_FinishCinematic(void);
qboolean SCR_DrawCinematic(void);

// [PS2_QUAKE]: Added to PS2 Quake for testing.
qboolean CinematicTest_PlayDirect(const char * filename);
qboolean CinematicTest_RunFrame(void);

#endif // CL_SCREEN_H
