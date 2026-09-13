/* ================================================================================================
 * File: render_context.cpp
 * Brief: The renderer's command recorder. See render_context.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/render_context.h"

namespace ps2::rc {

// The one recorder. Stateless beyond the command buffer it writes into, so it needs no
// initialization of its own - cmdbuf::Init is what has to have run before it is used.
static RenderContext s_context;

RenderContext & Ctx()
{
    return s_context;
}

} // namespace ps2::rc
