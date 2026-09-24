/* ================================================================================================
 * File: engine_profile.h
 * Brief: Profile probes for the engine's own C code.
 *        NOTE: Shared header between C and C++.
 *
 *        The renderer's probes see only what happens inside the refresh calls, which left
 *        everything else Qcommon_Frame does - the server frame, parsing what it sent, building
 *        the client's scene, mixing sound - as one unmeasured remainder. That remainder is a
 *        third of a typical frame's EE work and most of the frames that miss vsync, so these
 *        split it. PS2_PROFILE_SCOPED_EVENT is a C++ destructor and the engine is C, hence a
 *        begin/end pair per site instead.
 *
 *        The events themselves are declared with the rest in renderer/profile.h, which is what
 *        puts them in the frame log.
 *
 *        Reading the log: SV_Frame and CL_ReadPackets run before PS2_BeginFrame rolls the
 *        profiler over, so their time is charged to the row *before* the frame whose Frame
 *        column contains it. Shift those two columns down a row before adding a row up. A
 *        parse spike in the frame after a log dump is lost with the dropped row.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_DEBUG_ENGINE_PROFILE_H
#define PS2_DEBUG_ENGINE_PROFILE_H

/* One per probe site. A site never nests inside itself, so each keeps a single start time. */
enum
{
    PS2_PROF_SERVER,   /* SV_Frame - game logic, or reading the next demo packet on playback */
    PS2_PROF_CL_PARSE, /* CL_ReadPackets - parsing server messages */
    PS2_PROF_CL_SCENE, /* CL_AddEntities - entity, particle, temp entity and dlight lists */
    PS2_PROF_SND_MIX,  /* S_Update - spatialize and mix; the "Sound" probe nests inside it */
    PS2_PROF_FS_IO,    /* FS_FOpenFile/FS_Read/FS_FCloseFile - nests inside whichever phase loads */

    PS2_PROF_SITE_COUNT
};

#if PS2_QUAKE_PROFILE

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void PS2Quake_ProfileBegin(int site);
void PS2Quake_ProfileEnd(int site);

/* Names a file being opened in the frame log (ps2::debug::FrameLogNoteOpen). */
void PS2Quake_FrameLogNoteOpen(const char * fileName);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#else /* PS2_QUAKE_PROFILE */

#define PS2Quake_ProfileBegin(site) ((void)0)
#define PS2Quake_ProfileEnd(site)   ((void)0)
#define PS2Quake_FrameLogNoteOpen(fileName) ((void)0)

#endif /* PS2_QUAKE_PROFILE */

#endif /* PS2_DEBUG_ENGINE_PROFILE_H */
