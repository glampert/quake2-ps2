#pragma once
/* ================================================================================================
 * File: qwords.h
 * Brief: Whole-qword copies and stores for the EE, as the R5900's 128-bit lq/sq.
 *
 *        gcc never forms lq/sq on its own. A struct assignment of a 16-byte aligned type lowers
 *        to ld/sd pairs, and so does a store of an __int128 or of ps2sdk's u128 - twice the
 *        memory operations, which are the part of an instruction stream the EE is shortest of.
 *        These are for the places that move whole qwords on a hot path: the command buffer.
 *
 *        Integer lq/sq, like vu1::CopyDrawVertex, rather than the VU0 lqc2/sqc2 the math helpers
 *        use: what goes through here is GIF tags, VIF codes and packed colours as often as floats,
 *        and the integer path never reaches an FMAC that could flush a bit pattern it took for a
 *        denormal.
 *
 *        Every asm names exactly the qwords it reads and writes rather than clobbering "memory",
 *        so a caller keeps its own state in registers across a run of them. Both pointers must be
 *        16-byte aligned: lq/sq ignore the low four address bits rather than fault.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <tamtypes.h>
#include <utility>

namespace ps2 {

namespace detail {

// The memory an asm below reads or writes, so its constraint covers exactly those bytes and no
// more. u64 lanes rather than u128: only the object's address and size matter to a constraint.
template<unsigned N>
struct alignas(16) QwordBlock
{
    u64 dwords[2u * N];
};

} // namespace detail

// One qword.
Q_ALWAYS_INLINE void CopyQword(void * dst, const void * src)
{
    auto & d = *static_cast<detail::QwordBlock<1> *>(dst);
    const auto & s = *static_cast<const detail::QwordBlock<1> *>(src);
    asm volatile (
        "lq $8, 0x00(%1) \n\t"
        "sq $8, 0x00(%2) \n\t"
        : "=m" (d)
        : "r" (&s), "r" (&d), "m" (s)
        : "$8");
}

// Two qwords, both loads issued before either store so neither store waits on its load.
Q_ALWAYS_INLINE void CopyQwordPair(void * dst, const void * src)
{
    auto & d = *static_cast<detail::QwordBlock<2> *>(dst);
    const auto & s = *static_cast<const detail::QwordBlock<2> *>(src);
    asm volatile (
        "lq $8, 0x00(%1) \n\t"
        "lq $9, 0x10(%1) \n\t"
        "sq $8, 0x00(%2) \n\t"
        "sq $9, 0x10(%2) \n\t"
        : "=m" (d)
        : "r" (&s), "r" (&d), "m" (s)
        : "$8", "$9");
}

// N qwords, unrolled into pairs at compile time. The two ranges must not overlap.
template<int N>
Q_ALWAYS_INLINE void CopyQwords(void * dst, const void * src)
{
    static_assert(N > 0, "Nothing to copy!");

    u8 * const d = static_cast<u8 *>(dst);
    const u8 * const s = static_cast<const u8 *>(src);

    [d, s]<int... I>(std::integer_sequence<int, I...>)
    {
        (CopyQwordPair(d + (I * 32), s + (I * 32)), ...);
    }(std::make_integer_sequence<int, N / 2>{});

    if constexpr ((N & 1) != 0)
    {
        CopyQword(d + ((N - 1) * 16), s + ((N - 1) * 16));
    }
}

// One qword assembled from two 64-bit halves, 'lo' at the lower address, in a single sq. The
// halves meet in a register (pcpyld) instead of going out as two sd.
Q_ALWAYS_INLINE void StoreQword(void * dst, const u64 lo, const u64 hi)
{
    auto & d = *static_cast<detail::QwordBlock<1> *>(dst);
    asm volatile (
        "pcpyld $8, %2, %1   \n\t"
        "sq     $8, 0x00(%3) \n\t"
        : "=m" (d)
        : "r" (lo), "r" (hi), "r" (&d)
        : "$8");
}

// The same from four 32-bit words, w0 at the lowest address.
Q_ALWAYS_INLINE void StoreQword(void * dst, const u32 w0, const u32 w1, const u32 w2, const u32 w3)
{
    StoreQword(dst, static_cast<u64>(w0) | (static_cast<u64>(w1) << 32),
                    static_cast<u64>(w2) | (static_cast<u64>(w3) << 32));
}

} // namespace ps2
