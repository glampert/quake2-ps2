/* ================================================================================================
 * File: load_trace.h
 * Brief: Tracing of the level loading path: from a cinematic's end, through the server's spawn,
 *        the client's precache and the refresh registration, to the first 3D view on screen.
 *        Every file loaded along the way is traced too, with its size and how long it took.
 *        NOTE: Shared header between C and C++.
 *
 *        A trace line goes through Com_Printf, so it also lands in the log file
 *        (debug/log_file.h), stamped with the time:
 *
 *          [  41.802] [load +   93 ms] FS_LoadFile maps/base1.bsp: 2711 KB from pak in 3480 ms
 *
 *        "+ms" is the time since the previous trace line, which is how long the step it closes
 *        took. Nothing in a frame is traced, apart from a heartbeat a second while a level
 *        comes up (CL_Frame).
 *
 *        PS2_QUAKE_LOAD_TRACE is 0 in the repository: set it to 1 to trace, and back to 0
 *        before committing. When it is 0 every trace compiles to nothing. It also decides
 *        whether the log file is on by default, since that is where the trace is read back.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_DEBUG_LOAD_TRACE_H
#define PS2_DEBUG_LOAD_TRACE_H

#define PS2_QUAKE_LOAD_TRACE 0

#if PS2_QUAKE_LOAD_TRACE

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Prints "[load +<ms since the previous trace> ms] <message>" with Com_Printf. */
void PS2Quake_LoadTrace(const char * fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define PS2_LOAD_TRACE(...) PS2Quake_LoadTrace(__VA_ARGS__)

#else /* PS2_QUAKE_LOAD_TRACE */

#define PS2_LOAD_TRACE(...) ((void)0)

#endif /* PS2_QUAKE_LOAD_TRACE */
#endif /* PS2_DEBUG_LOAD_TRACE_H */
