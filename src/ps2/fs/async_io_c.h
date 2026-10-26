/* ================================================================================================
 * File: async_io_c.h
 * Brief: The background loader's hooks for id's C (async_io.h is the C++ API). filesys.c calls
 *        PS2_AsyncIoDrain before it closes a pack: a request may still be reading through it.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_FS_ASYNC_IO_C_H
#define PS2_FS_ASYNC_IO_C_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Waits until no background request is queued or running. Call before freeing or closing
   anything one might still read through or write into. */
void PS2_AsyncIoDrain(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PS2_FS_ASYNC_IO_C_H */
