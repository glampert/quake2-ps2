/* ================================================================================================
 * File: qcommon.h
 * Brief: The single seam between the modern C++ PS2 backend and the untouched C
 *        Quake II engine. Backend .cpp files include THIS rather than reaching
 *        into the engine headers directly, so the C-linkage wrapping and the few
 *        legacy-header workarounds live in exactly one place.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_QCOMMON_H
#define PS2_QCOMMON_H

// C++ standard headers must be included OUTSIDE the extern "C" block below.
#include <cstddef>
#include <cstdint>

// PS2_MemAlloc / MemTags (shared with common.c's Z_Malloc)
#include "ps2/system/heap.h"

// The C engine/game/client API plus the kept C "coupling" headers, given C
// linkage so the statically-linked engine (compiled as C) and this backend
// (compiled as C++) agree on unmangled symbol names.
extern "C" {
    #include "common/q_common.h" // Pulls in game/q_shared.h
    #include "client/ref.h"      // refexport_t / refimport_t / refdef_t
    #include "client/vid.h"      // VID_* + viddef
    #include "client/keys.h"     // Key_Event + key codes
}

// Helper assert macros that display the error on screen and halt.
// Prefer these over standard assert().
#define PS2_Assert(cond)                               \
    do {                                               \
        if (!(cond))                                   \
        {                                              \
            Sys_Error("Assert Failed: %s", #cond);     \
        }                                              \
    } while (0)

#define PS2_AssertMsg(cond, message)                   \
    do {                                               \
        if (!(cond))                                   \
        {                                              \
            Sys_Error("Assert Failed: %s", (message)); \
        }                                              \
    } while (0)

#endif // PS2_QCOMMON_H
