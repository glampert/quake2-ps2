/* ================================================================================================
 * File: readahead_stream.h
 * Brief: A sequential file stream the background loader (async_io.h) fills ahead of its reader:
 *        what a cinematic (.cin) and a demo (.dm2) read through, so their frames never wait on the
 *        drive. The stdio stream SCR_ReadNextFrame used to fread from refilled its 64 KB buffer on
 *        the main thread, 93 ms a time from a USB stick, and every refill frame overran audsrv's
 *        106 ms ring, which is the audible stutter.
 *        NOTE: Shared header between C and C++.
 *
 *        The stream is an fopencookie FILE*, unbuffered, so cl_cin.c's fread/FS_Read/feof/fclose
 *        and sv_send.c's fread work unchanged (the pkz deflated-stream pattern). Behind it a ring of
 *        32 KB slots, each one loader request at Priority::Stream covering one chunk of the file:
 *        all of them in flight from the open, the consumed head slot resubmitted for the next range.
 *        A read copies from the head slot and sleeps only when it has not landed yet, which on a USB
 *        stick means the loader fell more than the ring behind: 512 KB covers a 0.5 s stall during
 *        ntro's 700 KB/s peak, which is the worst the data has.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_FS_READAHEAD_STREAM_H
#define PS2_FS_READAHEAD_STREAM_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct fs_location_s;

/* Whether FS_FOpenFile should try the read-ahead stream for 'filename': a .cin or .dm2, with
   ps2_readahead_kb above 0. */
int PS2_WantsReadAheadStream(const char * filename);

/* Opens the stream on a located file (FS_LocateFile: a pak range, a stored pkz entry or a loose
   file; a deflated entry can't be read by range and is refused). On success the stream owns the
   location (a loose file's FILE* is closed with it). NULL when the loader is off, the ring can't be
   allocated or the kind is unsuitable: the caller then opens the plain stdio stream as before.
   'filename' is for messages. */
FILE * PS2_OpenReadAheadStream(const char * filename, const struct fs_location_s * loc);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PS2_FS_READAHEAD_STREAM_H */
