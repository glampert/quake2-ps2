#pragma once
/* ================================================================================================
 * File: aio_pump.h
 * Brief: Where the background loader's finished requests become engine assets. The loader
 *        (async_io.h) only lands bytes; each client (sounds, and in later stages textures and
 *        models) keeps its own table of requests in flight and finishes them on the main thread,
 *        from here, once a frame, under a time budget (ps2_aio_pump_us). The pump runs at the top
 *        of PS2_BeginFrame, so what landed in the last frame's idle time is in place before this
 *        frame's S_Update and draws. Sounds go first: a landed sound is mixed by the same frame.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::aio {

// Finishes landed requests, clients in order, until the frame's budget is spent. Main thread.
void Pump();

} // namespace ps2::aio
