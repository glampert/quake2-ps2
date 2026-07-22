/* ================================================================================================
 * File: model.cpp
 * Brief: Quake 2 3D model format loading and caching.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/model.h"

namespace ps2::mod {

// TODO ModelCache

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
}

void BeginRegistration(const char * map_name)
{
    (void)map_name;
}

void EndRegistration()
{
}

const ModelInstance * Find(const char * name)
{
    (void)name;
    return nullptr;
}

} // namespace ps2::mod
