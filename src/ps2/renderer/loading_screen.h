/* ================================================================================================
 * File: loading_screen.h
 * Brief: What the screen shows while a level loads: the "loading" plaque, with a status bar
 *        along the bottom naming the file being read, how many files and megabytes have been
 *        read so far, and for how long.
 *        NOTE: Shared header between C and C++.
 *
 *        SCR_BeginLoadingPlaque draws the plaque once and then stops the client drawing until
 *        the level is up. With ps2_gs_latency on, though, a frame only reaches the screen when
 *        the next one begins, so that plaque used to sit unseen behind a black screen for the
 *        whole load. Each loading screen frame is shown before the call that draws it returns,
 *        and the file hooks keep redrawing it, so a slow load from USB shows where it is.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_RENDERER_LOADING_SCREEN_H
#define PS2_RENDERER_LOADING_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* The loading plaque went up (SCR_BeginLoadingPlaque): show the loading screen now. */
void PS2_LoadingScreenBegin(void);

/* The plaque came down (SCR_EndLoadingPlaque, or its timeout): the client draws again. */
void PS2_LoadingScreenEnd(void);

/* A file is about to be read (FS_LoadFile, FS_FOpenFile). While the plaque is up it is named on
   the status bar, redrawn at most every few hundred milliseconds unless the file is large. */
void PS2_LoadingScreenNoteFile(const char * fileName, int lengthBytes);

/* Backend-internal, in ref.cpp: draws one loading screen frame with 'status' on the bar and
   shows it before returning. Returns 0 without drawing if a frame is already open, or while the
   world loader holds the frame chain's memory (cmdbuf::LentToWorldLoad): the screen then keeps
   the last frame drawn until the .bsp has been parsed. */
int PS2_DrawLoadingScreen(const char * status);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PS2_RENDERER_LOADING_SCREEN_H */
