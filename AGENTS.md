# Quake II for the PlayStation 2

id's Quake II (3.19) ported to the PS2 with only the free ps2dev SDK. Two halves:

- **`src/client`, `src/common`, `src/game`, `src/server`**: id's original C, kept as close to
  untouched as possible. Every change is tagged `// [PS2_QUAKE]: <why>`.
- **`src/ps2`**: the console backend, all new C++20 (no exceptions, no RTTI, strict `-Werror`).
  It plugs into the engine at the old platform seams: `refexport_t`, `Sys_*`, `NET_*`, `IN_*`,
  `SNDDMA_*`, `CDAudio_*`, `Sys_Save*`. [src/ps2/common.h](src/ps2/common.h) is the one header
  that bridges to the C engine.

[README.md](README.md) is the architecture document: the renderer, VU1 path, sound, input,
memory, save games, debugging cvars and the PCSX2 setup. Read the section you need before
changing a subsystem, and keep it current when behaviour changes. [CVARS.md](CVARS.md) lists
every backend cvar. Add new ones there, with their debug/release defaults and flags.

## Toolchain

- EE compiler: `mips64r5900el-ps2-elf-gcc`/`g++` (GCC 15). There is no `ee-gcc`/`ee-g++`, so
  don't go looking for them. VU tools: `openvcl`, `dvp-as`. Also `bin2c`.
- `$PS2DEV` = `~/ps2dev`, `$PS2SDK` = `~/ps2dev/ps2sdk` (EE headers in `ee/include`). The
  SDK's C sources, for checking what a library really does, are under
  `~/ps2dev/src/ps2dev/build/ps2sdk/ee/<lib>/src/`. gsKit is at `~/ps2dev/gsKit`.
- Submodules: `src/tools/vclpp` (VCL preprocessor, with its own nested `external/parse-utils`),
  `src/tools/vu-checker` (`check_vu_code.py`), `src/tools/miniz`. The build uses the pinned
  vclpp it builds into `build/tools/vclpp`, never one on `PATH`.

## Build and verify

- **`make` is the compile check.** It is fast, uses the real flags (the full GCC-only warning
  set, `-Werror`) and links the ELF. Don't build host-side stub harnesses just to see whether
  something compiles. ps2sdk headers shadow host libc++ ones, and the GCC-only warnings
  (`-Wlogical-op`, `-Wduplicated-*`) would be missed.
- **Check `make`'s exit status. Grepping its output for `error:` is not enough.** openvcl's
  diagnostics don't match that pattern, and a failed VU build leaves the stale `.vsm`/`.o`
  from the previous run, which then gets linked.
- `make` = debug (`-O2`, asserts on) → `build/debug/quake2.elf`. `make release` = `-O3`, no
  asserts, no debug-only code → `build/release/`. `make run` / `make release run` launch PCSX2.
  The symbols are in `quake2_unstripped.elf` next to each stripped ELF.
- `PS2_QUAKE_DEBUG`, `PS2_QUAKE_ASSERTS` and `PS2_QUAKE_PROFILE` are always defined to 0 or 1.
  Test them with `#if`, never `#ifdef` (`-Wundef` is on).
- make does not track flag changes. After editing `CONFIG_DEFS` or other flags,
  `rm -rf build/<config>/src`.
- Source lists in the Makefile are explicit. Add a new `.cpp` to `PS2_CXX_SRC`, and to
  `SIZE_OPT_CXX_SRC` (built `-Os`) if it is load-time or debug-only rather than per-frame.
  Run `make compiledb` after adding or removing files.
- Tests and runtime checks run in PCSX2 or in host harnesses: see
  [.claude/rules/testing-pcsx2.md](.claude/rules/testing-pcsx2.md).

## Git workflow

- One branch: commit directly on `main`. Don't create feature branches unless asked. Push
  only when asked.
- `git fetch` first. The user sometimes commits on GitHub directly, so fast-forward if
  behind.
- Push submodules before the repo that pins them: **parse-utils → vclpp → quake2-ps2**. A
  gitlink that reaches GitHub before its submodule commit breaks recursive clones.
- Commit subjects are one sentence ending in a period, often prefixed with the area:
  `CD music: stream the soundtrack from loose SPU2 ADPCM files.`,
  `vclpp: back to C++17, so GCC 9 builds it; CI on GitHub.`

## Things that bite (details in the rules)

- The EE FPU has no Inf/NaN (1/0 = FLT_MAX), and double is soft-float. Host and target
  silently disagree on degenerate math.
- ps2sdk stubs some libc calls to fail (`sysconf` → -1), and some of its register macros
  don't parenthesize their arguments. Verify before trusting either.
- SIF DMA target buffers need `alignas(64)`.
- The VU toolchain miscompiles silently. Only `check_vu_code.py` and the screen tell you.
- GS alpha 1.0 is `0x80`. Normalized ST spans the power-of-two TEX0 extent, not the image.
- PCSX2 models neither the EE cache nor GS-internal cost. A capture only gates regressions
  for those.
- `baseq2/autoexec.cfg` never runs on this port. Scripted test sessions need another way in.

## Rules index (`.claude/rules/`)

When a finding is durable (a hardware fact, a toolchain trap, a measured baseline, a test
recipe), record it in the matching rule file below, so it travels with the repo.

| File | Loaded for | Covers |
| --- | --- | --- |
| [testing-pcsx2.md](.claude/rules/testing-pcsx2.md) | always | PCSX2 setup and logs, scripted sessions, built-in tests, crash triage, the known TLB flake, host harnesses |
| [backend-cpp.md](.claude/rules/backend-cpp.md) | `src/ps2`, host tools | naming, types, file style, passing the strict `-Werror` set |
| [ps2-platform.md](.claude/rules/ps2-platform.md) | `src/ps2` | ps2sdk traps, EE FPU, SIF DMA, IOP modules, ROM FILEIO, memory card |
| [gs-renderer.md](.claude/rules/gs-renderer.md) | `src/ps2/renderer` | GS/libdraw facts, frame model, CLUTs, VRAM blocks, mipmaps, ST scaling |
| [vu-microprograms.md](.claude/rules/vu-microprograms.md) | VU sources, vu-checker | openvcl/dvp-as/vclpp traps, VU0 inline asm, VCL comment style, runtime probes |
| [performance.md](.claude/rules/performance.md) | `src/ps2`, frame-log scripts | EE codegen facts, what PCSX2 can measure, capture/A-B/asm-test recipes |
| [engine-c.md](.claude/rules/engine-c.md) | id's C | editing rules, engine quirks and bugs, registration, save-format duty |
| [memory-budget.md](.claude/rules/memory-budget.md) | heap, VRAM, assets, MapCycle | the 32 MB picture, map-transition peak, measured budgets |
| [audio.md](.claude/rules/audio.md) | `src/ps2/audio`, musenc | CD music format and pipeline decisions, costs |
| [save-games.md](.claude/rules/save-games.md) | `src/ps2/save`, g_save.c | save design, format, icon, config.cfg policy, how to test |
| [vclpp-submodule.md](.claude/rules/vclpp-submodule.md) | `src/tools/vclpp` | vclpp/parse-utils conventions, verification recipes, CI, MASP mode, tyra |
