---
paths:
  - "src/ps2/**"
  - "src/tools/host/**"
---

# C++ backend conventions (`src/ps2`)

## Language and structure

- `-std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-strict-aliasing`, with
  the `-Werror` set in the Makefile's `EE_CXX_WARNFLAGS`. ps2sdk and miniz headers are
  `-isystem`, so only our code is held to it. id's C is exempt. Don't apply any of this to it.
- Reach the engine through `ps2/common.h`, not by including engine headers directly.
- Seam functions keep their Quake names, because they are `extern "C"` symbols the C engine
  links against: `Sys_*`, `NET_*`, `VID_*`, `IN_*`, `SNDDMA_*`, `CDAudio_*`, `Sys_Save*`, the
  `PS2_*` refexport functions, `PS2_MemAlloc`.
- Allocation goes through `ps2::heap` with a `MemTag`. Failure is a fatal `Sys_Error`, not an
  exception.
- `alignas(N)` is the alignment idiom, not `__attribute__((aligned))`.

## Naming (Quake-flavoured; backend only, not vclpp/parse-utils)

- Types and **all** functions, including file-local `static`/anonymous-namespace helpers:
  PascalCase (`ps2::gs::Init`, `QwordCount`).
- Variables: camelCase. Private members: `m_`. Public struct members: no prefix.
- File/local statics: `s_` (`s_frame`). Exported globals: `g_`.
- Namespaces stay lowercase (`ps2::gs`, `ps2::rs`, `ps2::tex`). Leave `k`-prefixed constants
  as they are.
- A helper written in lowerCamelCase gets renamed to PascalCase, callers included.

## File style

- Every file opens with the banner block (`File:`, `Brief:` explaining what and *why*, the GPL
  v2 line). Copy it from a neighbour such as `renderer/gs.h`. Group code under
  `// ----...` section headers like the existing files.
- Comments explain decisions and hardware reasons, the way the existing code does. Formatting
  follows the root `_clang-format`.

## Sized types

- Use ps2sdk's `u8/u16/u32/u64` and `s8..s64` from `tamtypes.h` in backend code.
- `<cstdint>` is fine in headers shared with host tools, where `tamtypes.h` doesn't exist
  (e.g. `audio/spu_adpcm.h`, used by `tools/host/musenc.cpp`).
- **Never mix the two at a ps2sdk pointer boundary.** On the EE ABI `std::uint32_t` is
  `unsigned long` while `u32` is `unsigned int`: same width, incompatible pointer types.

## Passing the strict `-Werror` set

Only *implicit* conversions warn. An explicit `static_cast` is the fix almost everywhere.

- `-Wcast-align`: byte buffer → struct pointer must go through `void*`:
  `static_cast<const T *>(static_cast<const void *>(ptr))` (as image_load.cpp does).
- `-Wconversion`/`-Wsign-conversion`: `int / sizeof(X)` promotes the int to `size_t`, so
  write `static_cast<int>(sizeof(X))`. Size arguments to `memcpy`/`Alloc` need explicit
  `static_cast<size_t>`/`<u32>`.
- `-Wdouble-promotion`: float literals take the `f` suffix. Cast int operands to float in
  float math (`static_cast<float>(n)`), and keep constants float (`constexpr float`).
  `std::floor/ceil` on a float pick the float overload.
- `-Wnull-dereference`: **`Sys_Error` is not `[[noreturn]]`** (it's in a game header; don't
  change it), so neither it nor `PS2_Assert(p)` proves `p` non-null to GCC. After
  `if (!p) { Sys_Error(...); }` add `return X; // unreachable; Sys_Error halts (it is not marked noreturn)`.
  If a helper has any path returning null (e.g. a "measure" mode), every dereference of its
  result warns. Remove that path, e.g. by splitting a sizer type from the allocator. This can
  surface when an int index becomes a pointer.
- `-Wdeprecated-copy`: a user-provided `operator=` on an aggregate errors at every copy site.
  Wrap the *class definition* in `#pragma GCC diagnostic push/ignored "-Wdeprecated-copy"/pop`.
  GCC checks the state at the class, not at the use. Don't declare a copy ctor instead: any
  user-declared constructor ends aggregate init in C++20. A union holding such a member loses
  its implicit copy assignment.
- `-Wsign-conversion` on a template array bound: `T a[N]` with `template<int N>` warns. Write
  `T a[static_cast<u32>(N)]` and keep the parameter `int`, so comparisons against int counters
  stay clean.
- `-Wclass-memaccess`: `memset` of a struct with default member initializers needs
  `static_cast<void *>(&obj)`.
- `-Wduplicated-branches`: `GS_PSM_32 == 0`, so `pal ? GS_PSM_32 : 0` trips it.
- Don't include `io_common.h` in C++: it trips the "fio/fileXio directly" `#error` in the
  clangd view. Define the few `FIO_O_*` constants you need.
