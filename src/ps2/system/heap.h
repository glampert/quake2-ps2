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
namespace ps2::heap {

// NOTE: Be sure to update s_memTagNames[] in heap.cpp when changing this enum!
enum class MemTag : size_t
{
    ElfSys,    // Memory used by the system/kernel and the estimate size of the ELF executable.
    OpNew,     // C++ operator new/new[].
    Quake,     // Game allocations: Z_Malloc/Z_TagMalloc/etc.
    Renderer,  // Things related to rendering / the refresh module.
    TexImage,  // Allocs related to images/textures/palettes.
    AliasMdl,  // MD2/Alias models.
    SpriteMdl, // Sprite models.
    WorldMdl,  // World geometry.
    Lightmap,  // Lightmap atlas buffers (see renderer/lightmap.cpp).
    Audio,     // Decoded sound cache. Its own tag because it is one of the largest pools in the game.

    TagCount,  // Number of entries in this enum. Internal use only.
};

enum class MemAlign : size_t {};

void * Alloc(size_t sizeBytes, MemTag tag);
void * AllocAligned(MemAlign alignment, size_t sizeBytes, MemTag tag);
void Free(void * ptr, size_t sizeBytes, MemTag tag);

// Total EE RAM as reported by the kernel (32MB on a retail console).
size_t GetTotalMemBytes();

// EE RAM still available to the program right now: the gap between the current
// program break and the ceiling the kernel set up for the heap.
size_t GetAvailableMemBytes();

// Books the RAM that the game can never allocate - EE kernel, the loaded ELF
// image (text/data/bss), the main thread stack - into MemTag::ElfSys. Call once,
// as early as possible in main().
void TagsAddSystemMem();
void TagsAddMem(MemTag tag, size_t sizeBytes);

// Formats a byte count as "12.34 MB" (abbreviated) or "12.34 Megabytes" into the
// caller's buffer, and returns it so the call can be nested in a printf argument
// list. Takes the buffer rather than owning a static one so two calls in the same
// format string don't overwrite each other.
constexpr int kMemUnitStrSize = 32;
const char * FormatMemoryUnit(size_t memorySizeInBytes, bool abbreviated,
                              char * outBuffer, size_t outBufferSize);

struct MemStats
{
    size_t totalBytes;
    size_t peakBytes; // High-water mark of totalBytes. See GetPeakMemBytes.
    size_t totalAllocs;
    size_t totalFrees;
    size_t smallestAlloc;
    size_t largestAlloc;
};

const MemStats & GetStatsForMemTag(MemTag tag);
const char * GetNameForMemTag(MemTag tag);

// A read-only snapshot of the allocator's own view of the heap, which the memtag
// table cannot give: the tags say how many bytes each subsystem holds, this says
// how those bytes are arranged. Cheap (a walk of dlmalloc's bins) and, unlike the
// probe on the out-of-memory path, it allocates nothing - so it is safe to call
// from instrumentation without changing what it is measuring.
struct HeapStats
{
    size_t arenaBytes;    // everything dlmalloc has taken from the system, ever
    size_t inUseBytes;    // handed out to callers
    size_t freeBytes;     // free, across every chunk below
    size_t topChunkBytes; // the single contiguous run at the end of the arena
    size_t fastbinBytes;  // free bytes parked in fastbins
    size_t freeChunks;    // free chunk count, the top chunk included
    size_t fastbinChunks; // of which are fastbins: small, and deliberately left
                          // uncoalesced until the next large request needs them
};

HeapStats GetHeapStats();

// High-water mark of the sum of every tag - the largest the game was ever holding
// at one time. Not the same as adding up the per-tag peaks: those happen at
// different moments. This is the number that says whether a map change fits, since
// the transient (old map still resident while the new one loads) never shows up in
// a steady-state reading.
size_t GetPeakMemBytes();

// A second high-water of the same running total, which the caller can restart: the map cycle
// restarts it as each map is issued, so it reads the peak of one transition rather than of the
// whole run. With it, every tag's bytes at the moment that peak was set - what the peak was made
// of, which the per-tag peaks cannot say since those happen at different moments.
void ResetWindowPeak();
size_t GetWindowPeakMemBytes();
size_t GetWindowPeakTagBytes(MemTag tag);

// Renders the whole memory-tag table into the caller's buffer and returns it.
// Truncates rather than overrunning if the buffer is short; kMemTagsDumpSize
// is big enough for the full table.
constexpr int kMemTagsDumpSize = 2048;
const char * DumpMemTags(char * outBuffer, size_t outBufferSize);

} // namespace ps2::heap
#endif // __cplusplus

// ps2::heap C wrappers called by the Quake 2 code.
#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

void * PS2Quake_ZMalloc(size_t sizeBytes);
void PS2Quake_ZFree(void * ptr, size_t sizeBytes);

void * PS2Quake_AudioMalloc(size_t sizeBytes);
void PS2Quake_AudioFree(void * ptr, size_t sizeBytes);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // PS2_SYSTEM_HEAP_H
