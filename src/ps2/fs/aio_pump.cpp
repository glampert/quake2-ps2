/* ================================================================================================
 * File: aio_pump.cpp
 * Brief: The per-frame pump. See aio_pump.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/fs/aio_pump.h"
#include "ps2/fs/async_io.h"
#include "ps2/audio/sound_load.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/model.h"

namespace ps2::aio {

void Pump()
{
    static const cvar_t * s_budgetUs = Cvar_Get("ps2_aio_pump_us", "2000", 0);

    if (!Enabled())
    {
        return;
    }

    // 294.912 MHz: cycles per microsecond, rounded. A client finishes at least one landed request
    // even when the budget is spent, so nothing waits forever behind a long frame.
    const u32 budgetCycles = static_cast<u32>(s_budgetUs->value) * 295u;
    audio::PumpLoads(budgetCycles);
    tex::PumpLoads(budgetCycles);
    mod::PumpLoads(budgetCycles);
}

} // namespace ps2::aio
