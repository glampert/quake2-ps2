#pragma once
/* ================================================================================================
 * File: aio_test.h
 * Brief: Self-test of the background loader (fs/async_io.h): a fixed file list read through the
 *        loader and through FS_LoadFile, compared byte for byte, twice - once as the drive is,
 *        once with ps2_aio_throttle_kbps 900 so PCSX2's instant host reads pace like a USB stick -
 *        with a main-thread read timed every frame meanwhile, since that is what a frame pays while
 *        a chunk is in flight. It prints each file's source kind, size and request-to-landed time,
 *        the loader's read throughput, the longest main-thread read, and AioTest: PASS or FAIL.
 *        Run it once on a console from USB: PCSX2 has no data cache, so only the console can show a
 *        destination that shares a cache line with something else.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
namespace ps2::test {

// Runs one step of the loader test. Call once per frame; gated by the "ps2_testaio" cvar, a no-op
// when it is 0 and once the test has ended (test_quit.h).
void RunAioTest();

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG
