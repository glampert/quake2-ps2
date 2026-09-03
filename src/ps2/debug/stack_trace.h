#pragma once
/* ================================================================================================
 * File: stack_trace.h
 * Brief: EE call stack unwinding, for the fatal error paths.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include <tamtypes.h>

namespace ps2::debug {

// Upper bound on the frames a walk will report. Deep enough for the engine's
// recursive paths (BSP traversal, the menu stack) without making the caller's
// array big enough to matter on the stack.
constexpr int kStackTraceMaxFrames = 32;

namespace detail {

// Walks up the call stack from a (pc, $sp) pair belonging to one live frame.
// Not called directly - see CaptureStackTrace below.
int WalkStack(u32 pc, u32 sp, u32 * outFrames, int maxFrames);

} // namespace detail

// Unwinds the EE call stack and writes one address per frame into outFrames,
// innermost first, starting with the caller of this function. Each address names
// the call instruction rather than the instruction it returns to, so handing it
// to addr2line names the line that made the call.
//
// Returns how many frames it recovered, which can be fewer than maxFrames: the
// walk stops rather than guessing when it meets a frame it cannot decode.
//
// always_inline is load-bearing, not an optimization. It puts the (pc, $sp) pair
// below in the frame of whoever calls this, and that frame is guaranteed to be
// one the unwinder can decode: the caller is about to call WalkStack, and a
// function that makes a call has to open a frame and spill $ra into it. A
// CaptureStackTrace with a body of its own would carry neither guarantee - at -O2
// GCC turned exactly this function into a shrink-wrapped frameless leaf whose
// $sp already belonged to a frame further up, which unwinds to nonsense.
inline __attribute__((always_inline))
int CaptureStackTrace(u32 * outFrames, int maxFrames)
{
    // $sp does not move again for the rest of the caller's body (nothing here
    // allocates on the stack dynamically, and -Wvla rules out variable length
    // arrays), and the label gives a pc that cannot drift away from the code it
    // sits in, whatever the optimizer clones, splits or renames.
    u32 sp;
    u32 pc;
    asm volatile("move %0, $sp \n\t"
                 "la   %1, 1f  \n\t"
                 "1:"
                 : "=r"(sp), "=r"(pc));

    return detail::WalkStack(pc, sp, outFrames, maxFrames);
}

// Captures the stack and writes it to stdout, one "#N 0x00000000" line per frame
// inside a banner, followed by the addr2line invocation that resolves them.
// Flushes stdout, since the callers of this do not intend to come back.
void PrintStackTrace() Q_COLD_FUNC;

} // namespace ps2::debug
