#pragma once
/* ================================================================================================
 * File: hash.h
 * Brief: Shared string hashing used by the asset caches (textures, models) for
 *        name -> pool-slot lookup. 64-bit FNV-1a; hash collisions are verified
 *        against the stored name at each call site.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2 {

constexpr u64 kFnvSeed  = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

// 64-bit FNV-1a string hash.
constexpr u64 HashStr64(const char * str)
{
    if (str == nullptr || *str == '\0')
    {
        return 0;
    }

    u64 hash = kFnvSeed;
    while (*str != '\0')
    {
        hash ^= static_cast<u8>(*str++);
        hash *= kFnvPrime;
    }

    return hash;
}

} // namespace ps2
