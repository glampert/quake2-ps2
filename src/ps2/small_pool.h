#pragma once
/* ================================================================================================
 * File: small_pool.h
 * Brief: Simple minimal object pool allocator for use with Texture and Model caches.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include <tamtypes.h>
#include <bitset>

namespace ps2
{

template<typename T, u32 MaxSize>
class SmallPool final
{
public:
    static constexpr u16 kInvalidIndex = u16(~0);
    static_assert(MaxSize > 0 && MaxSize <= 0xFFFF, "SmallPool is limited to 65K entries!");

    SmallPool() = default;

    // Disable copy.
    SmallPool(const SmallPool &) = delete;
    SmallPool & operator=(const SmallPool &) = delete;

    void Init()
    {
        // Init is one-shot; a freshly constructed pool has both counters at zero.
        PS2_AssertMsg(m_free == 0 && m_used == 0, "SmallPool double init!");

        // All slots start free.
        m_free = MaxSize;
        m_used = 0;

        // First free index to be popped is slot zero.
        for (u32 i = 0; i < MaxSize; ++i)
        {
            m_freeIndices[i] = static_cast<u16>(MaxSize - 1 - i);
        }
    }

    bool IsFull() const
    {
        return m_free == 0;
    }

    bool IsEmpty() const
    {
        return m_used == 0;
    }

    u32 UsedCount() const
    {
        return m_used;
    }

    u16 Alloc()
    {
        if (IsFull())
        {
            return kInvalidIndex;
        }

        const u16 slot = m_freeIndices[--m_free];
        PS2_Assert(IsValid(slot));
        PS2_AssertMsg(!m_liveSlots[slot], "Slot already allocated!");

        m_liveSlots[slot] = true;
        ++m_used;

        PS2_Assert(m_free + m_used == MaxSize);
        return slot;
    }

    void Free(u16 slot)
    {
        PS2_Assert(!IsEmpty());
        PS2_Assert(IsValid(slot));
        PS2_AssertMsg(m_liveSlots[slot], "Slot already free!"); // Catch double-free.

        m_liveSlots[slot] = false;
        m_slots[slot] = {};
        m_freeIndices[m_free] = slot;

        ++m_free;
        --m_used;

        PS2_Assert(m_free + m_used == MaxSize);
    }

    static constexpr bool IsValid(u16 slot)
    {
        return slot < MaxSize;
    }

    T & Slot(u16 slot)
    {
        PS2_Assert(IsValid(slot) && m_liveSlots[slot]);
        return m_slots[slot];
    }

    const T & Slot(u16 slot) const
    {
        PS2_Assert(IsValid(slot) && m_liveSlots[slot]);
        return m_slots[slot];
    }

private:
    u32 m_free = 0;
    u32 m_used = 0;

    T m_slots[MaxSize] = {};
    u16 m_freeIndices[MaxSize] = {};
    std::bitset<MaxSize> m_liveSlots = {};
};

} // namespace ps2
