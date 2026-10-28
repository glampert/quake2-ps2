---
paths:
  - "src/ps2/renderer/**"
  - "src/ps2/input/**"
  - "src/ps2/fs/**"
  - "src/ps2/audio/**"
  - "src/ps2/math/**"
---

# Host harnesses: recipes per subsystem

Companion to [testing-pcsx2.md](testing-pcsx2.md). These check runtime logic only; `make`
stays the compile check (ps2sdk headers shadow host libc++ ones, and the GCC-only warnings
would be missed). The boot and log-file harnesses are in
[testing-storage-boot.md](testing-storage-boot.md).

- **Renderer sources:** in the scratchpad, make a `fakeinc/` with four headers and compile
  with `-Ifakeinc -I<repo>/src` (quoted includes miss the source's own dir, so `-I` order
  decides): `ps2/common.h` (`MAX_QPATH`, `PS2_QUAKE_DEBUG`, `Com_Printf`/`Com_DPrintf`/
  `Sys_Error` decls, `PS2_Assert`/`PS2_AssertMsg`), `tamtypes.h`, `gs_psm.h` (copy the
  `GS_PSM_*` values), `draw_buffers.h` (a `texbuffer_t`). Don't fake `texture.h`/`vram.h`.
  `#include` the **.cpp** so the test can walk anonymous-namespace statics. Stub `Sys_Error`
  as a counter (fatal paths `return` after it) and make the assert stub `exit(1)`. Use clang
  with ASan/UBSan.
- **Client-side sources** (e.g. input/rumble.cpp) compile against the *real* `client/client.h`
  chain on host clang in C++ mode. Make the fake `ps2/common.h` wrap
  `#include "common/q_common.h"` in `extern "C"`, define the globals the source touches, and
  fake only the hardware class. input.cpp's `IN_Move` (the stick dead zones) works this way
  with fake `ps2/input/pad.h` (settable stick floats plus libpad's `PAD_*` bits), `keyboard.h`,
  `rumble.h` and `ps2/renderer/profile.h`, and a `Cvar_Get` that merges flags like id's. Set
  the anonymous-namespace `s_gamepad` directly. Its 10,428 checks caught HEAD's 0.15 step
  response, rest drift from 20 counts up, and 5 of 5 seeded mutants (2026-10-11).
- **Emulate the EE FPU** in any math harness: `1/sqrt(0)` must give FLT_MAX, not inf (see
  [ps2-platform.md](ps2-platform.md)). Otherwise target-only bugs won't reproduce.
- **ASan can't see an overrun that stays inside one object** (a member array reading into
  the next member). Heap-allocate the object under test, so writes past its last member hit
  the redzone, and poison what must not be read with `ASAN_POISON_MEMORY_REGION` from
  `<sanitizer/asan_interface.h>` (it handles a partial first granule), unpoisoning after the
  call. Give outputs exact-size heap buffers with canaries. Then prove the harness by seeding
  defects into a shadow copy of the header (`-I<mutant dir>` first): the `half_band.h`
  harness caught 7 of 7 (off-by-ones, an over-long memmove, a wrong tap, a 32-bit overflow).
- **The pkz reader** (fs/pkz_pack.cpp) runs on the host the same way: include the `.cpp` with a
  fake `ps2/common.h` (MAX_QPATH/MAX_OSPATH/MAX_FILES_IN_PACK, Com_Printf, a Sys_Error that
  longjmps when a test expects it, and a `ps2::heap` that checks every Free's size and tag
  against its Alloc). macOS has no `fopencookie`: `#define fopencookie` and `setvbuf` to
  stand-ins *after* including `<cstdio>`, keep the cookie callbacks in a table, and call them
  the way newlib's unbuffered fread/fseek/ftell do. Link the miniz sources with the game's
  `save/miniz_cfg`. Keep the pack's path under `MAX_OSPATH`: stored files reopen the pack by
  name, and the scratchpad's absolute paths are longer (run from the data directory). The
  2026-10-11 harness checked all 3307 files of pak0 whole, 537 as streams through the BSP and
  MD2 loaders' seek patterns and 208K random reads/seeks, zips from Info-ZIP and Python's
  `zipfile` (local extra fields, a comment, directory entries), one damaged and five malformed
  packs, and caught 6 of 6 seeded mutants (restart index, stale dictionary, dictionary window,
  wrap copy, stale input, local extra field). macOS has no `timeout`: bound a mutant run with
  `perl -e 'alarm 600; exec @ARGV' ...`, and check the exit code isn't 127.
- Code with EE/VU0 inline asm can't run on the host. Use the standalone test ELF recipe in
  [performance.md](performance.md).
