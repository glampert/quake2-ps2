/* ================================================================================================
 * File: map_cycle.cpp
 * Brief: Map cycling memory smoke test. See map_cycle.h.
 *
 *  Drives the real console command ("map <name>") through the command buffer rather than
 *  calling into the server directly, so the sequence the test exercises is byte for byte
 *  the one a player produces. Cbuf_AddText also defers execution out of the middle of the
 *  frame we are in, which matters: SV_SpawnServer frees the resident world model, and the
 *  renderer is on the stack right now.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/common.h"
#include "ps2/tests/map_cycle.h"
#include "ps2/renderer/model.h"

#include <cstdio>
#include <cstring>

namespace ps2::test {
namespace {

// Every map in pak0, in single-player unit order (verified against the pak: 39
// maps, no gaps, no duplicates). Order matters - the test is about the cost of
// each transition, and this is the sequence a real playthrough produces.
constexpr const char * kMaps[] = {
    // Unit 1 - outer base
    "base1", "base2", "base3", "train",
    // Unit 2 - installation
    "bunk1", "ware1", "ware2",
    // Unit 3 - jail
    "jail1", "jail2", "jail3", "jail4", "jail5", "security",
    // Unit 4 - mine
    "mintro", "mine1", "mine2", "mine3", "mine4",
    // Unit 5 - factory
    "fact1", "fact3", "fact2",
    // Unit 6 - power plant
    "power1", "power2", "cool1",
    // Unit 7 - biggun
    "waste1", "waste2", "waste3", "biggun",
    // Unit 8 - hangar
    "hangar1", "hangar2", "space",
    // Unit 9 - research lab
    "lab", "boss1",
    // Unit 10 - city
    "city1", "city2", "city3", "strike",
    // Unit 11 - final
    "command", "boss2",
};

enum class State
{
    Idle,    // Nothing issued yet; kick off the next map.
    Loading, // Command issued, waiting for the world to come up.
    Dwelling // Map is up; stay in it so it actually renders.
};

// A map that never comes up is a failed test, not a reason to hang forever. The
// slowest stock load measured is ~8 s (power2 from host:), so this is generous.
constexpr int kLoadTimeoutMs = 90 * 1000;

// The world has to be resident for this many frames before we call it loaded.
// Guards the window between Cbuf_AddText and Cbuf_Execute, where the *previous*
// map is still up and would otherwise match if it happened to be the target.
constexpr int kFramesToConfirm = 2;

static State  s_state         = State::Idle;
static int    s_nextMap       = 0;
static bool   s_done          = false;
static bool   s_quitOnFinish  = true;
static int    s_issuedAtMs    = 0;
static int    s_dwellUntilMs  = 0;
static int    s_confirmFrames = 0;
static int    s_skipped       = 0;
static int    s_failed        = 0;
static size_t s_peakBeforeMap = 0;
static char   s_targetBsp[MAX_QPATH] = {};

void Restart()
{
    s_state         = State::Idle;
    s_nextMap       = 0;
    s_done          = false;
    s_issuedAtMs    = 0;
    s_dwellUntilMs  = 0;
    s_confirmFrames = 0;
    s_skipped       = 0;
    s_failed        = 0;
    s_peakBeforeMap = 0;
    s_targetBsp[0]  = '\0';
}

bool MapFileExists(const char * const bspName)
{
    FILE * file = nullptr;
    if (FS_FOpenFile(bspName, &file) < 0 || file == nullptr)
    {
        if (file != nullptr) { FS_FCloseFile(file); }
        return false;
    }
    FS_FCloseFile(file);
    return true;
}

bool TargetWorldIsResident()
{
    const mod::ModelInstance * const world = mod::GetWorldModel();
    return world != nullptr && std::strcmp(world->name, s_targetBsp) == 0;
}

size_t TagBytes(const ps2::heap::MemTag tag)
{
    return ps2::heap::GetStatsForMemTag(tag).totalBytes;
}

size_t LiveTotalBytes()
{
    size_t total = 0;
    for (int i = 0; i < static_cast<int>(ps2::heap::MemTag::TagCount); ++i)
    {
        total += TagBytes(static_cast<ps2::heap::MemTag>(i));
    }
    return total;
}

// One line per map, with the tags that actually move between levels. The peak is
// the global high-water (PS2_GetPeakMemBytes), so "NEW PEAK" marks the transition
// that cost the most - which is the number the whole test exists to find.
void ReportMap(const char * const name, const int index)
{
    char world[ps2::heap::kMemUnitStrSize], audio[ps2::heap::kMemUnitStrSize];
    char tex[ps2::heap::kMemUnitStrSize],   alias[ps2::heap::kMemUnitStrSize];
    char total[ps2::heap::kMemUnitStrSize], peak[ps2::heap::kMemUnitStrSize];
    char freeMem[ps2::heap::kMemUnitStrSize];

    const size_t peakNow = ps2::heap::GetPeakMemBytes();

    Com_Printf("MapCycle [%2d/%2d] %-9s World %-9s Audio %-9s Tex %-9s Mdl %-9s "
               "TOTAL %-9s PEAK %-9s FREE %-9s%s\n",
               index + 1, ArrayLength(kMaps), name,
               ps2::heap::FormatMemoryUnit(TagBytes(ps2::heap::MemTag::WorldMdl), true, world, sizeof(world)),
               ps2::heap::FormatMemoryUnit(TagBytes(ps2::heap::MemTag::Audio),    true, audio, sizeof(audio)),
               ps2::heap::FormatMemoryUnit(TagBytes(ps2::heap::MemTag::TexImage), true, tex,   sizeof(tex)),
               ps2::heap::FormatMemoryUnit(TagBytes(ps2::heap::MemTag::AliasMdl), true, alias, sizeof(alias)),
               ps2::heap::FormatMemoryUnit(LiveTotalBytes(),           true, total, sizeof(total)),
               ps2::heap::FormatMemoryUnit(peakNow,                    true, peak,  sizeof(peak)),
               ps2::heap::FormatMemoryUnit(ps2::heap::GetAvailableMemBytes(), true, freeMem, sizeof(freeMem)),
               (peakNow > s_peakBeforeMap) ? "  <- NEW PEAK" : "");
}

// Where the free memory sits, which the memtag table cannot show. A pass can end
// with megabytes free and still fail the next big allocation, because dlmalloc
// never moves a live block - so what matters is not how much is free but how it is
// arranged. Printed per pass so the trend across a long session is visible: a
// number that climbs pass over pass is the heap degrading, one that holds is not.
void ReportHeap(const int pass)
{
    const ps2::heap::HeapStats hs = ps2::heap::GetHeapStats();

    char a[ps2::heap::kMemUnitStrSize], b[ps2::heap::kMemUnitStrSize], c[ps2::heap::kMemUnitStrSize];

    // The top chunk is one contiguous run at the end of the arena, and fastbins are
    // small chunks dlmalloc deliberately leaves uncoalesced until a large request
    // needs them. Neither is fragmentation. What is left over is: free bytes stuck
    // in holes between live blocks, which only a future allocation of the right
    // size can ever use.
    const size_t nonInterior    = hs.topChunkBytes + hs.fastbinBytes;
    const size_t interior       = (hs.freeBytes > nonInterior) ? (hs.freeBytes - nonInterior) : 0u;
    const size_t interiorChunks = (hs.freeChunks > 1u) ? (hs.freeChunks - 1u) : 0u;

    Com_Printf("MapCycle: ---- heap after pass %d ----\n", pass);
    Com_Printf("MapCycle:   arena %s   in use %s   free %s\n",
               ps2::heap::FormatMemoryUnit(hs.arenaBytes, true, a, sizeof(a)),
               ps2::heap::FormatMemoryUnit(hs.inUseBytes, true, b, sizeof(b)),
               ps2::heap::FormatMemoryUnit(hs.freeBytes,  true, c, sizeof(c)));
    Com_Printf("MapCycle:   top chunk %s   fastbins %s in %zu\n",
               ps2::heap::FormatMemoryUnit(hs.topChunkBytes, true, a, sizeof(a)),
               ps2::heap::FormatMemoryUnit(hs.fastbinBytes,  true, b, sizeof(b)),
               hs.fastbinChunks);
    Com_Printf("MapCycle:   interior holes %s in %zu chunks (avg %s)\n",
               ps2::heap::FormatMemoryUnit(interior, true, a, sizeof(a)), interiorChunks,
               ps2::heap::FormatMemoryUnit((interiorChunks != 0u) ? (interior / interiorChunks) : 0u,
                                           true, b, sizeof(b)));

    if (hs.freeBytes != 0u)
    {
        // Share of free memory that is neither the top run nor a fastbin. An upper
        // bound on fragmentation, not a measurement: mallinfo reports no largest
        // free chunk, so an interior hole could well be bigger than the top and
        // serve a large request anyway. The number to watch is its trend, and
        // whether the top chunk still covers the biggest allocation a map needs.
        const double pct = 100.0 * static_cast<double>(interior) / static_cast<double>(hs.freeBytes);
        Com_Printf("MapCycle:   scattered %.1f%% of free space (upper bound on fragmentation)\n", pct);
        Com_Printf("MapCycle:   guaranteed contiguous: at least %s (the top chunk)\n",
                   ps2::heap::FormatMemoryUnit(hs.topChunkBytes, true, a, sizeof(a)));
    }
}

void Finish()
{
    char peak[ps2::heap::kMemUnitStrSize], total[ps2::heap::kMemUnitStrSize];

    Com_Printf("MapCycle: pass complete - %d loaded, %d skipped (not in pak), %d timed out.\n",
               ArrayLength(kMaps) - s_skipped - s_failed, s_skipped, s_failed);
    Com_Printf("MapCycle: worst moment across the whole run was %s of %s installed.\n",
               ps2::heap::FormatMemoryUnit(ps2::heap::GetPeakMemBytes(), true, peak, sizeof(peak)),
               ps2::heap::FormatMemoryUnit(ps2::heap::GetTotalMemBytes(), true, total, sizeof(total)));

    // Survives Restart(), so re-running the test in the same session numbers the
    // passes and makes drift between them obvious.
    static int s_passesRun = 0;
    ReportHeap(++s_passesRun);

    if (s_quitOnFinish)
    {
        Com_Printf("MapCycle test completed - quitting now...\n");
        Cbuf_AddText("quit\n");
    }

    s_done = true;
}

// Issues the next map, skipping any that are not in the pak. Returns false when
// the list is exhausted.
bool StartNextMap()
{
    while (s_nextMap < ArrayLength(kMaps))
    {
        const char * const name = kMaps[s_nextMap];
        std::snprintf(s_targetBsp, sizeof(s_targetBsp), "maps/%s.bsp", name);

        if (!MapFileExists(s_targetBsp))
        {
            Com_Printf("MapCycle: skipping '%s' (not in the pak).\n", name);
            ++s_skipped;
            ++s_nextMap;
            continue;
        }

        // Sampled before the load so ReportMap can tell whether *this* transition
        // set a new high-water, rather than just echoing the running maximum.
        s_peakBeforeMap = ps2::heap::GetPeakMemBytes();

        Cbuf_AddText(va("map %s\n", name));

        s_issuedAtMs    = Sys_Milliseconds();
        s_confirmFrames = 0;
        s_state         = State::Loading;
        return true;
    }
    return false;
}

} // namespace

void RunMapCycle()
{
    static const cvar_t * s_enabled = Cvar_Get("ps2_testmaps",       "0", 0);
    static const cvar_t * s_dwell   = Cvar_Get("ps2_testmaps_dwell", "8", 0);

    if (s_enabled->value == 0.0f || s_done)
    {
        return;
    }

    static bool s_cmdRegistered = false;
    if (!s_cmdRegistered)
    {
        Cmd_AddCommand("ps2_testmaps_restart", Restart);
        s_cmdRegistered = true;
    }

    switch (s_state)
    {
    case State::Idle:
        if (!StartNextMap())
        {
            Finish();
        }
        break;

    case State::Loading:
        if (TargetWorldIsResident() && ++s_confirmFrames >= kFramesToConfirm)
        {
            const int dwellMs = static_cast<int>(s_dwell->value * 1000.0f);
            s_dwellUntilMs = Sys_Milliseconds() + ((dwellMs > 0) ? dwellMs : 1);
            s_state = State::Dwelling;
            break;
        }
        if ((Sys_Milliseconds() - s_issuedAtMs) > kLoadTimeoutMs)
        {
            Com_Printf("MapCycle: '%s' never came up after %d seconds - moving on.\n",
                       kMaps[s_nextMap], kLoadTimeoutMs / 1000);
            ++s_failed;
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;

    case State::Dwelling:
        if (Sys_Milliseconds() >= s_dwellUntilMs)
        {
            ReportMap(kMaps[s_nextMap], s_nextMap);
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
