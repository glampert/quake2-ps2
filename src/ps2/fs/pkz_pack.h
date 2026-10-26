/* ================================================================================================
 * File: pkz_pack.h
 * Brief: Reading a compressed game pack, pakN.pkz (the format is in pkz_format.h), for the
 *        engine's file system. common/filesys.c looks for pakN.pkz before each pakN.pak and keeps
 *        its own directory hash, links and search order; this is the part that knows zips.
 *        NOTE: Shared header between C and C++.
 *
 *        Everything inflates on the EE, with miniz's tinfl (already linked for the save games).
 *        During a load the EE mostly waits on the IOP's file server anyway, and the IOP itself
 *        - a 37 MHz R3000 with no data cache, already running the USB, FAT and sound drivers -
 *        would inflate about as fast as USB reads, so it would become the bottleneck.
 *
 *        Only the main thread may use a pack: whole-file reads and streams share its descriptor.
 *        The background loader (async_io.h) reads through a second one, PS2_PkzLocate's, and
 *        never through the main thread's.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_FS_PKZ_PACK_H
#define PS2_FS_PKZ_PACK_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct ps2_pkz_s ps2_pkz_t;

/* Called by PS2_PkzListFiles once per file, in directory order, with the index the calls below
   take. 'name' is NUL-terminated and shorter than MAX_QPATH. */
typedef void (*ps2_pkz_file_fn)(void * context, int index, const char * name, int lengthBytes);

/* Opens the pack at 'path' and reads its directory. NULL if there is no such file. A file that
   isn't a pack this build can read is a Sys_Error: this runs inside Qcommon_Init, before
   Com_Error has anywhere to go (see FS_LoadPackFile). */
ps2_pkz_t * PS2_PkzOpen(const char * path);

/* How many files PS2_PkzListFiles will report. Directory entries and names of MAX_QPATH or more
   characters are skipped, with a warning. */
int PS2_PkzNumFiles(const ps2_pkz_t * pkz);

/* Reports every file to 'fn', then frees the copy of the zip directory PS2_PkzOpen kept for it.
   Call it once, right after PS2_PkzOpen. */
void PS2_PkzListFiles(ps2_pkz_t * pkz, ps2_pkz_file_fn fn, void * context);

/* Reads all of file 'index' into 'dest', which holds its length. Returns 0 if the pack is
   damaged there, for the caller to report with the file's name. */
int PS2_PkzRead(ps2_pkz_t * pkz, int index, void * dest);

/* Opens file 'index' as a stdio stream at its first byte: fread, fseek, ftell and fclose work on
   it as on a pak's. A stored file is a plain stream on the pack, positioned at the file's offset
   in it; a deflated one is an inflating stream whose positions start at 0. The loaders take
   ftell at the start and seek relative to it, so both work. 'name' is for error messages.
   NULL if the pack is damaged there. */
FILE * PS2_PkzOpenStream(ps2_pkz_t * pkz, int index, const char * name);

/* Where file 'index' lies, for the background loader: the pack's reader descriptor (a second
   one on the pack, opened here on first use and owned by the loader side), the data's offset and
   compressed size, its inflated size, and whether it is deflated (otherwise stored: the range is
   the file's bytes). 1 on success, 0 if the pack is damaged there, -1 if the descriptor couldn't
   be opened (the caller loads synchronously). Reads at most a local header, on the main thread. */
int PS2_PkzLocate(ps2_pkz_t * pkz, int index, int * fd, unsigned * offset, unsigned * compBytes,
                  unsigned * rawBytes, int * deflated);

/* Closes the pack. Streams still open on it keep it alive until they are closed. Call
   PS2_AsyncIoDrain first: a background read may still be going through the reader descriptor. */
void PS2_PkzClose(ps2_pkz_t * pkz);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PS2_FS_PKZ_PACK_H */
