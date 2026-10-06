#pragma once
/* ================================================================================================
 * File: exception_handler.h
 * Brief: Turns an EE CPU exception into a readable post-mortem instead of a silent hang.
 *
 *        Without this a bad pointer produces three lines of PCSX2 output and nothing else:
 *
 *            TLB Miss, pc=0x82000   addr=0x0        [load]
 *            TLB Miss, pc=0x8001044 addr=0x8001044  [load]
 *
 *        which names neither the function nor the call that reached it. With it, the same
 *        fault reports the cause, the faulting instruction, the address it touched and an
 *        unwound call stack - one addr2line away from the source line - on screen, in the log
 *        file and on stdout.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::debug {

// Installs the CPU exception handlers. Call once, early in main() - before the
// heap and the renderer, so a fault during their setup is caught too.
//
// Stripped in release; this pulls in ps2sdk's libeedebug, which replaces the EE's
// level 1 exception vectors, and a release build should not be carrying a
// debugger's vector table around.
void InstallExceptionHandlers();

} // namespace ps2::debug
#endif // PS2_QUAKE_DEBUG
