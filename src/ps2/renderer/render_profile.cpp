/* ================================================================================================
 * File: render_profile.cpp
 * Brief: Profile events shared by more than one renderer source file.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/render_profile.h"

namespace ps2::prof_evt {

PS2_PROFILE_DEFINE_EVENT(Frame,      "Frame",      kScreenOverlay, 0);
PS2_PROFILE_DEFINE_EVENT(VSync,      "VSync",      kScreenOverlay, 1);
PS2_PROFILE_DEFINE_EVENT(GsWait,     "GsWait",     kScreenOverlay, 2);
PS2_PROFILE_DEFINE_EVENT(View,       "View",       kScreenOverlay, 3);
PS2_PROFILE_DEFINE_EVENT(World,      "World",      kScreenOverlay, 4);
PS2_PROFILE_DEFINE_EVENT(Vis,        "Vis",        kScreenOverlay, 5);
PS2_PROFILE_DEFINE_EVENT(TexChains,  "TexChains",  kScreenOverlay, 6);
PS2_PROFILE_DEFINE_EVENT(LmChains,   "LmChains",   kScreenOverlay, 7);
PS2_PROFILE_DEFINE_EVENT(Entities,   "Entities",   kScreenOverlay, 8);
PS2_PROFILE_DEFINE_EVENT(Particles,  "Particles",  kScreenOverlay, 9);
PS2_PROFILE_DEFINE_EVENT(AlphaSurfs, "AlphaSurfs", kScreenOverlay, 10);
PS2_PROFILE_DEFINE_EVENT(Sky,        "Sky",        kScreenOverlay, 11);

} // namespace ps2::prof_evt
