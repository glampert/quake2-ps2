/* ================================================================================================
 * File: io_log.h
 * Brief: The I/O log: what each level load read, from where, and how much of its time went to
 *        waiting for the drive and to inflating a compressed pack (fs/pkz_pack.h). Built for
 *        comparing a load from pakN.pak with the same load from pakN.pkz on a console.
 *        NOTE: Shared header between C and C++.
 *
 *        PS2_QUAKE_IO_LOG is 0 in the repository, and every hook then compiles to nothing. For a
 *        local test build pass -DPS2_QUAKE_IO_LOG=1 in CONFIG_DEFS, after clearing the objects
 *        (make doesn't track flags):
 *
 *          rm -rf build/debug/src && make CONFIG_DEFS='-DPS2_QUAKE_DEBUG=1 -DPS2_QUAKE_ASSERTS=1 -DPS2_QUAKE_PROFILE=1 -DPS2_QUAKE_IO_LOG=1'
 *
 *        and the same rm -rf and a plain make afterwards.
 *
 *        A load is the loading plaque's lifetime (PS2_LoadingScreenBegin to End). Every file it
 *        reads gets a line, and the load a summary:
 *
 *          #     ms      bytes  read_bytes  read_ms  inflate_ms  source  name
 *              1203    2776004     1151277     1105          92  pkz     maps/base1.bsp
 *          # load 3 maps/base1.bsp: 412 files, 18.31 MB delivered, 9.42 MB read; 41.2 s, of which
 *          #   30.1 s in file calls: 27.7 s reading, 2.4 s inflating
 *
 *        "ms" is the time spent inside the file system's calls for that file (open, reads, close;
 *        for a stream, summed over its life inside the load), "read_ms" the part of it spent
 *        waiting for the drive, "inflate_ms" the part spent in tinfl. For a pak or a loose file
 *        every call is a wait on the drive, so read_ms is the whole of ms; read_bytes there are
 *        the bytes asked for, which a stream's stdio buffer can round up by up to 64 KB.
 *        Streams read with raw fread (the demo and cinematic readers) are not seen.
 *
 *        The lines stay in RAM until the load ends, then go to stdout and are appended to
 *        <gamedir>/io_log.txt with one open/write/close, as frame_log.txt is: the log's own
 *        writes stay out of the load they measure. Each boot starts the file afresh. The in-game
 *        console says when a load starts being recorded and where it was written, or why the
 *        write failed: on a console nothing reads stdout, so that is how a run is checked. The load trace with ps2_logfile 1 costs about
 *        23 ms per line on a USB drive, which is why it can't be used for this.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_DEBUG_IO_LOG_H
#define PS2_DEBUG_IO_LOG_H

#ifndef PS2_QUAKE_IO_LOG
#define PS2_QUAKE_IO_LOG 0
#endif

#if PS2_QUAKE_IO_LOG

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* The loading plaque went up / came down. */
void PS2_IoLogLoadBegin(void);
void PS2_IoLogLoadEnd(void);

/* A file system call is starting. The *End hook that closes it charges its time to a file. */
void PS2_IoLogCallBegin(void);

/* FS_LoadFile read 'name' whole from 'source' ("pak", "pkz" or "file"). */
void PS2_IoLogLoadFileEnd(const char * name, int lengthBytes, const char * source);

/* FS_FOpenFile opened 'file' on 'name', FS_Read read from it, FS_FCloseFile closed it. */
void PS2_IoLogStreamOpenEnd(const char * name, const void * file, int lengthBytes, const char * source);
void PS2_IoLogStreamReadEnd(const void * file, int bytes);
void PS2_IoLogStreamCloseEnd(const void * file);

/* From the pkz backend, inside a call: it read 'bytes' from the drive in 'ticks', it inflated
   for 'ticks', and the stream it is about to return inflates (so its reads report themselves). */
void PS2_IoLogStorageRead(unsigned bytes, unsigned ticks);
void PS2_IoLogInflate(unsigned ticks);
void PS2_IoLogCompressedStream(const void * file);

/* The clock the log counts in: the EE timer's 576 kHz tick. */
unsigned PS2_IoLogTicks(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define PS2_IO_LOG_LOAD_BEGIN()                    PS2_IoLogLoadBegin()
#define PS2_IO_LOG_LOAD_END()                      PS2_IoLogLoadEnd()
#define PS2_IO_LOG_CALL_BEGIN()                    PS2_IoLogCallBegin()
#define PS2_IO_LOG_LOAD_FILE_END(n, len, src)      PS2_IoLogLoadFileEnd((n), (len), (src))
#define PS2_IO_LOG_STREAM_OPEN_END(n, f, len, src) PS2_IoLogStreamOpenEnd((n), (f), (len), (src))
#define PS2_IO_LOG_STREAM_READ_END(f, bytes)       PS2_IoLogStreamReadEnd((f), (bytes))
#define PS2_IO_LOG_STREAM_CLOSE_END(f)             PS2_IoLogStreamCloseEnd((f))

#else /* PS2_QUAKE_IO_LOG */

#define PS2_IO_LOG_LOAD_BEGIN()                    ((void)0)
#define PS2_IO_LOG_LOAD_END()                      ((void)0)
#define PS2_IO_LOG_CALL_BEGIN()                    ((void)0)
#define PS2_IO_LOG_LOAD_FILE_END(n, len, src)      ((void)0)
#define PS2_IO_LOG_STREAM_OPEN_END(n, f, len, src) ((void)0)
#define PS2_IO_LOG_STREAM_READ_END(f, bytes)       ((void)0)
#define PS2_IO_LOG_STREAM_CLOSE_END(f)             ((void)0)

#endif /* PS2_QUAKE_IO_LOG */
#endif /* PS2_DEBUG_IO_LOG_H */
