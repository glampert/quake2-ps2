/* ================================================================================================
 * File: heap.cpp
 * Brief: C++ side of the program-wide dlmalloc heap. Provides operator new/delete
 *        (routed to the dlmalloc-backed global malloc from dlmalloc.c) and the
 *        tag-accounting layer, which common.c's Z_Malloc and the renderer allocate through.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/system/heap.h"
#include "ps2/common.h" // Sys_Error, etc

#include <new>
#include <cstdlib> // malloc/free/calloc/realloc (overridden to dlmalloc in dlmalloc.c)
#include <cstdio>  // snprintf
#include <cstring> // memset

// memalign lives in <malloc.h>; declare it here to avoid dragging that header's
// other (non-strict-clean) declarations into this translation unit.
extern "C" void * memalign(size_t alignment, size_t size);

// ------------------------------------------------------------------------------------------------
// operator new / delete -> dlmalloc global heap
//
// Built with -fno-exceptions, so we cannot throw std::bad_alloc; a failed
// allocation is a fatal Sys_Error instead. delete is always noexcept.
// ------------------------------------------------------------------------------------------------

void * operator new(size_t n)   { return PS2_MemAlloc(n, MEMTAG_OPNEW); }
void * operator new[](size_t n) { return PS2_MemAlloc(n, MEMTAG_OPNEW); }

void * operator new(size_t n, const std::nothrow_t &)   noexcept { return PS2_MemAlloc(n, MEMTAG_OPNEW); }
void * operator new[](size_t n, const std::nothrow_t &) noexcept { return PS2_MemAlloc(n, MEMTAG_OPNEW); }

void * operator new(size_t n, std::align_val_t al)   { return PS2_MemAllocAligned(static_cast<size_t>(al), n, MEMTAG_OPNEW); }
void * operator new[](size_t n, std::align_val_t al) { return PS2_MemAllocAligned(static_cast<size_t>(al), n, MEMTAG_OPNEW); }

void operator delete(void * p)   noexcept { PS2_MemFree(p, /*size unknown*/0, MEMTAG_OPNEW); }
void operator delete[](void * p) noexcept { PS2_MemFree(p, /*size unknown*/0, MEMTAG_OPNEW); }

void operator delete(void * p, size_t n)   noexcept { PS2_MemFree(p, n, MEMTAG_OPNEW); }
void operator delete[](void * p, size_t n) noexcept { PS2_MemFree(p, n, MEMTAG_OPNEW); }

void operator delete(void * p, const std::nothrow_t &)   noexcept { PS2_MemFree(p, /*size unknown*/0, MEMTAG_OPNEW); }
void operator delete[](void * p, const std::nothrow_t &) noexcept { PS2_MemFree(p, /*size unknown*/0, MEMTAG_OPNEW); }

void operator delete(void * p, std::align_val_t)   noexcept { PS2_MemFree(p, /*size unknown*/0, MEMTAG_OPNEW); }
void operator delete[](void * p, std::align_val_t) noexcept { PS2_MemFree(p, /*size unknown*/0, MEMTAG_OPNEW); }

void operator delete(void * p, size_t n, std::align_val_t)   noexcept { PS2_MemFree(p, n, MEMTAG_OPNEW); }
void operator delete[](void * p, size_t n, std::align_val_t) noexcept { PS2_MemFree(p, n, MEMTAG_OPNEW); }

// ------------------------------------------------------------------------------------------------
// Tagged allocation + memory accounting.
// C linkage (declared extern "C" in heap.h) so common.c can call these.
// ------------------------------------------------------------------------------------------------

struct MemoryStats
{
    size_t totalBytes;
    size_t totalAllocs;
    size_t totalFrees;
    size_t smallestAlloc;
    size_t largestAlloc;
};

static MemoryStats s_memTagCounts[MEMTAG_COUNT] = {};

// NOTE: These should match the Ps2MemTag declaration order!
static const char * const s_memTagNames[MEMTAG_COUNT] = {
    "Misc",
    "OpNew",
    "Quake",
    "Renderer",
    "TexImage",
    "Mdl_Alias",
    "Mdl_Sprite",
    "Mdl_World",
};

static inline size_t MemTagToIndex(PS2MemTag tag)
{
    const int t = static_cast<int>(tag);
    return (t >= 0 && t < MEMTAG_COUNT) ? static_cast<size_t>(t) : static_cast<size_t>(MEMTAG_MISC);
}

static inline void AccountAlloc(PS2MemTag tag, size_t bytes)
{
    MemoryStats * c = &s_memTagCounts[MemTagToIndex(tag)];
    c->totalBytes  += bytes;
    c->totalAllocs += 1u;
    if (c->smallestAlloc == 0u || bytes < c->smallestAlloc) { c->smallestAlloc = bytes; }
    if (bytes > c->largestAlloc) { c->largestAlloc = bytes; }
}

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

extern "C" {

void * PS2_MemAlloc(size_t sizeBytes, PS2MemTag tag)
{
    const size_t n = (sizeBytes != 0u ? sizeBytes : 1u);
    void * p = std::malloc(n);
    if (p == nullptr)
    {
        Sys_Error("PS2_MemAlloc: failed to allocate %zu bytes (%s)",
                  n, s_memTagNames[MemTagToIndex(tag)]);
    }
    AccountAlloc(tag, n);
    return p;
}

void * PS2_MemAllocAligned(size_t alignment, size_t sizeBytes, PS2MemTag tag)
{
    const size_t n = (sizeBytes != 0u ? sizeBytes : 1u);
    void * p = memalign(alignment, n);
    if (p == nullptr)
    {
        Sys_Error("PS2_MemAllocAligned: failed to allocate %zu bytes (align %zu, %s)",
                  n, alignment, s_memTagNames[MemTagToIndex(tag)]);
    }
    AccountAlloc(tag, n);
    return p;
}

void PS2_MemFree(void * ptr, size_t sizeBytes, PS2MemTag tag)
{
    if (ptr == nullptr) { return; }
    MemoryStats * c = &s_memTagCounts[MemTagToIndex(tag)];
    c->totalFrees += 1u;
    if (sizeBytes != 0u)
    {
        c->totalBytes = (c->totalBytes >= sizeBytes) ? (c->totalBytes - sizeBytes) : 0u;
    }
    std::free(ptr);
}

void PS2_TagsAddMem(PS2MemTag tag, size_t sizeBytes)
{
    s_memTagCounts[MemTagToIndex(tag)].totalBytes += sizeBytes;
}

const char * PS2_FormatMemoryUnit(size_t memorySizeInBytes, int abbreviated)
{
    static char s_str[64];
    const char * unit;
    double value;

    if (memorySizeInBytes >= (1024u * 1024u * 1024u))
    {
        unit  = abbreviated ? "GB" : "Gigabytes";
        value = static_cast<double>(memorySizeInBytes) / (1024.0 * 1024.0 * 1024.0);
    }
    else if (memorySizeInBytes >= (1024u * 1024u))
    {
        unit  = abbreviated ? "MB" : "Megabytes";
        value = static_cast<double>(memorySizeInBytes) / (1024.0 * 1024.0);
    }
    else if (memorySizeInBytes >= 1024u)
    {
        unit  = abbreviated ? "KB" : "Kilobytes";
        value = static_cast<double>(memorySizeInBytes) / 1024.0;
    }
    else
    {
        unit  = abbreviated ? "B" : "Bytes";
        value = static_cast<double>(memorySizeInBytes);
    }

    snprintf(s_str, sizeof(s_str), "%.2f %s", value, unit);
    return s_str;
}

} // extern "C"
