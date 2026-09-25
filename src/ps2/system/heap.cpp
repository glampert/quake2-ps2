/* ================================================================================================
 * File: heap.cpp
 * Brief: C++ side of the program-wide dlmalloc heap. Provides operator new/delete
 *        (routed to the dlmalloc-backed global malloc from dlmalloc.c) and the
 *        tag-accounting layer, which common.c's Z_Malloc and the renderer allocate through.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/system/heap.h"
#include "ps2/common.h"            // Sys_Error, etc
#include "ps2/debug/stack_trace.h" // PrintStackTrace

#include <new>
#include <cstdio>   // snprintf
#include <cstring>  // memset
#include <cstdint>  // uintptr_t
#include <unistd.h> // sbrk
#include <kernel.h> // EndOfHeap, GetMemorySize

// dlmalloc
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow" // issue with mallinfo
extern "C" {
    #define USE_DL_PREFIX 1
    #include "dlmalloc/malloc.h"

    // malloc.h declares this one unprefixed even under USE_DL_PREFIX (both arms of
    // its #ifndef say "mallinfo"), so the name it actually exports goes undeclared.
    // malloc.c does define dlmallinfo - see public_mALLINFo - so declare it here.
    struct mallinfo dlmallinfo(void);
}
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------
// operator new / delete -> dlmalloc global heap redirect
//
// Built with -fno-exceptions, so we cannot throw std::bad_alloc; a failed
// allocation is a fatal Sys_Error instead. delete is always noexcept.
// ------------------------------------------------------------------------------------------------

void * operator new(std::size_t n)   { return ps2::heap::Alloc(n, ps2::heap::MemTag::OpNew); }
void * operator new[](std::size_t n) { return ps2::heap::Alloc(n, ps2::heap::MemTag::OpNew); }

void * operator new(std::size_t n, const std::nothrow_t &)   noexcept { return ps2::heap::Alloc(n, ps2::heap::MemTag::OpNew); }
void * operator new[](std::size_t n, const std::nothrow_t &) noexcept { return ps2::heap::Alloc(n, ps2::heap::MemTag::OpNew); }

void * operator new(std::size_t n, std::align_val_t al)   { return ps2::heap::AllocAligned(ps2::heap::MemAlign(al), n, ps2::heap::MemTag::OpNew); }
void * operator new[](std::size_t n, std::align_val_t al) { return ps2::heap::AllocAligned(ps2::heap::MemAlign(al), n, ps2::heap::MemTag::OpNew); }

void operator delete(void * p)   noexcept { ps2::heap::Free(p, dlmalloc_usable_size(p), ps2::heap::MemTag::OpNew); }
void operator delete[](void * p) noexcept { ps2::heap::Free(p, dlmalloc_usable_size(p), ps2::heap::MemTag::OpNew); }

void operator delete(void * p, std::size_t n)   noexcept { ps2::heap::Free(p, n, ps2::heap::MemTag::OpNew); }
void operator delete[](void * p, std::size_t n) noexcept { ps2::heap::Free(p, n, ps2::heap::MemTag::OpNew); }

void operator delete(void * p, const std::nothrow_t &)   noexcept { ps2::heap::Free(p, dlmalloc_usable_size(p), ps2::heap::MemTag::OpNew); }
void operator delete[](void * p, const std::nothrow_t &) noexcept { ps2::heap::Free(p, dlmalloc_usable_size(p), ps2::heap::MemTag::OpNew); }

void operator delete(void * p, std::align_val_t)   noexcept { ps2::heap::Free(p, dlmalloc_usable_size(p), ps2::heap::MemTag::OpNew); }
void operator delete[](void * p, std::align_val_t) noexcept { ps2::heap::Free(p, dlmalloc_usable_size(p), ps2::heap::MemTag::OpNew); }

void operator delete(void * p, std::size_t n, std::align_val_t)   noexcept { ps2::heap::Free(p, n, ps2::heap::MemTag::OpNew); }
void operator delete[](void * p, std::size_t n, std::align_val_t) noexcept { ps2::heap::Free(p, n, ps2::heap::MemTag::OpNew); }

namespace ps2::heap {

// ------------------------------------------------------------------------------------------------
// Tagged allocation + memory accounting
// ------------------------------------------------------------------------------------------------

static constexpr auto kMemTagCount = static_cast<size_t>(MemTag::TagCount);
static MemStats s_memTagCounts[kMemTagCount] = {};

// NOTE: These should match the MemTag enum declaration order!
static const char * const s_memTagNames[kMemTagCount] = {
    "ELF_Sys",
    "OpNew",
    "Quake",
    "Renderer",
    "TexImage",
    "Alias",
    "Sprite",
    "World",
    "Lightmap",
    "Audio",
};

static inline size_t MemTagToIndex(const MemTag tag)
{
    const auto t = static_cast<size_t>(tag);
    return t < kMemTagCount ? t : static_cast<size_t>(MemTag::ElfSys);
}

// Running sum of every tag's totalBytes, and the largest it has ever been. Kept
// incrementally rather than summed on demand so the peak is sampled at every
// allocation - the map-change transient we care about lasts milliseconds and would
// be missed by anything that only looks when asked.
static size_t s_liveTotalBytes = 0;
static size_t s_peakTotalBytes = 0;

// The restartable peak (ResetWindowPeak) and every tag's bytes when it was set. The snapshot is
// retaken whenever the window peak rises, which happens during a level load and almost never
// after, so its cost lands where nothing is being timed.
static size_t s_windowPeakBytes = 0;
static size_t s_windowPeakTagBytes[kMemTagCount] = {};

static inline void AccountAlloc(const MemTag tag, const size_t bytes)
{
    MemStats & s = s_memTagCounts[MemTagToIndex(tag)];
    s.totalBytes  += bytes;
    s.totalAllocs += 1u;

    if (s.smallestAlloc == 0u || bytes < s.smallestAlloc) { s.smallestAlloc = bytes; }
    if (bytes > s.largestAlloc) { s.largestAlloc = bytes; }
    if (s.totalBytes > s.peakBytes) { s.peakBytes = s.totalBytes; }

    s_liveTotalBytes += bytes;
    if (s_liveTotalBytes > s_peakTotalBytes) { s_peakTotalBytes = s_liveTotalBytes; }

    if (s_liveTotalBytes > s_windowPeakBytes) [[unlikely]]
    {
        s_windowPeakBytes = s_liveTotalBytes;
        for (size_t i = 0; i < kMemTagCount; ++i)
        {
            s_windowPeakTagBytes[i] = s_memTagCounts[i].totalBytes;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Heap dump
// ------------------------------------------------------------------------------------------------

// The largest block dlmalloc would actually hand out right now, found by probing
// and immediately giving back. mallinfo reports free bytes and free chunk count
// but not the largest run, and the largest run is the number that decides whether
// a big contiguous request can be served.
//
// Only ever called on the way to Sys_Error. It allocates, so it perturbs the heap
// it is measuring - acceptable exactly once, at the point where the program is
// already terminating.
__attribute__((cold, noinline))
static size_t LargestAllocatableBlock(const size_t upperBound)
{
    if (upperBound < 16u) { return 0u; }

    // Binary search the largest size that succeeds. ~24 iterations for a 32 MB
    // span, each one a malloc/free pair.
    size_t lo = 0u;         // known to succeed (trivially)
    size_t hi = upperBound; // known to fail (the caller just proved it)

    while (hi - lo > 4096u)
    {
        const size_t mid = lo + ((hi - lo) / 2u);
        void * const p = dlmalloc(mid);
        if (p != nullptr)
        {
            dlfree(p);
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

__attribute__((cold, noinline))
static void PrintDlmallocStats(const size_t failedRequest)
{
    const HeapStats hs = GetHeapStats();

    const size_t arena    = hs.arenaBytes;
    const size_t inUse    = hs.inUseBytes;
    const size_t freeTot  = hs.freeBytes;
    const size_t freeChks = hs.freeChunks;
    const size_t keepCost = hs.topChunkBytes;
    const size_t largest  = LargestAllocatableBlock(failedRequest);

    char a[kMemUnitStrSize], b[kMemUnitStrSize], c[kMemUnitStrSize];

    std::printf("-------------------------- DLMALLOC ---------------------------\n");
    std::printf("Arena (sbrk'd)   : %s\n", FormatMemoryUnit(arena, true, a, sizeof(a)));
    std::printf("In use           : %s\n", FormatMemoryUnit(inUse, true, a, sizeof(a)));
    std::printf("Free total       : %s  in %zu chunks (avg %s)\n",
                FormatMemoryUnit(freeTot, true, a, sizeof(a)), freeChks,
                FormatMemoryUnit((freeChks != 0u) ? (freeTot / freeChks) : 0u, true, b, sizeof(b)));
    std::printf("Top releasable   : %s\n", FormatMemoryUnit(keepCost, true, a, sizeof(a)));
    std::printf("Largest free blk : %s   (the failed request wanted %s)\n",
                FormatMemoryUnit(largest, true, a, sizeof(a)),
                FormatMemoryUnit(failedRequest, true, b, sizeof(b)));

    // The verdict, spelled out, so the log answers the question without arithmetic.
    if (freeTot >= failedRequest)
    {
        std::printf("VERDICT: FRAGMENTATION. %s free in total, but the largest single run is\n"
                    "         only %s. The bytes exist; they are not adjacent.\n",
                    FormatMemoryUnit(freeTot, true, a, sizeof(a)),
                    FormatMemoryUnit(largest, true, c, sizeof(c)));
    }
    else
    {
        std::printf("VERDICT: EXHAUSTION. Only %s free in total, less than the request.\n",
                    FormatMemoryUnit(freeTot, true, a, sizeof(a)));
    }
    std::printf("-------------------------- DLMALLOC ---------------------------\n");
    std::fflush(stdout);
}

__attribute__((cold, noinline))
static void OutOfMemory(const size_t requestSize, const MemTag tag, const char * const funcName)
{
    // The call stack goes to stdout, not to Sys_Error: the panic screen it
    // paints has 24 lines to spend on the message and the memtag table, and
    // stdout is where the PCSX2/ps2client log goes.
    std::printf("%s: failed to allocate %zu bytes (tag: %s)\n",
                funcName, requestSize, s_memTagNames[MemTagToIndex(tag)]);

    PrintDlmallocStats(requestSize);
    ps2::debug::PrintStackTrace();

    char dump[kMemTagsDumpSize];
    Sys_Error("%s: failed to allocate %zu bytes (%s)\n%s",
              funcName, requestSize, s_memTagNames[MemTagToIndex(tag)],
              DumpMemTags(dump, sizeof(dump)));
}

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void * Alloc(const size_t sizeBytes, const MemTag tag)
{
    const size_t n = (sizeBytes != 0u ? sizeBytes : 1u);
    void * p = dlmalloc(n);

    if (p == nullptr) [[unlikely]]
    {
        OutOfMemory(sizeBytes, tag, "ps2::heap::Alloc");
    }

    AccountAlloc(tag, n);
    return p;
}

void * AllocAligned(const MemAlign alignment, const size_t sizeBytes, const MemTag tag)
{
    const size_t n = (sizeBytes != 0u ? sizeBytes : 1u);
    void * p = dlmemalign(static_cast<size_t>(alignment), n);

    if (p == nullptr) [[unlikely]]
    {
        OutOfMemory(sizeBytes, tag, "ps2::heap::AllocAligned");
    }

    AccountAlloc(tag, n);
    return p;
}

void Free(void * ptr, const size_t sizeBytes, const MemTag tag)
{
    if (ptr == nullptr) { return; }
    MemStats & s = s_memTagCounts[MemTagToIndex(tag)];
    s.totalFrees += 1u;
    if (sizeBytes != 0u)
    {
        const size_t taken = (s.totalBytes >= sizeBytes) ? sizeBytes : s.totalBytes;
        s.totalBytes      -= taken;
        s_liveTotalBytes  -= (s_liveTotalBytes >= taken) ? taken : s_liveTotalBytes;
    }
    dlfree(ptr);
}

size_t GetTotalMemBytes()
{
    // GetMemorySize() is an EE kernel syscall returning the installed RAM in
    // bytes. Retail consoles answer 32MB; fall back to that if it ever fails.
    const s32 memSize = GetMemorySize();
    return (memSize > 0) ? static_cast<size_t>(memSize) : (32u * 1024u * 1024u);
}

size_t GetAvailableMemBytes()
{
    // sbrk(0) hands back the current program break without moving it. crt0 hands
    // the kernel [_end, all remaining RAM) via SetupHeap, so the break starts at
    // _end (just past our bss) and only ever grows - and since dlmalloc.c routes
    // every allocator in the link through dlmalloc, dlmalloc is the sole caller
    // of sbrk. The gap up to EndOfHeap() is therefore exactly the RAM nothing has
    // claimed yet. Note this is *unclaimed* memory: blocks dlmalloc has already
    // sbrk'd and since freed sit in its free lists and do not show up here.
    const std::uintptr_t brk = reinterpret_cast<std::uintptr_t>(sbrk(0));
    const std::uintptr_t top = reinterpret_cast<std::uintptr_t>(EndOfHeap());

    if (brk == 0u || brk == ~static_cast<std::uintptr_t>(0) || top <= brk)
    {
        return 0u; // sbrk failed, or the heap is exhausted
    }
    return static_cast<size_t>(top - brk);
}

void TagsAddSystemMem()
{
    const size_t totalBytes = GetTotalMemBytes();
    const size_t availBytes = GetAvailableMemBytes();

    // Everything the game will never get to allocate: the first megabyte of RAM
    // reserved for the EE kernel, our ELF image (text/data/bss), and the stack
    // the kernel carved out above the heap ceiling.
    if (totalBytes > availBytes)
    {
        const size_t totalUsedBytes = totalBytes - availBytes;

        Sys_ConsoleOutput(va("RAM available: %.2f MB, used by ELF + system: %.2f MB\n",
                             static_cast<double>(availBytes) / 1024.0 / 1024.0,
                             static_cast<double>(totalUsedBytes) / 1024.0 / 1024.0));

        TagsAddMem(MemTag::ElfSys, totalUsedBytes);
    }
}

void TagsAddMem(const MemTag tag, const size_t sizeBytes)
{
    AccountAlloc(tag, sizeBytes);
}

const char * FormatMemoryUnit(const size_t memorySizeInBytes, const bool abbreviated,
                              char * const outBuffer, const size_t outBufferSize)
{
    const char * unit;
    double value;

    if (outBuffer == nullptr || outBufferSize == 0u)
    {
        return "";
    }

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

    std::snprintf(outBuffer, outBufferSize, "%.2f %s", value, unit);
    return outBuffer;
}

const MemStats & GetStatsForMemTag(const MemTag tag)
{
    return s_memTagCounts[MemTagToIndex(tag)];
}

const char * GetNameForMemTag(const MemTag tag)
{
    return s_memTagNames[MemTagToIndex(tag)];
}

size_t GetPeakMemBytes()
{
    return s_peakTotalBytes;
}

void ResetWindowPeak()
{
    s_windowPeakBytes = s_liveTotalBytes;
    for (size_t i = 0; i < kMemTagCount; ++i)
    {
        s_windowPeakTagBytes[i] = s_memTagCounts[i].totalBytes;
    }
}

size_t GetWindowPeakMemBytes()
{
    return s_windowPeakBytes;
}

size_t GetWindowPeakTagBytes(const MemTag tag)
{
    return s_windowPeakTagBytes[MemTagToIndex(tag)];
}

HeapStats GetHeapStats()
{
    // dlmallinfo only consolidates when the heap is still uninitialised (top == 0);
    // after the first allocation it is a pure walk of the bins, so calling this
    // does not disturb the arrangement it is reporting.
    const struct mallinfo mi = dlmallinfo();

    // mallinfo's fields are ints over unsigned-long bookkeeping, so they can read
    // negative once the arena passes 2 GB. Never on a 32 MB console, but clamp
    // rather than report something absurd if it ever does.
    const auto Clamp = [](int v) -> size_t { return (v < 0) ? 0u : static_cast<size_t>(v); };

    HeapStats outStats;
    outStats.arenaBytes    = Clamp(mi.arena);
    outStats.inUseBytes    = Clamp(mi.uordblks);
    outStats.freeBytes     = Clamp(mi.fordblks);
    outStats.topChunkBytes = Clamp(mi.keepcost);
    outStats.fastbinBytes  = Clamp(mi.fsmblks);
    outStats.freeChunks    = Clamp(mi.ordblks);
    outStats.fastbinChunks = Clamp(mi.smblks);
    return outStats;
}

const char * DumpMemTags(char * const outBuffer, size_t const outBufferSize)
{
    char unitStr[kMemUnitStrSize];
    char peakStr[kMemUnitStrSize];
    char * const end = outBuffer + outBufferSize;
    size_t memTotal = 0;

    if (outBuffer == nullptr || outBufferSize == 0u)
    {
        return "";
    }

    // snprintf clamps and reports the *untruncated* length, so advance by
    // whichever is smaller: a dump that outgrows the caller's buffer stops
    // filling it instead of walking off the end.
    char * ptr = outBuffer;
    const auto append = [&ptr, end](const char * fmt, auto... args)
    {
        if (ptr >= end) { return; }
        const size_t left = static_cast<size_t>(end - ptr);
        const int n = std::snprintf(ptr, left, fmt, args...);
        ptr += (n < 0) ? 0 : ((static_cast<size_t>(n) < left) ? n : static_cast<int>(left - 1u));
    };

    append("%s", "--------------------------- MEMTAGS ---------------------------\n");
    append("%s", "Tag      Total     Peak      Allocs  Frees   Small    Large\n");

    for (int i = 0; i < static_cast<int>(MemTag::TagCount); ++i)
    {
        memTotal += s_memTagCounts[i].totalBytes;

        append("%-8s %-9s %-9s %-7zu %-7zu %-8zu %-8zu\n",
               s_memTagNames[i],
               FormatMemoryUnit(s_memTagCounts[i].totalBytes, true, unitStr, sizeof(unitStr)),
               FormatMemoryUnit(s_memTagCounts[i].peakBytes,  true, peakStr, sizeof(peakStr)),
               s_memTagCounts[i].totalAllocs,
               s_memTagCounts[i].totalFrees,
               s_memTagCounts[i].smallestAlloc,
               s_memTagCounts[i].largestAlloc);
    }

    // PEAK MEM is the high-water of the sum, not the sum of the per-tag peaks:
    // those happen at different moments, so adding them up would over-report.
    append("\nTOTAL MEM: %s", FormatMemoryUnit(memTotal, true, unitStr, sizeof(unitStr)));
    append("   PEAK MEM: %s\n", FormatMemoryUnit(s_peakTotalBytes, true, peakStr, sizeof(peakStr)));
    append("FREE MEM (sbrk): %s\n", FormatMemoryUnit(GetAvailableMemBytes(), true, unitStr, sizeof(unitStr)));
    append("%s", "--------------------------- MEMTAGS ---------------------------");

    outBuffer[outBufferSize - 1u] = '\0';
    return outBuffer;
}

} // namespace ps2::heap

// ------------------------------------------------------------------------------------------------
// ps2::heap C wrappers called by the Quake 2 code
// ------------------------------------------------------------------------------------------------

extern "C" {

void * PS2Quake_ZMalloc(size_t sizeBytes)
{
    return ps2::heap::Alloc(sizeBytes, ps2::heap::MemTag::Quake);
}

void PS2Quake_ZFree(void * ptr, size_t sizeBytes)
{
    ps2::heap::Free(ptr, sizeBytes, ps2::heap::MemTag::Quake);
}

void * PS2Quake_AudioMalloc(size_t sizeBytes)
{
    return ps2::heap::Alloc(sizeBytes, ps2::heap::MemTag::Audio);
}

void PS2Quake_AudioFree(void * ptr, size_t sizeBytes)
{
    ps2::heap::Free(ptr, sizeBytes, ps2::heap::MemTag::Audio);
}

} // extern "C"
