/* ================================================================================================
 * File: miniz_export.h
 * Brief: Stands in for the header miniz's CMake build generates, and carries this port's
 *        miniz configuration. Every miniz header includes this one before anything else,
 *        so the library (built from the src/tools/miniz submodule) and the save code that
 *        includes miniz.h see the same settings - the compressor's struct size depends on
 *        them. Plain C: it is part of the C library build too.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Statically linked: no symbol visibility decoration. */
#define MINIZ_EXPORT

/* Only raw deflate/inflate (tdefl/tinfl) and mz_crc32 are used. */
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS

/* Every buffer, the compressor state included, comes from the tagged game heap. */
#define MINIZ_NO_MALLOC

/* Halves the compressor state (tdefl_compressor: 312 KB -> 164 KB on the EE), for a
 * slightly worse ratio. It only exists while a save entry is being written. */
#define TDEFL_LESS_MEMORY 1

#endif /* MINIZ_EXPORT_H */
