/* ================================================================================================
 * File: heap.h
 * Brief: C/C++ memory allocation and stats.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_SYSTEM_HEAP_H
#define PS2_SYSTEM_HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// NOTE: Be sure to update s_memTagNames[] in heap.cpp when changing this enum!
typedef enum
{
    MEMTAG_MISC,       // Miscellaneous/uncategorized (includes the estimate size of the ELF executable).
    MEMTAG_OPNEW,      // C++ operator new/new[].
    MEMTAG_QUAKE,      // Game allocations: Z_Malloc/Z_TagMalloc/etc.
    MEMTAG_RENDERER,   // Things related to rendering / the refresh module.
    MEMTAG_TEXIMAGE,   // Allocs related to images/textures/palettes.
    MEMTAG_MDL_ALIAS,  // MD2/Alias models.
    MEMTAG_MDL_SPRITE, // Sprite models.
    MEMTAG_MDL_WORLD,  // World geometry.
    MEMTAG_COUNT,      // Number of entries in this enum. Internal use only.
} PS2MemTag;

void * PS2_MemAlloc(size_t sizeBytes, PS2MemTag tag);
void * PS2_MemAllocAligned(size_t alignment, size_t sizeBytes, PS2MemTag tag);
void PS2_MemFree(void * ptr, size_t sizeBytes, PS2MemTag tag);
void PS2_TagsAddMem(PS2MemTag tag, size_t sizeBytes);

// Formatter for printing the memory tags.
const char * PS2_FormatMemoryUnit(size_t memorySizeInBytes, int abbreviated);

typedef struct
{
    size_t totalBytes;
    size_t totalAllocs;
    size_t totalFrees;
    size_t smallestAlloc;
    size_t largestAlloc;
} PS2MemStats;

const PS2MemStats * PS2_GetStatsForMemTag(PS2MemTag tag);
const char * PS2_GetNameForMemTag(PS2MemTag tag);
const char * PS2_DumpMemTags(void);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // PS2_SYSTEM_HEAP_H
