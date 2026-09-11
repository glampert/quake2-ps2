/* ================================================================================================
 * File: clip.cpp
 * Brief: Triangle clipping against the clip volume the VU1 microprogram judges.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/clip.h"

namespace ps2::clip::detail {
// An global variable rather than a function-local static: ClipVertex has a default
// member initializer, so a local static would be lazily constructed behind a guard
// byte that every gather then has to test. This one is dynamic-initialized before
// main and costs the caller a plain address.
Scratch g_sharedScratch;
} // namespace ps2::clip::detail
