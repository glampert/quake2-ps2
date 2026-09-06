#pragma once
/* ================================================================================================
 * File: render_profile.h
 * Brief: Profile events shared by more than one renderer source file.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/profile.h"

namespace ps2::prof_evt {

PS2_PROFILE_DECLARE_EVENT(Frame);
PS2_PROFILE_DECLARE_EVENT(VSync);
PS2_PROFILE_DECLARE_EVENT(GsWait);
PS2_PROFILE_DECLARE_EVENT(View);
PS2_PROFILE_DECLARE_EVENT(World);
PS2_PROFILE_DECLARE_EVENT(Vis);
PS2_PROFILE_DECLARE_EVENT(TexChains);
PS2_PROFILE_DECLARE_EVENT(LmChains);
PS2_PROFILE_DECLARE_EVENT(Entities);
PS2_PROFILE_DECLARE_EVENT(Particles);
PS2_PROFILE_DECLARE_EVENT(AlphaSurfs);
PS2_PROFILE_DECLARE_EVENT(Sky);

} // namespace ps2::prof_evt
